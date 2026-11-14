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
 * A cascade on a base communicator whose rank 0 is not rank 0 of MPI_COMM_WORLD.
 *
 * The program initializes MPI itself and reverses the ranks: rank 0 of the base communicator is
 * the last rank of MPI_COMM_WORLD. GMPISubClientOptimizer runs on that base with groups of two
 * (one geneva client and one sub-client), and every rank checks that its role follows its rank in
 * the base communicator: 0 is the submitter, an odd rank a client, an even rank above 0 a
 * bystander (a sub-client). One iteration is then run, as example 16 does. Who is master must
 * follow courtier's communicator; a consumer that took it from MPI_COMM_WORLD would make a
 * sub-client the master. Run with five ranks.
 */

// Standard header files go here
#include <exception>
#include <memory>
#include <print>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "common/GConfigEmission.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "geneva/GMPIEnvironmentAccess.hpp"
#include "geneva/GMPISubClientOptimizer.hpp"

// The individual of example 16
#include "GMPISubClientParaboloidIndividualMultiD.hpp"

using namespace Gem::Geneva;
using Gem::Courtier::processRole;

namespace {

constexpr const char *CONFIGFILE = "config/GMPISubClientOptimizer.json";

/** @brief Runs one optimization on the submitter, or the client loop elsewhere, as example 16.
 *  @param optimizer The optimizer
 *  @return The exit code of the client loop, or 0 on the submitter */
int run(GMPISubClientOptimizer &optimizer) {
    optimizer.registerSubClientJob(GMPISubClientParaboloidIndividualMultiD::subClientJob);
    if(optimizer.clientMode()) {
        return optimizer.clientRun();
    }

    const auto p = std::make_shared<GMPISubClientParaboloidIndividualMultiD>();
    optimizer.push_back(p);
    optimizer.registerAdaptionConfig(
        "PERSONALITY_EA",
        GMPISubClientParaboloidIndividualMultiD::buildAdaptionConfig(*p)
    );
    optimizer & "ea";
    const auto best =
        optimizer.optimize()->getBestGlobalIndividual<GMPISubClientParaboloidIndividualMultiD>();
    return 0;
}

/** @brief The role a rank of the base communicator must have with groups of two.
 *  @param base_rank The rank in the base communicator
 *  @return The expected role */
processRole expectedRole(int base_rank) {
    if(base_rank == 0) {
        return processRole::SUBMITTER;
    }
    return (base_rank % 2 == 1) ? processRole::CLIENT : processRole::BYSTANDER;
}

/** @brief The test itself; see the file comment.
 *  @param argc The number of command line arguments
 *  @param argv The command line arguments
 *  @return 0 if every check passed on this rank */
int runTest(int argc, char **argv) {
    // The build materializes the configuration with --update-configs, outside mpirun: no MPI then
    // (the optimizer returns before any MPI call in that mode).
    if(Gem::Common::configEmissionRequested(argc, argv)) {
        GMPISubClientOptimizer optimizer(argc, argv, CONFIGFILE, {}, MPI_COMM_NULL);
        return run(optimizer);
    }

    int provided = 0;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    int world_rank = 0;
    int world_size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    // The base communicator reverses the ranks of MPI_COMM_WORLD.
    MPI_Comm base = MPI_COMM_NULL;
    MPI_Comm_split(MPI_COMM_WORLD, 0, world_size - 1 - world_rank, &base);
    int base_rank = 0;
    MPI_Comm_rank(base, &base_rank);

    int result = 0;
    {
        GMPISubClientOptimizer optimizer(argc, argv, CONFIGFILE, {}, base);

        const processRole expected = expectedRole(base_rank);
        const processRole role = mpiProcessRole();
        if(role != expected) {
            std::println(
                "FAIL: world rank {} (base rank {}) has the role {}, expected {}",
                world_rank,
                base_rank,
                static_cast<int>(role),
                static_cast<int>(expected)
            );
            result = 1;
        }

        const int rc = run(optimizer);
        if(rc != 0) {
            std::println(
                "FAIL: world rank {} (base rank {}) ended with {}",
                world_rank,
                base_rank,
                rc
            );
            result = rc;
        }
    }

    MPI_Comm_free(&base);
    MPI_Finalize();
    if(result == 0) {
        std::println(
            "OK: world rank {} (base rank {}) had its role and finished",
            world_rank,
            base_rank
        );
    }
    return result;
}

} // namespace

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
