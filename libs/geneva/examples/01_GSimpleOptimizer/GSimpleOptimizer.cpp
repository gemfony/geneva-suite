/**
 * @file GSimpleOptimizer.cpp
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

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;

// The short form of a main(): Geneva's runOptimization() hands Geneva's description to courtier's
// launcher, which owns the command line, the consumer and the role of this process; the lambda runs
// on the submitter only. 02_GParaboloid2D shows the same program in the long form, with the
// launcher built in main().
int main(int argc, char **argv) {
    // What the process that submits the work does; a client never runs it
    auto produce = [](GOptimizer &go, const GOptimizationDescription &) {
        //-----------------------------------------------------------------------
        // Create a factory for GFunctionIndividual objects and perform
        // any necessary initial work.
        auto const gfi_ptr =
            std::make_shared<gind::GFunctionIndividualFactory>("./config/GFunctionIndividual.json");

        // Add a content creator so GOptimizer can generate its own individuals, if necessary
        go.registerContentCreator(gfi_ptr);

        // The genome carries only structure; the configured adaptor lives on an OA-owned config
        // the factory authors. Register it for the adapting algorithms (EA / SA) so GOptimizer
        // hands it over before they run.
        {
            //! [adaptors-goptimizer-register]
            auto sample = gfi_ptr->get_as<gind::GFunctionIndividual>();
            auto cfg = gfi_ptr->getAdaptionConfig(*sample);
            go.registerAdaptionConfig("ea", cfg);
            go.registerAdaptionConfig("sa", cfg);
            //! [adaptors-goptimizer-register]
        }

        // Add a default optimization algorithm to the GOptimizer object. This is optional.
        // Indeed "ea" is the default setting anyway. However, if you do not like it, you
        // can register another default algorithm here, which will then be used, unless
        // you specify other algorithms on the command line. You can also add a smart
        // pointer to an optimization algorithm here instead of its mnemonic.
        go.registerDefaultAlgorithm("ea");

        // Perform the actual optimization
        auto const p = go.optimize().getBestGlobalIndividual<gind::GFunctionIndividual>();

        // Here you can do something with the best individual ("p") found.
        // We simply print its content here, by means of an operator<< implemented
        // in the GFunctionIndividual code.
        std::cout << "Best result found:" << '\n' << p << '\n';
        return 0;
    };
    return runOptimization(argc, argv, {.produce = produce});
}
