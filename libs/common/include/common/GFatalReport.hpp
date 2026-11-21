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
#include <exception>
#include <string>

namespace Gem::Common {

/******************************************************************************/
/*
 * How a program ends on an error nobody handles: with a clear message on std::cerr and the exit
 * code EXIT_FAILURE (1), instead of std::terminate()'s abort and core dump (exit code 134), which
 * reads as an internal fault although the cause is most often the user's (a configuration file that
 * cannot be applied, say). installFatalReport() makes this the terminate handler of the process.
 * An exception that no handler catches -- in main() or in any other thread -- reaches it without
 * unwinding the stack, so no destructor runs on the way: in particular none that would finalize
 * MPI and wait for other processes. (An exception stopped by a noexcept function, or rethrown from
 * a catch, unwinds up to that frame first.) The process ends with std::_Exit(), and mpirun
 * ends the whole job when one of its processes ends that way. A std::terminate() with
 * no exception in flight is an internal fault (a joinable std::thread destroyed, a pure virtual
 * call): the handler prints its line and calls std::abort(), for the core dump.
 *
 * Go2's constructor installs it; a program without Go2 calls installFatalReport() in the first line
 * of main(). A terminate handler set before is replaced; a program that wants one of its own sets
 * it afterwards.
 */

/** @brief Makes the fatal report the terminate handler of the process; idempotent */
void installFatalReport() noexcept;

/** @brief The text the fatal report prints for @p error (an empty pointer: no exception) */
[[nodiscard]] std::string fatalMessage(const std::exception_ptr &error);

/******************************************************************************/

} /* namespace Gem::Common */
