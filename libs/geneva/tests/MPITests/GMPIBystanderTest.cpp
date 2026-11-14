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

/*
 * A bystander in a plain Go2 program on the MPI consumer.
 *
 * The program initializes MPI itself and splits MPI_COMM_WORLD: ranks 0 and 1 share a
 * communicator, rank 2 gets MPI_COMM_NULL. It hands that communicator to the MPI module
 * (mpiEnvironmentOn()) and keeps the environment, so rank 0 is the submitter, rank 1 a client and
 * rank 2 a bystander, which takes no part in the consumer's protocol. Every rank checks its role
 * through consumerRole() and then runs as a Go2 program does: rank 2 must be in client mode, and
 * its clientRun() must return 0 at once; ranks 0 and 1 run one iteration. Rank 0 checks that the
 * end of the run (the consumer's shutdown) takes well below the shutdown grace. Each rank lets go
 * of everything before it finalizes MPI. Run with three ranks, each in a directory of its own.
 */

// Standard header files go here
#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <print>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "courtier/GCourtierEnums.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/GMPIEnvironmentAccess.hpp"
#include "geneva/Go2.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"

using namespace Gem::Geneva;
using Gem::Courtier::processRole;

namespace {

/** The longest a bystander's clientRun() may take. */
constexpr std::chrono::seconds BYSTANDERLIMIT{1};

/** The longest the end of the run may take on the submitter: well below the shutdown grace. */
constexpr auto SHUTDOWNLIMIT = Gem::Courtier::GMPICONSUMERSHUTDOWNGRACE / 2;

/** @brief The submitter's part: one iteration of a small population.
 *  @param go The Go2 object */
void optimize(Go2 &go) {
    const auto factory =
        std::make_shared<gind::GFunctionIndividualFactory>("./config/GFunctionIndividual.json");
    go.registerContentCreator(factory);
    const auto sample = factory->get_as<gind::GFunctionIndividual>();
    go.registerAdaptionConfig("PERSONALITY_EA", factory->getAdaptionConfig(*sample));

    const auto ea = std::make_shared<oa::GEvolutionaryAlgorithm>();
    ea->setPopulationSizes(4, 2);
    ea->setMaxIteration(1);
    go & ea;
    (void)go.optimize();
}

/** @brief Runs this rank as a Go2 program does, and checks what the test is about.
 *  @param argc The number of command line arguments
 *  @param argv The command line arguments
 *  @param world_rank The rank in MPI_COMM_WORLD
 *  @return 0 if every check passed on this rank */
int runAsGo2Program(int argc, char **argv, int world_rank) {
    int result = 0;
    std::chrono::steady_clock::time_point end_of_optimization{};
    {
        Go2 go(argc, argv, "./config/Go2.json");
        if(go.clientMode()) {
            if(world_rank == 0) {
                std::println("FAIL: rank 0, the submitter, is in client mode");
                result = 1;
            }
            const auto start = std::chrono::steady_clock::now();
            const int rc = go.clientRun();
            const auto took = std::chrono::steady_clock::now() - start;
            if(rc != 0) {
                std::println("FAIL: rank {}: clientRun() returned {}", world_rank, rc);
                result = 1;
            }
            if(world_rank == 2 && took > BYSTANDERLIMIT) {
                std::println(
                    "FAIL: rank 2, the bystander, took {} in clientRun()",
                    std::chrono::duration_cast<std::chrono::milliseconds>(took)
                );
                result = 1;
            }
            return result;
        }

        if(world_rank != 0) {
            std::println("FAIL: rank {} is not in client mode", world_rank);
            return 1;
        }
        optimize(go);
        end_of_optimization = std::chrono::steady_clock::now();
    } // the consumer is released here: it tells the client to stop and waits for it

    const auto took = std::chrono::steady_clock::now() - end_of_optimization;
    if(took > SHUTDOWNLIMIT) {
        std::println(
            "FAIL: the end of the run took {} on the submitter",
            std::chrono::duration_cast<std::chrono::milliseconds>(took)
        );
        return 1;
    }
    return result;
}

/** @brief The test itself; see the file comment.
 *  @param argc The number of command line arguments
 *  @param argv The command line arguments
 *  @return 0 if every check passed on this rank */
int runTest(int argc, char **argv) {
    int provided = 0;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    int world_rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

    // Ranks 0 and 1 share a communicator; rank 2 gets MPI_COMM_NULL.
    MPI_Comm base = MPI_COMM_NULL;
    MPI_Comm_split(MPI_COMM_WORLD, world_rank < 2 ? 0 : MPI_UNDEFINED, world_rank, &base);

    int result = 0;
    {
        const auto environment = mpiEnvironmentOn([base]() -> MPI_Comm { return base; });

        constexpr std::array<processRole, 3> EXPECTED{
            processRole::SUBMITTER,
            processRole::CLIENT,
            processRole::BYSTANDER
        };
        const processRole role = consumerRole(ConsumerSpec{.mnemonic = "mpi"});
        if(world_rank > 2 || role != EXPECTED.at(static_cast<std::size_t>(world_rank))) {
            std::println("FAIL: rank {} has the role {}", world_rank, static_cast<int>(role));
            result = 1;
        }
        else {
            result = runAsGo2Program(argc, argv, world_rank);
        }
    }

    if(base != MPI_COMM_NULL) {
        MPI_Comm_free(&base);
    }
    MPI_Finalize();
    if(result == 0) {
        std::println("OK: rank {} had its role and finished", world_rank);
    }
    return result;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) -- only the printing in the handlers could throw
int main(int argc, char **argv) {
    try {
        return runTest(argc, argv);
    }
    catch(const std::exception &e) {
        std::println(stderr, "FAIL: {}", e.what());
    }
    catch(...) {
        std::println(stderr, "FAIL: an unknown exception");
    }
    return 1;
}
