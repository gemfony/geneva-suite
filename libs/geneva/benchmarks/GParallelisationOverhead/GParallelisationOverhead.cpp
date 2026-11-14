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
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

// Boost header files go here

// Geneva header files go here
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "dietrich/GPlotDesigner.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/Go2.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GOptimizationAlgorithmFactoryT.hpp"

// The individual that should be optimized
#include "geneva/individuals/GDelayIndividual.hpp"

using namespace Gem::Geneva;
using namespace Gem::Common;
using namespace Gem::Dietrich; // plotting types live here now

/******************************************************************************/
/// The slope of the reference line must reach this fraction of the number of evaluations of a
/// run; below it the reference is rejected as not serial.
constexpr double MINSERIALSLOPEFRACTION = 0.8;

/******************************************************************************/
/**
 * Stores the line of the serial reference, f(x) = a + b*x with the errors of a and b, in a file
 * for the run that measures the parallel times.
 *
 * @param file_name The file to write
 * @param ab The parameters a, error of a, b, error of b
 */
void writeReferenceLine(
    const std::string &file_name,
    const std::tuple<double, double, double, double> &ab
) {
    std::ofstream out(file_name);
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << std::get<0>(ab) << ' '
        << std::get<1>(ab) << ' ' << std::get<2>(ab) << ' ' << std::get<3>(ab) << '\n';
    if(not out) {
        throw std::runtime_error("GParallelisationOverhead: could not write " + file_name);
    }
}

/**
 * Reads the line of the serial reference that an earlier run with --reference has stored.
 *
 * @param file_name The file to read
 * @return The parameters a, error of a, b, error of b
 */
std::tuple<double, double, double, double> readReferenceLine(const std::string &file_name) {
    std::ifstream in(file_name);
    double a = 0.;
    double a_err = 0.;
    double b = 0.;
    double b_err = 0.;
    if(not(in >> a >> a_err >> b >> b_err)) {
        throw std::runtime_error(
            "GParallelisationOverhead: no serial reference in \"" + file_name +
            "\". Run the program once with --reference first; it measures the reference on one "
            "local worker and stores it there."
        );
    }
    return {a, a_err, b, b_err};
}

/******************************************************************************/
/**
 * Starts a series of reference measurements to be compared with the parallel
 * measurements. The reference is serial: an algorithm submits through the one
 * consumer of its process, so this function registers the local thread consumer
 * with a single worker as that consumer. It therefore runs in a process of its
 * own (see main()). At the end it checks that the evaluations did run one
 * after another.
 *
 * @param delay_config The parsed delay-individual configuration
 * @param ab The parameters a and b of the line best describing all measurements, so that f(x)=a+b*x
 */
void startReferenceMeasurement(
    const gind::GDelayIndividual::Config &delay_config,
    std::tuple<double, double, double, double> &ab
) {
    std::cout << "Starting reference measurement" << std::endl;

    std::vector<std::tuple<double, double>> referenceExecutionTimes;

    // Create a factory for serial EA algorithms
    oa::GOptimizationAlgorithmFactoryT<oa::GEvolutionaryAlgorithm>
        ea_serial_factory("./config/GEvolutionaryAlgorithm.json");

    // Create an evolutionary algorithm
    auto ea_alg_master = ea_serial_factory.get<oa::GEvolutionaryAlgorithm>();

    // The algorithm submits through the process-wide consumer. Make that the serial one, whatever
    // was asked for on the command line: the reference is the time one worker needs.
    const auto serial_consumer = Gem::Geneva::buildConsumerSetup(
        Gem::Geneva::ConsumerSpec{.mnemonic = "stc", .n_threads = 1}
    );

    //---------------------------------------------------------------------
    // Cycle through the configured delays, building a fresh delay individual for each one.
    const std::uint32_t interMeasurementDelay = delay_config.inter_measurement_delay;
    const std::uint32_t nMeasurementsPerIteration = delay_config.n_measurements;
    const auto sleep_times = gind::GDelayIndividual::parseSleepTimes(delay_config);
    std::size_t iter = 0;
    for(const auto &sleep_time_tuple : sleep_times) {
        auto gdi_ptr = gind::GDelayIndividual::create(
            delay_config,
            gind::GDelayIndividual::tupleToTime(sleep_time_tuple)
        );

        for(std::uint32_t i = 0; i < nMeasurementsPerIteration; i++) {
            std::cout << "Serial measurement " << i << " in iteration " << iter << std::endl;

            // Create a clone of the evolutionary algorithm
            auto ea_alg = ea_alg_master->clone<oa::GEvolutionaryAlgorithm>();

            // Make the individual known to the optimizer
            ea_alg->push_back(gdi_ptr->clone());

            // GDelay's genome is transport ballast (its values are irrelevant to the timing benchmark),
            // but the EA still mutates + re-evaluates it every generation -- that cycle IS the measured
            // workload. Author a real Gauss adaptor on its OA-owned config and hand it to the EA.
            {
                auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*gdi_ptr);
                for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
                    cfg->groupDouble(i).gauss(0.025, 0.1, 0., 1., 1.);
                }
                ea_alg->setAdaptionConfig(cfg);
            }

            // Do the actual optimization and measure the time
            std::chrono::system_clock::time_point const startTime = std::chrono::system_clock::now();
            ea_alg->optimize();
            std::chrono::system_clock::time_point const endTime = std::chrono::system_clock::now();
            std::chrono::duration<double> const duration = endTime - startTime;

            referenceExecutionTimes.push_back(
                std::tuple<double, double>(gdi_ptr->getFixedSleepTime().count(), duration.count())
            );
        }

        // Wait for late arrivals
        std::this_thread::sleep_for(std::chrono::seconds(interMeasurementDelay));

        // Increment the iteration counter
        iter++;
    }

    // Calculate the regression parameters a and b, including errors
    ab = getRegressionParameters(referenceExecutionTimes);

    // A run that evaluates n individuals one after another takes n times the delay (plus a
    // constant), so the slope of the line is the number of evaluations of a run. If the slope is
    // well below that number, the evaluations overlapped and this was no serial reference.
    // The first iteration evaluates the whole population, every later one the children only.
    const double population = static_cast<double>(ea_alg_master->getDefaultPopulationSize());
    const double n_children = static_cast<double>(ea_alg_master->getDefaultNChildren());
    const double n_evaluations =
        population + (static_cast<double>(ea_alg_master->getMaxIteration()) - 1.) * n_children;
    const double slope = std::get<2>(ab);
    std::cout << "Reference: " << slope << " s of run time per second of delay; a serial run has "
              << n_evaluations << " evaluations" << std::endl;
    if(slope < MINSERIALSLOPEFRACTION * n_evaluations) {
        throw std::runtime_error(
            "GParallelisationOverhead: the reference measurement did not run serially (its "
            "evaluations overlapped), so it cannot serve as a reference."
        );
    }

    std::cout << "End of reference measurement" << std::endl;
}

/******************************************************************************/
/**
 * Starts a series of (usually parallel) measurements. The tuples in the return-
 * argument has the following structure:
 * - The sleep-time
 * - The error on the sleep-time (always 0)
 * - The mean value of all measurements of an iteration
 * - The sigma / error of the mean value
 *
 * @param go A reference to the optimization wrapper
 * @param delay_config The parsed delay-individual configuration
 * @param parallelExecutionTimes A vector of parallel execution times, together with errors
 */
void startParallelMeasurement(
    Go2 &go,
    const gind::GDelayIndividual::Config &delay_config,
    std::vector<std::tuple<double, double, double, double>> &parallelExecutionTimes
) {
    std::cout << "Starting parallel measurement" << std::endl;

    //---------------------------------------------------------------------
    // Make sure the output vector is empty
    parallelExecutionTimes.clear();

    //---------------------------------------------------------------------
    // Cycle through the configured delays, building a fresh delay individual for each one.
    const std::uint32_t interMeasurementDelay = delay_config.inter_measurement_delay;
    const std::uint32_t nMeasurementsPerIteration = delay_config.n_measurements;
    const auto sleep_times = gind::GDelayIndividual::parseSleepTimes(delay_config);
    std::size_t iter = 0;
    for(const auto &sleep_time_tuple : sleep_times) {
        auto gdi_ptr = gind::GDelayIndividual::create(
            delay_config,
            gind::GDelayIndividual::tupleToTime(sleep_time_tuple)
        );

        // The parallel half needs the same OA-owned adaption config as the reference half above: the
        // genome carries structure only, so an evolutionary algorithm that is handed none refuses to
        // init(). Authoring the identical Gauss adaptor on both sides is also what makes the two halves
        // comparable at all -- the measured workload is the adapt + re-evaluate cycle.
        {
            auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*gdi_ptr);
            for(std::size_t j = 0; j < cfg->doubleGroups().size(); j++) {
                cfg->groupDouble(j).gauss(0.025, 0.1, 0., 1., 1.);
            }
            go.registerAdaptionConfig("PERSONALITY_EA", cfg);
        }

        // One run that is not measured, before the first delay: it waits until the workers of a
        // networked consumer have connected and have the genome's layout, so that the first
        // measurement does not carry that start-up time.
        if(iter == 0) {
            std::cout << "Warm-up run" << std::endl;
            go.push_back(gdi_ptr);
            go.optimize();
            go.clear();
        }

        std::vector<double> delaySummary;
        for(std::uint32_t i = 0; i < nMeasurementsPerIteration; i++) {
            std::cout << "Parallel measurement " << i << " in iteration " << iter << std::endl;

            // Make the individual known to the optimizer
            go.push_back(gdi_ptr);

            // Do the actual optimization and measure the time. Every run starts at iteration 0:
            // the iteration limit is absolute, so a run that started at the iterations accumulated
            // so far would end after a single iteration and measure almost nothing.
            std::chrono::system_clock::time_point const startTime = std::chrono::system_clock::now();
            go.optimize();
            std::chrono::system_clock::time_point const endTime = std::chrono::system_clock::now();
            std::chrono::duration<double> const duration = endTime - startTime;

            delaySummary.push_back(duration.count());

            // Clean up the collection
            go.clear();
        }

        // Calculate the mean value and standard deviation of all measurements
        std::tuple<double, double> ms = Gem::Common::GStandardDeviation<double>(delaySummary);
        // Output the results
        parallelExecutionTimes.push_back(
            std::tuple<double, double, double, double>(
                gdi_ptr->getFixedSleepTime().count(),
                0. // No error on the sleep time
                ,
                std::get<0>(ms) // mean
                ,
                std::get<1>(ms) // standard deviation
            )
        );

        // Clean up the delay vector
        delaySummary.clear();

        // Wait for late arrivals
        std::this_thread::sleep_for(std::chrono::seconds(interMeasurementDelay));

        // Increment the iteration counter
        iter++;
    }

    std::cout << "End of parallel measurement" << std::endl;
}

/******************************************************************************/
/**
 * Calculate suitable timings including errors for the reference measurement
 */
std::vector<std::tuple<double, double, double, double>> getReferenceTimes(
    const std::tuple<double, double, double, double> &ab,
    const std::vector<std::tuple<double, double, double, double>> &measurementTemplate
) {
    std::vector<std::tuple<double, double, double, double>> referenceExecutionTimes =
        measurementTemplate;

    std::vector<std::tuple<double, double, double, double>>::iterator it;
    for(it = referenceExecutionTimes.begin(); it != referenceExecutionTimes.end(); ++it) {
        double const sleepTime = std::get<0>(*it); // Left unmodified, taken from measurementTemplate

        double const a = std::get<0>(ab);
        double const a_err = std::get<1>(ab);
        double const b = std::get<2>(ab);
        double const b_err = std::get<3>(ab);

        std::get<1>(*it) = 0.;                // No error on the sleep time
        std::get<2>(*it) = a + b * sleepTime; // a line
        std::get<3>(*it) = sqrt(std::pow(a_err, 2.) + std::pow(sleepTime * b_err, 2.));
    }

    return referenceExecutionTimes;
}

/******************************************************************************/

// A failure (no reference to read, a reference that was not serial) ends the program with its
// message and a non-zero status instead of an abort.
int main(int argc, char **argv) try {
    std::vector<std::tuple<double, double, double, double>> parallelExecutionTimes,
        referenceExecutionTimes;

    // The serial reference and the parallel measurement are two runs of this program. An
    // algorithm submits through the one consumer of its process, so a reference taken next to
    // the parallel measurement would run on the same workers and be no reference at all.
    bool reference_run = false;
    std::string reference_file = "referenceLine.txt";
    boost::program_options::options_description user_options;
    user_options.add_options()(
        "reference",
        boost::program_options::value<bool>(&reference_run)->implicit_value(true)->default_value(false),
        "Measure the serial reference (on one local worker, whatever consumer is named) and store "
        "its line in --referenceFile, instead of measuring the parallel times"
    )(
        "referenceFile",
        boost::program_options::value<std::string>(&reference_file)->default_value(reference_file),
        "The file that holds the line of the serial reference"
    );

    // For the parallel measurement
    Go2 go_parallel(argc, argv, "./config/Go2.json", user_options);

    //---------------------------------------------------------------------
    // Client mode
    if(go_parallel.clientMode()) {
        return go_parallel.clientRun();
    }

    //---------------------------------------------------------------------
    // Read the delay-individual configuration for reference measurements ...
    auto delay_config_ref =
        gind::GDelayIndividual::readConfig("./config/GDelayIndividual-reference.json");
    // ... and for parallel measurements
    auto delay_config_par = gind::GDelayIndividual::readConfig("./config/GDelayIndividual.json");

    // Add default optimization algorithms to the parallel Go2 object
    go_parallel.registerDefaultAlgorithm("ea");

    // --update-configs: the delay-individual configs were materialized by the readConfig calls above; now
    // let Go2 refresh the configs it owns and exit, without measuring anything.
    if(go_parallel.updateConfigsMode()) {
        go_parallel.optimize(); // refreshes Go2's owned configs, then exits
        return 0;               // not reached (optimize() std::exit()s in --update-configs mode)
    }

    //---------------------------------------------------------------------
    // The reference run: measure, store the line, done.
    if(reference_run) {
        std::tuple<double, double, double, double> ab;
        startReferenceMeasurement(delay_config_ref, ab);
        writeReferenceLine(reference_file, ab);
        std::cout << "The serial reference is in " << reference_file << std::endl;
        return 0;
    }

    //---------------------------------------------------------------------
    // The parallel run: needs the reference of an earlier run.
    const std::tuple<double, double, double, double> ab = readReferenceLine(reference_file);
    startParallelMeasurement(go_parallel, delay_config_par, parallelExecutionTimes);

    // Calculate reference times from the line parameters
    referenceExecutionTimes = getReferenceTimes(ab, parallelExecutionTimes);

    // Calculate the errors
    std::vector<std::tuple<double, double, double, double>> const ratioWithErrors =
        getRatioErrors(referenceExecutionTimes, parallelExecutionTimes);

    //---------------------------------------------------------------------
    // The plot shows the speed-up only, over the evaluation time: once with a linear and once
    // with a logarithmic speed-up axis. The serial and the parallel run times are straight lines
    // of little interest; they go to the text file.
    const auto gratio_ptr = std::make_shared<GGraph2ED>();
    gratio_ptr->setPlotLabel("Speed-up over serial execution");
    gratio_ptr->setXAxisLabel("Evaluation time [s]");
    gratio_ptr->setYAxisLabel("Speed-up");
    (*gratio_ptr) & ratioWithErrors;

    const auto gratio_log_ptr = std::make_shared<GGraph2ED>();
    gratio_log_ptr->setPlotLabel("Speed-up over serial execution, logarithmic speed-up axis");
    gratio_log_ptr->setXAxisLabel("Evaluation time [s]");
    gratio_log_ptr->setYAxisLabel("Speed-up");
    gratio_log_ptr->setYAxisLogarithmic(true);
    (*gratio_log_ptr) & ratioWithErrors;

    GPlotDesigner gpd("Speed-up as a function of the evaluation time", 1, 2);
    gpd.registerPlotter(gratio_ptr);
    gpd.registerPlotter(gratio_log_ptr);
    gpd.setCanvasDimensions(800, 1000);
    gpd.writeToFile(delay_config_par.result_file);

    //---------------------------------------------------------------------
    // The numbers behind the plot, one line per delay
    std::ofstream table(delay_config_par.short_result_file);
    table << "# evaluation time [s]; serial run time [s] (fitted) and its error; parallel run time "
             "[s] "
             "and its error; speed-up and its error"
          << '\n';
    for(std::size_t i = 0; i < parallelExecutionTimes.size(); i++) {
        table << std::get<0>(parallelExecutionTimes[i]) << '\t'
              << std::get<2>(referenceExecutionTimes[i]) << '\t'
              << std::get<3>(referenceExecutionTimes[i]) << '\t'
              << std::get<2>(parallelExecutionTimes[i]) << '\t'
              << std::get<3>(parallelExecutionTimes[i]) << '\t' << std::get<2>(ratioWithErrors[i])
              << '\t' << std::get<3>(ratioWithErrors[i]) << '\n';
    }

    return 0;
}
catch(const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
}
