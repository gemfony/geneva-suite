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

// The stop request (common/GStopRequest.hpp): what lets a long run end cleanly when a batch system
// sends its warning. The flag cannot be cleared, so every case runs in a child process of its own,
// which reports through its exit code.

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>

#include <csignal>

#include "common/GStopRequest.hpp"

#include "GChildProcess.hpp"

using namespace Gem::Common;
using namespace Gem::Common::Tests;

namespace {

/** @brief The handler of @p signum in this process */
void (*handlerOf(int signum))(int) {
    struct sigaction current{};
    ::sigaction(signum, nullptr, &current);
    return current.sa_handler;
}

/** @brief Installs the handlers, raises @p signum and says whether the flag is set */
int raiseAfterInstall(int signum) {
    if(stopRequested()) {
        return CHILD_FAILED; // set before any signal
    }
    installStopHandlers();
    if(stopRequested()) {
        return CHILD_FAILED; // set by the installation alone
    }
    if(std::raise(signum) != 0) {
        return CHILD_FAILED;
    }
    return stopRequested() ? CHILD_PASSED : CHILD_FAILED;
}

} // namespace

TEST_CASE("installStopHandlers(): SIGUSR1 requests a stop", "[common][stop-request]") {
    CHECK(passed(inChild([] { return raiseAfterInstall(SIGUSR1); })));
}

TEST_CASE("installStopHandlers(): SIGHUP requests a stop", "[common][stop-request]") {
    CHECK(passed(inChild([] { return raiseAfterInstall(SIGHUP); })));
}

TEST_CASE(
    "installStopHandlers(): a second installation changes nothing",
    "[common][stop-request]"
) {
    CHECK(passed(inChild([] {
        installStopHandlers();
        auto *const usr1 = handlerOf(SIGUSR1);
        auto *const hup = handlerOf(SIGHUP);
        installStopHandlers();
        if(handlerOf(SIGUSR1) != usr1 || handlerOf(SIGHUP) != hup || stopRequested()) {
            return CHILD_FAILED;
        }
        if(std::raise(SIGUSR1) != 0) {
            return CHILD_FAILED;
        }
        return stopRequested() ? CHILD_PASSED : CHILD_FAILED;
    })));
}

TEST_CASE(
    "installStopHandlers(): SIGTERM keeps its default and ends the process",
    "[common][stop-request]"
) {
    const int status = inChild([] {
        // The default to start from: a process started in the background may inherit an ignored
        // signal, which installStopHandlers() is not to change either
        std::signal(SIGTERM, SIG_DFL);
        auto *const interrupt = handlerOf(SIGINT);
        installStopHandlers();
        if(handlerOf(SIGTERM) != SIG_DFL || handlerOf(SIGINT) != interrupt) {
            return CHILD_FAILED;
        }
        std::raise(SIGTERM);
        return CHILD_FAILED; // not reached: SIGTERM ends the process
    });
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGTERM);
}

TEST_CASE("stopRequested(): no stop is requested without a signal", "[common][stop-request]") {
    CHECK(passed(inChild([] {
        installStopHandlers();
        return stopRequested() ? CHILD_FAILED : CHILD_PASSED;
    })));
}
