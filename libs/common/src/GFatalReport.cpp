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

#include "common/GFatalReport.hpp"

// Standard headers go here
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Geneva headers go here
#include "common/GExceptions.hpp"

namespace Gem::Common {

namespace {

/******************************************************************************/
/**
 * The end hooks of the process. Created at the first use and never destroyed before the static
 * objects of the process are: a hook registered by a library outlives any call of reportFatal()
 * from main().
 */
struct GFatalEndHooks {
    std::mutex mutex;                            ///< Guards hooks
    std::vector<std::function<void(int)>> hooks; ///< In the order of their registration
};

/**
 * @brief The end hooks of the process
 * @return The one object that holds them
 */
GFatalEndHooks &fatalEndHooks() {
    static GFatalEndHooks end_hooks;
    return end_hooks;
}

/**
 * @brief Describes the exception in flight for the user
 * @return "<what()>" for a geneva_exception, "<what()> (a standard exception)" for any other
 *         std::exception, a sentence for one of unknown type or none at all
 */
std::string describeCurrentException() {
    const std::exception_ptr current = std::current_exception();
    if(not current) {
        return "reportFatal() was called while no exception was in flight";
    }
    try {
        std::rethrow_exception(current);
    }
    catch(const geneva_exception &e) {
        return e.what();
    }
    catch(const std::exception &e) {
        return std::string(e.what()) + " (a standard exception)";
    }
    catch(...) {
        return "an exception of unknown type";
    }
}

} // namespace

/******************************************************************************/
/**
 * Registers a function that reportFatal() calls before it returns. Hooks are called in the order
 * of their registration, each with the exit code; one that throws is skipped. A hook may end the
 * process (an MPI library ends the job with MPI_Abort).
 *
 * @param hook The function to call, with the exit code reportFatal() returns
 */
void addFatalEndHook(std::function<void(int)> hook) {
    auto &end_hooks = fatalEndHooks();
    const std::scoped_lock lock(end_hooks.mutex);
    end_hooks.hooks.push_back(std::move(hook));
}

/******************************************************************************/
/**
 * Reports the exception in flight -- the message of a geneva_exception or of any other
 * std::exception, or that its type is unknown -- on std::cerr, calls the end hooks registered with
 * addFatalEndHook() and returns EXIT_FAILURE. Meant for the handler of a function-try-block on
 * main(), so that a program ends on an error it does not handle with a clear message and the exit
 * code 1 rather than an abort.
 *
 * @return EXIT_FAILURE, the exit code of the program
 */
int reportFatal() noexcept {
    try {
        std::cerr << '\n'
                  << "Error: " << describeCurrentException() << '\n'
                  << "The program ends with the exit code " << EXIT_FAILURE << "." << '\n';
    }
    catch(...) { // NOLINT(bugprone-empty-catch): the exit code is all that is left to give
    }
    std::cout.flush();
    std::cerr.flush();

    std::vector<std::function<void(int)>> hooks;
    try {
        auto &end_hooks = fatalEndHooks();
        const std::scoped_lock lock(end_hooks.mutex);
        hooks = end_hooks.hooks;
    }
    catch(...) { // NOLINT(bugprone-empty-catch): no hook can be called then
    }
    for(const auto &hook : hooks) {
        try {
            hook(EXIT_FAILURE);
        }
        catch(...) { // NOLINT(bugprone-empty-catch): a failing hook must not stop the others
        }
    }
    return EXIT_FAILURE;
}

/******************************************************************************/

} /* namespace Gem::Common */
