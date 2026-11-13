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

// Standard header files go here
#include <csignal>

// Geneva header files go here
// G_SIGHUP (SIGHUP / CTRL_CLOSE_EVENT) is defined here
#include "common/GCommonEnums.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief A handler for SIGHUP or CTRL_CLOSE_EVENT signals. This function works both
 * for Windows and Unix systems. Register it with e.g.
 * `signal(G_SIGHUP, Gem::Common::sigHupHandler)` to allow interruption of an
 * optimization run without loss of data.
 *
 * @param signum The number of the signal that was raised
 */
void sigHupHandler(int signum);

/******************************************************************************/
/**
 * @brief Checks whether a SIGHUP or CTRL_CLOSE_EVENT signal has been sent.
 *
 * @return A boolean indicating whether a SIGHUP / CTRL_CLOSE_EVENT signal was received
 */
bool G_SIGHUP_SENT();

/******************************************************************************/

} /* namespace Gem::Common */
