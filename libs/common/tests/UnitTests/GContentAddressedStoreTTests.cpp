/********************************************************************************
 *
 * This file is part of the Geneva library collection. The following license
 * applies to this file:
 *
 * ------------------------------------------------------------------------------
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ------------------------------------------------------------------------------
 *
 * Note that other files in the Geneva library collection may use a different
 * license. Please see the licensing information in each file.
 *
 ********************************************************************************
 *
 * See the NOTICE file in the top-level directory of the Geneva library
 * collection for a list of contributors and copyright information.
 *
 ********************************************************************************/

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "common/concurrency/GContentAddressedStoreT.hpp"

using namespace Gem::Common::Concurrency;

namespace {
using Store = GContentAddressedStoreT<int, std::string, int>;
} // namespace

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE(
    "GContentAddressedStoreT: non-copyable and default-constructible",
    "[common][content-store]"
) {
    static_assert(std::is_default_constructible_v<Store>);
    static_assert(not std::is_copy_constructible_v<Store>);
    static_assert(not std::is_copy_assignable_v<Store>);
}

// ---------------------------------------------------------------------------
// Blob store: put / has / tryGet

TEST_CASE("GContentAddressedStoreT: put/has/tryGet round-trip", "[common][content-store]") {
    Store s;
    CHECK(s.size() == 0);
    CHECK_FALSE(s.has(1));

    std::string out = "stale";
    CHECK_FALSE(s.tryGet(1, out));
    CHECK(out == "stale"); // miss leaves out untouched

    s.put(1, "blob-1");
    CHECK(s.has(1));
    CHECK(s.size() == 1);
    REQUIRE(s.tryGet(1, out));
    CHECK(out == "blob-1");
}

TEST_CASE(
    "GContentAddressedStoreT: put is idempotent and overwrites in place",
    "[common][content-store]"
) {
    Store s;
    s.put(1, "first");
    s.put(1, "second"); // same id -> overwrite, not a second entry
    CHECK(s.size() == 1);
    std::string out;
    REQUIRE(s.tryGet(1, out));
    CHECK(out == "second");
}

// ---------------------------------------------------------------------------
// LRU capacity eviction + recency

TEST_CASE(
    "GContentAddressedStoreT: capacity bound evicts the least-recently-used",
    "[common][content-store]"
) {
    Store s;
    s.setCapacity(2);
    s.put(1, "a");
    s.put(2, "b");
    s.put(3, "c"); // exceeds cap 2 -> id 1 (oldest) evicted
    CHECK(s.size() == 2);
    CHECK_FALSE(s.has(1));
    CHECK(s.has(2));
    CHECK(s.has(3));
}

TEST_CASE(
    "GContentAddressedStoreT: a tryGet refreshes recency so the touched id survives",
    "[common][content-store]"
) {
    Store s;
    s.setCapacity(2);
    s.put(1, "a");
    s.put(2, "b"); // recency (MRU->LRU): 2, 1

    std::string out;
    REQUIRE(s.tryGet(1, out)); // touch 1 -> recency: 1, 2
    s.put(3, "c");             // evict LRU == 2, not 1
    CHECK(s.has(1));           // survived because it was touched
    CHECK_FALSE(s.has(2));     // evicted
    CHECK(s.has(3));
}

TEST_CASE(
    "GContentAddressedStoreT: shrinking the capacity evicts down immediately",
    "[common][content-store]"
) {
    Store s;
    s.put(1, "a");
    s.put(2, "b");
    s.put(3, "c");
    s.put(4, "d"); // unbounded so far
    CHECK(s.size() == 4);
    s.setCapacity(2); // shrink -> evicts the two LRU (1, 2)
    CHECK(s.size() == 2);
    CHECK(s.has(3));
    CHECK(s.has(4));
}

TEST_CASE("GContentAddressedStoreT: capacity 0 means unbounded", "[common][content-store]") {
    Store s;
    s.setCapacity(0);
    for(int i = 0; i < 1000; ++i) {
        s.put(i, "x");
    }
    CHECK(s.size() == 1000);
}

// ---------------------------------------------------------------------------
// Per-peer ack tracking

TEST_CASE("GContentAddressedStoreT: per-peer ack set/query/forget", "[common][content-store]") {
    Store s;
    CHECK_FALSE(s.peerHas(7, 1));
    CHECK(s.trackedPeers() == 0);

    s.markPeerHas(7, 1);
    s.markPeerHas(7, 2);
    s.markPeerHas(8, 1);
    CHECK(s.peerHas(7, 1));
    CHECK(s.peerHas(7, 2));
    CHECK(s.peerHas(8, 1));
    CHECK_FALSE(s.peerHas(8, 2));
    CHECK(s.trackedPeers() == 2);

    s.forgetPeer(7); // session 7 ended
    CHECK_FALSE(s.peerHas(7, 1));
    CHECK(s.peerHas(8, 1)); // peer 8 unaffected
    CHECK(s.trackedPeers() == 1);
}

TEST_CASE(
    "GContentAddressedStoreT: evicting a blob also clears it from every peer's ack set",
    "[common][content-store]"
) {
    // The critical invariant: after a blob is evicted, no peer may still be recorded as holding it --
    // otherwise the source would reference an id by content it can no longer answer (a deadlock).
    Store s;
    s.setCapacity(1);
    s.put(1, "a");
    s.markPeerHas(7, 1);
    s.markPeerHas(8, 1);
    REQUIRE(s.peerHas(7, 1));
    REQUIRE(s.peerHas(8, 1));

    s.put(2, "b"); // cap 1 -> id 1 evicted
    CHECK_FALSE(s.has(1));
    CHECK_FALSE(s.peerHas(7, 1)); // ack cleared on eviction
    CHECK_FALSE(s.peerHas(8, 1));
}

TEST_CASE("GContentAddressedStoreT: clear resets blobs and acks", "[common][content-store]") {
    Store s;
    s.put(1, "a");
    s.markPeerHas(7, 1);
    s.clear();
    CHECK(s.size() == 0);
    CHECK(s.trackedPeers() == 0);
    CHECK_FALSE(s.has(1));
    CHECK_FALSE(s.peerHas(7, 1));
}

// ---------------------------------------------------------------------------
// Concurrency: many threads put / tryGet / mark must stay consistent and bounded.

TEST_CASE(
    "GContentAddressedStoreT: concurrent put/get/ack remains consistent and bounded",
    "[common][content-store][concurrency]"
) {
    Store s;
    constexpr int kCap = 64;
    s.setCapacity(kCap);

    constexpr int kWriters = 4;
    constexpr int kPerWriter = 500;
    std::atomic<bool> go{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;

    for(int w = 0; w < kWriters; ++w) {
        ts.emplace_back([w, &s, &go, &bad] {
            while(not go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for(int i = 0; i < kPerWriter; ++i) {
                const int id = w * kPerWriter + i;
                s.put(id, "v");
                s.markPeerHas(w, id);
                std::string out;
                (void)
                    s.tryGet(id, out); // may already be evicted under churn; just must not corrupt
            }
        });
    }

    go.store(true, std::memory_order_release);
    for(auto &t : ts) {
        t.join();
    }

    // The capacity bound must hold no matter the interleaving.
    if(s.size() > static_cast<std::size_t>(kCap)) {
        ++bad;
    }
    CHECK(bad.load() == 0);
    CHECK(s.size() <= static_cast<std::size_t>(kCap));
}

// ---------------------------------------------------------------------------
// internFor: cache, acknowledge and decide in one step

TEST_CASE(
    "GContentAddressedStoreT: internFor caches a blob and tells which peer must get it in full",
    "[common][content-store]"
) {
    Store s;
    int built = 0;
    auto const make = [&built]() {
        ++built;
        return std::string("blob");
    };
    CHECK(s.internFor(7, 1, make)); // new: cached, and peer 7 gets it in full
    CHECK(s.has(1));
    CHECK(s.peerHas(7, 1));
    CHECK_FALSE(s.internFor(7, 1, make)); // peer 7 holds it: referred to by id
    CHECK(s.internFor(8, 1, make));       // another peer gets it in full, from the cache
    CHECK(built == 1);                    // built only while it was absent
    std::string out;
    CHECK(s.tryGet(1, out));
    CHECK(out == "blob");
}

/**
 * Regression: a sender that cached and acknowledged with four calls (has, put, peerHas,
 * markPeerHas) could, with an eviction by another thread in between, leave an acknowledgement for
 * a blob the store no longer held. Capacity 1, two ids, two threads; at the end of each round
 * no acknowledgement names an id the store does not hold.
 */
TEST_CASE(
    "GContentAddressedStoreT: internFor never leaves an acknowledgement without its blob",
    "[common][content-store][concurrency]"
) {
    int orphans = 0;
    for(int round = 0; round < 200; ++round) {
        Store s;
        s.setCapacity(1);
        auto const sender = [&s](int first) {
            for(int i = 0; i < 200; ++i) {
                const int id = (first + i) % 2;
                (void)s.internFor(7, id, [id]() { return std::to_string(id); });
            }
        };
        std::thread a(sender, 0);
        std::thread b(sender, 1);
        a.join();
        b.join();
        for(int id = 0; id < 2; ++id) {
            if(s.peerHas(7, id) && not s.has(id)) {
                ++orphans;
            }
        }
    }
    CHECK(orphans == 0);
}

/**
 * The same, deterministic: an eviction by another thread is started from within the step (from
 * make_blob) and has to wait for it, so it comes after the acknowledgement and drops it with its
 * blob. Done as four calls, it came between the caching and the acknowledgement.
 */
TEST_CASE(
    "GContentAddressedStoreT: an eviction during internFor drops the acknowledgement with its blob",
    "[common][content-store][concurrency]"
) {
    Store s;
    s.setCapacity(1);
    std::thread evictor;
    std::atomic<bool> evicting{false};
    CHECK(s.internFor(7, 0, [&s, &evictor, &evicting]() {
        evictor = std::thread([&s, &evicting]() {
            evicting.store(true);
            s.put(1, "1"); // waits for the step's lock
        });
        while(not evicting.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return std::string("0");
    }));
    evictor.join();
    CHECK(s.has(1));
    CHECK_FALSE(s.has(0));
    CHECK_FALSE(s.peerHas(7, 0)); // no acknowledgement without its blob
}
