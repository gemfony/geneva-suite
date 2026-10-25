/**
 * @file GAlgorithmChaining.cpp
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

// The individual that should be optimized (reused from the Geneva library)
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;

/********************************************************************************
 * This example demonstrates how to CHAIN several optimization algorithms in a
 * single run. Algorithms are appended to the Go2 object with the &-operator;
 * Go2 then runs them in sequence, handing the best individual(s) of one
 * algorithm to the next. A common pattern is a global explorer (evolutionary
 * algorithm) followed by a local refiner (gradient descent).
 ********************************************************************************/
int main(int argc, char **argv) {
    Go2 go(argc, argv, "./config/Go2.json");

    //---------------------------------------------------------------------------
    // Client mode (for networked/multi-threaded execution)
    if(go.clientMode()) {
        return go.clientRun();
    }

    //---------------------------------------------------------------------------
    // Register a content creator so Go2 can populate the algorithms with
    // GFunctionIndividual objects (no problem-specific individual or factory has
    // to be written for this example -- both come from the Geneva library).
    std::shared_ptr<gind::GFunctionIndividualFactory> const gfi_ptr(
        new gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json")
    );
    go.registerContentCreator(gfi_ptr);

    // The genome carries only structure; the configured Gauss adaptor lives on an OA-owned config the
    // factory authors. Register it for the evolutionary algorithm (gradient descent does not adapt).
    {
        auto sample = gfi_ptr->get_as<gind::GFunctionIndividual>();
        go.registerAdaptionConfig("PERSONALITY_EA", gfi_ptr->getAdaptionConfig(*sample));
    }

    //---------------------------------------------------------------------------
    //! [algo-overview-chaining#1]
    // Chain two optimization algorithms with the &-operator: the evolutionary
    // algorithm runs first (global exploration), then a conjugate gradient descent
    // refines its best result (local exploitation). Each algorithm runs its own full
    // iteration budget; the best individuals are passed from one to the next.
    //! [go2-chain#1]
    go & "ea" & "cgd";
    //! [go2-chain#1]
    //! [algo-overview-chaining#1]

    //---------------------------------------------------------------------------
    // Perform the actual (chained) optimization
    //! [algo-overview-chaining#2]
    //! [go2-chain#2]
    auto const p =
        go.optimize()->getBestGlobalIndividual<gind::GFunctionIndividual>();
    //! [algo-overview-chaining#2]
    //! [go2-chain#2]

    std::cout << "Best result found:" << '\n' << p << '\n';
}
