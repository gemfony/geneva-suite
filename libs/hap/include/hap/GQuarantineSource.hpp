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
 * The number of 64-bit words a QUARANTINE proxy reads per claimed span. Unlike
 * STAGED, the span is NOT copied out -- the proxy reads these words straight from
 * the shared pool (the "stable span in place" that makes QUARANTINE lower-overhead
 * than STAGED, at the price of a benign race; see GQuarantineSource.cpp).
 */
constexpr std::size_t QUARANTINE_CHUNK_WORDS = 4096;

namespace detail {

/******************************************************************************/
/**
 * @brief Claims the next QUARANTINE_CHUNK_WORDS-word span from the shared rotating pool.
 *
 * QUARANTINE's claim seam. It returns a pointer into the lock-free, bulk-filled
 * Gem::Hap::detail::GRotatingPool (shared with STAGED); the caller reads the
 * @f$QUARANTINE\_CHUNK\_WORDS@f$ words from it @e in @e place, with no copy -- the lower-overhead
 * trade against STAGED (which copies out). Because it does not snapshot, QUARANTINE relies on the
 * pool's benign race on every read. See Gem::Hap::detail::GRotatingPool for the ring, the
 * background producer, the N@f$\ge@f$3 quarantine, and the conditions under which the race is
 * harmless (aligned-64-bit atomicity on x86-64 / AArch64, lock-free @f$\Rightarrow@f$ deadlock-free).
 * This is the library-private seam used by GRandomT<randomSource::QUARANTINE>; the implementation
 * lives in GQuarantineSource.cpp.
 *
 * @return Pointer to a QUARANTINE_CHUNK_WORDS-word span inside a shared pool
 */
const std::uint64_t *quarantineClaimSpan();

/******************************************************************************/

} /* namespace detail */
} /* namespace Gem::Hap */
