/**
 * @file GSleepGroupIndividual.cpp
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

#include "GSleepGroupIndividual.hpp"

// Standard header files go here
#include <stdexcept>
#include <thread>

// Geneva header files go here
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE

#include "GSumOfSquares.hpp"

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::SubClientOverhead::GSleepGroupIndividual) // NOLINT

namespace Gem::Geneva::SubClientOverhead {

/******************************************************************************/
/**
 * Evaluates in this process alone: sleeps the evaluation time and computes the sum of the squares
 * of the parameter values. (A client with sub-clients evaluates in its group instead; see the
 * class.)
 *
 * @return The sum of the squares of the parameter values
 */
std::vector<double> GSleepGroupIndividual::evaluate() {
    std::vector<double> parameters;
    this->streamline(parameters);
    std::this_thread::sleep_for(std::chrono::duration<double>(evaluation_time_));
    return {sumOfSquares(parameters)};
}

/******************************************************************************/
/**
 * @return The nominal duration of an evaluation [s]
 */
double GSleepGroupIndividual::evaluationTime() const noexcept {
    return evaluation_time_;
}

/**
 * @return Whether the processes of a group share the evaluation time (or each sleeps all of it)
 */
bool GSleepGroupIndividual::split() const noexcept {
    return split_;
}

/******************************************************************************/
/**
 * Reads the configuration file of the benchmark, writing it with defaults if it is not there.
 *
 * @param config_file The file
 * @return What it holds
 */
GSleepGroupIndividual::Config
GSleepGroupIndividual::readConfig(const std::filesystem::path &config_file) {
    Config c;
    Gem::Common::GParserBuilder gpb;
    gpb.registerFileParameter("n_variables", c.n_variables, c.n_variables)
        << "The number of (ballast) double parameters of the genome";
    gpb.registerFileParameter("delays", c.delays, c.delays)
        << "The evaluation times the benchmark cycles through, as (seconds,milliseconds), ...";
    gpb.registerFileParameter("result_file", c.result_file, c.result_file)
        << "The plot script of the results";
    gpb.registerFileParameter("short_result_file", c.short_result_file, c.short_result_file)
        << "The table of the results";
    gpb.registerFileParameter("n_measurements", c.n_measurements, c.n_measurements)
        << "The number of measurements of each evaluation time";
    gpb.registerFileParameter(
        "inter_measurement_delay",
        c.inter_measurement_delay,
        c.inter_measurement_delay
    ) << "The seconds to wait after the measurements of an evaluation time";
    gpb.registerFileParameter("split_evaluation", c.split_evaluation, c.split_evaluation)
        << "Whether the processes of a group share an evaluation time (or each sleeps all of it)";
    gpb.registerFileParameter("poll_pause_min", c.poll_pause_min, c.poll_pause_min)
        << "Microseconds: the first pause of a process that looks for a message and finds none;"
        << " the pause doubles up to poll_pause_max";
    gpb.registerFileParameter("poll_pause_max", c.poll_pause_max, c.poll_pause_max)
        << "Microseconds: the longest pause between two looks for a message";
    gpb.parseConfigFile(config_file);
    return c;
}

/******************************************************************************/
/**
 * The evaluation times of a configuration.
 *
 * @param c The configuration
 * @return Its delays as durations
 */
std::vector<std::chrono::duration<double>> GSleepGroupIndividual::evaluationTimes(const Config &c) {
    std::vector<std::chrono::duration<double>> times;
    for(const auto &[seconds, milliseconds] : Gem::Common::stringToUIntTupleVec(c.delays)) {
        times.emplace_back(std::chrono::seconds(seconds) + std::chrono::milliseconds(milliseconds));
    }
    return times;
}

/******************************************************************************/
/**
 * An individual of the given evaluation time, with the configured ballast.
 *
 * @param c The configuration
 * @param evaluation_time The nominal duration of its evaluation
 * @return The individual
 */
std::shared_ptr<GSleepGroupIndividual> GSleepGroupIndividual::create(
    const Config &c,
    const std::chrono::duration<double> &evaluation_time
) {
    auto p = std::make_shared<GSleepGroupIndividual>();
    p->evaluation_time_ = evaluation_time.count();
    p->split_ = c.split_evaluation;
    gen::GGenomeBuilder gb;
    for(std::size_t var = 0; var < c.n_variables; var++) {
        gb.addDouble(0.5);
    }
    p->setGenome(gb.build());
    return p;
}

} /* namespace Gem::Geneva::SubClientOverhead */

// The individual stays free of MPI (CMakeLists.txt). Every mpi.h defines MPI_VERSION, so an include
// of an MPI header, directly or through another header, fails the build here also where mpi.h is
// on the compiler's own search path (an MPI compiler wrapper, CPATH, a system include directory).
#ifdef MPI_VERSION
#error "GSleepGroupIndividual must stay free of MPI"
#endif
