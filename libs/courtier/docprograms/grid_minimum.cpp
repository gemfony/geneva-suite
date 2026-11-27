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
 * A program without Geneva on courtier's launcher: it looks for the smallest value of a function
 * on a grid, cut into slices (GGridSlice.hpp) that a consumer distributes. The program says what
 * it produces; the command line says where it is processed:
 *
 *   courtier_grid_minimum                                        # local threads (stc)
 *   courtier_grid_minimum --consumer beast                       # websocket server ...
 *   courtier_grid_minimum --consumer beast --client              # ... and its clients
 *   courtier_grid_minimum --consumer mqtt [--client]             # the same over an MQTT broker
 *   mpirun -np 3 courtier_grid_minimum --consumer mpi            # rank 0 submits, 1 and 2 serve
 *
 * Each consumer reaches the program through a module of its own, built for GGridSlice and lying
 * next to the program (grid_minimum_<consumer>.cpp): no consumer is built into it, stc included.
 */

// Standard header files go here
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

// Geneva header files go here
#include "GGridSlice.hpp"
#include "common/GCommandLineParser.hpp"
#include "common/GFatalReport.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "courtier/GLauncherT.hpp"
#include "courtier/GSubmissionPolicy.hpp"

int main(int argc, char **argv) {
    Gem::Common::installFatalReport(); // an error nobody handles: its message, exit code 1

    //! [courtier-launcher-program]
    // One launcher for the program's one item type; its consumer modules lie beside it
    Gem::Courtier::GLauncherT<GGridSlice> launcher(argc, argv);
    launcher.setConsumerModules(
        Gem::Courtier::GConsumerModules::besideProgram("libcourtier_grid_minimum_")
    );

    // The program's own options, in the same command line and help text
    std::size_t n_slices = 40;
    std::size_t n_points = 2'500;
    auto own_options = [&](Gem::Common::GParserBuilder &gpb) {
        gpb.registerCLParameter<std::size_t>("slices", n_slices, 40, "Slices");
        gpb.registerCLParameter<std::size_t>("points", n_points, 2'500, "Points");
    };

    // The submitter only: a client, or an MPI rank other than 0, never runs it
    auto produce = [&] {
        std::vector<std::unique_ptr<GGridSlice>> batch;
        const double width = 10. / static_cast<double>(n_slices); // [-5, 5]
        for(std::size_t s = 0; s < n_slices; ++s) {
            const double lo = -5. + width * static_cast<double>(s);
            batch.push_back(std::make_unique<GGridSlice>(lo, lo + width, n_points));
        }
        auto &registry = Gem::Courtier::GConsumerRegistryT<GGridSlice>::instance();
        registry.consumer()->processBatch(
            std::span(batch),
            Gem::Courtier::GSubmissionPolicy::full_success_or_fatal()
        );
        const GGridSlice *best = batch.front().get();
        for(const auto &slice : batch) {
            if(slice->fMin() < best->fMin()) {
                best = slice.get();
            }
        }
        std::cout << "grid_minimum: smallest value " << best->fMin();
        std::cout << " at x = " << best->xMin() << '\n';
        return std::abs(best->xMin() - 1.25) < 1e-2 ? 0 : 1;
    };

    return launcher.run({.userOptions = own_options, .produce = produce});
    //! [courtier-launcher-program]
}
