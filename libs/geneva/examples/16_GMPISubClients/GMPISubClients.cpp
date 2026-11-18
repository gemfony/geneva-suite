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

// Standard header files go here
#include <iostream>
#include <print>

// Boost header files go here

// MPI header files go here
#include <mpi.h>

// Geneva header files go here
#include "courtier/GMPIHelperFunctions.hpp"
#include "geneva/GMPISubClientOptimizer.hpp"

// The individual that should be optimized
#include "GMPISubClientParaboloidIndividualMultiD.hpp"

using namespace Gem::Geneva;

int main(int argc, char **argv) {
    GMPISubClientOptimizer optimizer(argc, argv, "config/GMPISubClientOptimizer.json");

    // register the sub-client job which is executed by sub-clients who need to communicate with geneva clients inside a subgroup
    optimizer.registerSubClientJob(GMPISubClientParaboloidIndividualMultiD::subClientJob);

    //---------------------------------------------------------------------
    // Initialize a client, if requested
    if(optimizer.clientMode()) {
        return optimizer.clientRun();
    }

    //---------------------------------------------------------------------
    // Add individuals and algorithms and perform the actual optimization cycle

    // Make an individual known to the optimizer
    std::shared_ptr<GMPISubClientParaboloidIndividualMultiD> const p(
        new GMPISubClientParaboloidIndividualMultiD()
    );
    optimizer.push_back(p);

    // The genome carries only structure; its Gauss adaptors live on an OA-owned config. Register it for
    // the evolutionary algorithm so it is handed over before the EA runs.
    optimizer.registerAdaptionConfig(
        "PERSONALITY_EA",
        GMPISubClientParaboloidIndividualMultiD::buildAdaptionConfig(*p)
    );

    // Add an evolutionary algorithm to the Go2 class.
    optimizer & "ea";

    auto timeStart{std::chrono::system_clock::now()};

    // Perform the actual optimization
    auto const bestIndividual_ptr =
        optimizer.optimize()->getBestGlobalIndividual<GMPISubClientParaboloidIndividualMultiD>();

    auto timeElapsed{std::chrono::system_clock::now() - timeStart};

    std::println(
        "Optimization finished in {} milliseconds",
        std::chrono::duration_cast<std::chrono::milliseconds>(timeElapsed).count()
    );

    // Do something with the best result
}
