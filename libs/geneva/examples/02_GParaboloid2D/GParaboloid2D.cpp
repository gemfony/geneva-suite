/**
 * @file GParaboloid2D.cpp
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

// Geneva header files go here
#include "courtier/GLauncherT.hpp"
#include "geneva/Go2.hpp"

// The individual that should be optimized
#include "GParaboloidIndividual2D.hpp"

using namespace Gem::Geneva;

// The long form of a main(): Geneva's description and courtier's launcher are built here, and the
// producing function is handed to the launcher. A program takes this form when it sets something
// on the launcher (a consumer chosen in code, a module path, an item processor); without that it
// is the same as the short form of 01_GSimpleOptimizer (runOptimization()).
//! [go2-long-form]
int main(int argc, char **argv) {
    // What Geneva says about itself: its configuration keys, its options, its modules
    GOptimizationDescription geneva;
    // courtier's launcher owns the command line, the consumer and the role of this
    // process
    Gem::Courtier::GLauncherT<gen::GGenome> launcher(argc, argv, "./config/Go2.json");

    // The producing function runs on the submitter only: a client or an MPI rank
    // other than 0 never runs it
    auto produce = [&geneva] {
        Go2 go(geneva);

        // Make an individual known to the optimizer
        std::shared_ptr<GParaboloidIndividual2D> const p(new GParaboloidIndividual2D());
        go.push_back(p);

        // The genome carries only structure; its Gauss adaptors live on an OA-owned
        // config. Register it for the evolutionary algorithm so Go2 hands it over
        // before the EA runs.
        go.registerAdaptionConfig(
            "PERSONALITY_EA",
            GParaboloidIndividual2D::buildAdaptionConfig(*p)
        );

        // Add an evolutionary algorithm to the Go2 class.
        go & "ea";

        // Perform the actual optimization
        auto const *algorithm = go.optimize();
        auto const best = algorithm->getBestGlobalIndividual<GParaboloidIndividual2D>();

        // Do something with the best result
        std::cout << "Best fitness found: " << best->raw_fitness(0) << '\n';
        return 0;
    };
    return launcher.run(geneva, {.produce = produce});
}
//! [go2-long-form]
