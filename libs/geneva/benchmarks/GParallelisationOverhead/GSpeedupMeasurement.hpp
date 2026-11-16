/**
 * @file GSpeedupMeasurement.hpp
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

// The speed-up measurement of the benchmarks whose individuals sleep instead of calculating
// (GParallelisationOverhead, GSubClientOverhead): a serial reference, the parallel run times
// through the consumer of the process, and the result table and plot script. The benchmarks
// differ only in the individual they measure, which they hand in as a factory.

#pragma once

// Standard header files go here
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

// Boost header files go here
#include <boost/program_options.hpp>

// Geneva header files go here
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "courtier/consumers/GStdThreadConsumerConfig.hpp"
#include "dietrich/GPlotDesigner.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/Go2.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GOptimizationAlgorithmFactoryT.hpp"

namespace Gem::Geneva::SpeedupMeasurement {

/******************************************************************************/
/// The slope of the reference line must reach this fraction of the number of evaluations of a
/// run; below it the reference is rejected as not serial.
inline constexpr double MINSERIALSLOPEFRACTION = 0.8;

/// The serial reference fits a line with errors through its measurements; that takes at least
/// three of them.
inline constexpr std::size_t MINREFERENCEPOINTS = 3;

/// A line f(x) = a + b*x: a, the error of a, b, the error of b
using Line = std::tuple<double, double, double, double>;

/// Per evaluation time: the evaluation time, its error (0), a run time and its error
using Timings = std::vector<std::tuple<double, double, double, double>>;

/// Builds the individual that is measured, for one evaluation time
using IndividualFactory =
    std::function<std::shared_ptr<gen::GGenome>(const std::chrono::duration<double> &)>;

/******************************************************************************/
/** What is measured: the evaluation times, and how often and with which pause */
struct Schedule {
    std::vector<std::chrono::duration<double>> delays; ///< The evaluation times, one after another
    std::uint32_t n_measurements = 1;                  ///< Measurements per evaluation time
    std::uint32_t inter_measurement_delay = 1;         ///< Seconds to wait after an evaluation time
};

/******************************************************************************/
/** The gate before the measurement: how many clients the server waits for, and how long */
struct ClientGate {
    std::size_t clients = 0;          ///< Clients to wait for; 0: none (nothing is waited for)
    std::size_t limitSeconds = 600;   ///< How long the server waits for them at most
};

/// How often the gate asks the consumer how many clients are connected
inline constexpr std::chrono::milliseconds CLIENTGATEPOLL{100};

/**
 * Adds the options of the gate to a benchmark's options: --waitForClients and --clientWaitLimit.
 *
 * @param options Where they are added
 * @param gate Where the values go
 */
inline void
addClientGateOptions(boost::program_options::options_description &options, ClientGate &gate) {
    options.add_options()(
        "waitForClients",
        boost::program_options::value<std::size_t>(&gate.clients)->default_value(gate.clients),
        "The server waits, before the measurement starts, until its consumer reports this many "
        "connected clients (0: it does not wait). A client counts once it has asked for work on "
        "an open connection; one that has died without its connection ending counts until the "
        "transport notices the loss. A consumer that cannot tell how many clients it has (MPI) "
        "is not waited for; the log says so"
    )(
        "clientWaitLimit",
        boost::program_options::value<std::size_t>(&gate.limitSeconds)
            ->default_value(gate.limitSeconds),
        "Seconds the server waits for the clients of --waitForClients; the program ends with an "
        "error if fewer have connected by then"
    );
}

/**
 * Waits until the consumer of @p go reports at least @p gate.clients connected clients, at most
 * @p gate.limitSeconds, and says how long it waited. The clients need no signal: a connected client asks
 * for work at once and waits, parked by the server, until the first batch is submitted.
 *
 * @param go The optimization wrapper whose consumer is asked (and built, if it is not yet)
 * @param gate How many clients, and how long
 * @throws std::runtime_error if fewer clients have connected within the limit, naming how many
 */
inline void waitForClients(Go2 &go, const ClientGate &gate) {
    if(gate.clients == 0) {
        return;
    }
    const auto consumer = go.consumer();
    if(not consumer) {
        throw std::runtime_error("waitForClients: this process has no consumer to ask");
    }
    const auto start = std::chrono::steady_clock::now();
    std::optional<std::size_t> connected = consumer->connectedClients();
    if(not connected) {
        std::cout << "Waiting for clients: the consumer " << go.getConsumerName()
                  << " cannot tell how many clients are connected; the measurement starts without "
                     "waiting for "
                  << gate.clients << " clients" << std::endl;
        return;
    }
    while(*connected < gate.clients) {
        const auto waited = std::chrono::steady_clock::now() - start;
        if(waited >= std::chrono::seconds(gate.limitSeconds)) {
            throw std::runtime_error(
                "only " + std::to_string(*connected) + " of " + std::to_string(gate.clients) +
                " clients connected within the limit of " + std::to_string(gate.limitSeconds) +
                " s (--clientWaitLimit)"
            );
        }
        std::this_thread::sleep_for(CLIENTGATEPOLL);
        connected = consumer->connectedClients().value_or(0);
    }
    const std::chrono::duration<double> waited = std::chrono::steady_clock::now() - start;
    std::cout << "Waiting for clients: " << *connected << " of " << gate.clients
              << " clients connected after " << std::fixed << std::setprecision(1)
              << waited.count() << " s" << std::defaultfloat << std::endl;
}

/******************************************************************************/
/**
 * Stores the line of the serial reference in a file for the run that measures the parallel times.
 *
 * @param file_name The file to write
 * @param ab The parameters a, error of a, b, error of b
 */
inline void writeReferenceLine(const std::string &file_name, const Line &ab) {
    std::ofstream out(file_name);
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << std::get<0>(ab) << ' '
        << std::get<1>(ab) << ' ' << std::get<2>(ab) << ' ' << std::get<3>(ab) << '\n';
    if(not out) {
        throw std::runtime_error("could not write " + file_name);
    }
}

/**
 * Reads the line of the serial reference that an earlier run with --reference has stored.
 *
 * @param file_name The file to read
 * @return The parameters a, error of a, b, error of b
 */
inline Line readReferenceLine(const std::string &file_name) {
    std::ifstream in(file_name);
    double a = 0.;
    double a_err = 0.;
    double b = 0.;
    double b_err = 0.;
    if(not(in >> a >> a_err >> b >> b_err)) {
        throw std::runtime_error(
            "no serial reference in \"" + file_name +
            "\". Run the program once with --reference first; it measures the reference on one "
            "local worker and stores it there."
        );
    }
    return {a, a_err, b, b_err};
}

/******************************************************************************/
/**
 * The adaption both halves of a measurement use: the genome is transport ballast, but the EA
 * still mutates and re-evaluates it every generation -- that cycle is the measured workload.
 *
 * @param sample An individual of the measurement
 * @return A Gauss adaptor for every double group of the genome
 */
inline std::shared_ptr<oa::GAdaptionConfigBase> ballastAdaption(const gen::GGenome &sample) {
    auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(sample);
    for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
        cfg->groupDouble(i).gauss(0.025, 0.1, 0., 1., 1.);
    }
    return cfg;
}

/******************************************************************************/
/**
 * Measures the serial reference. An algorithm submits through the one consumer of its process,
 * so this registers the local thread consumer with a single worker as that consumer; it therefore
 * runs in a process of its own. At the end it checks that the evaluations did run one after
 * another, and it refuses fewer measurements than a line with errors needs.
 *
 * @param schedule The evaluation times and measurements
 * @param make_individual Builds the individual for one evaluation time
 * @return The line through the run times over the evaluation time
 */
inline Line measureReference(const Schedule &schedule, const IndividualFactory &make_individual) {
    std::cout << "Starting reference measurement" << std::endl;

    std::vector<std::tuple<double, double>> reference_times;
    oa::GOptimizationAlgorithmFactoryT<oa::GEvolutionaryAlgorithm> ea_serial_factory(
        "./config/GEvolutionaryAlgorithm.json"
    );
    auto ea_alg_master = ea_serial_factory.get<oa::GEvolutionaryAlgorithm>();

    // The algorithm submits through the process-wide consumer. Make that the serial one, whatever
    // was asked for on the command line: the reference is the time one worker needs.
    const auto serial_consumer =
        Gem::Geneva::buildConsumer(Gem::Courtier::GStdThreadConsumerConfig{.nThreads = 1});

    std::size_t iter = 0;
    for(const auto &delay : schedule.delays) {
        const auto individual = make_individual(delay);
        for(std::uint32_t i = 0; i < schedule.n_measurements; i++) {
            std::cout << "Serial measurement " << i << " in iteration " << iter << std::endl;
            auto ea_alg = ea_alg_master->clone<oa::GEvolutionaryAlgorithm>();
            ea_alg->push_back(individual->clone());
            ea_alg->setAdaptionConfig(ballastAdaption(*individual));

            const auto start = std::chrono::system_clock::now();
            ea_alg->optimize();
            const std::chrono::duration<double> duration = std::chrono::system_clock::now() - start;
            reference_times.emplace_back(delay.count(), duration.count());
        }
        // Wait for late arrivals
        std::this_thread::sleep_for(std::chrono::seconds(schedule.inter_measurement_delay));
        iter++;
    }

    // A line through two points has no error estimate (the errors come out infinite or
    // undefined), and a reference without errors would make every speed-up error meaningless.
    if(reference_times.size() < MINREFERENCEPOINTS) {
        throw std::runtime_error(
            "the serial reference has " + std::to_string(reference_times.size()) +
            " measurements; it needs at least " + std::to_string(MINREFERENCEPOINTS) +
            " (delays times n_measurements in the reference configuration) to give its line an "
            "error."
        );
    }
    const Line ab = Gem::Common::getRegressionParameters(reference_times);

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
            "the reference measurement did not run serially (its evaluations overlapped), so it "
            "cannot serve as a reference."
        );
    }

    std::cout << "End of reference measurement" << std::endl;
    return ab;
}

/******************************************************************************/
/**
 * Measures the (usually parallel) run times through the consumer of the process. Before anything
 * runs, it waits for the clients of @p gate (waitForClients()).
 *
 * @param go The optimization wrapper whose consumer is measured
 * @param schedule The evaluation times and measurements
 * @param make_individual Builds the individual for one evaluation time
 * @param gate How many clients the measurement waits for
 * @return Per evaluation time: the evaluation time, 0, the mean run time and its standard deviation
 */
inline Timings measureParallel(
    Go2 &go,
    const Schedule &schedule,
    const IndividualFactory &make_individual,
    const ClientGate &gate = {}
) {
    waitForClients(go, gate);
    std::cout << "Starting parallel measurement" << std::endl;

    Timings parallel_times;
    std::size_t iter = 0;
    for(const auto &delay : schedule.delays) {
        const auto individual = make_individual(delay);

        // The same OA-owned adaption as the reference: the genome carries structure only, so an
        // evolutionary algorithm that is handed none refuses to init(), and the identical adaptor
        // on both sides is what makes the two halves comparable at all.
        go.registerAdaptionConfig("PERSONALITY_EA", ballastAdaption(*individual));

        // One run that is not measured, before the first delay: the clients (connected, if the
        // gate waited for them) receive the genome's layout, so that the first measurement does
        // not carry that start-up time.
        if(iter == 0) {
            std::cout << "Warm-up run" << std::endl;
            go.push_back(individual);
            go.optimize();
            go.clear();
        }

        std::vector<double> durations;
        for(std::uint32_t i = 0; i < schedule.n_measurements; i++) {
            std::cout << "Parallel measurement " << i << " in iteration " << iter << std::endl;
            go.push_back(individual);

            // Every run starts at iteration 0: the iteration limit is absolute, so a run that
            // started at the iterations accumulated so far would end after a single iteration.
            const auto start = std::chrono::system_clock::now();
            go.optimize();
            const std::chrono::duration<double> duration = std::chrono::system_clock::now() - start;
            durations.push_back(duration.count());
            go.clear();
        }

        const auto [mean, sigma] = Gem::Common::GStandardDeviation<double>(durations);
        parallel_times.emplace_back(delay.count(), 0., mean, sigma);

        // Wait for late arrivals
        std::this_thread::sleep_for(std::chrono::seconds(schedule.inter_measurement_delay));
        iter++;
    }

    std::cout << "End of parallel measurement" << std::endl;
    return parallel_times;
}

/******************************************************************************/
/**
 * Writes the results: the plot script (the speed-up over the evaluation time, with a linear and
 * with a logarithmic speed-up axis) and the table behind it, one line per evaluation time.
 *
 * @param ab The line of the serial reference
 * @param parallel_times What measureParallel() returned
 * @param result_file The plot script
 * @param short_result_file The table
 */
inline void writeResults(
    const Line &ab,
    const Timings &parallel_times,
    const std::string &result_file,
    const std::string &short_result_file
) {
    // The serial run time at every measured evaluation time, from the reference line
    Timings reference_times = parallel_times;
    for(auto &row : reference_times) {
        const double sleep_time = std::get<0>(row);
        std::get<1>(row) = 0.;
        std::get<2>(row) = std::get<0>(ab) + (std::get<2>(ab) * sleep_time);
        std::get<3>(row) =
            std::sqrt(std::pow(std::get<1>(ab), 2.) + std::pow(sleep_time * std::get<3>(ab), 2.));
    }
    const Timings ratio = Gem::Common::getRatioErrors(reference_times, parallel_times);

    // The plot shows the speed-up only, over the evaluation time: once with a linear and once
    // with a logarithmic speed-up axis. The serial and the parallel run times are straight lines
    // of little interest; they go to the text file.
    const auto linear = std::make_shared<Gem::Dietrich::GGraph2ED>();
    linear->setPlotLabel("Speed-up over serial execution");
    linear->setXAxisLabel("Evaluation time [s]");
    linear->setYAxisLabel("Speed-up");
    (*linear) & ratio;

    const auto logarithmic = std::make_shared<Gem::Dietrich::GGraph2ED>();
    logarithmic->setPlotLabel("Speed-up over serial execution, logarithmic speed-up axis");
    logarithmic->setXAxisLabel("Evaluation time [s]");
    logarithmic->setYAxisLabel("Speed-up");
    logarithmic->setYAxisLogarithmic(true);
    (*logarithmic) & ratio;

    Gem::Dietrich::GPlotDesigner gpd("Speed-up as a function of the evaluation time", 1, 2);
    gpd.registerPlotter(linear);
    gpd.registerPlotter(logarithmic);
    gpd.setCanvasDimensions(800, 1000);
    gpd.writeToFile(result_file);

    std::ofstream table(short_result_file);
    table << "# evaluation time [s]; serial run time [s] (fitted) and its error; parallel run time "
             "[s] and its error; speed-up and its error"
          << '\n';
    for(std::size_t i = 0; i < parallel_times.size(); i++) {
        table << std::get<0>(parallel_times[i]) << '\t' << std::get<2>(reference_times[i]) << '\t'
              << std::get<3>(reference_times[i]) << '\t' << std::get<2>(parallel_times[i]) << '\t'
              << std::get<3>(parallel_times[i]) << '\t' << std::get<2>(ratio[i]) << '\t'
              << std::get<3>(ratio[i]) << '\n';
    }
}

/******************************************************************************/
/**
 * The two runs of a speed-up benchmark: with @p reference_run the serial reference, stored in
 * @p reference_file; otherwise the parallel measurement against that reference and its results.
 *
 * @param go The optimization wrapper whose consumer is measured
 * @param reference_run Whether this is the reference run
 * @param reference_file The file that holds the reference line
 * @param reference The schedule of the reference
 * @param parallel The schedule of the parallel measurement
 * @param make_individual Builds the individual for one evaluation time
 * @param result_file The plot script
 * @param short_result_file The table
 * @param gate How many clients the parallel measurement waits for
 */
inline void
run(Go2 &go,
    bool reference_run,
    const std::string &reference_file,
    const Schedule &reference,
    const Schedule &parallel,
    const IndividualFactory &make_individual,
    const std::string &result_file,
    const std::string &short_result_file,
    const ClientGate &gate = {}) {
    if(reference_run) {
        writeReferenceLine(reference_file, measureReference(reference, make_individual));
        std::cout << "The serial reference is in " << reference_file << std::endl;
        return;
    }
    const Line ab = readReferenceLine(reference_file);
    writeResults(
        ab,
        measureParallel(go, parallel, make_individual, gate),
        result_file,
        short_result_file
    );
}

} /* namespace Gem::Geneva::SpeedupMeasurement */
