/**
 * @file GSleepGroup.cpp
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

#include "GSleepGroup.hpp"

// Standard header files go here
#include <algorithm>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Geneva header files go here
#include "courtier/mpi/GMPICascadeConfig.hpp"
#include "courtier/mpi/GMPIPolledCalls.hpp"

#include "GSumOfSquares.hpp"

namespace Gem::Geneva::SubClientOverhead {

namespace {

/// lead to member: the time to sleep, then the member's share of the parameters
constexpr int PARTTAG = 1;
/// member to lead: its result (one double)
constexpr int DONETAG = 2;

/******************************************************************************/
/**
 * Throws if an MPI call failed.
 *
 * @param code What the call returned
 * @param what The call, for the message
 */
void check(int code, const char *what) {
    if(code != MPI_SUCCESS) {
        throw std::runtime_error(
            std::string("GSleepGroup: ") + what + " failed with " + std::to_string(code)
        );
    }
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
 * The lead's half of one evaluation of nominal duration @p evaluation_time, in a round of the
 * cascade. Split, each of the group's n processes sleeps evaluation_time / n, the lead included;
 * otherwise every process sleeps the whole time. The parameters are shared out in n contiguous
 * slices, the first being the lead's. Every member gets a message with its time and its slice
 * before the lead sleeps its own part; the round ends when every member has answered with the sum
 * of the squares of its slice. The cascade runs one round at a time.
 *
 * @param group The group's communicator; the lead is its rank 0
 * @param evaluation_time The nominal duration of the evaluation
 * @param split Whether the processes of the group share the evaluation time
 * @param parameters The parameter values of the individual
 * @param pause_min The first pause of the wait for the answers
 * @param pause_max The longest pause of that wait
 * @return The sum of the squares of all parameter values
 */
double leadHalf(
    MPI_Comm group,
    std::chrono::duration<double> evaluation_time,
    bool split,
    std::span<const double> parameters,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    const auto size = static_cast<std::size_t>(groupSize(group));
    const double part =
        split ? evaluation_time.count() / static_cast<double>(size) : evaluation_time.count();
    const auto slice = [&parameters, size](std::size_t rank) {
        const std::size_t begin = rank * parameters.size() / size;
        const std::size_t end = (rank + 1) * parameters.size() / size;
        return parameters.subspan(begin, end - begin);
    };

    std::vector<std::vector<double>> messages(size);
    std::vector<double> answers(size, 0.);
    std::vector<MPI_Request> requests;
    requests.reserve(2 * size);
    for(std::size_t rank = 1; rank < size; rank++) {
        auto &message = messages[rank];
        message.push_back(part);
        std::ranges::copy(slice(rank), std::back_inserter(message));
        requests.push_back(MPI_REQUEST_NULL);
        check(
            MPI_Isend(
                message.data(),
                static_cast<int>(message.size()),
                MPI_DOUBLE,
                static_cast<int>(rank),
                PARTTAG,
                group,
                &requests.back()
            ),
            "MPI_Isend"
        );
        requests.push_back(MPI_REQUEST_NULL);
        check(
            MPI_Irecv(
                &answers[rank],
                1,
                MPI_DOUBLE,
                static_cast<int>(rank),
                DONETAG,
                group,
                &requests.back()
            ),
            "MPI_Irecv"
        );
    }
    std::this_thread::sleep_for(std::chrono::duration<double>(part));
    answers[0] = sumOfSquares(slice(0));
    check(Gem::Courtier::waitPolled(requests, pause_min, pause_max), "MPI_Testall");
    return std::ranges::fold_left(answers, 0., std::plus{});
}

/******************************************************************************/
/**
 * Sets the pauses of the member's waits, before its first round.
 *
 * @param pause_min The first pause after a look that found nothing
 * @param pause_max The longest pause
 */
void GSleepGroupMember::setPauses(
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    Gem::Courtier::checkPollPauses(pause_min, pause_max);
    pause_min_ = pause_min;
    pause_max_ = pause_max;
}

/**
 * Serves one part of its lead (rank 0 of @p group): waits for the lead's message, sleeps the time
 * it names and answers with the sum of the squares of the parameter values it carries.
 *
 * @param group The group's communicator
 */
void GSleepGroupMember::serveRound(MPI_Comm group) {
    const MPI_Status status = Gem::Courtier::probePolled(0, PARTTAG, group, pause_min_, pause_max_);
    int count = 0;
    check(MPI_Get_count(&status, MPI_DOUBLE, &count), "MPI_Get_count");
    std::vector<double> message(static_cast<std::size_t>(std::max(count, 1)));
    check(
        MPI_Recv(message.data(), count, MPI_DOUBLE, 0, PARTTAG, group, MPI_STATUS_IGNORE),
        "MPI_Recv"
    );
    std::this_thread::sleep_for(std::chrono::duration<double>(message.front()));
    double result = sumOfSquares(std::span(message).subspan(1));
    // The MPI checker does not see that waitPolled() waits for the request.
    // NOLINTBEGIN(clang-analyzer-optin.mpi.MPI-Checker)
    MPI_Request request = MPI_REQUEST_NULL;
    check(MPI_Isend(&result, 1, MPI_DOUBLE, 0, DONETAG, group, &request), "MPI_Isend");
    check(Gem::Courtier::waitPolled(std::span(&request, 1), pause_min_, pause_max_), "MPI_Testall");
    // NOLINTEND(clang-analyzer-optin.mpi.MPI-Checker)
    parts_++;
}

/**
 * Prints how many parts the member served, for the checks.
 *
 * @param group The group's communicator
 */
void GSleepGroupMember::tearDown(MPI_Comm group) {
    int rank = 0;
    check(MPI_Comm_rank(group, &rank), "MPI_Comm_rank");
    std::cout << "GSleepGroup: member " << rank << " served " << parts_ << " parts\n";
}

/******************************************************************************/
/**
 * Sets the pauses the lead's line names.
 *
 * @param pause_min The first pause after a look that found nothing
 * @param pause_max The longest pause
 */
void GSleepGroupLeadLog::setPauses(
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    pause_min_ = pause_min;
    pause_max_ = pause_max;
}

/**
 * Counts one round led; may be called by any thread.
 */
void GSleepGroupLeadLog::countRound() noexcept {
    rounds_++;
}

/**
 * Prints the rounds the lead led, with the group's size and the pauses, for the checks.
 *
 * @param group The group's communicator
 */
void GSleepGroupLeadLog::tearDown(MPI_Comm group) {
    std::cout << "GSleepGroup: lead of " << groupSize(group) << " processes led " << rounds_.load()
              << " rounds (poll pauses " << pause_min_.count() << " to " << pause_max_.count()
              << " us)\n";
}

} /* namespace Gem::Geneva::SubClientOverhead */
