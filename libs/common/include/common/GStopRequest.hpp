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

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief Installs the handlers of a stop request, SIGHUP and SIGUSR1 (on Windows the closing of
 * the console window, CTRL_CLOSE_EVENT). They ask for a clean stop after the current iteration, as
 * a batch system warns a job before it ends it (Slurm: --signal=USR1@<s>). A handler only notes the
 * request; whoever can end the work cleanly asks stopRequested() -- in Geneva every optimization
 * algorithm, after each iteration. A process that does not ask, such as a client, goes on serving.
 *
 * SIGTERM and SIGINT (Ctrl-C) are not caught: they end a process at once. Under a batch system the
 * hard end reaches every task together, and a clean stop could not finish a distributed iteration
 * whose clients end at the same moment.
 *
 * courtier's launcher calls it in every process; a program without the launcher calls it itself.
 * Calling it again changes nothing.
 *
 * @throws gem_exception if a handler cannot be installed
 */
void installStopHandlers();

/******************************************************************************/
/**
 * @brief Whether a stop was requested since installStopHandlers()
 * @return true once one of its signals has arrived
 */
[[nodiscard]] bool stopRequested();

/******************************************************************************/

} /* namespace Gem::Common */
