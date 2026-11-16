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

/*
 * The watch of the cascade's test program (GCourtierMpiCascadeTest): the program defines some MPI
 * functions itself and forwards them to their PMPI_ forms (MPI's profiling interface), counting
 * what is called -- also from courtier's MPI library, which the program links (the program exports
 * its symbols, so its definitions take precedence). At MPI_Finalize it prints, for its world rank,
 * whether every non-blocking collective that was started has completed and every communicator that
 * was made has been freed, and ends the process with 1 if not.
 */

#include <cstddef>

namespace Gem::Tests::CallWatch {

/** @brief How often MPI_Init_thread was called in this process */
[[nodiscard]] std::size_t watchedInitThreads();
/** @brief How often MPI_Ibcast was called in this process */
[[nodiscard]] std::size_t watchedIbcasts();
/** @brief How many non-blocking collectives (MPI_Ibcast, MPI_Iscatter, MPI_Igather, MPI_Ibarrier)
 *  were started in this process */
[[nodiscard]] std::size_t watchedCollectivesStarted();
/** @brief How many started non-blocking collectives have not been seen to complete */
[[nodiscard]] std::size_t watchedCollectivesPending();
/** @brief How often MPI_Comm_split was called in this process */
[[nodiscard]] std::size_t watchedSplits();
/** @brief How many communicators made by MPI_Comm_split or MPI_Comm_dup have not been freed */
[[nodiscard]] std::size_t watchedCommunicatorsAlive();

/** @brief The next MPI_Ibcast of this process fails at its start (MPI_ERR_OTHER, nothing started):
 *  a seam for the tests of the cascade's handling of MPI errors */
void failNextIbcastStart();
/** @brief The next MPI_Testall of this process fails (MPI_ERR_OTHER; its requests stay active) */
void failNextTestall();

} /* namespace Gem::Tests::CallWatch */
