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
#include "geneva/Go2.hpp"

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;

int main(int argc, char **argv) {
    Go2 go(argc, argv, "./config/Go2.json");

    //---------------------------------------------------------------------------
    // Client mode
    if(go.clientMode()) {
        return go.clientRun();
    } // Execution will end here in client mode

    //---------------------------------------------------------------------------
    // As we are dealing with a server, register a signal handler that allows us
    // to interrupt execution "on the run"
    //! [go2-sighup]
    signal(G_SIGHUP, Gem::Common::sigHupHandler);
    //! [go2-sighup]

    //---------------------------------------------------------------------------
    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    std::shared_ptr<gind::GFunctionIndividualFactory> const gfi_ptr(
        new gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json")
    );

    // Add a content creator so Go2 can generate its own individuals, if necessary
    go.registerContentCreator(gfi_ptr);

    // The genome carries only structure; the configured adaptor lives on an OA-owned config the factory
    // authors. Register it for the adapting algorithms (EA / SA) so Go2 hands it over before they run.
    {
        //! [adaptors-go2-register]
        auto sample = gfi_ptr->get_as<gind::GFunctionIndividual>();
        auto cfg = gfi_ptr->getAdaptionConfig(*sample);
        go.registerAdaptionConfig("PERSONALITY_EA", cfg);
        go.registerAdaptionConfig("PERSONALITY_SA", cfg);
        //! [adaptors-go2-register]
    }

    // Add a default optimization algorithm to the Go2 object. This is optional.
    // Indeed "ea" is the default setting anyway. However, if you do not like it, you
    // can register another default algorithm here, which will then be used, unless
    // you specify other algorithms on the command line. You can also add a smart
    // pointer to an optimization algorithm here instead of its mnemonic.
    go.registerDefaultAlgorithm("ea");

    // Perform the actual optimization
    auto const p =
        go.optimize()->getBestGlobalIndividual<gind::GFunctionIndividual>();

    // Here you can do something with the best individual ("p") found.
    // We simply print its content here, by means of an operator<< implemented
    // in the GFunctionIndividual code.
    std::cout << "Best result found:" << '\n' << p << '\n';
}
