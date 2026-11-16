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
#include <atomic>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Gem::Geneva::SubClientOverhead {

namespace {

/// lead to member: the time to sleep, then the member's share of the parameters
constexpr int PARTTAG = 1;
/// member to lead: its result (one double)
constexpr int DONETAG = 2;
/// lead to member: no part follows (an empty message)
constexpr int STOPTAG = 3;

/** @brief The group of this process; set once before the first evaluation */
MPI_Comm &groupStorage() {
    static MPI_Comm group = MPI_COMM_NULL;
    return group;
}

/** @brief The pauses of this process; set once before the first wait */
PollPauses &pauseStorage() {
    static PollPauses pauses;
    return pauses;
}

/** @brief The rounds this process has led */
std::atomic<std::uint64_t> &roundCounter() {
    static std::atomic<std::uint64_t> rounds{0};
    return rounds;
}

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

/******************************************************************************/
/**
 * Looks until @p look returns true, without spinning: between two looks it pauses, with a pause
 * that doubles from the configured minimum up to the maximum while nothing comes.
 *
 * @param look Returns true when what is waited for is there
 */
template <typename F>
void pollUntil(const F &look) {
    const PollPauses pauses = pollPauses();
    auto pause = pauses.min;
    while(not look()) {
        std::this_thread::sleep_for(pause);
        pause = std::min(2 * pause, pauses.max);
    }
}

/**
 * Waits until every request has completed (see pollUntil()).
 *
 * @param requests The requests to wait for
 */
void waitAll(std::span<MPI_Request> requests) {
    if(requests.empty()) {
        return; // a group of one, which may run without MPI
    }
    pollUntil([&requests]() {
        int done = 0;
        check(
            MPI_Testall(
                static_cast<int>(requests.size()),
                requests.data(),
                &done,
                MPI_STATUSES_IGNORE
            ),
            "MPI_Testall"
        );
        return done != 0;
    });
}

/**
 * The size of a group; a null communicator is a group of one.
 *
 * @param group The group
 * @return Its number of processes
 */
int groupSize(MPI_Comm group) {
    if(group == MPI_COMM_NULL) {
        return 1;
    }
    int size = 1;
    check(MPI_Comm_size(group, &size), "MPI_Comm_size");
    return size;
}

/**
 * What a process computes on its share of the parameters: the sum of their squares.
 *
 * @param share The share
 * @return The sum of the squares
 */
double sumOfSquares(std::span<const double> share) {
    double sum = 0.;
    for(const double x : share) {
        sum += x * x;
    }
    return sum;
}

} /* namespace */

/******************************************************************************/
/**
 * The lead's half of one evaluation of nominal duration @p evaluation_time. Split, each of the
 * group's n processes sleeps evaluation_time / n, the lead included; otherwise every process
 * sleeps the whole time. The parameters are shared out in n contiguous slices, the first being
 * the lead's. Every member gets a message with its time and its slice before the lead sleeps its
 * own part; the round ends when every member has answered with the sum of the squares of its
 * slice. Rounds of one process run one after the other, since a group can serve only one at a
 * time; a client that evaluates on several threads gets them in turn.
 *
 * @param group The group's communicator; the lead is its rank 0 (MPI_COMM_NULL: a group of one)
 * @param evaluation_time The nominal duration of the evaluation
 * @param split Whether the processes of the group share the evaluation time
 * @param parameters The parameter values of the individual
 * @return The sum of the squares of all parameter values
 */
double leadRound(
    MPI_Comm group,
    std::chrono::duration<double> evaluation_time,
    bool split,
    std::span<const double> parameters
) {
    static std::mutex one_round_at_a_time;
    const std::scoped_lock lock(one_round_at_a_time);

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
    waitAll(requests);
    roundCounter()++;
    return std::ranges::fold_left(answers, 0., std::plus{});
}

/******************************************************************************/
/**
 * The member's half: waits for a message of its lead (rank 0 of @p group), sleeps the time it
 * names, answers with the sum of the squares of the parameter values it carries, and so on until
 * the lead releases the group. Prints how many parts it served, for the checks.
 *
 * @param group The group's communicator
 * @return 0, the exit code of the member
 */
int serveGroup(MPI_Comm group) {
    int rank = 0;
    check(MPI_Comm_rank(group, &rank), "MPI_Comm_rank");
    std::uint64_t parts = 0;
    std::vector<double> message;
    while(true) {
        MPI_Status status{};
        pollUntil([&]() {
            int found = 0;
            check(MPI_Iprobe(0, MPI_ANY_TAG, group, &found, &status), "MPI_Iprobe");
            return found != 0;
        });
        int count = 0;
        check(MPI_Get_count(&status, MPI_DOUBLE, &count), "MPI_Get_count");
        message.resize(static_cast<std::size_t>(std::max(count, 1)));
        check(
            MPI_Recv(
                message.data(),
                count,
                MPI_DOUBLE,
                0,
                status.MPI_TAG,
                group,
                MPI_STATUS_IGNORE
            ),
            "MPI_Recv"
        );
        if(status.MPI_TAG == STOPTAG) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(message.front()));
        double result = sumOfSquares(std::span(message).subspan(1));
        // The MPI checker does not see that waitAll() waits for the request.
        // NOLINTBEGIN(clang-analyzer-optin.mpi.MPI-Checker)
        MPI_Request request = MPI_REQUEST_NULL;
        check(MPI_Isend(&result, 1, MPI_DOUBLE, 0, DONETAG, group, &request), "MPI_Isend");
        waitAll(std::span(&request, 1));
        // NOLINTEND(clang-analyzer-optin.mpi.MPI-Checker)
        parts++;
    }
    std::cout << "GSleepGroup: member " << rank << " served " << parts << " parts\n";
    return 0;
}

/******************************************************************************/
/**
 * Tells every member of @p group that no part follows, and waits until they have the message.
 *
 * @param group The group's communicator (MPI_COMM_NULL: a group of one, nothing to tell)
 */
void releaseGroup(MPI_Comm group) {
    const int size = groupSize(group);
    std::vector<MPI_Request> requests(static_cast<std::size_t>(size - 1), MPI_REQUEST_NULL);
    for(int rank = 1; rank < size; rank++) {
        check(
            MPI_Isend(nullptr, 0, MPI_DOUBLE, rank, STOPTAG, group, &requests[rank - 1]),
            "MPI_Isend"
        );
    }
    waitAll(requests);
    std::cout << "GSleepGroup: lead of " << size << " processes led " << roundsLed()
              << " rounds (poll pauses " << pollPauses().min.count() << " to "
              << pollPauses().max.count() << " us)\n";
}

/******************************************************************************/
/**
 * @return The group the evaluations of this process use (MPI_COMM_NULL: a group of one)
 */
MPI_Comm processGroup() {
    return groupStorage();
}

/**
 * Sets the group of this process, before its first evaluation (before any thread evaluates).
 *
 * @param group The group's communicator, the lead being its rank 0
 */
void setProcessGroup(MPI_Comm group) {
    groupStorage() = group;
}

/**
 * Sets the pauses between two looks for a message, before the first wait.
 *
 * @param pauses The bounds; the minimum must be positive and not above the maximum
 */
void setPollPauses(PollPauses pauses) {
    if(pauses.min.count() <= 0 or pauses.min > pauses.max) {
        throw std::runtime_error("GSleepGroup: the poll pauses must satisfy 0 < min <= max");
    }
    pauseStorage() = pauses;
}

/**
 * @return The pauses between two looks for a message
 */
PollPauses pollPauses() {
    return pauseStorage();
}

/**
 * @return The rounds this process has led so far
 */
std::uint64_t roundsLed() {
    return roundCounter().load();
}

} /* namespace Gem::Geneva::SubClientOverhead */
