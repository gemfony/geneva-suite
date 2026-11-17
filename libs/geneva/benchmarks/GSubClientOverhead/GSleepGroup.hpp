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
// computes; everybody sleeps its part of the evaluation. The groups, their rounds and their end
// are the MPI cascade's (courtier/mpi/GMPICascade.hpp): the lead's half runs in a round of the
// cascade, the member's half is the cascade's member. Both halves take the group's communicator
// and nothing else, so they are the same whether the lead talks to its server through MPI or
// through websockets. Nobody waits busily: a process that waits for a message looks with a
// non-blocking test and pauses between looks, with a pause that starts short and grows while
// nothing comes (Open MPI's blocking calls spin, which with hundreds of processes on a few cores
// would measure the scheduler). Every evaluation is a real exchange: the lead sends each member a
// message with its part and receives a small answer; that exchange, and the waiting for it, is
// the overhead the benchmark measures.

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "courtier/mpi/GMPICascade.hpp"

namespace Gem::Geneva::SubClientOverhead {

/******************************************************************************/
/**
 * @brief The lead's half of one evaluation, called in a round of the cascade: sends every member
 * of @p group a message with its part (the time to sleep and its share of the parameter values),
 * sleeps its own part, computes on its own share and returns when every member has answered with
 * its small result.
 * @return The sum of the squares of all parameter values, computed by the group
 */
double leadHalf(
    MPI_Comm group,
    std::chrono::duration<double> evaluation_time,
    bool split,
    std::span<const double> parameters,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
);

/******************************************************************************/
/**
 * The member's half of the evaluations: in every round it waits for its lead's message, sleeps
 * the time the message names and answers with the sum of the squares of the parameter values it
 * carries. When the lead releases the group, it prints how many parts it served.
 */
class GSleepGroupMember final : public Gem::Courtier::GMPIGroupMemberI {
public:
    /** @brief Sets the pauses of the member's waits, before its first round */
    void setPauses(std::chrono::microseconds, std::chrono::microseconds);
    /** @brief Serves one part of its lead */
    void serveRound(MPI_Comm) override;
    /** @brief Prints how many parts the member served */
    void tearDown(MPI_Comm) override;

private:
    std::chrono::microseconds pause_min_{0}; ///< The first pause of a wait
    std::chrono::microseconds pause_max_{0}; ///< The longest pause of a wait
    std::uint64_t parts_ = 0;                ///< The parts served
};

/******************************************************************************/
/**
 * What a lead logs about its group: it counts the rounds it led and, when the lead releases the
 * group, prints them with the group's size and the pauses.
 */
class GSleepGroupLeadLog final : public Gem::Courtier::GMPIGroupLifecycleI {
public:
    /** @brief Sets the pauses the line names */
    void setPauses(std::chrono::microseconds, std::chrono::microseconds);
    /** @brief Counts one round led; any thread */
    void countRound() noexcept;
    /** @brief Prints the rounds led, the group's size and the pauses */
    void tearDown(MPI_Comm) override;

private:
    std::chrono::microseconds pause_min_{0}; ///< The first pause of a wait
    std::chrono::microseconds pause_max_{0}; ///< The longest pause of a wait
    std::atomic<std::uint64_t> rounds_{0};   ///< The rounds led
};

} /* namespace Gem::Geneva::SubClientOverhead */
