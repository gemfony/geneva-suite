/**
 * @file GSerializationOverhead.cpp
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

// The number of consecutive measurements
const std::size_t NMEASUREMENTS = 100;

// The maximum object size
const std::size_t MAXOBJECTSIZE = 100;

// The step size
const std::size_t STEPSIZE = 10;

// The default serialization mode
const Gem::Common::serializationMode DEFAULTSERMODE = Gem::Common::serializationMode::GEM_BINARY;
// const Gem::Common::serializationMode DEFAULTSERMODE = Gem::Common::serializationMode::GEM_JSON;

using namespace Gem::Common;
using namespace Gem::Dietrich; // plotting types live here now
using namespace Gem::Geneva;
using namespace Gem::Geneva::Individuals;

// NOLINTNEXTLINE(readability-function-size) -- single benchmark main measuring adaption and serialization timings for one individual across the configured measurement sweep
int main(int argc, char **argv) {
    std::string const caption =
        "Times for adaption and serialization (" + Gem::Common::to_string(NMEASUREMENTS) +
        " measurements each; serialization in " +
        std::string(serializationModeToString(DEFAULTSERMODE)) + ")";
    GPlotDesigner gpd(caption, 2, NPERFOBJECTTYPES);

    std::shared_ptr<GGraph2D> const gdo_adapt_ptr(new GGraph2D());
    gdo_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gdo_adapt_ptr->setPlotLabel("GDoubleObject / Adaption");
    gdo_adapt_ptr->setXAxisLabel("Number of parameters");
    gdo_adapt_ptr->setYAxisLabel("Time (s)");

    std::shared_ptr<GGraph2D> const gdo_ser_ptr(new GGraph2D());
    gdo_ser_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gdo_ser_ptr->setPlotLabel("GDoubleObject / Serialization");
    gdo_ser_ptr->setXAxisLabel("Number of parameters");
    gdo_ser_ptr->setYAxisLabel("Time (s)");

    std::shared_ptr<GGraph2D> const gcdo_adapt_ptr(new GGraph2D());
    gcdo_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gcdo_adapt_ptr->setPlotLabel("GConstrainedDoubleObject / Adaption");
    gcdo_adapt_ptr->setXAxisLabel("Number of parameters");
    gcdo_adapt_ptr->setYAxisLabel("Time (s)");

    std::shared_ptr<GGraph2D> const gcdo_ser_ptr(new GGraph2D());
    gcdo_ser_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gcdo_ser_ptr->setPlotLabel("GConstrainedDoubleObject / Serialization");
    gcdo_ser_ptr->setXAxisLabel("Number of parameters");
    gcdo_ser_ptr->setYAxisLabel("Time (s)");

    std::shared_ptr<GGraph2D> const gcdoc_adapt_ptr(new GGraph2D());
    gcdoc_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gcdoc_adapt_ptr->setPlotLabel("GConstrainedDoubleObjectCollection / Adaption");
    gcdoc_adapt_ptr->setXAxisLabel("Number of parameters");
    gcdoc_adapt_ptr->setYAxisLabel("Time (s)");

    std::shared_ptr<GGraph2D> const gcdoc_ser_ptr(new GGraph2D());
    gcdoc_ser_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gcdoc_ser_ptr->setPlotLabel("GConstrainedDoubleObjectCollection / Serialization");
    gcdoc_ser_ptr->setXAxisLabel("Number of parameters");
    gcdoc_ser_ptr->setYAxisLabel("Time (s)");

    std::shared_ptr<GGraph2D> const gdc_adapt_ptr(new GGraph2D());
    gdc_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gdc_adapt_ptr->setPlotLabel("GDoubleCollection / Adaption");
    gdc_adapt_ptr->setXAxisLabel("Number of parameters");
    gdc_adapt_ptr->setYAxisLabel("Time (s)");

    std::shared_ptr<GGraph2D> const gdc_ser_ptr(new GGraph2D());
    gdc_ser_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gdc_ser_ptr->setPlotLabel("GDoubleCollection / Serialization");
    gdc_ser_ptr->setXAxisLabel("Number of parameters");
    gdc_ser_ptr->setYAxisLabel("Time (s)");

    std::shared_ptr<GGraph2D> const gcdc_adapt_ptr(new GGraph2D());
    gcdc_adapt_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gcdc_adapt_ptr->setPlotLabel("GConstrainedDoubleCollection / Adaption");
    gcdc_adapt_ptr->setXAxisLabel("Number of parameters");
    gcdc_adapt_ptr->setYAxisLabel("Time (s)");

    std::shared_ptr<GGraph2D> const gcdc_ser_ptr(new GGraph2D());
    gcdc_ser_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::CURVE);
    gcdc_ser_ptr->setPlotLabel("GConstrainedDoubleCollection / Serialization");
    gcdc_ser_ptr->setXAxisLabel("Number of parameters");
    gcdc_ser_ptr->setYAxisLabel("Time (s)");

    for(std::size_t s = 1; s <= MAXOBJECTSIZE; s += (s < 10 ? 1 : STEPSIZE)) {
        std::cout << "Starting measurement for object size " << s << std::endl;

        for(std::size_t o = 0; o < NPERFOBJECTTYPES; o++) {
            // Create a GTestIndividual2 object of the desired size
            std::shared_ptr<GTestIndividual2> const gti_ptr(new GTestIndividual2(s, PERFOBJECTTYPE(o)));

            // One adapter held across the measurement loop (the adaption state + logic are OA-owned;
            // a standalone individual drives them via a self-owned scratch + the config the
            // individual authors -- its genome carries only structure).
            OptimizationAlgorithms::StandaloneAdapter adapter(*gti_ptr, gti_ptr->getAdaptionConfig());

            // First test the time needed for NMEASUREMENTS
            // consecutive adaptions
            std::chrono::system_clock::time_point const pre_adapt = std::chrono::system_clock::now();
            for(std::size_t i = 1; i <= NMEASUREMENTS; i++) {
                adapter.adapt(*gti_ptr);
            }
            std::chrono::system_clock::time_point const post_adapt = std::chrono::system_clock::now();

            // Now measure the time needed for NMEASUREMENTS
            // consecutive (de-)serializations in the fastest mode (binary)
            std::chrono::system_clock::time_point const pre_serialization =
                std::chrono::system_clock::now();
            for(std::size_t i = 1; i <= NMEASUREMENTS; i++) {
                gti_ptr->fromString(
                    gti_ptr->toString(DEFAULTSERMODE),
                    DEFAULTSERMODE
                );
            }
            std::chrono::system_clock::time_point const post_serialization =
                std::chrono::system_clock::now();

            std::chrono::duration<double> const adaptionTime = post_adapt - pre_adapt;
            std::chrono::duration<double> const serializationTime =
                post_serialization - pre_serialization;

            double const adaptionTimeD = adaptionTime.count();
            double const serializationTimeD = serializationTime.count();

            switch(o) {
            case 0:
                gdo_adapt_ptr->add(static_cast<double>(s), adaptionTimeD);
                gdo_ser_ptr->add(static_cast<double>(s), serializationTimeD);
                break;

            case 1:
                gcdo_adapt_ptr->add(static_cast<double>(s), adaptionTimeD);
                gcdo_ser_ptr->add(static_cast<double>(s), serializationTimeD);
                break;

            case 2:
                gcdoc_adapt_ptr->add(static_cast<double>(s), adaptionTimeD);
                gcdoc_ser_ptr->add(static_cast<double>(s), serializationTimeD);
                break;

            case 3:
                gdc_adapt_ptr->add(static_cast<double>(s), adaptionTimeD);
                gdc_ser_ptr->add(static_cast<double>(s), serializationTimeD);
                break;

            case 4:
                gcdc_adapt_ptr->add(static_cast<double>(s), adaptionTimeD);
                gcdc_ser_ptr->add(static_cast<double>(s), serializationTimeD);
                break;

            default:
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "Error in main(): Incorrect object type requested: " << o << std::endl
                );
                break;
            }
        }
    }

    gpd.registerPlotter(gdo_adapt_ptr);
    gpd.registerPlotter(gdo_ser_ptr);
    gpd.registerPlotter(gcdo_adapt_ptr);
    gpd.registerPlotter(gcdo_ser_ptr);
    gpd.registerPlotter(gcdoc_adapt_ptr);
    gpd.registerPlotter(gcdoc_ser_ptr);
    gpd.registerPlotter(gdc_adapt_ptr);
    gpd.registerPlotter(gdc_ser_ptr);
    gpd.registerPlotter(gcdc_adapt_ptr);
    gpd.registerPlotter(gcdc_ser_ptr);

    // Emit the result file
    gpd.writeToFile("result.C");
}
