/**
 * @file GStarter.cpp
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

// Boost header files go here

// Geneva header files go here
#include "geneva/GOptimizer.hpp"
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"

// The individual that should be optimized
#include "GStarterIndividual.hpp"

using namespace Gem::Geneva;

// clang-format off
//! [goptimizer-main-quickstart]
int main(int argc, char **argv) {
    // Geneva on courtier's launcher: the lambda runs on the process that submits work
    return runOptimization(argc, argv, {.produce = [](GOptimizer &go, const auto &) {
        // Add a "pluggable optimization monitor" to GOptimizer. This particular monitor records the
        // solutions it sees into its stream, the file records/all_solutions.csv.
        //! [monitors:register-simple]
        auto allSolutionLogger_ptr = std::make_shared<GAllSolutionFileLogger>();
        // Also record the initial population, prior to optimization
        allSolutionLogger_ptr->setPrintInitial();

        go.registerPluggableOM(allSolutionLogger_ptr);
        //! [monitors:register-simple]

        // Create a factory for GStarterIndividual objects and perform
        // any necessary initial work.
        auto gsif_ptr =
            std::make_shared<GStarterIndividualFactory>("./config/GStarterIndividual.json");

        // Add a content creator so GOptimizer can generate its own individuals if necessary
        go.registerContentCreator(gsif_ptr);

        // The genome carries only structure; its Gauss adaptor lives on an OA-owned
        // config the factory authors from the configuration. Register it for the
        // adapting algorithms (EA / SA).
        {
            auto sample = gsif_ptr->get_as<GStarterIndividual>();
            auto cfg = gsif_ptr->getAdaptionConfig(*sample);
            go.registerAdaptionConfig("PERSONALITY_EA", cfg);
            go.registerAdaptionConfig("PERSONALITY_SA", cfg);
        }

        // Perform the actual optimization
        //! [results-harvest]
        auto bestIndividual_ptr =
            go.optimize()->getBestGlobalIndividual<GStarterIndividual>();

        // Do something with the best result. Here we simply print the result to stdout.
        std::cout << bestIndividual_ptr << '\n';
        //! [results-harvest]
        return 0;
    }});
}
//! [goptimizer-main-quickstart]
// clang-format on
