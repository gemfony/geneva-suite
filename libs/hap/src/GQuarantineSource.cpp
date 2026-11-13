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

#include "hap/GQuarantineSource.hpp"
#include "GRotatingPool.hpp" // the shared lock-free rotating-block pool

#include <cstdint>

namespace Gem::Hap::detail {

/******************************************************************************/
/**
 * @brief Claims the next span from the process-wide quarantine pool set (read in place).
 *
 * The pool is a function-local static, constructed on the first claim (after the
 * random factory exists) and shared by every QUARANTINE proxy. QUARANTINE reads
 * the span in place -- the lower-overhead, benign-race read mode of
 * Gem::Hap::detail::GRotatingPool (see there for the ring and the conditions under which the
 * race does no harm).
 *
 * @return Pointer to a QUARANTINE_CHUNK_WORDS-word span inside a shared pool
 */
const std::uint64_t *quarantineClaimSpan() {
    static GRotatingPool<QUARANTINE_CHUNK_WORDS> pool; // thread-safe init (C++ magic statics)
    return pool.claimSpan();
}

/******************************************************************************/

} /* namespace Gem::Hap::detail */
