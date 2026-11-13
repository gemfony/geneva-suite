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
#include <type_traits>
#include <vector>

#include "common/concurrency/GThreadSafeKeyedStoreT.hpp"

using namespace Gem::Common::Concurrency;

namespace {
using Store = GThreadSafeKeyedStoreT<std::string, int>;
} // namespace

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE("GThreadSafeKeyedStoreT: non-copyable, default-constructible", "[common][keyed-store]") {
    static_assert(std::is_default_constructible_v<Store>);
    static_assert(not std::is_copy_constructible_v<Store>);
    static_assert(not std::is_copy_assignable_v<Store>);
}

// ---------------------------------------------------------------------------
// get / set

TEST_CASE("GThreadSafeKeyedStoreT: set/get round-trip (out-param and optional)", "[common][keyed-store]") {
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

TEST_CASE("GThreadSafeKeyedStoreT: a miss leaves the out-param untouched and yields nullopt",
          "[common][keyed-store]") {
    Store s;
    s.set("known", 9);
    int v = 1234;
    CHECK_FALSE(s.get("unknown", v));
    CHECK(v == 1234);                       // untouched on miss
    CHECK_FALSE(s.get("unknown").has_value());
}

TEST_CASE("GThreadSafeKeyedStoreT: set overwrites; setOnce refuses to overwrite", "[common][keyed-store]") {
    Store s;
    CHECK(s.setOnce("k", 10));
    CHECK(s.get("k").value() == 10);
    CHECK_FALSE(s.setOnce("k", 99));        // already present -> no-op
    CHECK(s.get("k").value() == 10);
    s.set("k", 99);                         // plain set overwrites
    CHECK(s.get("k").value() == 99);
}

TEST_CASE("GThreadSafeKeyedStoreT: remove drops only present keys and reports correctly",
          "[common][keyed-store]") {
    Store s;
    s.set("a", 1);
    s.set("b", 2);
    CHECK(s.remove("a"));
    CHECK_FALSE(s.contains("a"));
    CHECK(s.size() == 1);
    CHECK_FALSE(s.remove("a"));             // already gone
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
    CHECK(vals[0] == 1);                     // alpha
    CHECK(vals[1] == 2);                     // mu
    CHECK(vals[2] == 3);                     // zeta
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
// Concurrency: many writers + many readers stay consistent.

// NOLINTNEXTLINE(readability-function-size) -- one coherent concurrency stress-test kernel (spawns writer/reader threads sharing atomics + the store under test, joins, then asserts); splitting would only scatter the tightly-coupled thread lambdas
TEST_CASE("GThreadSafeKeyedStoreT: concurrent set/get/snapshot remains consistent",
          "[common][keyed-store][concurrency]") {
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
