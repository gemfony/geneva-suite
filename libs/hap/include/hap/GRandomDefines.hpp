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

// Standard includes go here
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <random>

// Geneva headers go here
#include "common/GCommonHelperFunctions.hpp" // getNHardwareThreads
#include "hap/GHapEnums.hpp"                 // the constants of the random factory

namespace Gem::Hap {

/******************************************************************************/
// The seed type used by the random factory's seeding machinery
using seed_type = std::mt19937::result_type;

/******************************************************************************/
/**
 * @brief The producer-thread count used when an automatic ("0") count is requested.
 *
 * A request of 0 producer threads means "size the producer pool to the hardware" (mirroring the
 * "0 == auto" convention elsewhere in Geneva). Random-number production is cheap and the producers
 * block on the bounded fresh-package buffer once it is full, so an idle surplus costs little; the
 * hardware count is nevertheless scaled by @c PRODUCERTHREADS_HW_DIVISOR and clamped to
 * <tt>[DEFAULT01PRODUCERTHREADS, MAXAUTOPRODUCERTHREADS]</tt> to bound contention on the single
 * fresh-package queue. The hardware count is the number of hardware threads assigned to the
 * process, @c Gem::Common::getNHardwareThreads().
 *
 * @return A hardware-derived producer-thread count, clamped to <tt>[DEFAULT01PRODUCERTHREADS, MAXAUTOPRODUCERTHREADS]</tt>
 */
inline std::uint16_t autoProducerThreadCount() {
    const unsigned int hw = Gem::Common::getNHardwareThreads();
    const unsigned int scaled =
        std::max<unsigned int>(DEFAULT01PRODUCERTHREADS, hw / PRODUCERTHREADS_HW_DIVISOR);
    return static_cast<std::uint16_t>(std::min<unsigned int>(scaled, MAXAUTOPRODUCERTHREADS));
}

/******************************************************************************/

} /* namespace Gem::Hap */
