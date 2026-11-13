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

#pragma once

// NOTE: library-private header (pulls in GFillBackend behind PRIVATE compile
// definitions); only included by gemfony-hap TUs, never installed.

// Geneva headers go here
#include "common/concurrency/GSPSCStagingRingT.hpp" // the shared lock-free staging ring
#include "hap/GRandomFactory.hpp"                   // for randomFactory() / getSeed()
#include "GFillBackend.hpp"                          // library-private engine selection (GPU/SIMD/scalar)

// Standard headers go here
#include <cstddef>
#include <cstdint>

namespace Gem::Hap::detail {

/******************************************************************************/
/**
 * @brief The STAGED / QUARANTINE random sources' bulk-filled rotating pool: a Hap-specific binding of
 * the shared lock-free Gem::Common::Concurrency::GSPSCStagingRingT to the Hap fill backend.
 *
 * The generic ring (the rotating pools, the atomic claim cursor, the background producer and the benign
 * refill-vs-stale-reader race analysed there) lives in common; this thin wrapper only owns the
 * @ref GFillBackend (the SIMD/cuRAND/scalar engine, seeded from the random factory) and feeds its
 * @c generate() to the ring as the bulk-fill callback. claimSpan() / copyChunkInto() forward straight
 * through, so the per-chunk read path is unchanged.
 *
 * @note Member order matters: @c ring_ is declared after @c backend_ so it is destroyed first -- its
 * destructor joins the producer thread (which calls into @c backend_) before @c backend_ itself goes
 * away. The fill callback captures @c this and reaches @c backend_, which is fully constructed by the
 * time the ring's constructor runs the initial fill / starts the producer.
 *
 * @tparam ChunkWords     Words per claimed chunk
 * @tparam ChunksPerPool  Chunks per pool (pool size = ChunkWords * ChunksPerPool words)
 * @tparam NPools         Number of rotating pools (must be >= 3 for the quarantine separation)
 */
template <std::size_t ChunkWords, std::size_t ChunksPerPool = 64, int NPools = 4>
class GRotatingPool {
public:
    GRotatingPool()
      : backend_(static_cast<std::uint64_t>(randomFactory()->getSeed()))
      , ring_([this](std::uint64_t *dst, std::size_t n_words) { backend_.generate(dst, n_words); }) {}

    GRotatingPool(const GRotatingPool &) = delete;
    GRotatingPool(GRotatingPool &&) = delete;
    GRotatingPool &operator=(const GRotatingPool &) = delete;
    GRotatingPool &operator=(GRotatingPool &&) = delete;

    /** @brief Claims the next chunk and returns a pointer to its span for in-place reading.
     *  @return Pointer to ChunkWords words inside one of the pools */
    const std::uint64_t *claimSpan() {
        return ring_.claimSpan();
    }

    /** @brief Claims the next chunk and copies it out word-by-word into dst.
     *  @param dst Destination buffer (at least ChunkWords words) */
    void copyChunkInto(std::uint64_t *dst) {
        ring_.copyChunkInto(dst);
    }

private:
    GFillBackend backend_; ///< the shared bulk source (chosen once); MUST outlive ring_ (declared first)
    Gem::Common::Concurrency::GSPSCStagingRingT<ChunkWords, ChunksPerPool, NPools> ring_;
};

/******************************************************************************/

} /* namespace Gem::Hap::detail */
