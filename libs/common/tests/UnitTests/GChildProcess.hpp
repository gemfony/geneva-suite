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

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>

/******************************************************************************/
/**
 * @file
 * Runs a part of a test in a child process of its own, for the tests whose state cannot be undone
 * in the test process -- the stop request of common/GStopRequest.hpp cannot be cleared. The child
 * reports through its exit code.
 */

namespace Gem::Common::Tests {

/** @brief The exit code of a child whose checks all held */
inline constexpr int CHILD_PASSED = 0;
/** @brief The exit code of a child whose check failed */
inline constexpr int CHILD_FAILED = 1;

/**
 * @brief Runs @p body in a child process and returns how the child ended
 *
 * @param body Returns the child's exit code; an exception ends the child with 2
 * @return The status of waitpid()
 */
template <typename body_type>
int inChild(body_type const &body) {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if(child == 0) {
        int code = 2;
        try {
            code = body();
        }
        catch(...) { // NOLINT(bugprone-empty-catch): the exit code reports it
        }
        std::_Exit(code);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    return status;
}

/** @brief Whether the child ended normally with CHILD_PASSED */
inline bool passed(int status) {
    return WIFEXITED(status) && WEXITSTATUS(status) == CHILD_PASSED;
}

} /* namespace Gem::Common::Tests */
