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
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#include "common/concurrency/GThreadSafeSetT.hpp"

using namespace Gem::Common::Concurrency;

namespace {
using Set = GThreadSafeSetT<std::uint64_t>;
} // namespace

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE("GThreadSafeSetT: non-copyable", "[common][ts-set]") {
    static_assert(not std::is_copy_constructible_v<Set>);
    static_assert(not std::is_copy_assignable_v<Set>);
}

// ---------------------------------------------------------------------------
// Membership

TEST_CASE("GThreadSafeSetT: insert/erase/contains report set semantics", "[common][ts-set]") {
    Set s;
    CHECK(s.empty());
    CHECK(s.insert(7));
    CHECK_FALSE(s.insert(7));     // already present -> no-op, reports false
    CHECK(s.size() == 1);
    CHECK(s.contains(7));
    CHECK_FALSE(s.contains(8));

    CHECK(s.erase(7));
    CHECK_FALSE(s.erase(7));      // already gone
    CHECK(s.empty());
}

// ---------------------------------------------------------------------------
// drain

TEST_CASE("GThreadSafeSetT: drain returns every element (ascending) and empties the set", "[common][ts-set]") {
    Set s;
    s.insert(30);
    s.insert(10);
    s.insert(20);

    const auto out = s.drain();
    REQUIRE(out.size() == 3);
    CHECK(out[0] == 10);
    CHECK(out[1] == 20);
    CHECK(out[2] == 30);
    CHECK(s.empty());

    CHECK(s.drain().empty()); // draining an empty set is fine
}

TEST_CASE("GThreadSafeSetT: clear empties the set", "[common][ts-set]") {
    Set s;
    s.insert(1);
    s.insert(2);
    s.clear();
    CHECK(s.empty());
}

// ---------------------------------------------------------------------------
// Concurrency: this models the CheckoutLease use -- one strand inserts/erases
// while another thread drains (the abandoning destructor). Every id is either
// erased (returned normally) or drained (reclaimed) exactly once: nothing is
// lost or double-counted.

TEST_CASE("GThreadSafeSetT: concurrent insert/erase/drain conserves every id", "[common][ts-set][concurrency]") {
    Set s;
    constexpr std::uint64_t kIds = 100000;

    std::atomic<long> erased{0};
    std::atomic<long> drained{0};
    std::atomic<bool> inserting{true};
    std::atomic<bool> go{false};

    // Inserter: hand out ids 0..kIds-1.
    std::thread inserter([&s, &inserting, &go] {
        while(not go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for(std::uint64_t id = 0; id < kIds; ++id) {
            s.insert(id);
        }
        inserting.store(false, std::memory_order_release);
    });
    // Eraser: returns some of them normally (only erases ids it actually removed).
    std::thread eraser([&s, &erased, &go] {
        while(not go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for(std::uint64_t id = 0; id < kIds; id += 2) { // try to erase the evens
            if(s.erase(id)) {
                erased.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    // Drainer: periodically reclaims whatever is currently in flight.
    std::thread drainer([&s, &drained, &inserting, &go] {
        while(not go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while(inserting.load(std::memory_order_acquire)) {
            drained.fetch_add(static_cast<long>(s.drain().size()), std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    go.store(true, std::memory_order_release);
    inserter.join();
    eraser.join();
    drainer.join();

    // Mop up anything inserted after the drainer's last pass.
    drained.fetch_add(static_cast<long>(s.drain().size()), std::memory_order_relaxed);

    // Every id inserted left exactly once: either erased normally or drained for reclaim.
    CHECK(erased.load() + drained.load() == static_cast<long>(kIds));
    CHECK(s.empty());
}
