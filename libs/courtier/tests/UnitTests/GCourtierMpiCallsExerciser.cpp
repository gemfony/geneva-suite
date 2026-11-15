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

// The binding of the MPI transport's calls to MPI (GMPICalls), on two ranks under mpirun: rank 1
// sends three messages of different tags and lengths, rank 0 probes and receives them, and each
// send is seen to complete; then rank 0 gives up a large send that rank 1 has not received yet.
// With the argument --never-received, rank 1 receives nothing at all: rank 0 gives up a large and a
// small send that never complete, and both ranks must still finalize MPI and end (the test that runs
// this has a time limit).
// Exit status 0 on success; the first failure is printed and ends the program with 1.

// Standard headers go here
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// MPI headers go here
#include <mpi.h>

// Geneva headers go here
#include "courtier/mpi/GMPICalls.hpp"
#include "courtier/mpi/GMPIEnvironmentAccess.hpp"

using Gem::Courtier::Consumers::GMPICalls;
using Gem::Courtier::Consumers::GMPIProbed;
using Gem::Courtier::Consumers::GMPIRequest;

namespace {

using clock_type = std::chrono::steady_clock;
constexpr auto PATIENCE = std::chrono::seconds(20);

/** @brief Ends the program with a message if @p condition does not hold */
void require(bool condition, const std::string &what, int rank) {
    if(not condition) {
        std::cerr << "GCourtierMpiCallsExerciser, rank " << rank << ": " << what << '\n';
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
}

/** @brief The messages rank 1 sends: tag and bytes */
std::vector<std::pair<int, std::string>> messages() {
    return {{50, "the first"}, {52, ""}, {51, std::string(std::size_t{1} << 20U, 'x')}};
}

/** @brief Rank 1: sends, and waits until every send has completed */
void sender(GMPICalls &calls) {
    std::vector<std::unique_ptr<std::string>> bytes;
    std::vector<GMPIRequest> requests;
    for(auto &[tag, text] : messages()) {
        bytes.push_back(std::make_unique<std::string>(std::move(text)));
        requests.push_back(calls.startSend(0, tag, *bytes.back()));
        require(requests.back() != GMPIRequest::NONE, "a send that was started has no handle", 1);
    }
    std::size_t completed = 0;
    const auto give_up_at = clock_type::now() + PATIENCE;
    while(completed < requests.size() && clock_type::now() < give_up_at) {
        completed += calls.testSome(requests);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    require(completed == requests.size(), "not every send completed", 1);
    for(const auto request : requests) {
        require(request == GMPIRequest::NONE, "a completed send was not set to NONE", 1);
    }
    require(calls.testSome(requests) == 0, "a send completed twice", 1);
}

/** @brief Rank 0: probes and receives, and checks what arrived */
void receiver(GMPICalls &calls) {
    const auto expected = messages();
    std::size_t received = 0;
    const auto give_up_at = clock_type::now() + PATIENCE;
    while(received < expected.size() && clock_type::now() < give_up_at) {
        const std::optional<GMPIProbed> probed = calls.probe();
        if(not probed) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        const auto &[tag, text] = expected[received];
        require(probed->source == 1, "a message from rank " + std::to_string(probed->source), 0);
        require(probed->tag == tag, "the tag " + std::to_string(probed->tag) + " out of order", 0);
        require(probed->size == text.size(), "a probe told the wrong length", 0);
        require(calls.receive(*probed) == text, "a message arrived changed", 0);
        ++received;
    }
    require(received == expected.size(), "not every message arrived", 0);
}

} /* anonymous namespace */

/** @brief Rank 0 gives up sends that rank 1 never receives; rank 1 receives nothing */
void neverReceived(GMPICalls &calls, MPI_Comm communicator) {
    const int rank = calls.rank();
    if(rank == 0) {
        // A large message, which MPI holds back until the other rank receives it, and a small one
        auto large = std::make_unique<std::string>(std::size_t{16} << 20U, 'z');
        auto small = std::make_unique<std::string>("small");
        const GMPIRequest large_request = calls.startSend(1, 54, *large);
        const GMPIRequest small_request = calls.startSend(1, 55, *small);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::vector<GMPIRequest> requests{large_request};
        require(
            calls.testSome(requests) == 0,
            "a send to a rank that receives nothing completed",
            0
        );
        calls.giveUp(large_request, std::move(large));
        calls.giveUp(small_request, std::move(small));
    }
    MPI_Barrier(communicator);
}

int main(int argc, char **argv) {
    const bool never_received = argc > 1 && std::string(argv[1]) == "--never-received";
    // The environment initializes MPI; it is let go of before main returns.
    auto environment = Gem::Courtier::mpiEnvironment();
    if(never_received) {
        {
            GMPICalls calls(environment);
            require(calls.size() == 2, "this program runs on two ranks", calls.rank());
            neverReceived(calls, environment->communicator());
        }
        environment.reset(); // MPI_Finalize, with the given-up sends
        return EXIT_SUCCESS;
    }
    {
        GMPICalls calls(environment);
        const int rank = calls.rank();
        require(calls.size() == 2, "this program runs on two ranks", rank);

        if(rank == 1) {
            sender(calls);
        }
        else {
            receiver(calls);
        }
        MPI_Barrier(environment->communicator());

        // A large send that rank 1 does not receive before it is given up. Whether the cancel
        // succeeds is MPI's business: rank 1 receives the message if it arrives all the same.
        if(rank == 0) {
            auto bytes = std::make_unique<std::string>(std::size_t{4} << 20U, 'y');
            const GMPIRequest request = calls.startSend(1, 53, *bytes);
            calls.giveUp(request, std::move(bytes));
        }
        MPI_Barrier(environment->communicator());
        if(rank == 1) {
            const auto look_until = clock_type::now() + std::chrono::milliseconds(500);
            while(clock_type::now() < look_until) {
                if(const auto probed = calls.probe()) {
                    require(probed->tag == 53, "an unexpected message after the give-up", 1);
                    static_cast<void>(calls.receive(*probed));
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        MPI_Barrier(environment->communicator());
    }
    environment.reset();
    return EXIT_SUCCESS;
}
