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

#include "hap/GStagedSource.hpp"
#include "GRotatingPool.hpp" // the shared lock-free rotating-block pool

#include <cstddef>
#include <cstdint>

namespace Gem::Hap::detail {

/******************************************************************************/
/**
 * @brief Copies one chunk of fresh random words out of the process-wide staging pool.
 *
 * The pool is a function-local static, constructed on the first claim (after the
 * random factory exists) and shared by every STAGED proxy. It is the same
 * lock-free Gem::Hap::detail::GRotatingPool the QUARANTINE source uses (see there for the
 * ring, the producer, and the benign-race analysis), but STAGED copies the
 * claimed chunk out (word-by-word, each an aligned 64-bit atomic load) into the
 * caller's private double buffer: after the copy the proxy serves from its own
 * memory and touches the pool no further, so a dormant STAGED proxy is fully
 * race-free (it pins no shared memory). This replaces the earlier mutex-guarded
 * single-buffer pool, removing the lock that serialised proxies under load.
 *
 * @param dst Start of the destination buffer (must hold at least n words)
 * @param n   Number of 64-bit words to copy out (must equal STAGED_CHUNK_WORDS)
 */
void stagedClaim(std::uint64_t *dst, [[maybe_unused]] std::size_t n) {
    static GRotatingPool<STAGED_CHUNK_WORDS> pool; // thread-safe init (C++ magic statics)
    pool.copyChunkInto(dst);
}

/******************************************************************************/

} /* namespace Gem::Hap::detail */
