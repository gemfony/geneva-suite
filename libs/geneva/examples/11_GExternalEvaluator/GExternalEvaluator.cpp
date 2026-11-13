/**
 * @file GExternalEvaluator.cpp
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
#include "geneva/individuals/GExternalEvaluatorIndividual.hpp"

using namespace Gem::Geneva;

int main(int argc, char **argv) {
    Go2 go(argc, argv, "./config/Go2.json");

    //---------------------------------------------------------------------
    // Client mode
    if(go.clientMode()) {
        return go.clientRun();
    } // Execution will end here in client mode

    //---------------------------------------------------------------------

    // Create a factory for GExternalEvaluatorIndividual objects and perform
    // any necessary initial work.
    std::shared_ptr<gind::GExternalEvaluatorIndividualFactory> const geei_ptr(
        new gind::GExternalEvaluatorIndividualFactory("./config/GExternalEvaluatorIndividual.json")
    );

    // Add a content creator so Go2 can generate its own individuals, if necessary^
    go.registerContentCreator(geei_ptr);

    // The genome carries only structure; the configured Gauss / bi-Gauss adaptor for the active variables
    // lives on an OA-owned config the factory authors (fixed variables stay un-adapted). Register it for
    // the adapting algorithms (EA / SA) so Go2 hands it over before they run.
    //
    // Skipped in --update-configs mode: building the sample constructs an individual whose genome is
    // defined by the external evaluator program (queried at construction), which is irrelevant to a
    // configuration refresh and absent when the configs are materialized. Go2 still refreshes the
    // individual's config via the content creator registered above.
    if(not go.updateConfigsMode()) {
        auto sample = geei_ptr->get_as<gind::GExternalEvaluatorIndividual>();
        auto cfg = geei_ptr->getAdaptionConfig(*sample);
        go.registerAdaptionConfig("PERSONALITY_EA", cfg);
        go.registerAdaptionConfig("PERSONALITY_SA", cfg);
    }

    // Add a default optimization algorithm to the Go2 object
    go.registerDefaultAlgorithm("ea");

    // Perform the actual optimization
    auto const p =
        go.optimize()->getBestGlobalIndividual<gind::GExternalEvaluatorIndividual>();

    // Extract the best individuals found
    auto const bestInds =
        go.getBestGlobalIndividuals<gind::GExternalEvaluatorIndividual>();

    // Note that the "archive" call is specific to the GExternalEvaluatorIndividual (it is now a static
    // helper on the individual, reading the program/run-id off the archived individuals themselves)
    gind::GExternalEvaluatorIndividual::archive(bestInds);

    // The factory will, upon its deletion at the end of this function, call the external evaluator with
    // the --finalize switch (via GExternalEvaluatorIndividual::finalize, the factory's teardown hook)

    //---------------------------------------------------------------------------
}
