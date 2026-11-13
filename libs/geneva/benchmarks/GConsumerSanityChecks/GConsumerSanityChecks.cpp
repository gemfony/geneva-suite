/**
 * @file GConsumerSanityChecks.cpp
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
#include <geneva/Go2.hpp>

// The individual that should be optimized
#include "geneva/individuals/GDelayIndividual.hpp"
#include "geneva/oa/GAdaption.hpp"

using namespace Gem::Geneva;
namespace oa = Gem::Geneva::OptimizationAlgorithms;
namespace po = boost::program_options;

/******************************************************************************/
/**
 * The main function
 */
int main(int argc, char **argv) {
    //---------------------------------------------------------------------------
    // We want to add additional command line options

    std::string monitorTimings = "empty";

    // Assemble command line options
    boost::program_options::options_description user_options;
    user_options.add_options()(
        "monitorTimings",
        po::value<std::string>(&monitorTimings)
            ->implicit_value(std::string("timingsLog"))
            ->default_value("empty"),
        "Logs the times for all processing steps"
    );

    Go2 go(argc, argv, "./config/Go2.json", user_options);

    //---------------------------------------------------------------------------
    // Client mode
    if(go.clientMode()) {
        return go.clientRun();
    } // Execution will end here in client mode

    //---------------------------------------------------------------------------
    // Read the delay-individual configuration (the individual owns its own config
    // parsing and construction; there is no bespoke factory anymore).
    auto delay_config = gind::GDelayIndividual::readConfig("./config/GDelayIndividual.json");
    auto sleep_times = gind::GDelayIndividual::parseSleepTimes(delay_config);

    //---------------------------------------------------------------------------
    // Register pluggable optimization monitors, if requested by the user
    // See example 12 for more monitors

    if(monitorTimings != "empty") {
        std::shared_ptr<GProcessingTimesLogger> const processingTimesLogger_ptr(
            new GProcessingTimesLogger(
                "hist_" + monitorTimings + ".C",
                "hist2D_" + monitorTimings + ".C",
                monitorTimings + ".txt",
                100 // nBins in x-direction
                ,
                100 // nBins in y-direction
            )
        );
        go.registerPluggableOM(processingTimesLogger_ptr);
    }

    //---------------------------------------------------------------------------

    // Add a content creator so Go2 can generate its own individuals, if necessary
    auto firstInd = gind::GDelayIndividual::create(
        delay_config,
        gind::GDelayIndividual::tupleToTime(sleep_times.at(0))
    );
    go.push_back(firstInd);

    // GDelay's genome is transport ballast (its VALUES are irrelevant to the timing benchmark), but the EA
    // still mutates + re-evaluates it every generation -- that mutate/evaluate cycle IS the workload being
    // measured. So author a real Gauss adaptor on its OA-owned config and register it for the EA.
    {
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*firstInd);
        for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
            cfg->groupDouble(i).gauss(0.025, 0.1, 0., 1., 1.);
        }
        go.registerAdaptionConfig("PERSONALITY_EA", cfg);
    }

    // Add a default optimization algorithm to the Go2 object. This is optional.
    // Indeed "ea" is the default setting anyway. However, if you do not like it, you
    // can register another default algorithm here, which will then be used, unless
    // you specify other algorithms on the command line. You can also add a smart
    // pointer to an optimization algorithm here instead of its mnemonic.
    go.registerDefaultAlgorithm("ea");

    // Perform the actual optimization
    auto const p =
        go.optimize()->getBestGlobalIndividual<gind::GDelayIndividual>();
}
