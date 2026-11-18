/**
 * @file GMPISubClients.cpp
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

// Parallelism inside one evaluation: the processes of an MPI job form courtier's MPI cascade
// (courtier/mpi/GMPICascade.hpp). Rank 0 is the submitter and runs the optimization through Go2;
// the other processes form groups of sub_client_group_size processes (config/GMPICascade.json).
// The first process of a group is its lead, a client of the MPI consumer; the others are its
// members, which run no Geneva code. A lead evaluates every item in a round of its group: the
// parameter values are shared out over the group, and every process computes its part
// (GParaboloidGroup.hpp). Started as
//   mpirun -np <1 + clients x group size> ./GMPISubClients --consumer mpi

// Standard header files go here
#include <chrono>
#include <memory>
#include <print>
#include <string>
#include <tuple>
#include <vector>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "common/GConfigEmission.hpp"
#include "common/GFatalReport.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GConsumerSpec.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/mpi/GMPICascade.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/Go2.hpp"

#include "GParaboloidGroup.hpp"
#include "GParaboloidGroupIndividual.hpp"

using namespace Gem::Geneva;
using Gem::Courtier::GMPICascade;
using Gem::Courtier::processRole;

namespace {

/******************************************************************************/
/**
 * The submitter: an evolutionary algorithm through Go2.
 *
 * @param argc The number of arguments
 * @param argv The arguments
 * @param cascade The cascade
 * @return The exit code
 */
int optimize(int argc, char **argv, GMPICascade &cascade) {
    if(not Gem::Common::configEmissionRequested(argc, argv)) { // the cascade runs without MPI then
        cascade.requireRole(consumerRole(Gem::Courtier::ConsumerSpec{.mnemonic = "mpi"}));
    }
    Go2 go(argc, argv, "./config/Go2.json");

    const auto individual = std::make_shared<GParaboloidGroupIndividual>();
    go.push_back(individual);
    go.registerAdaptionConfig(
        "PERSONALITY_EA",
        GParaboloidGroupIndividual::buildAdaptionConfig(*individual)
    );
    go & "ea";

    const auto start = std::chrono::steady_clock::now();
    const auto best = go.optimize()->getBestGlobalIndividual<GParaboloidGroupIndividual>();
    std::println(
        "Optimization finished in {} milliseconds",
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        )
            .count()
    );
    return 0;
}

/******************************************************************************/
/**
 * A lead: its client of the MPI consumer evaluates every item in a round of its group.
 *
 * @param argc The number of arguments
 * @param argv The arguments
 * @param cascade The cascade
 * @return The exit code
 */
int lead(int argc, char **argv, GMPICascade &cascade) {
    // Built here only: on a member it would load the consumers, and a member runs no Geneva code
    const auto [role, client] = buildClientFromCommandLine(argc, argv);
    cascade.requireRole(role);
    const auto pause_min = cascade.config().pauseMin;
    const auto pause_max = cascade.config().pauseMax;
    // process() keeps the item's lifecycle (status, timing; an exception flags the item)
    client->setItemProcessor([&cascade, pause_min, pause_max](gen::GGenome &item) {
        std::ignore = item.process([&](const gen::GGenome &candidate) -> std::vector<double> {
            std::vector<double> parameters;
            candidate.streamline(parameters);
            return {cascade.round([&](MPI_Comm group) {
                return leadHalf(
                    group,
                    parameters,
                    GParaboloidGroupIndividual::DELAYPERPARAMETER,
                    pause_min,
                    pause_max
                );
            })};
        });
    });
    client->run();
    cascade.release(); // the members leave serve()
    return 0;
}

} /* namespace */

/******************************************************************************/

int main(int argc, char **argv) {
    // An error nobody handles ends the program with a message and the exit code 1; the leads and
    // the members construct no Go2, which would install this
    Gem::Common::installFatalReport();
    if(Gem::Common::configEmissionRequested(argc, argv)) {
        // --update-configs: the cascade makes no MPI call, its callable writes its file
        Gem::Common::beginConfigEmission();
    }

    // Declared before the cascade, so that it outlives it
    GParaboloidGroupMember member(
        GParaboloidGroupIndividual::NPARAMETERS,
        GParaboloidGroupIndividual::DELAYPERPARAMETER
    );
    GMPICascade cascade([] {
        Gem::Courtier::GMPICascadeConfig config;
        config.load(std::string(Gem::Courtier::GMPICASCADECONFIGFILE));
        config.layout = Gem::Courtier::GMPICascadeLayout::SUBMITTER_AND_GROUPS;
        return config;
    });

    switch(cascade.role()) {
    case processRole::BYSTANDER: // a member
        member.setPauses(cascade.config().pauseMin, cascade.config().pauseMax);
        return cascade.serve(member);
    case processRole::CLIENT: // a lead
        return lead(argc, argv, cascade);
    default: // the submitter
        return optimize(argc, argv, cascade);
    }
}
