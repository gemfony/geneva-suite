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
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#include "common/concurrency/GAgingStoreT.hpp"

using namespace Gem::Common::Concurrency;

namespace {
// Move-only value type exercises that the store never copies its payload.
using UPtr = std::unique_ptr<int>;
using MoStore = GAgingStoreT<int, UPtr>;
using IntStore = GAgingStoreT<int, int>;
} // namespace

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE("GAgingStoreT: non-copyable, holds move-only values", "[common][aging-store]") {
    static_assert(not std::is_copy_constructible_v<MoStore>);
    static_assert(not std::is_copy_assignable_v<MoStore>);
}

// ---------------------------------------------------------------------------
// Keyed face

TEST_CASE("GAgingStoreT: retain/takeRetained round-trip; take removes; miss yields nullopt",
          "[common][aging-store]") {
    MoStore s;
    s.configure(/*cap*/ 8, /*ttl*/ 100);
    s.retain(7, std::make_unique<int>(42));
    CHECK(s.retainedSize() == 1);

    CHECK_FALSE(s.takeRetained(99).has_value()); // miss

    auto v = s.takeRetained(7);
    REQUIRE(v.has_value());
    CHECK(**v == 42);
    CHECK(s.retainedSize() == 0);                // take removed it
    CHECK_FALSE(s.takeRetained(7).has_value());  // gone now
}

TEST_CASE("GAgingStoreT: retain overwrites an existing key; dropRetained removes", "[common][aging-store]") {
    MoStore s;
    s.configure(8, 100);
    s.retain(1, std::make_unique<int>(10));
    s.retain(1, std::make_unique<int>(20)); // overwrite
    CHECK(s.retainedSize() == 1);
    auto v = s.takeRetained(1);
    REQUIRE(v.has_value());
    CHECK(**v == 20);

    s.retain(2, std::make_unique<int>(99));
    s.dropRetained(2);
    CHECK(s.retainedSize() == 0);
    s.dropRetained(2); // dropping an absent key is a no-op
}

// ---------------------------------------------------------------------------
// FIFO face

TEST_CASE("GAgingStoreT: park/drainParked preserves FIFO order and empties", "[common][aging-store]") {
    MoStore s;
    s.configure(8, 100);
    CHECK(s.park(std::make_unique<int>(1)) == 0); // no eviction under cap
    CHECK(s.park(std::make_unique<int>(2)) == 0);
    CHECK(s.park(std::make_unique<int>(3)) == 0);
    CHECK(s.parkedSize() == 3);

    auto out = s.drainParked();
    REQUIRE(out.size() == 3);
    CHECK(*out[0] == 1);
    CHECK(*out[1] == 2);
    CHECK(*out[2] == 3);
    CHECK(s.parkedSize() == 0); // drain emptied the FIFO
}

TEST_CASE("GAgingStoreT: park enforces the capacity bound, evicting the oldest first",
          "[common][aging-store]") {
    MoStore s;
    s.configure(/*cap*/ 2, /*ttl*/ 100);
    CHECK(s.park(std::make_unique<int>(1)) == 0);
    CHECK(s.park(std::make_unique<int>(2)) == 0);
    CHECK(s.park(std::make_unique<int>(3)) == 1); // over cap -> evicts the oldest (1)
    CHECK(s.parkedSize() == 2);

    auto out = s.drainParked();
    REQUIRE(out.size() == 2);
    CHECK(*out[0] == 2); // 1 was evicted
    CHECK(*out[1] == 3);
}

// ---------------------------------------------------------------------------
// Aging: both faces evict past the TTL horizon as the epoch advances.

TEST_CASE("GAgingStoreT: advanceEpochAndEvict ages out both faces past the TTL", "[common][aging-store]") {
    MoStore s;
    s.configure(/*cap*/ 100, /*ttl*/ 2);

    s.retain(1, std::make_unique<int>(11)); // stamped at epoch 0
    s.park(std::make_unique<int>(22));      // stamped at epoch 0
    CHECK(s.epoch() == 0);

    CHECK(s.advanceEpochAndEvict() == 0); // epoch 1: age 1 < ttl 2 -> nothing evicted
    CHECK(s.retainedSize() == 1);
    CHECK(s.parkedSize() == 1);

    // epoch 2: age 2 >= ttl 2 -> both evicted. Only the parked eviction is counted in the return.
    CHECK(s.advanceEpochAndEvict() == 1);
    CHECK(s.retainedSize() == 0);
    CHECK(s.parkedSize() == 0);
}

TEST_CASE("GAgingStoreT: advanceEpochAndEvict bounds the keyed face to cap (oldest epoch first)",
          "[common][aging-store]") {
    MoStore s;
    s.configure(/*cap*/ 2, /*ttl*/ 1000);

    s.retain(10, std::make_unique<int>(0)); // epoch 0
    s.advanceEpochAndEvict();               // epoch 1
    s.retain(11, std::make_unique<int>(1)); // epoch 1
    s.retain(12, std::make_unique<int>(2)); // epoch 1  -> 3 retained, over cap 2

    s.advanceEpochAndEvict(); // epoch 2: bounds keyed face to 2, evicts the lowest-epoch entry (key 10)
    CHECK(s.retainedSize() == 2);
    CHECK_FALSE(s.takeRetained(10).has_value()); // key 10 was the oldest -> evicted
    CHECK(s.takeRetained(11).has_value());
    CHECK(s.takeRetained(12).has_value());
}

TEST_CASE("GAgingStoreT: buffering() tracks the capacity (0 -> disabled)", "[common][aging-store]") {
    MoStore s;
    CHECK_FALSE(s.buffering()); // default cap is 0 -> retention disabled
    s.configure(4, 8);
    CHECK(s.buffering());
    s.configure(0, 8);
    CHECK_FALSE(s.buffering());
}

// ---------------------------------------------------------------------------
// Concurrency: conservation under churn. Whatever is produced is either drained
// or counted as evicted -- never silently lost or duplicated.

TEST_CASE("GAgingStoreT: concurrent park/drain/advance conserves every item", "[common][aging-store][concurrency]") {
    IntStore s;
    s.configure(/*cap*/ 64, /*ttl*/ 1'000'000); // huge ttl: only the cap evicts, deterministically counted

    constexpr int kProducers = 6;
    constexpr int kPerProducer = 20000;
    const int produced = kProducers * kPerProducer;

    std::atomic<long> evicted{0};
    std::atomic<long> drained{0};
    std::atomic<bool> producing{true};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;

    // Producers park unique-ish payloads; each park() reports how many it evicted.
    for(int p = 0; p < kProducers; ++p) {
        threads.emplace_back([&s, &evicted, &go] {
            while(not go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for(int i = 0; i < kPerProducer; ++i) {
                evicted.fetch_add(static_cast<long>(s.park(i)), std::memory_order_relaxed);
            }
        });
    }
    // A roundadvancer ages the store (only cap evicts here, since ttl is huge).
    threads.emplace_back([&s, &evicted, &producing, &go] {
        while(not go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while(producing.load(std::memory_order_acquire)) {
            evicted.fetch_add(static_cast<long>(s.advanceEpochAndEvict()), std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    // A drainer repeatedly empties the FIFO.
    threads.emplace_back([&s, &drained, &producing, &go] {
        while(not go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while(producing.load(std::memory_order_acquire)) {
            drained.fetch_add(static_cast<long>(s.drainParked().size()), std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    go.store(true, std::memory_order_release);
    // Join the producers first.
    for(int p = 0; p < kProducers; ++p) {
        threads[static_cast<std::size_t>(p)].join();
    }
    producing.store(false, std::memory_order_release);
    threads[kProducers].join();     // advancer
    threads[kProducers + 1].join(); // drainer

    // Final drain mops up anything the drainer missed after producing stopped.
    drained.fetch_add(static_cast<long>(s.drainParked().size()), std::memory_order_relaxed);

    CHECK(drained.load() + evicted.load() == produced); // nothing lost, nothing duplicated
    CHECK(s.parkedSize() == 0);
}
