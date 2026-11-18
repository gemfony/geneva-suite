/**
 * @file GParaboloidGroup.cpp
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

#include "GParaboloidGroup.hpp"

// Standard header files go here
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Geneva header files go here
#include "courtier/mpi/GMPIPolledCalls.hpp"

#include "GParaboloidPart.hpp"

namespace Gem::Geneva {

namespace {

/******************************************************************************/
/**
 * Throws if an MPI call failed; out of a half, that ends the MPI job (see GMPICascade).
 *
 * @param code What the call returned
 * @param what The call, for the message
 */
void check(int code, const char *what) {
    if(code != MPI_SUCCESS) {
        throw std::runtime_error(
            std::string("GParaboloidGroup: ") + what + " failed with " + std::to_string(code)
        );
    }
}

/**
 * The part of one process of a group, the same on every process: the number of parameters
 * divided by the size of the group (the lead takes the rest).
 *
 * @param n_parameters The parameters of an evaluation
 * @param size The size of the group
 * @return The size of a part
 */
int partSize(std::size_t n_parameters, int size) {
    return static_cast<int>(n_parameters / static_cast<std::size_t>(size));
}

/**
 * The size of a group.
 *
 * @param group The group
 * @return Its number of processes
 */
int groupSize(MPI_Comm group) {
    int size = 1;
    check(MPI_Comm_size(group, &size), "MPI_Comm_size");
    return size;
}

} /* namespace */

/******************************************************************************/
/**
 * The lead's half of one evaluation, in a round of the cascade. The parameters are scattered in
 * equal parts over the n processes of the group, the first part being the lead's; the lead also
 * takes the rest the division leaves. Every process squares its part; the lead gathers the sums.
 *
 * @param group The group's communicator; the lead is its rank 0
 * @param parameters The parameter values of the individual
 * @param delay_per_parameter The simulated time of one square
 * @param pause_min The first pause of a wait
 * @param pause_max The longest pause of a wait
 * @return The sum of the squares of all parameter values
 */
double leadHalf(
    MPI_Comm group,
    std::span<const double> parameters,
    std::chrono::milliseconds delay_per_parameter,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    const int size = groupSize(group);
    const int part = partSize(parameters.size(), size);
    std::vector<double> own(static_cast<std::size_t>(part));
    check(
        Gem::Courtier::mpiScatter(
            parameters.data(),
            part,
            own.data(),
            MPI_DOUBLE,
            0,
            group,
            pause_min,
            pause_max
        ),
        "mpiScatter"
    );
    const auto rest = parameters.subspan(static_cast<std::size_t>(part * size));
    double sum = sumOfSquares(own, delay_per_parameter) + sumOfSquares(rest, delay_per_parameter);

    std::vector<double> sums(static_cast<std::size_t>(size));
    check(
        Gem::Courtier::mpiGather(&sum, 1, sums.data(), MPI_DOUBLE, 0, group, pause_min, pause_max),
        "mpiGather"
    );
    return std::ranges::fold_left(sums, 0., std::plus{});
}

/******************************************************************************/
/**
 * The constructor.
 *
 * @param n_parameters The number of parameters of an evaluation
 * @param delay_per_parameter The simulated time of one square
 */
GParaboloidGroupMember::GParaboloidGroupMember(
    std::size_t n_parameters,
    std::chrono::milliseconds delay_per_parameter
)
  : n_parameters_(n_parameters)
  , delay_per_parameter_(delay_per_parameter) { /* nothing */
}

/**
 * Sets the pauses of the member's waits, before its first round.
 *
 * @param pause_min The first pause after a look that found nothing
 * @param pause_max The longest pause
 */
void GParaboloidGroupMember::setPauses(
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    pause_min_ = pause_min;
    pause_max_ = pause_max;
}

/**
 * Serves one round of its lead (rank 0 of @p group): receives its part of the parameter values,
 * squares them and sends the sum to the lead.
 *
 * @param group The group's communicator
 */
void GParaboloidGroupMember::serveRound(MPI_Comm group) {
    const int part = partSize(n_parameters_, groupSize(group));
    std::vector<double> own(static_cast<std::size_t>(part));
    check(
        Gem::Courtier::
            mpiScatter(nullptr, part, own.data(), MPI_DOUBLE, 0, group, pause_min_, pause_max_),
        "mpiScatter"
    );
    double sum = sumOfSquares(own, delay_per_parameter_);
    check(
        Gem::Courtier::mpiGather(&sum, 1, nullptr, MPI_DOUBLE, 0, group, pause_min_, pause_max_),
        "mpiGather"
    );
    rounds_++;
}

/**
 * Prints how many rounds the member served.
 *
 * @param group The group's communicator
 */
void GParaboloidGroupMember::tearDown(MPI_Comm group) {
    int rank = 0;
    check(MPI_Comm_rank(group, &rank), "MPI_Comm_rank");
    std::cout << "GParaboloidGroup: member " << rank << " served " << rounds_ << " rounds\n";
}

/******************************************************************************/

} /* namespace Gem::Geneva */
