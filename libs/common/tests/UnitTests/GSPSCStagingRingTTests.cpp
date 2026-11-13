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

#include "common/concurrency/GSPSCStagingRingT.hpp"

using namespace Gem::Common::Concurrency;

namespace {

// Small geometry keeps the tests fast: 8 words/chunk, 4 chunks/pool, 4 pools.
constexpr std::size_t kChunkWords = 8;
constexpr std::size_t kChunksPerPool = 4;
constexpr int kNPools = 4;
using Ring = GSPSCStagingRingT<kChunkWords, kChunksPerPool, kNPools>;

constexpr std::uint64_t kSentinel = 0xA5A5A5A5DEADBEEFULL;

/** @brief A fill that paints every word of a pool with the same sentinel. With this fill EVERY word
 *  any reader ever claims must equal the sentinel: an older (stale) generation was painted the same,
 *  and a torn old-or-new word mixes two sentinels -- still the sentinel. So any non-sentinel word is a
 *  genuine bug (garbage / wrong offset / uninitialised), giving a zero-flake validity oracle. */
auto sentinel_fill = [](std::uint64_t *dst, std::size_t n) {
    for(std::size_t i = 0; i < n; ++i) {
        dst[i] = kSentinel;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE("GSPSCStagingRingT: non-copyable, non-movable", "[common][spsc-ring]") {
    static_assert(not std::is_copy_constructible_v<Ring>);
    static_assert(not std::is_move_constructible_v<Ring>);
    static_assert(not std::is_copy_assignable_v<Ring>);
    static_assert(not std::is_move_assignable_v<Ring>);
}

// ---------------------------------------------------------------------------
// The injected fill runs for the initial generations at construction.

TEST_CASE("GSPSCStagingRingT: the fill callback populates the initial generations", "[common][spsc-ring]") {
    std::atomic<int> fill_calls{0};
    {
        Ring const ring([&fill_calls](std::uint64_t *dst, std::size_t n) {
            fill_calls.fetch_add(1, std::memory_order_relaxed);
            for(std::size_t i = 0; i < n; ++i) {
                dst[i] = kSentinel;
            }
        });
        // The constructor fills NPools generations before the producer thread even starts.
        CHECK(fill_calls.load() >= kNPools);
    }
}

// ---------------------------------------------------------------------------
// Single-threaded read paths agree and are valid.

TEST_CASE("GSPSCStagingRingT: claimSpan and copyChunkInto both yield filled words", "[common][spsc-ring]") {
    Ring ring(sentinel_fill);

    for(int c = 0; c < 32; ++c) {
        const std::uint64_t *span = ring.claimSpan();
        for(std::size_t i = 0; i < kChunkWords; ++i) {
            CHECK(span[i] == kSentinel);
        }

        std::uint64_t buf[kChunkWords] = {0};
        ring.copyChunkInto(buf);
        for(std::size_t i = 0; i < kChunkWords; ++i) {
            CHECK(buf[i] == kSentinel);
        }
    }
}

// ---------------------------------------------------------------------------
// Distinct chunks within one pool decode to distinct, non-overlapping memory.

TEST_CASE("GSPSCStagingRingT: consecutive claims advance through distinct chunk slots",
          "[common][spsc-ring]") {
    // A fill that paints each word with its absolute index within the pool, so we can see the offset
    // decode walk chunk slots 0,1,2,... within a pool (each generation re-paints the same pattern).
    auto index_fill = [](std::uint64_t *dst, std::size_t n) {
        for(std::size_t i = 0; i < n; ++i) {
            dst[i] = i;
        }
    };
    Ring ring(index_fill);

    // The first kChunksPerPool claims map to offsets 0, W, 2W, ... within generation-0's pool.
    for(std::size_t chunk = 0; chunk < kChunksPerPool; ++chunk) {
        const std::uint64_t *span = ring.claimSpan();
        const std::uint64_t expected_base = chunk * kChunkWords;
        for(std::size_t i = 0; i < kChunkWords; ++i) {
            // Words are consecutive within the chunk and start at the chunk's offset in the pool.
            REQUIRE(span[i] == expected_base + i);
        }
    }
}

// ---------------------------------------------------------------------------
// Many concurrent readers: every claimed word is valid; the ring never tears or hangs.

// NOLINTNEXTLINE(readability-function-size) -- one coherent concurrency stress-test kernel (spawns reader threads sharing atomics + the ring under test, joins, then asserts); splitting would only scatter the tightly-coupled thread lambdas
TEST_CASE("GSPSCStagingRingT: concurrent readers only ever see valid words", "[common][spsc-ring][concurrency]") {
    Ring ring(sentinel_fill);

    constexpr int kReaders = 8;
    constexpr int kClaimsPerReader = 50000;
    std::atomic<long> bad{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for(int r = 0; r < kReaders; ++r) {
        // Half the readers use the in-place span, half copy out -- exercise both read modes.
        const bool copy_out = (r % 2 == 0);
        readers.emplace_back([&ring, &bad, &go, copy_out] {
            while(not go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::uint64_t buf[kChunkWords];
            for(int c = 0; c < kClaimsPerReader; ++c) {
                if(copy_out) {
                    ring.copyChunkInto(buf);
                    for(unsigned long long const w : buf) {
                        if(w != kSentinel) {
                            bad.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                else {
                    const std::uint64_t *span = ring.claimSpan();
                    for(std::size_t i = 0; i < kChunkWords; ++i) {
                        if(span[i] != kSentinel) {
                            bad.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for(auto &t : readers) {
        t.join();
    }

    CHECK(bad.load() == 0);
}
