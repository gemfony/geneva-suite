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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard headers go here
#include <cstddef>
#include <cstdint>

namespace Gem::Hap {

/******************************************************************************/
/**
 * The number of 64-bit words a STAGED proxy claims per chunk -- and the size of
 * each of its two private double-buffer halves. One claim copies this many words
 * out of the shared staging pool into a proxy-private buffer; the proxy then
 * serves that many numbers with no further synchronisation. Sized so the copy
 * amortises the per-claim cost to a negligible fraction of a single draw.
 */
constexpr std::size_t STAGED_CHUNK_WORDS = 4096;

namespace detail {

/******************************************************************************/
/**
 * @brief Copies one chunk of fresh random words out of the shared rotating pool.
 *
 * STAGED's claim seam. It claims a chunk from the lock-free, bulk-filled
 * Gem::Hap::detail::GRotatingPool (shared with QUARANTINE) and copies it -- word-by-word, each an
 * aligned 64-bit atomic load -- into the caller's private buffer @f$dst@f$:
 * @f[
 *   dst[i] \;\leftarrow\; \texttt{pool.claimSpan}()[i], \qquad i = 0,\dots,n-1 .
 * @f]
 * After the copy the proxy serves from its own memory and touches the pool no further, so a
 * dormant STAGED proxy pins no shared memory. See GRotatingPool for the ring, the producer, and
 * the (benign, aligned-64-bit) race analysis. The implementation lives in GStagedSource.cpp.
 *
 * @param dst Start of the destination buffer (must hold at least n words)
 * @param n   Number of 64-bit words to copy out (equal to STAGED_CHUNK_WORDS)
 */
void stagedClaim(std::uint64_t *dst, std::size_t n);

/******************************************************************************/

} /* namespace detail */
} /* namespace Gem::Hap */
