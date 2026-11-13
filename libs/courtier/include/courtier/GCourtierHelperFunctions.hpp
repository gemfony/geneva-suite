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
#include <string>

// Boost headers go here

// Geneva headers go here
#include "courtier/GCourtierEnums.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief Translates the processingStatus into a clear-text string.
 * @param ps The processingStatus to be translated
 * @return A string representing the processing status (empty for an unrecognized value)
 */
std::string psToStr(const processingStatus &ps);

/**
 * @brief Translates a protocol frame kind into a clear-text string.
 * @param fk The GFrameKind to be translated
 * @return A string representing the frame kind (empty for an unrecognized value)
 */
std::string fkToStr(const GFrameKind &fk);

/******************************************************************************/

} /* namespace Gem::Courtier */
