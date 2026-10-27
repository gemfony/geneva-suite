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
 * @brief A GENERIC optimizer that has NO optimization problem compiled in.
 *
 * The problem (individual) is supplied at RUNTIME by an individual plugin, selected with
 * @c --individual /path/to/lib<Problem>.so (or the @c individual_plugin_path config-file key). This one
 * binary can therefore optimise ANY loadable problem without recompiling Geneva. Server and client are the
 * same binary launched with different options, so both load the same plugin the same way.
 *
 * Run it, e.g.:
 * @code
 *   ./GGenericOptimizer --individual ./libGLoadableParaboloid.so
 * @endcode
 */

// Standard headers
#include <csignal>
#include <iostream>
#include <ranges>

// Geneva headers
#include "geneva/Go2.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeFactory.hpp"

using namespace Gem::Geneva;

int main(int argc, char **argv) {
    // Go2 parses the config file + command line and, if --individual (or the config key) was given, loads
    // the individual plugin right here -- before any population is built or, on a client, any work item is
    // deserialized. No problem-specific code is compiled into this launcher.
    Go2 go(argc, argv, "./config/Go2.json");

    //---------------------------------------------------------------------------
    // Client mode: just process work handed out by the server (the plugin is already loaded above, so the
    // client can deserialize the incoming individuals).
    if(go.clientMode()) {
        return go.clientRun();
    }
    signal(G_SIGHUP, Gem::Common::sigHupHandler);

    //---------------------------------------------------------------------------
    // The optimization problem came from the loaded plugin (or, if you compiled one in, from
    // registerContentCreator()). Pull its OA-owned adaption config from the factory through the base
    // interface -- the launcher never needs to know the concrete problem type -- and hand it to the
    // adapting algorithms. Skipping this is a hard error for an adapting algorithm (EA / SA).
    if(auto factory = go.getContentCreator()) {
        auto oef = std::dynamic_pointer_cast<Genome::GGenomeFactory>(factory);
        auto sample = (*factory)(); // a sample individual (also parses the factory's config)
        const auto *flat = dynamic_cast<const Genome::GGenome *>(sample.get());
        if(oef && (flat != nullptr)) {
            if(auto cfg = oef->getAdaptionConfig(*flat)) {
                go.registerAdaptionConfig("PERSONALITY_EA", cfg);
                go.registerAdaptionConfig("PERSONALITY_SA", cfg);
            }
        }
    }
    else {
        std::cerr << "No optimization problem is available. Load one with --individual <path>.so"
                  << " (or the individual_plugin_path config-file key)." << '\n';
        return 1;
    }

    // "ea" is the default algorithm anyway; state it explicitly for clarity.
    go.registerDefaultAlgorithm("ea");

    //---------------------------------------------------------------------------
    // Optimize and report. The best individual is read back through the flat-genome base, so this launcher
    // stays problem-agnostic.
    auto best = go.optimize()->getBestGlobalIndividual<Genome::GGenome>();
    const auto [raw, transformed] = best->getFitnessTuple();

    std::vector<double> v;
    best->streamline<double>(v);

    std::cout << "Best result found (raw fitness = " << raw << "):" << '\n';
    // A multi-criterion problem stores one raw result per evaluation criterion; the single "raw fitness"
    // above is only the main one. Surface every criterion so the multi-objective result is visible (for a
    // Pareto-sorted run the reported individual is one member of the non-dominated front).
    if(const std::size_t n_criteria = best->getNStoredResults(); n_criteria > 1) {
        for(std::size_t c = 0; c < n_criteria; ++c) {
            std::cout << "  raw fitness[" << c << "] = " << best->raw_fitness(c) << '\n';
        }
    }
    for(auto const& [i, x] : std::views::enumerate(v)) {
        std::cout << "  x[" << i << "] = " << x << '\n';
    }
    return 0;
}
