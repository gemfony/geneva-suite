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

TEST_CASE("GContentAddressedStoreT: non-copyable, default-constructible", "[common][content-store]") {
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
    CHECK(out == "stale");                 // miss leaves out untouched

    s.put(1, "blob-1");
    CHECK(s.has(1));
    CHECK(s.size() == 1);
    REQUIRE(s.tryGet(1, out));
    CHECK(out == "blob-1");
}

TEST_CASE("GContentAddressedStoreT: put is idempotent and overwrites in place", "[common][content-store]") {
    Store s;
    s.put(1, "first");
    s.put(1, "second");                    // same id -> overwrite, not a second entry
    CHECK(s.size() == 1);
    std::string out;
    REQUIRE(s.tryGet(1, out));
    CHECK(out == "second");
}

// ---------------------------------------------------------------------------
// LRU capacity eviction + recency

TEST_CASE("GContentAddressedStoreT: capacity bound evicts the least-recently-used", "[common][content-store]") {
    Store s;
    s.setCapacity(2);
    s.put(1, "a");
    s.put(2, "b");
    s.put(3, "c");                         // exceeds cap 2 -> id 1 (oldest) evicted
    CHECK(s.size() == 2);
    CHECK_FALSE(s.has(1));
    CHECK(s.has(2));
    CHECK(s.has(3));
}

TEST_CASE("GContentAddressedStoreT: a tryGet refreshes recency so the touched id survives",
          "[common][content-store]") {
    Store s;
    s.setCapacity(2);
    s.put(1, "a");
    s.put(2, "b");                         // recency (MRU->LRU): 2, 1

    std::string out;
    REQUIRE(s.tryGet(1, out));             // touch 1 -> recency: 1, 2
    s.put(3, "c");                         // evict LRU == 2, not 1
    CHECK(s.has(1));                       // survived because it was touched
    CHECK_FALSE(s.has(2));                 // evicted
    CHECK(s.has(3));
}

TEST_CASE("GContentAddressedStoreT: shrinking the capacity evicts down immediately", "[common][content-store]") {
    Store s;
    s.put(1, "a");
    s.put(2, "b");
    s.put(3, "c");
    s.put(4, "d");                         // unbounded so far
    CHECK(s.size() == 4);
    s.setCapacity(2);                      // shrink -> evicts the two LRU (1, 2)
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

    s.forgetPeer(7);                       // session 7 ended
    CHECK_FALSE(s.peerHas(7, 1));
    CHECK(s.peerHas(8, 1));                // peer 8 unaffected
    CHECK(s.trackedPeers() == 1);
}

TEST_CASE("GContentAddressedStoreT: evicting a blob also clears it from every peer's ack set",
          "[common][content-store]") {
    // The critical invariant: after a blob is evicted, no peer may still be recorded as holding it --
    // otherwise the source would reference an id by content it can no longer answer (a deadlock).
    Store s;
    s.setCapacity(1);
    s.put(1, "a");
    s.markPeerHas(7, 1);
    s.markPeerHas(8, 1);
    REQUIRE(s.peerHas(7, 1));
    REQUIRE(s.peerHas(8, 1));

    s.put(2, "b");                         // cap 1 -> id 1 evicted
    CHECK_FALSE(s.has(1));
    CHECK_FALSE(s.peerHas(7, 1));          // ack cleared on eviction
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

TEST_CASE("GContentAddressedStoreT: concurrent put/get/ack remains consistent and bounded",
          "[common][content-store][concurrency]") {
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
                (void)s.tryGet(id, out);    // may already be evicted under churn; just must not corrupt
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
