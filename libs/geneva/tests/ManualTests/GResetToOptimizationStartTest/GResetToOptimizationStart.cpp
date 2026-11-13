/**
 * @file GResetToOptimizationStart.cpp
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

const std::size_t NRESETS = 3; // The number of resets performed for each algorithm

/**
 * This manual test checks the functionality of the
 * resetToOptimizationStart() function of optimization algorithms.
 * We simply use the Go2 class so we may use its command line
 * parsing ability to retrieve algorithms.
 */
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
    signal(G_SIGHUP, Gem::Common::sigHupHandler);

    //---------------------------------------------------------------------------
    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    std::shared_ptr<gind::GFunctionIndividualFactory> const gfif_ptr(
        new gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json")
    );

    // Check that algorithms were indeed registered and fix, if this was not the case.
    if(go.getNAlgorithms() == 0) {
        glogger << "In GResetToOptimizationStart:" << '\n'
                << "No algorithms were registered." << '\n'
                << "We will add an Evolutionary Algorithm" << '\n'
                << GLOGGING;

        go & "ea";
    }

    // Retrieve the registered algorithms
    auto algorithms_cnt = go.getRegisteredAlgorithms();

    std::cout << "Got algorithms_cnt of size " << algorithms_cnt.size() << '\n';

    for(auto const &alg_ptr : algorithms_cnt) {
        for(std::size_t resetCounter = 0; resetCounter < NRESETS; resetCounter++) {
            auto ind = gfif_ptr->get_as<gind::GFunctionIndividual>();
            alg_ptr->push_back(ind->clone());
            // The genome carries only structure; the adaptor lives on an OA-owned config the factory
            // authors. Hand it to the algorithm (a no-op for non-adapting algorithms; adopted by EA / SA).
            alg_ptr->setAdaptionConfig(gfif_ptr->getAdaptionConfig(*ind));
            alg_ptr->optimize();

            if(resetCounter < NRESETS) {
                alg_ptr->resetToOptimizationStart();
                std::cout << "Algorithm was reset" << '\n';
            }
        }
    }

    std::cout << "Done ..." << '\n';
    return (0);
}
