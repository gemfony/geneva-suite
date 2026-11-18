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
 * An MPI cascade on a base communicator whose rank 0 is not rank 0 of MPI_COMM_WORLD.
 *
 * The program initializes MPI itself and reverses the ranks: rank 0 of the base communicator is the
 * last rank of MPI_COMM_WORLD. A cascade (Gem::Courtier::GMPICascade) splits that base into the
 * submitter and groups of two (a lead and a member). Every rank checks that its role follows its
 * rank in the base communicator -- 0 is the submitter, an odd rank a lead (CLIENT), an even rank
 * above 0 a member (BYSTANDER) -- both the cascade's role and the role that Geneva's MPI module,
 * loaded at run time, reports through consumerRole(): the module must find the environment the
 * cascade built. A consumer that took the roles from MPI_COMM_WORLD would make a member the
 * submitter. Then one iteration of a small population is run: the submitter through Go2, every
 * lead evaluates each item in a round of its group, every member serves the rounds. MPI stays the
 * program's to finalize. Run with five ranks.
 */

// Standard header files go here
#include <exception>
#include <memory>
#include <print>
#include <tuple>
#include <vector>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "courtier/GBaseClientT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/mpi/GMPICascade.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/Go2.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"

using namespace Gem::Geneva;
using Gem::Courtier::processRole;

namespace {

/** @brief A member that meets its lead once per round. */
class GBarrierMember final : public Gem::Courtier::GMPIGroupMemberI {
public:
    /** @brief Meets the lead @param group The group's communicator */
    void serveRound(MPI_Comm group) override {
        MPI_Barrier(group);
    }
};

/** @brief The role a rank of the base communicator must have with groups of two.
 *  @param base_rank The rank in the base communicator
 *  @return The expected role */
processRole expectedRole(int base_rank) {
    if(base_rank == 0) {
        return processRole::SUBMITTER;
    }
    return (base_rank % 2 == 1) ? processRole::CLIENT : processRole::BYSTANDER;
}

/** @brief The submitter's part: one iteration of a small population.
 *  @param argc The number of command line arguments
 *  @param argv The command line arguments */
void optimize(int argc, char **argv) {
    Go2 go(argc, argv, "./config/Go2.json");
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

/** @brief A lead's part: its client evaluates every item in a round of its group.
 *  @param argc The number of command line arguments
 *  @param argv The command line arguments
 *  @param cascade The cascade
 *  @return 0 if the client had the lead's role */
int lead(int argc, char **argv, Gem::Courtier::GMPICascade &cascade) {
    const auto [role, client] = buildClientFromCommandLine(argc, argv);
    cascade.requireRole(role);
    client->setItemProcessor([&cascade](gen::GGenome &item) {
        std::ignore = item.process([&cascade](const gen::GGenome &candidate) {
            std::vector<double> parameters;
            candidate.streamline(parameters);
            return std::vector<double>{cascade.round([&parameters](MPI_Comm group) {
                MPI_Barrier(group);
                double sum = 0.;
                for(const double p : parameters) {
                    sum += p * p;
                }
                return sum;
            })};
        });
    });
    client->run();
    cascade.release();
    return 0;
}

/** @brief The test itself; see the file comment.
 *  @param argc The number of command line arguments
 *  @param argv The command line arguments
 *  @return 0 if every check passed on this rank */
int runTest(int argc, char **argv) {
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
        GBarrierMember member; // outlives the cascade
        Gem::Courtier::GMPICascade cascade(
            [] {
                return Gem::Courtier::GMPICascadeConfig{
                    .groupSize = 2,
                    .layout = Gem::Courtier::GMPICascadeLayout::SUBMITTER_AND_GROUPS
                };
            },
            base
        );

        // The roles are checked; the run follows the cascade's role either way, so that every rank
        // reaches its end.
        const processRole expected = expectedRole(base_rank);
        const processRole consumer_role =
            consumerRole(Gem::Courtier::ConsumerSpec{.mnemonic = "mpi"});
        if(cascade.role() != expected || consumer_role != expected) {
            std::println(
                "FAIL: world rank {} (base rank {}) has the role {} in the cascade and {} for the "
                "MPI consumer, expected {}",
                world_rank,
                base_rank,
                static_cast<int>(cascade.role()),
                static_cast<int>(consumer_role),
                static_cast<int>(expected)
            );
            result = 1;
        }

        int rc = 0;
        switch(cascade.role()) {
        case processRole::BYSTANDER:
            rc = cascade.serve(member);
            break;
        case processRole::CLIENT:
            rc = lead(argc, argv, cascade);
            break;
        default:
            cascade.requireRole(consumer_role);
            optimize(argc, argv);
            break;
        }
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
