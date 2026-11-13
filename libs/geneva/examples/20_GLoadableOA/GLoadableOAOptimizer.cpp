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

/**
 * @file
 * Example 20 -- a generic optimizer that runs a RUNTIME-LOADED optimization algorithm.
 *
 * The algorithm is NOT compiled in: it is contributed by a shared object loaded with --module, then selected
 * by its mnemonic. The optimization individual (the parabola, a GFunctionIndividual) IS compiled in. Run:
 *
 *   ./GLoadableOAOptimizer --module ./libGRandomSearch.so --optimizationAlgorithms rsearch
 *
 * Go2 loads the module during construction (before the mnemonics are resolved), so "rsearch" resolves to the
 * loaded GRandomSearch exactly as a built-in algorithm would. This is the OA analogue of example 18's
 * loadable individual: an optimization algorithm authored, compiled and shipped entirely outside the Geneva
 * library, usable without recompiling anything.
 */

// Standard header files go here
#include <iostream>

// Geneva header files go here
#include "geneva/Go2.hpp"

// The individual to be optimized (compiled in). The algorithm comes from --module.
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;

int main(int argc, char **argv) {
    Go2 go(argc, argv, "./config/Go2.json");

    //---------------------------------------------------------------------------
    // Client mode (for networked consumers)
    if(go.clientMode()) {
        return go.clientRun();
    }

    //---------------------------------------------------------------------------
    // As this is a server, register a signal handler that allows interrupting execution "on the run".
    signal(G_SIGHUP, Gem::Common::sigHupHandler);

    //---------------------------------------------------------------------------
    // Supply the (compiled-in) optimization individual: the standard benchmark-function individual.
    std::shared_ptr<gind::GFunctionIndividualFactory> const gfi_ptr(
        new gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json")
    );

    // Let Go2 generate its own individuals from this factory as needed.
    go.registerContentCreator(gfi_ptr);

    // The adaptor lives on an OA-owned config the factory authors. Register it for the adapting algorithms
    // (EA / SA) so this binary also works with a built-in --optimizationAlgorithms ea/sa. The loaded random
    // search does not adapt, so it simply ignores this.
    {
        auto sample = gfi_ptr->get_as<gind::GFunctionIndividual>();
        auto cfg = gfi_ptr->getAdaptionConfig(*sample);
        go.registerAdaptionConfig("PERSONALITY_EA", cfg);
        go.registerAdaptionConfig("PERSONALITY_SA", cfg);
    }

    //---------------------------------------------------------------------------
    // Run the optimization with the algorithm chosen on the command line (e.g. the loaded "rsearch").
    auto const best =
        go.optimize()->getBestGlobalIndividual<gind::GFunctionIndividual>();

    std::cout << "Best result found:" << '\n' << best << '\n';
}
