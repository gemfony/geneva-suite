/**
 * @file GMetaOptimizer.cpp
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
#include "geneva/individuals/GMetaOptimizerIndividualT.hpp"
// The single facility for meta-optimization: it runs the umbrella-individuals on its own orchestration
// thread pool, while their sub-optimizations submit to the one process-wide work consumer.
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GMetaEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GOptimizationAlgorithmFactoryT.hpp"

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
    signal(G_SIGHUP, Gem::Common::sigHupHandler);

    //---------------------------------------------------------------------------
    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    std::shared_ptr<gind::GFunctionIndividualFactory> const gfi_ptr(
        new gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json")
    );

    // Create a factory for GMetaOptimizerIndividual objects and perform
    // any necessary initial work.
    std::shared_ptr<gind::GMetaOptimizerIndividualFactoryT<gind::GFunctionIndividual>> const gmoi_ptr(
        new gind::GMetaOptimizerIndividualFactoryT<gind::GFunctionIndividual>(
            "./config/GMetaOptimizerIndividual.json"
        )
    );

    // Add a pluggable optimization monitor (targeted at evolutionary algorithms) and register
    // it with Go2.
    go.registerPluggableOM(
        std::shared_ptr<gind::GOptOptMonitorT<gind::GFunctionIndividual>>(
            new gind::GOptOptMonitorT<gind::GFunctionIndividual>("./optProgress.C")
        )
    );

    //! [meta.example-main#1]
    // Register the GFunctionIndividualFactory with the meta-optimizer,
    // so it can be handed to the meta-optimization individuals later
    gmoi_ptr->registerIndividualFactory(gfi_ptr);

    // Add a content creator so Go2 can generate its own individuals, if necessary
    go.registerContentCreator(gmoi_ptr);
    //! [meta.example-main#1]

    // --update-configs: the meta-optimizer's sub-problem configs are read only when a sub-optimization
    // actually runs (not during a config refresh), so materialize them directly here -- the sub-problem
    // individual config and the (sub-)evolutionary-algorithm config the umbrella individuals run.
    if(go.updateConfigsMode()) {
        gfi_ptr->get();
        oa::GOptimizationAlgorithmFactoryT<oa::GEvolutionaryAlgorithm>(
            "./config/GSubEvolutionaryAlgorithm.json")
            .get<oa::GOptimizationAlgorithmBase>();
    }

    //! [meta.example-main#2]
    // The meta genome carries only structure; its adaptors (n_parents flip, n_children integer-Gauss,
    // doubles Gauss) live on an OA-owned config the meta individual authors. Register it for the outer EA.
    {
        auto sample = gmoi_ptr->get_as<gind::GMetaOptimizerIndividualT<gind::GFunctionIndividual>>();
        go.registerAdaptionConfig("PERSONALITY_EA", sample->getAdaptionConfig());
    }
    //! [meta.example-main#2]

    // Drive the meta-optimization with the dedicated meta-EA: it evaluates the umbrella-individuals on
    // its own orchestration pool, so each umbrella's sub-EA can submit to the one work consumer without
    // the meta level competing for it (a plain EA here would deadlock once the sub-EA shares that
    // consumer). It is a standard EA otherwise (PERSONALITY_EA), so the registered adaption config and
    // the EA-targeted monitor apply unchanged.
    //! [meta.example-main#3]
    go.registerDefaultAlgorithm(std::make_shared<oa::GMetaEvolutionaryAlgorithm>());

    // Perform the actual optimization
    auto const bestIndividual_ptr =
        go.optimize()->getBestGlobalIndividual<gind::GMetaOptimizerIndividualT<gind::GFunctionIndividual>>();
    //! [meta.example-main#3]

    // Do something with the best result. Here we simply print the result to std-out.
    std::cout << "Best Result was:" << '\n' << *bestIndividual_ptr << '\n';

    //---------------------------------------------------------------------------
}
