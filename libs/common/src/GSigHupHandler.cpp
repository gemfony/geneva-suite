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

#include "common/GSigHupHandler.hpp"
#include "common/GCommonEnums.hpp"
#include <csignal>

namespace Gem::Common {

/******************************************************************************/
// Needed to allow catching of a SIGHUP or CTRL_CLOSE_EVENT event.
// Note that "volatile" is needed in order for the signal handler to work.
namespace {
volatile std::sig_atomic_t GenevaSigHupSent = 0;
} // namespace

/******************************************************************************/
/**
 * @brief A handler for SIGHUP or CTRL_CLOSE_EVENT signals. This function works both
 * for Windows and Unix systems.
 *
 * @param signum The number of the signal that was raised; the internal flag is set only when it matches G_SIGHUP
 */
void sigHupHandler(int signum) {
    if(G_SIGHUP == signum) {
        GenevaSigHupSent = 1;
    }
}

/******************************************************************************/
/**
 * @brief Checks whether a SIGHUP or CTRL_CLOSE_EVENT signal has been sent.
 *
 * @return true if a SIGHUP (or CTRL_CLOSE_EVENT) signal has been received since program start, false otherwise
 */
bool G_SIGHUP_SENT() {
    return (1 == GenevaSigHupSent);
}

/******************************************************************************/

} /* namespace Gem::Common */
