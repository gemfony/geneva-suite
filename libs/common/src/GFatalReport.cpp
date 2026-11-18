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
#include <iostream>

// Geneva headers go here
#include "common/GExceptions.hpp"

namespace Gem::Common {

namespace {

/******************************************************************************/
/**
 * The terminate handler of installFatalReport(): prints fatalMessage() of the exception in flight,
 * flushes the standard streams and ends the process with EXIT_FAILURE, without unwinding and
 * without running any destructor. Without an exception in flight (an internal fault) it aborts
 * instead, for the core dump.
 */
[[noreturn]] void reportAndExit() noexcept {
    const std::exception_ptr error = std::current_exception();
    try {
        std::cerr << fatalMessage(error);
        std::cout.flush();
        std::clog.flush(); // the logger's console stream
        std::cerr.flush();
    }
    catch(...) { // NOLINT(bugprone-empty-catch): the exit code is all that is left to give
    }
    if(not error) {
        std::abort();
    }
    std::_Exit(EXIT_FAILURE);
}

} // namespace

/******************************************************************************/
/**
 * Makes the fatal report the terminate handler of the process (std::set_terminate()), so that the
 * process ends on an exception nobody catches with the message of fatalMessage() and the exit code
 * EXIT_FAILURE. Installs it once per process; later calls do nothing, so that a handler the program
 * sets afterwards stays.
 */
void installFatalReport() noexcept {
    static const bool installed = (std::set_terminate(&reportAndExit), true);
    (void)installed;
}

/******************************************************************************/
/**
 * The text the fatal report prints for an exception.
 *
 * @param error The exception; empty if std::terminate() was called without one
 * @return "Error: " and the message of a geneva_exception, of any other std::exception (marked as
 *         a standard exception) or a sentence for one of unknown type or none, then a line with the
 *         exit code (without an exception: that the program aborts)
 */
std::string fatalMessage(const std::exception_ptr &error) {
    std::string text;
    if(not error) {
        text = "std::terminate() was called without an exception";
    }
    else {
        try {
            std::rethrow_exception(error);
        }
        catch(const geneva_exception &e) {
            text = e.what();
        }
        catch(const std::exception &e) {
            text = std::string(e.what()) + " (a standard exception)";
        }
        catch(...) {
            text = "an exception of unknown type";
        }
    }
    const std::string end =
        error ? "ends with the exit code " + std::to_string(EXIT_FAILURE) : std::string("aborts");
    return "\nError: " + text + "\nThe program " + end + ".\n";
}

/******************************************************************************/

} /* namespace Gem::Common */
