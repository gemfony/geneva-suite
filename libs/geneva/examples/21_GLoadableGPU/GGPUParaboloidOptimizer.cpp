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
 * Example 21 -- a generic optimizer that runs a RUNTIME-LOADED GPU problem. The optimization individual AND
 * its device marshaller are contributed by ONE shared object loaded with --module; nothing problem-specific
 * is compiled into this launcher. Selecting the GPU consumer then evaluates the whole thing on the device:
 *
 *   ./GGPUParaboloidOptimizer --module ./libGGPUParaboloid.so --consumer gpu
 *
 * Go2 loads the module during construction: the individual claims the single content-creator slot and the
 * marshaller registers under the "cuda" device target, so `--consumer gpu` builds the generic GPU consumer
 * around the loaded marshaller (whose kernel, kernels/paraboloid.cu, is NVRTC-compiled at run time). The
 * same binary also runs on the CPU with `--consumer stc`, using the individual's own evaluate(). This is the
 * GPU North Star of the loadable-module scheme (the OA analogue is example 20, the individual analogue 19).
 */

// Standard headers
#include <csignal>
#include <iostream>
#include <ranges>
#include <vector>

// Geneva headers
#include "courtier/gpu/GGPUConsumerConfig.hpp" // materialize the GPU config under --update-configs
#include "geneva/Go2.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeFactory.hpp"

using namespace Gem::Geneva;

int main(int argc, char **argv) {
    // Go2 parses config + command line and loads the --module here, in its constructor: the module
    // contributes BOTH the individual (claimed as the optimization problem) AND its GPU marshaller
    // (registered under the "cuda" device target), so `--consumer gpu` can build the GPU consumer around it.
    Go2 go(argc, argv, "./config/Go2.json");

    // --update-configs: Go2 forces the local thread-pool consumer for a config refresh, so the GPU consumer
    // -- and hence GGPUConsumer.json -- would otherwise be skipped. Materialize it directly (created with
    // defaults if absent; the example's config-overrides.json then sets the kernel path). No device is
    // touched. go.optimize() below then refreshes Go2's own and the loaded individual's configs, and exits.
    if(go.updateConfigsMode()) {
        Gem::Courtier::GPU::GGPUConsumerConfig gpu_cfg;
        gpu_cfg.load("./config/GGPUConsumer.json");
        go.optimize();
    }

    // Client mode (networked consumers): the module is already loaded, so a client can deserialize work.
    if(go.clientMode()) {
        return go.clientRun();
    }
    signal(G_SIGHUP, Gem::Common::sigHupHandler);

    // Pull the loaded problem's OA-owned adaption config from its factory through the base interface (the
    // launcher never needs the concrete type) and hand it to the adapting algorithms (EA / SA). Skipping this
    // is a hard error for an adapting algorithm.
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
        std::cerr << "No optimization problem is available. Load one with --module <path>.so"
                  << " (contributing an individual)." << '\n';
        return 1;
    }

    // "ea" is the default anyway; state it explicitly for clarity.
    go.registerDefaultAlgorithm("ea");

    // Optimize and report. The best individual is read back through the flat-genome base, so this launcher
    // stays problem-agnostic.
    auto best = go.optimize()->getBestGlobalIndividual<Genome::GGenome>();
    const auto [raw, transformed] = best->getFitnessTuple();

    std::vector<double> v;
    best->streamline<double>(v);

    std::cout << "Best result found (raw fitness = " << raw << "):" << '\n';
    for(auto const &[i, x] : std::views::enumerate(v)) {
        std::cout << "  x[" << i << "] = " << x << '\n';
    }
    return 0;
}
