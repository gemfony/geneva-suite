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

#include "courtier/GProcessable.hpp" // detail::lineageSalt
#include <cstdint>

#include "common/GCommonHelperFunctions.hpp" // randomBits64

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The random salt of this process: the first half of every lineage id minted in it. Drawn
 * once, on first use, from Gem::Common::randomBits64(). Defined here rather than in the header so
 * that a module loaded at run time and its host share one instance.
 * @return The salt of this process
 */
std::uint64_t detail::lineageSalt() noexcept {
    static const std::uint64_t SALT = Gem::Common::randomBits64();
    return SALT;
}

/******************************************************************************/

} /* namespace Gem::Courtier */
