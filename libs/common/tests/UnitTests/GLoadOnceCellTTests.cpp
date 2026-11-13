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
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "common/concurrency/GLoadOnceCellT.hpp"

using namespace Gem::Common::Concurrency;

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE("GLoadOnceCellT: non-copyable and non-movable", "[common][cell]") {
    static_assert(not std::is_copy_constructible_v<GLoadOnceCellT<int>>);
    static_assert(not std::is_copy_assignable_v<GLoadOnceCellT<int>>);
    // The mutex member also makes the cell non-movable.
    static_assert(not std::is_move_constructible_v<GLoadOnceCellT<int>>);
    static_assert(not std::is_move_assignable_v<GLoadOnceCellT<int>>);
}

// ---------------------------------------------------------------------------
// Externally-filled flavour: ensureLoaded() + loaded() + get()

TEST_CASE("GLoadOnceCellT: fills once, reads immutably, refuses read-before-load", "[common][cell]") {
    GLoadOnceCellT<std::vector<double>> cell;

    CHECK_FALSE(cell.loaded());
    CHECK_THROWS(cell.get()); // read before load is a programming error

    int fillCount = 0;
    const auto fill = [&fillCount]() {
        ++fillCount;
        return std::vector<double>{1.5, 2.5, 3.5};
    };

    cell.ensureLoaded(fill);
    REQUIRE(cell.loaded());
    REQUIRE(fillCount == 1);
    CHECK(cell.get() == std::vector<double>{1.5, 2.5, 3.5});

    // A second ensureLoaded() is a no-op: the payload is immutable after the first fill.
    cell.ensureLoaded([]() { return std::vector<double>{9.9}; });
    CHECK(fillCount == 1);
    CHECK(cell.get() == std::vector<double>{1.5, 2.5, 3.5});
}

// ---------------------------------------------------------------------------
// Lazily-self-computed flavour: getOrCompute()

TEST_CASE("GLoadOnceCellT: getOrCompute computes once and returns a stable reference", "[common][cell]") {
    GLoadOnceCellT<int> cell;
    int computeCount = 0;

    CHECK_FALSE(cell.loaded());

    const int &first = cell.getOrCompute([&computeCount]() { ++computeCount; return 42; });
    CHECK(cell.loaded());
    CHECK(first == 42);
    CHECK(computeCount == 1);

    // The producer is not run again; the cached value (and its address) are stable.
    const int &second = cell.getOrCompute([&computeCount]() { ++computeCount; return 99; });
    CHECK(second == 42);
    CHECK(computeCount == 1);
    CHECK(&first == &second);

    // get() now succeeds and agrees with the cached value.
    CHECK(cell.get() == 42);
}

// ---------------------------------------------------------------------------
// Re-keying: reset() drops the payload so the next access fills afresh

TEST_CASE("GLoadOnceCellT: reset() re-opens the cell for a new fill", "[common][cell]") {
    GLoadOnceCellT<int> cell;
    int computeCount = 0;
    const auto compute = [&computeCount]() { return ++computeCount; };

    CHECK(cell.getOrCompute(compute) == 1);
    CHECK(cell.getOrCompute(compute) == 1); // still load-once between resets

    cell.reset();
    CHECK_FALSE(cell.loaded());
    CHECK_THROWS(cell.get()); // an unloaded cell refuses get() again

    CHECK(cell.getOrCompute(compute) == 2); // the producer runs once more, exactly once
    CHECK(cell.getOrCompute(compute) == 2);
    CHECK(cell.get() == 2);
    CHECK(computeCount == 2);
}

TEST_CASE("GLoadOnceCellT: reset() on an unloaded cell is harmless", "[common][cell]") {
    GLoadOnceCellT<std::vector<double>> cell;

    cell.reset();
    CHECK_FALSE(cell.loaded());
    CHECK_THROWS(cell.get());

    cell.ensureLoaded([]() { return std::vector<double>{4.25}; });
    CHECK(cell.get() == std::vector<double>{4.25});

    cell.reset();
    cell.reset(); // and repeated resets are equally harmless
    CHECK_FALSE(cell.loaded());
    cell.ensureLoaded([]() { return std::vector<double>{8.5}; });
    CHECK(cell.get() == std::vector<double>{8.5});
}

TEST_CASE("GLoadOnceCellT: a throwing producer does not consume the fill", "[common][cell]") {
    GLoadOnceCellT<int> cell;
    int attempts = 0;

    // The load-once guarantee must not turn a *failed* fill into a permanently empty cell: this is
    // what a caller filling from a failure-prone source (a config file, a parse) relies on.
    CHECK_THROWS(cell.getOrCompute([&attempts]() -> int { ++attempts; throw std::runtime_error("nope"); }));
    CHECK_FALSE(cell.loaded());
    CHECK(attempts == 1);

    CHECK(cell.getOrCompute([&attempts]() { ++attempts; return 5; }) == 5);
    CHECK(cell.loaded());
    CHECK(attempts == 2);
}

// ---------------------------------------------------------------------------
// Concurrency: the producer runs exactly once under contention

TEST_CASE("GLoadOnceCellT: concurrent first calls fill exactly once", "[common][cell]") {
    GLoadOnceCellT<int> cell;
    std::atomic<int> computeCount{0};

    constexpr int nThreads = 16;
    std::vector<std::thread> threads;
    std::vector<int> results(nThreads, 0);
    threads.reserve(nThreads);

    for(int t = 0; t < nThreads; ++t) {
        threads.emplace_back([&, t]() {
            results[t] = cell.getOrCompute([&computeCount]() {
                computeCount.fetch_add(1, std::memory_order_relaxed);
                return 7;
            });
        });
    }
    for(auto &th : threads) { th.join(); }

    CHECK(computeCount.load() == 1);       // the producer ran exactly once despite the race
    CHECK(cell.loaded());
    for(int t = 0; t < nThreads; ++t) {
        CHECK(results[t] == 7);            // every thread observed the single filled value
    }
}
