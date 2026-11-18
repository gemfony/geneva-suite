/**
 * @file GParaboloidGroup.hpp
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

// The two halves of an evaluation that a group of an MPI cascade (courtier/mpi/GMPICascade.hpp)
// shares: the lead's half, which the lead runs in a round of the cascade, and the member's half,
// the cascade's member. The lead scatters the parameter values over the group, every process
// squares its part (GParaboloidPart.hpp), and the lead gathers the sums. Both halves use MPI and
// courtier only, no Geneva: a member runs no Geneva code. Their waits are polled
// (courtier/mpi/GMPIPolledCalls.hpp), so that a waiting process does not keep a core busy.

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "courtier/mpi/GMPICascade.hpp"

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief The lead's half of one evaluation: every process of @p group gets an equal part of
 * @p parameters, the lead also the rest; the lead returns the sum of the squares of all of them
 */
double leadHalf(
    MPI_Comm,
    std::span<const double>,
    std::chrono::milliseconds,
    std::chrono::microseconds,
    std::chrono::microseconds
);

/******************************************************************************/
/**
 * The member's half of the evaluations: in every round it receives its part of the parameter
 * values, squares them and sends the sum to its lead. When its lead releases the group, it prints
 * how many rounds it served.
 */
class GParaboloidGroupMember final : public Gem::Courtier::GMPIGroupMemberI {
public:
    /** @brief The number of parameters of an evaluation and the time of one square */
    GParaboloidGroupMember(std::size_t, std::chrono::milliseconds);

    /** @brief Sets the pauses of the member's waits, before its first round */
    void setPauses(std::chrono::microseconds, std::chrono::microseconds);
    /** @brief Serves one round of its lead */
    void serveRound(MPI_Comm) override;
    /** @brief Prints how many rounds the member served */
    void tearDown(MPI_Comm) override;

private:
    std::size_t n_parameters_;                      ///< The parameters of an evaluation
    std::chrono::milliseconds delay_per_parameter_; ///< The time of one square
    std::chrono::microseconds pause_min_{0};        ///< The first pause of a wait
    std::chrono::microseconds pause_max_{0};        ///< The longest pause of a wait
    std::uint64_t rounds_ = 0;                      ///< The rounds served
};

/******************************************************************************/

} /* namespace Gem::Geneva */
