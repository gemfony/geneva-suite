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
#include <thread>
#include <type_traits>
#include <vector>

#include "common/concurrency/GCompletionLatchT.hpp"

using namespace Gem::Common::Concurrency;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE("GCompletionLatchT: non-copyable", "[common][latch]") {
    static_assert(not std::is_copy_constructible_v<GCompletionLatchT>);
    static_assert(not std::is_copy_assignable_v<GCompletionLatchT>);
}

// ---------------------------------------------------------------------------
// Single-threaded count-down semantics

TEST_CASE("GCompletionLatchT: count_down reports the last completion and reaches zero", "[common][latch]") {
    GCompletionLatchT latch(3);
    CHECK(latch.remaining() == 3);
    CHECK_FALSE(latch.count_down());   // 3 -> 2
    CHECK(latch.remaining() == 2);
    CHECK_FALSE(latch.count_down());   // 2 -> 1
    CHECK(latch.count_down());         // 1 -> 0  (the last)
    CHECK(latch.remaining() == 0);
}

TEST_CASE("GCompletionLatchT: a latch armed for zero is already complete", "[common][latch]") {
    GCompletionLatchT latch(0);
    CHECK(latch.remaining() == 0);
    latch.wait();                      // returns immediately, no hang
    CHECK(latch.wait_for(0ms));        // already complete -> true
}

TEST_CASE("GCompletionLatchT: wait() returns once the count is fully drained (single thread)",
          "[common][latch]") {
    GCompletionLatchT latch(2);
    latch.count_down();
    latch.count_down();
    latch.wait();                      // must not block
    CHECK(latch.remaining() == 0);
}

// ---------------------------------------------------------------------------
// wait_for timeout behaviour

TEST_CASE("GCompletionLatchT: wait_for times out while completions are outstanding", "[common][latch]") {
    GCompletionLatchT latch(1);
    CHECK_FALSE(latch.wait_for(20ms));  // still 1 outstanding -> timeout
    latch.count_down();
    CHECK(latch.wait_for(20ms));        // now drained -> success without waiting
}

// ---------------------------------------------------------------------------
// Re-arm for reuse

TEST_CASE("GCompletionLatchT: arm() re-uses the latch for a fresh round", "[common][latch]") {
    GCompletionLatchT latch(1);
    CHECK(latch.count_down());
    CHECK(latch.remaining() == 0);

    latch.arm(2);
    CHECK(latch.remaining() == 2);
    CHECK_FALSE(latch.count_down());
    CHECK(latch.count_down());
    latch.wait();
    CHECK(latch.remaining() == 0);
}

// ---------------------------------------------------------------------------
// The real use: N worker threads count down, one waiter blocks until all done.
// This is exactly the GStdThreadConsumerT per-batch pattern. The latch is held
// via shared_ptr by every worker AND the waiter, mirroring the consumer, so it
// outlives the last count_down() (see the lifetime note on the primitive).

TEST_CASE("GCompletionLatchT: waiter unblocks only after every worker counts down",
          "[common][latch][concurrency]") {
    constexpr std::size_t kWorkers = 64;
    auto latch = std::make_shared<GCompletionLatchT>(kWorkers);
    std::atomic<std::size_t> done{0};

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for(std::size_t i = 0; i < kWorkers; ++i) {
        workers.emplace_back([latch, &done] {
            // A little jitter so completions genuinely interleave with the waiter.
            std::this_thread::sleep_for(1ms);
            done.fetch_add(1, std::memory_order_relaxed);
            latch->count_down();
        });
    }

    latch->wait();
    // When wait() returns, every worker must have passed its count_down().
    CHECK(done.load() == kWorkers);
    CHECK(latch->remaining() == 0);

    for(auto &t : workers) {
        t.join();
    }
}

TEST_CASE("GCompletionLatchT: stress -- repeated batches each release exactly their own waiter",
          "[common][latch][concurrency]") {
    constexpr int kRounds = 200;
    constexpr std::size_t kBatch = 16;

    for(int round = 0; round < kRounds; ++round) {
        auto latch = std::make_shared<GCompletionLatchT>(kBatch);
        std::atomic<std::size_t> completed{0};

        std::vector<std::thread> ts;
        ts.reserve(kBatch);
        for(std::size_t i = 0; i < kBatch; ++i) {
            ts.emplace_back([latch, &completed] {
                completed.fetch_add(1, std::memory_order_relaxed);
                latch->count_down();
            });
        }

        latch->wait();
        REQUIRE(completed.load() == kBatch);   // no early wakeup across 200 rounds

        for(auto &t : ts) {
            t.join();
        }
    }
}
