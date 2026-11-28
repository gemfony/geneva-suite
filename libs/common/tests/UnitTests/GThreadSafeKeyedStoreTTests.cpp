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
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/concurrency/GThreadSafeKeyedStoreT.hpp"

using namespace Gem::Common::Concurrency;

namespace {
using Store = GThreadSafeKeyedStoreT<std::string, int>;
} // namespace

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE(
    "GThreadSafeKeyedStoreT: non-copyable and default-constructible",
    "[common][keyed-store]"
) {
    static_assert(std::is_default_constructible_v<Store>);
    static_assert(not std::is_copy_constructible_v<Store>);
    static_assert(not std::is_copy_assignable_v<Store>);
    static_assert(KeyedStore<Store, std::string, int>);
}

// ---------------------------------------------------------------------------
// get / set

TEST_CASE(
    "GThreadSafeKeyedStoreT: set/get round-trip (out-param and optional)",
    "[common][keyed-store]"
) {
    Store s;
    CHECK(s.empty());
    s.set("a", 1);
    s.set("b", 2);
    CHECK_FALSE(s.empty());
    CHECK(s.size() == 2);
    CHECK(s.contains("a"));
    CHECK_FALSE(s.contains("c"));

    int v = -1;
    REQUIRE(s.get("a", v));
    CHECK(v == 1);

    auto ov = s.get("b");
    REQUIRE(ov.has_value());
    CHECK(*ov == 2);
}

TEST_CASE(
    "GThreadSafeKeyedStoreT: a miss leaves the out-param untouched and yields nullopt",
    "[common][keyed-store]"
) {
    Store s;
    s.set("known", 9);
    int v = 1234;
    CHECK_FALSE(s.get("unknown", v));
    CHECK(v == 1234); // untouched on miss
    CHECK_FALSE(s.get("unknown").has_value());
}

TEST_CASE(
    "GThreadSafeKeyedStoreT: set overwrites; setOnce refuses to overwrite",
    "[common][keyed-store]"
) {
    Store s;
    CHECK(s.setOnce("k", 10));
    CHECK(s.get("k").value() == 10);
    CHECK_FALSE(s.setOnce("k", 99)); // already present -> no-op
    CHECK(s.get("k").value() == 10);
    s.set("k", 99); // plain set overwrites
    CHECK(s.get("k").value() == 99);
}

TEST_CASE(
    "GThreadSafeKeyedStoreT: remove drops only present keys and reports correctly",
    "[common][keyed-store]"
) {
    Store s;
    s.set("a", 1);
    s.set("b", 2);
    CHECK(s.remove("a"));
    CHECK_FALSE(s.contains("a"));
    CHECK(s.size() == 1);
    CHECK_FALSE(s.remove("a")); // already gone
    CHECK_FALSE(s.remove("never"));
    CHECK(s.size() == 1);
}

// ---------------------------------------------------------------------------
// snapshots are in key order

TEST_CASE("GThreadSafeKeyedStoreT: keys() and values() are in key order", "[common][keyed-store]") {
    Store s;
    s.set("zeta", 3);
    s.set("alpha", 1);
    s.set("mu", 2);

    const auto keys = s.keys();
    REQUIRE(keys.size() == 3);
    CHECK(keys[0] == "alpha");
    CHECK(keys[1] == "mu");
    CHECK(keys[2] == "zeta");

    const auto vals = s.values();
    REQUIRE(vals.size() == 3);
    CHECK(vals[0] == 1); // alpha
    CHECK(vals[1] == 2); // mu
    CHECK(vals[2] == 3); // zeta
}

TEST_CASE("GThreadSafeKeyedStoreT: clear empties the store", "[common][keyed-store]") {
    Store s;
    s.set("a", 1);
    s.set("b", 2);
    s.clear();
    CHECK(s.empty());
    CHECK(s.size() == 0);
    CHECK(s.keys().empty());
    CHECK(s.values().empty());
}

// ---------------------------------------------------------------------------
// waitUntilEmpty: at once when empty, on the last remove() (not on an earlier one), on clear(), and
// false at its limit. The waits that must end early have a long limit and a much shorter bound on
// the time they took, so a remove() or clear() that does not notify is seen as a bound exceeded.

TEST_CASE(
    "GThreadSafeKeyedStoreT: waitUntilEmpty returns at once when the store is empty",
    "[common][keyed-store][concurrency]"
) {
    using namespace std::chrono_literals;
    Store const s;
    const auto begin = std::chrono::steady_clock::now();
    CHECK(s.waitUntilEmpty(0ms));
    CHECK(s.waitUntilEmpty(10s));
    CHECK(std::chrono::steady_clock::now() - begin < 1s);
}

TEST_CASE(
    "GThreadSafeKeyedStoreT: waitUntilEmpty is false at its limit while an entry stays",
    "[common][keyed-store][concurrency]"
) {
    using namespace std::chrono_literals;
    Store s;
    s.set("a", 1);
    const auto begin = std::chrono::steady_clock::now();
    CHECK_FALSE(s.waitUntilEmpty(50ms));
    CHECK(std::chrono::steady_clock::now() - begin >= 50ms);
    CHECK(s.size() == 1);
}

TEST_CASE(
    "GThreadSafeKeyedStoreT: waitUntilEmpty returns on the last remove and on clear",
    "[common][keyed-store][concurrency]"
) {
    using namespace std::chrono_literals;
    constexpr auto LIMIT = 10s; // what the waits may take at most
    constexpr auto BOUND = 3s;  // what they take with a notification: about 40 ms

    // Shared with the removing threads, which may still hold the store's mutex when the waiter
    // returns (the lifetime rule of waitUntilEmpty()).
    auto s = std::make_shared<Store>();

    SECTION("the last of two removals ends the wait; the first does not") {
        s->set("a", 1);
        s->set("b", 2);
        std::jthread const remover([s] {
            std::this_thread::sleep_for(20ms);
            s->remove("a");
            std::this_thread::sleep_for(20ms);
            s->remove("b");
        });
        const auto begin = std::chrono::steady_clock::now();
        CHECK(s->waitUntilEmpty(LIMIT));
        CHECK(std::chrono::steady_clock::now() - begin < BOUND);
        CHECK(s->empty()); // not woken (or not returning) after the first removal
    }

    SECTION("clear() ends the wait") {
        s->set("c", 3);
        std::jthread const clearer([s] {
            std::this_thread::sleep_for(20ms);
            s->clear();
        });
        const auto begin = std::chrono::steady_clock::now();
        CHECK(s->waitUntilEmpty(LIMIT));
        CHECK(std::chrono::steady_clock::now() - begin < BOUND);
    }
}

// ---------------------------------------------------------------------------
// Concurrency: many writers + many readers stay consistent.

// NOLINTNEXTLINE(readability-function-size) -- one coherent concurrency stress-test kernel (spawns writer/reader threads sharing atomics + the store under test, joins, then asserts); splitting would only scatter the tightly-coupled thread lambdas
TEST_CASE(
    "GThreadSafeKeyedStoreT: concurrent set/get/snapshot remains consistent",
    "[common][keyed-store][concurrency]"
) {
    Store s;
    constexpr int kWriters = 4;
    constexpr int kPerWriter = 256;
    constexpr int kReaders = 4;
    constexpr int kReadPasses = 64;
    constexpr int kMaxValue = kWriters * 1000 + kPerWriter;

    std::atomic<bool> go{false};
    std::atomic<int> out_of_range{0};
    std::vector<std::thread> ts;

    for(int w = 0; w < kWriters; ++w) {
        ts.emplace_back([w, &s, &go] {
            while(not go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for(int i = 0; i < kPerWriter; ++i) {
                s.set("w" + std::to_string(w) + "_k" + std::to_string(i), w * 1000 + i);
            }
        });
    }
    for(int r = 0; r < kReaders; ++r) {
        ts.emplace_back([&s, &go, &out_of_range] {
            while(not go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for(int i = 0; i < kReadPasses; ++i) {
                for(int const v : s.values()) {
                    if(v < 0 || v >= kMaxValue) {
                        ++out_of_range;
                    }
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for(auto &t : ts) {
        t.join();
    }

    CHECK(out_of_range.load() == 0);
    CHECK(s.size() == static_cast<std::size_t>(kWriters * kPerWriter));
}

/******************************************************************************/
TEST_CASE(
    "GThreadSafeKeyedStoreT: take hands one value over and wakes a waiter on the last",
    "[common][keyed-store]"
) {
    Store s;
    s.set("a", 1);
    s.set("b", 2);
    CHECK(s.take("a") == std::optional<int>{1});
    CHECK_FALSE(s.take("a").has_value());
    CHECK(s.size() == 1);
    std::jthread taker([&s] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        static_cast<void>(s.take("b"));
    });
    CHECK(s.waitUntilEmpty(std::chrono::seconds(5)));
}

/******************************************************************************/
TEST_CASE(
    "GThreadSafeKeyedStoreT: drain hands every entry over in key order and empties the store",
    "[common][keyed-store]"
) {
    Store s;
    s.set("b", 2);
    s.set("a", 1);
    const auto drained = s.drain();
    REQUIRE(drained.size() == 2);
    CHECK(drained[0] == std::pair<std::string, int>{"a", 1});
    CHECK(drained[1] == std::pair<std::string, int>{"b", 2});
    CHECK(s.empty());
    CHECK(s.waitUntilEmpty(std::chrono::milliseconds(0)));
    CHECK(s.drain().empty());
}

/******************************************************************************/
namespace {
/** @brief A mapped value without a default constructor. */
struct NoDefault {
    explicit NoDefault(int v)
      : value(v) { /* nothing */
    }
    int value;
};
} // namespace

TEST_CASE(
    "GThreadSafeKeyedStoreT: set and setOnce take a mapped type without a default constructor",
    "[common][keyed-store]"
) {
    GThreadSafeKeyedStoreT<std::string, NoDefault> s;
    s.set("a", NoDefault{1});
    s.set("a", NoDefault{2}); // overwrites
    CHECK(s.setOnce("b", NoDefault{3}));
    CHECK_FALSE(s.setOnce("b", NoDefault{4})); // left unchanged
    CHECK(s.get("a")->value == 2);
    CHECK(s.get("b")->value == 3);
}
