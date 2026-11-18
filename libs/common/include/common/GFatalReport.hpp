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
#include <functional>

namespace Gem::Common {

/******************************************************************************/
/*
 * How a program ends on an error it does not handle: with a clear message on std::cerr and the
 * exit code EXIT_FAILURE (1), instead of std::terminate()'s abort and core dump (exit code 134),
 * which reads as an internal fault although the cause is most often the user's (a configuration
 * file that cannot be applied, say). A main() takes it up with a function-try-block, which leaves
 * the body of main() as it is:
 *
 * @code
 * int main(int argc, char **argv) try {
 *     Gem::Geneva::Go2 go(argc, argv, "./config/Go2.json");
 *     // ...
 * }
 * catch(...) {
 *     return Gem::Common::reportFatal();
 * }
 * @endcode
 *
 * Before reportFatal() returns, it calls the end hooks that libraries registered at run time
 * (addFatalEndHook()), in the order of their registration. They exist for what a plain return
 * cannot end: courtier's MPI side registers one when it initializes MPI, which ends the whole MPI
 * job, so that a rank that returns cannot leave the others waiting for it. common knows nothing
 * of such libraries; without a hook, reportFatal() just returns.
 */

/** @brief Registers a function that reportFatal() calls, with the exit code, before it returns */
void addFatalEndHook(std::function<void(int)>);

/** @brief Reports the exception in flight on std::cerr, calls the end hooks and returns
 *  EXIT_FAILURE; for the handler of a function-try-block on main() */
[[nodiscard]] int reportFatal() noexcept;

/******************************************************************************/

} /* namespace Gem::Common */
