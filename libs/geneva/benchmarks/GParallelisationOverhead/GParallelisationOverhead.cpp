/**
 * @file GParallelisationOverhead.cpp
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
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

// Boost header files go here

// Geneva header files go here
#include "geneva/Go2.hpp"

// The individual that should be optimized
#include "geneva/individuals/GDelayIndividual.hpp"

// The measurement it shares with GSubClientOverhead
#include "GSpeedupMeasurement.hpp"

using namespace Gem::Geneva;

/******************************************************************************/

int main(int argc, char **argv) {
    // The serial reference and the parallel measurement are two runs of this program. An
    // algorithm submits through the one consumer of its process, so a reference taken next to
    // the parallel measurement would run on the same workers and be no reference at all.
    bool reference_run = false;
    std::string reference_file = "referenceLine.txt";
    SpeedupMeasurement::ClientGate gate;
    auto user_options = [&](Gem::Common::GParserBuilder &gpb) {
        gpb.registerCLParameter<bool>(
            "reference",
            reference_run,
            false,
            "Measure the serial reference (on one local worker, whatever consumer is named) and "
            "store its line in --referenceFile, instead of measuring the parallel times",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
        gpb.registerCLParameter<std::string>(
            "referenceFile",
            reference_file,
            reference_file,
            "The file that holds the line of the serial reference"
        );
        SpeedupMeasurement::addClientGateOptions(gpb, gate);
    };

    // The parallel measurement, or the reference
    return runOptimization(
        argc,
        argv,
        {.userOptions = user_options,
         .produce = [&](Go2 &go, const GOptimizationDescription &) -> int {
             //---------------------------------------------------------------------
             // Read the delay-individual configuration for reference measurements ...
             auto delay_config_ref =
                 gind::GDelayIndividual::readConfig("./config/GDelayIndividual-reference.json");
             // ... and for parallel measurements
             auto delay_config_par =
                 gind::GDelayIndividual::readConfig("./config/GDelayIndividual.json");

             // Add default optimization algorithms to the parallel Go2 object
             go.registerDefaultAlgorithm("ea");

             // --update-configs: the delay-individual configs were materialized by the readConfig calls above; now
             // let Go2 refresh the configs it owns and exit, without measuring anything.
             if(Gem::Common::GParserBuilder::updateInPlace()) {
                 go.optimize(); // refreshes Go2's owned configs, then ends the process
                 return 0;      // not reached
             }

             //---------------------------------------------------------------------
             // The reference run measures and stores the line; the parallel run reads it, measures and
             // writes the results.
             const auto to_schedule = [](const gind::GDelayIndividual::Config &c) {
                 SpeedupMeasurement::Schedule schedule{
                     .n_measurements = c.n_measurements,
                     .inter_measurement_delay = c.inter_measurement_delay
                 };
                 for(const auto &delay : gind::GDelayIndividual::parseSleepTimes(c)) {
                     schedule.delays.push_back(gind::GDelayIndividual::tupleToTime(delay));
                 }
                 return schedule;
             };
             const auto &delay_config = reference_run ? delay_config_ref : delay_config_par;
             SpeedupMeasurement::run(
                 go,
                 reference_run,
                 reference_file,
                 to_schedule(delay_config_ref),
                 to_schedule(delay_config_par),
                 [&delay_config](const std::chrono::duration<double> &delay)
                     -> std::shared_ptr<gen::GGenome> {
                     return gind::GDelayIndividual::create(delay_config, delay);
                 },
                 delay_config_par.result_file,
                 delay_config_par.short_result_file,
                 gate
             );
             return 0;
         }}
    );
}
