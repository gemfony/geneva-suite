/**
 * @file GRandomWalk.cpp
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
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// Boost header files go here

// Geneva header files go here
#include "dietrich/GPlotDesigner.hpp"
#include "geneva/individuals/GTestIndividual2.hpp"
#include "geneva/oa/GAdaption.hpp"

using namespace Gem::Common;
using namespace Gem::Dietrich; // plotting types live here now
using namespace Gem::Geneva;
using namespace Gem::Geneva::Individuals;

const std::size_t NPOINTS = 1000;

// NOLINTNEXTLINE(readability-function-size) -- main() of a manual plotting demo: sets up one GPlotDesigner canvas and its per-parameter-object graphs, then runs the shared random-walk/adaption loop for each; splitting would scatter one linear demo script
int main(int argc, char **argv) {
    std::string const caption = "Random walks by adaption of different FP-based parameter objects";
    GPlotDesigner gpd(caption, 2, 3);

    std::shared_ptr<GGraph2D> const gdo_adapt_ptr(new GGraph2D());
    gdo_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gdo_adapt_ptr->setPlotLabel("GDoubleObject");
    gdo_adapt_ptr->setXAxisLabel("x");
    gdo_adapt_ptr->setYAxisLabel("y");
    // gdo_adapt_ptr->setDrawArrows();

    std::shared_ptr<GGraph2D> const gcdo_adapt_ptr(new GGraph2D());
    gcdo_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gcdo_adapt_ptr->setPlotLabel("GConstrainedDoubleObject");
    gcdo_adapt_ptr->setXAxisLabel("x");
    gcdo_adapt_ptr->setYAxisLabel("y");
    // gcdo_adapt_ptr->setDrawArrows();

    std::shared_ptr<GGraph2D> const gcdoc_adapt_ptr(new GGraph2D());
    gcdoc_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gcdoc_adapt_ptr->setPlotLabel("GConstrainedDoubleObjectCollection");
    gcdoc_adapt_ptr->setXAxisLabel("x");
    gcdoc_adapt_ptr->setYAxisLabel("y");
    // gcdoc_adapt_ptr->setDrawArrows();

    std::shared_ptr<GGraph2D> const gdc_adapt_ptr(new GGraph2D());
    gdc_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gdc_adapt_ptr->setPlotLabel("GDoubleCollection");
    gdc_adapt_ptr->setXAxisLabel("x");
    gdc_adapt_ptr->setYAxisLabel("y");
    // gdc_adapt_ptr->setDrawArrows();

    std::shared_ptr<GGraph2D> const gcdc_adapt_ptr(new GGraph2D());
    gcdc_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gcdc_adapt_ptr->setPlotLabel("GConstrainedDoubleCollection");
    gcdc_adapt_ptr->setXAxisLabel("x");
    gcdc_adapt_ptr->setYAxisLabel("y");
    // gcdc_adapt_ptr->setDrawArrows();

    for(std::size_t o = 0; o < NPERFOBJECTTYPES; o++) {
        // Create a GTestIndividual2 object of size 2
        std::shared_ptr<GTestIndividual2> const gti_ptr(new GTestIndividual2(2, PERFOBJECTTYPE(o)));

        // One adapter held across the walk, so the self-adapting sigma persists between steps (the
        // adaption state + logic are OA-owned; a standalone individual drives them via a
        // self-owned scratch + the config the individual authors -- its genome carries only structure).
        Gem::Geneva::OptimizationAlgorithms::StandaloneAdapter adapter(*gti_ptr, gti_ptr->getAdaptionConfig());

        std::vector<double> par;
        for(std::size_t i = 0; i < NPOINTS; i++) {
            gti_ptr->streamline(par);

            switch(o) {
            case 0:
                (*gdo_adapt_ptr) & std::tuple<double, double>(par[0], par[1]);
                break;

            case 1:
                (*gcdo_adapt_ptr) & std::tuple<double, double>(par[0], par[1]);
                break;

            case 2:
                (*gcdoc_adapt_ptr) & std::tuple<double, double>(par[0], par[1]);
                break;

            case 3:
                (*gdc_adapt_ptr) & std::tuple<double, double>(par[0], par[1]);
                break;

            case 4:
                (*gcdc_adapt_ptr) & std::tuple<double, double>(par[0], par[1]);
                break;

            default:
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "Error in main(): Incorrect object type requested: " << o << '\n'
                );
                break;
            }

            adapter.adapt(*gti_ptr);
        }
    }

    gpd.registerPlotter(gdo_adapt_ptr);
    gpd.registerPlotter(gcdo_adapt_ptr);
    gpd.registerPlotter(gcdoc_adapt_ptr);
    gpd.registerPlotter(gdc_adapt_ptr);
    gpd.registerPlotter(gcdc_adapt_ptr);

    // Emit the result file
    gpd.writeToFile("result.C");
}
