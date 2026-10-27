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

#include "weft/GWeft.hpp"

namespace Gem::Weft {

/******************************************************************************/
/**
 * @brief Returns the Weft library version as a human-readable string.
 *
 * The version is stamped in at build time (see WEFT_VERSION_STRING in the CMake
 * configuration). Weft carries no dependency on Gem::Common's version macros so
 * that it stays a self-contained, reusable foundation library.
 *
 * @return The Weft version string.
 */
std::string version() {
#ifdef WEFT_VERSION_STRING
    return WEFT_VERSION_STRING;
#else
    return "unknown";
#endif
}

/******************************************************************************/

} // namespace Gem::Weft
