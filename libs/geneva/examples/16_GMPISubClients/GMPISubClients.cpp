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
// (courtier/mpi/GMPICascade.hpp). Rank 0 is the submitter and runs the optimization through Go2
// on courtier's launcher; the other processes form groups of sub_client_group_size processes
// (config/GMPICascade.json). The first process of a group is its lead, a client of the MPI
// consumer, whose items the launcher hands to an item processor; the others are its members,
// which run no Geneva code. A lead evaluates every item in a round of its group: the
// parameter values are shared out over the group, and every process computes its part
// (GParaboloidGroup.hpp). Started as
//   mpirun -np <1 + clients x group size> ./GMPISubClients --consumer mpi

// Standard header files go here
#include <chrono>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <tuple>
#include <vector>

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "common/GConfigEmission.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GLauncherT.hpp"
#include "courtier/mpi/GMPICascade.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp"
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
 * @param go The Go2 of the run
 * @return The exit code
 */
int optimize(Go2 &go) {
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

} /* namespace */

/******************************************************************************/

int main(int argc, char **argv) {
    // Geneva's description and courtier's launcher, built before the cascade: a request for the
    // help text is answered before the cascade would start MPI (the long form of a main(), as the
    // launcher is given the leads' item processor below)
    GOptimizationDescription geneva;
    Gem::Courtier::GLauncherT<gen::GGenome> launcher(argc, argv, "./config/Go2.json");
    if(launcher.helpRequested()) {
        return launcher.run(geneva, {});
    }
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
    if(cascade.role() == processRole::BYSTANDER) { // a member, which runs no Geneva code
        member.setPauses(cascade.config().pauseMin, cascade.config().pauseMax);
        return cascade.serve(member);
    }

    // A lead: its client of the MPI consumer evaluates every item in a round of its group;
    // process() keeps the item's lifecycle (status, timing; an exception flags the item)
    const auto pause_min = cascade.config().pauseMin;
    const auto pause_max = cascade.config().pauseMax;
    launcher.registerItemProcessor([&cascade, pause_min, pause_max](gen::GGenome &item) {
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

    const int rc = launcher.run(
        geneva,
        {.prepare = [&cascade](processRole role) -> std::optional<int> {
             // The consumer's role must be the cascade's (none under --update-configs, which
             // runs without MPI)
             if(not Gem::Common::GParserBuilder::updateInPlace()) {
                 cascade.requireRole(role);
             }
             return std::nullopt;
         },
         .produce =
             [&geneva] {
                 Go2 go(geneva);
                 return optimize(go);
             }}
    );
    if(cascade.role() == processRole::CLIENT) { // also if the launcher ended before its role
        cascade.release();                      // the members leave serve()
    }
    return rc;
}
