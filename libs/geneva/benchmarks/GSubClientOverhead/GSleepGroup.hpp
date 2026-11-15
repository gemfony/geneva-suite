/**
 * @file GSleepGroup.hpp
 */

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

// A group of processes that share the evaluation of one individual: its lead (a client of the
// optimization) and the members of its group (sub-clients, which run no Geneva code). Nobody
// computes; everybody sleeps its part of the evaluation. The two halves take the group's
// communicator and nothing else, so they are the same whether the lead talks to its server
// through MPI or through websockets. Nobody waits busily: a process that waits for a message
// looks with a non-blocking test and pauses between looks, with a pause that starts short and
// grows while nothing comes (Open MPI's blocking calls spin, which with hundreds of processes on
// a few cores would measure the scheduler). Every evaluation is a real exchange: the lead sends
// each member a message with its part and receives a small answer; that exchange, and the
// waiting for it, is the overhead the benchmark measures.

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <chrono>
#include <cstdint>
#include <span>

// MPI header files go here
#include <mpi.h>

namespace Gem::Geneva::SubClientOverhead {

/** @brief How long a process that waits for a message pauses between two looks: from min,
 *  doubling while nothing comes, up to max. Small bounds: the pause is part of the overhead that
 *  is measured, and an artefact of running hundreds of processes on one host. */
struct PollPauses {
    std::chrono::microseconds min{50};    ///< The first pause after a look that found nothing
    std::chrono::microseconds max{1'000}; ///< The longest pause
};

/** @brief Sets the pauses of this process (the same in every process of a run). */
void setPollPauses(PollPauses);

/** @brief The pauses of this process. */
PollPauses pollPauses();

/******************************************************************************/
/**
 * @brief The lead's half of one evaluation: sends every member of @p group a message with its
 * part (the time to sleep and its share of the parameter values), sleeps its own part, computes
 * on its own share and returns when every member has answered with its small result. A group of
 * one (MPI_COMM_NULL, or a communicator of size 1) does it all itself and sends nothing. Rounds
 * of one process run one after the other, whatever threads call this.
 * @return The sum of the squares of all parameter values, computed by the group
 */
double leadRound(
    MPI_Comm group,
    std::chrono::duration<double> evaluation_time,
    bool split,
    std::span<const double> parameters
);

/** @brief The member's half: serves parts of its lead until the lead releases the group. */
int serveGroup(MPI_Comm group);

/** @brief Tells the members of @p group that no part follows; once, from the lead, at the end. */
void releaseGroup(MPI_Comm group);

/** @brief The group the evaluations of this process use (MPI_COMM_NULL: a group of one). */
MPI_Comm processGroup();

/** @brief Sets the group of this process, before its first evaluation. */
void setProcessGroup(MPI_Comm group);

/** @brief The rounds this process has led so far. */
std::uint64_t roundsLed();

} /* namespace Gem::Geneva::SubClientOverhead */
