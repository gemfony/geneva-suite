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

#include "common/GStopRequest.hpp"

// Standard header files go here
#include <atomic>
#include <csignal>

// Geneva header files go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"

namespace Gem::Common {

/******************************************************************************/
namespace {

// The one state a signal handler can share with the program: set by the handler, read by other
// threads; an atomic without a lock is safe in both (false until a signal arrives)
std::atomic<bool> stopFlag; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static_assert(
    std::atomic<bool>::is_always_lock_free,
    "a signal handler may only use lock-free atomics"
);

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
BOOL WINAPI onConsoleEvent(DWORD event) {
    if(event == CTRL_CLOSE_EVENT) {
        stopFlag.store(true);
        return TRUE;
    }
    return FALSE;
}
#else
// Notes the request and nothing else (async-signal safety)
void onStopSignal(int /* signum */) {
    stopFlag.store(true);
}
#endif

} // namespace

/******************************************************************************/
/**
 * @brief Installs the handlers of a stop request (see the header)
 *
 * @throws gem_exception if a handler cannot be installed
 */
void installStopHandlers() {
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
    const bool installed = SetConsoleCtrlHandler(onConsoleEvent, TRUE) != 0;
#else
    struct sigaction action{};
    action.sa_handler = onStopSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART; // a system call the signal interrupts goes on
    bool installed = true;
    for(const int signum : {SIGHUP, SIGUSR1}) {
        installed = installed && sigaction(signum, &action, nullptr) == 0;
    }
#endif
    if(not installed) {
        throw gem_exception(
            g_error_streamer() << "In Gem::Common::installStopHandlers(): Error!" << '\n'
                               << "A handler of a stop request could not be installed" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Whether a stop was requested since installStopHandlers()
 *
 * @return true once one of its signals has arrived
 */
bool stopRequested() {
    return stopFlag.load();
}

/******************************************************************************/

} /* namespace Gem::Common */
