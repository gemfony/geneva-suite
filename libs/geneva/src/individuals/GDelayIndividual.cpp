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

#include "geneva/individuals/GDelayIndividual.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "hap/GRandomLeasePool.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GDelayIndividual) // NOLINT
namespace Gem::Geneva::Individuals {

/******************************************************************************/
/**
 * The default constructor. Intentionally private -- needed only for (de-)serialization.
 */
GDelayIndividual::GDelayIndividual()
  : fixed_sleep_time_(1.) { /* nothing */
}

/******************************************************************************/
/**
 * A standard copy constructor
 *
 * @param cp A copy of another GDelayIndividual
 */
GDelayIndividual::GDelayIndividual(const GDelayIndividual &cp)
  : gen::GGenomeT<GDelayIndividual>(cp)
  , fixed_sleep_time_(cp.fixed_sleep_time_)
  , may_crash_(cp.may_crash_)
  , throw_likelihood_(cp.throw_likelihood_)
  , sleep_randomly_(cp.sleep_randomly_)
  , rand_sleep_boundaries_(cp.rand_sleep_boundaries_) { /* nothing */
}

/******************************************************************************/
/**
 * The standard destructor
 */
GDelayIndividual::~GDelayIndividual() { /* nothing */
}

/******************************************************************************/
/**
 * @brief The actual fitness calculation takes place here.
 *
 * Sleeps for either a fixed or a random amount of time (to emulate an expensive evaluation),
 * optionally throws with a configured likelihood, and returns a random value -- no real
 * optimization is performed.
 *
 * @return A random value in [0, 1); the result is not used for any actual optimization
 */
std::vector<double> GDelayIndividual::evaluate() {
    // The candidate holds no RNG of its own -- lease a proxy for this evaluation's random draws.
    auto             gr_lease = Gem::Hap::randomLeasePool().acquire();
    Gem::Hap::GRandomBase &gr = *gr_lease;

    std::uniform_real_distribution<double> uniform_real_distribution;

    if(sleep_randomly_) {
        // Calculate the sleep time
        double const sleep_time = uniform_real_distribution(
            gr,
            std::uniform_real_distribution<double>::param_type(
                std::get<0>(rand_sleep_boundaries_),
                std::get<1>(rand_sleep_boundaries_)
            )
        );

        std::chrono::duration<double> const random_sleep_time(sleep_time);

        // Sleep for a random amount of time in a given time window
        std::this_thread::sleep_for(random_sleep_time);
    }
    else {
        // Sleep for a fixed amount of time
        std::this_thread::sleep_for(std::chrono::duration<double>(fixed_sleep_time_));
    }

    // Throw if we were asked to do so
    if(may_crash_) {
        if(uniform_real_distribution(
               gr,
               std::uniform_real_distribution<double>::param_type(0., 1.)
           ) < throw_likelihood_) {
            throw fitnessException();
        }
    }

    // Return a random value - we do not perform any real optimization
    return {uniform_real_distribution(
        gr,
        std::uniform_real_distribution<double>::param_type(0., 1.)
    )};
}

/******************************************************************************/
/**
 * @brief Retrieval of the current value of the fixed_sleep_time_ variable.
 *
 * @return The current fixed sleep time, expressed as a duration in seconds
 */
std::chrono::duration<double> GDelayIndividual::getFixedSleepTime() const {
    return std::chrono::duration<double>(fixed_sleep_time_);
}

/******************************************************************************/
/**
 * @brief Sets the fixed sleep-time to a user-defined value.
 *
 * @param sleep_time The desired fixed sleep time, as a duration in seconds (and fractions thereof)
 */
void GDelayIndividual::setFixedSleepTime(const std::chrono::duration<double> &sleep_time) {
    fixed_sleep_time_ = sleep_time.count();
}

/******************************************************************************/
/**
 * @brief Indicate that the fitness function may crash at the end of the sleep time, and set the likelihood for such a crash.
 *
 * @param may_crash If true, the fitness function may throw at the end of the sleep time
 * @param throw_likelihood The probability of a throw, enforced into the range [0, 1] (0 = never, 1 = always)
 */
void GDelayIndividual::setMayCrash(bool may_crash, double throw_likelihood) {
    may_crash_ = may_crash;

    // Enforce a throw_likelihood in the allowed value range
    throw_likelihood_ = Gem::Common::enforceRangeConstraint(
        throw_likelihood,
        0.,
        1.,
        "GDelayIndividual::setMayCrash()"
    );
}

/******************************************************************************/
/**
 * @brief Check whether the fitness function may crash at the end of the sleep time.
 *
 * @return true if the fitness function is configured to possibly throw, false otherwise
 */
bool GDelayIndividual::getMayCrash() const {
    return may_crash_;
}

/******************************************************************************/
/**
 * @brief Check the likelihood for a crash at the end of the sleep time.
 *
 * @return The configured throw probability, in the range [0, 1]
 */
double GDelayIndividual::getCrashLikelihood() const {
    return throw_likelihood_;
}

/******************************************************************************/
/**
 * @brief Indicates that the fitness function should sleep for a random time within a given window.
 *
 * @param sleep_randomly If true, the fitness function sleeps a random amount of time; if false, it uses the fixed sleep time
 * @param rand_sleep_boundaries A (lower, upper) tuple bounding the random sleep period, in seconds; must satisfy 0 <= lower < upper
 */
void GDelayIndividual::setRandomSleep(
    bool sleep_randomly,
    std::tuple<double, double> rand_sleep_boundaries
) {
    sleep_randomly_ = sleep_randomly;

    // Enforce that the sanity of the lower and upper boundaries
    if(std::get<0>(rand_sleep_boundaries) < 0. ||
       std::get<0>(rand_sleep_boundaries) >= std::get<1>(rand_sleep_boundaries)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GDelayIndividual::setRandomSleep(): Error!" << '\n'
            << "Got invalid boundaries for the sleep time: " << std::get<0>(rand_sleep_boundaries)
            << " / " << std::get<1>(rand_sleep_boundaries) << '\n'
        );
    }

    rand_sleep_boundaries_ = rand_sleep_boundaries;
}

/******************************************************************************/
/**
 * @brief Checks whether the fitness function has a random sleep schedule.
 *
 * @return true if random sleeping is enabled, false if a fixed sleep time is used
 */
bool GDelayIndividual::getMaySleepRandomly() const {
    return sleep_randomly_;
}

/******************************************************************************/
/**
 * @brief Retrieves the time window for random sleeps.
 *
 * @return A (lower, upper) tuple bounding the random sleep period, in seconds
 */
std::tuple<double, double> GDelayIndividual::getSleepWindow() const {
    return rand_sleep_boundaries_;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Registers the delay configuration options, binding them to the passed Config.
 *
 * @param gpb The GParserBuilder object with which the configuration file options are registered
 * @param c The Config struct whose fields are bound to the registered options (filled on parse)
 */
void GDelayIndividual::describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
    gpb.registerFileParameter("n_variables", c.n_variables, c.n_variables)
        << "The number of variables to act on";

    gpb.registerFileParameter("delays", c.delays, c.delays)
        << "A list of delays through which the benchmark should cycle. Format: seconds:milliseconds";

    gpb.registerFileParameter("sleep_randomly", c.sleep_randomly, c.sleep_randomly)
        << "Indicates whether the individual should sleep for a random amount of time" << '\n'
        << "rather than a fixed amount of time";

    gpb.registerFileParameter(
        "lower_rand_sleep_boundary",
        c.lower_rand_sleep_boundary,
        c.lower_rand_sleep_boundary
    ) << "The lower boundary for random sleep times in the" << '\n'
      << "fitness function (seconds, double value)";

    gpb.registerFileParameter(
        "upper_rand_sleep_boundary",
        c.upper_rand_sleep_boundary,
        c.upper_rand_sleep_boundary
    ) << "The upper boundary for random sleep times in the" << '\n'
      << "fitness function (seconds, double value)";

    gpb.registerFileParameter("result_file", c.result_file, c.result_file)
        << "The name of a file to which results should be stored";

    gpb.registerFileParameter("short_result_file", c.short_result_file, c.short_result_file)
        << "The name of a file to which short results should be stored";

    gpb.registerFileParameter("n_measurements", c.n_measurements, c.n_measurements)
        << "The number of measurements for each delay";

    gpb.registerFileParameter("inter_measurement_delay", c.inter_measurement_delay, c.inter_measurement_delay)
        << "The amount of seconds to wait between two measurements";

    gpb.registerFileParameter<bool, double>(
        "may_throw",
        "throw_likelihood",
        c.may_crash,
        c.throw_likelihood,
        [&c](bool may_crash, double throw_likelihood) {
            c.may_crash = may_crash;
            // Enforce a throw_likelihood in the allowed value range
            c.throw_likelihood = Gem::Common::enforceRangeConstraint(
                throw_likelihood,
                0.,
                1.,
                "GDelayIndividual::describeConfig()"
            );
        },
        "throw_behaviour"
    ) << "Indicates whether the fitness function may throw after the sleep time"
      << Gem::Common::nextComment() << "Indicates the likelihood that the fitness function throws";
}

/******************************************************************************/
/**
 * @brief Reads a delay configuration file, creating it with default values if it does not yet exist.
 *
 * The benchmark calls this once and then drives the delay sequence.
 *
 * @param configFile The path to the configuration file to read (created with defaults if absent)
 * @return A Config struct populated from the configuration file
 */
GDelayIndividual::Config GDelayIndividual::readConfig(std::filesystem::path const &configFile) {
    Config c;
    Gem::Common::GParserBuilder gpb;
    describeConfig(gpb, c);

    if(not gpb.parseConfigFile(configFile)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GDelayIndividual::readConfig(): Error!" << '\n'
            << "Could not parse configuration file " << configFile.string() << '\n'
        );
    }

    return c;
}

/******************************************************************************/
/**
 * @brief Parses the textual "delays" list of a Config into (seconds, milliseconds) tuples.
 *
 * @param c The Config whose "delays" string field is parsed
 * @return A vector of (seconds, milliseconds) tuples, one per delay in the list
 */
std::vector<std::tuple<unsigned int, unsigned int>> GDelayIndividual::parseSleepTimes(const Config &c) {
    return Gem::Common::stringToUIntTupleVec(c.delays);
}

/******************************************************************************/
/**
 * @brief Converts a (seconds, milliseconds) tuple to a chrono duration.
 *
 * @param time_tuple A tuple of (seconds, milliseconds) in unsigned int format
 * @return The combined duration, expressed in seconds (as a double-based duration)
 */
std::chrono::duration<double>
GDelayIndividual::tupleToTime(const std::tuple<unsigned int, unsigned int> &time_tuple) {
    std::chrono::duration<double> const t =
        std::chrono::seconds(Gem::Common::narrow<long>(std::get<0>(time_tuple))) +
        std::chrono::milliseconds(Gem::Common::narrow<long>(std::get<1>(time_tuple)));

    return t;
}

/******************************************************************************/
/**
 * @brief Builds a configured delay individual for one fixed sleep time.
 *
 * The genome is n_variables unbounded double parameters (structure only) -- pure transport ballast for
 * the overhead measurement, carrying no adaptor (no adaption config is registered for it).
 *
 * @param c The Config supplying crash, random-sleep and n_variables settings for the new individual
 * @param sleepTime The fixed sleep time assigned to the new individual, as a duration in seconds
 * @return A shared pointer to the newly created and configured GDelayIndividual
 */
std::shared_ptr<GDelayIndividual>
GDelayIndividual::create(const Config &c, const std::chrono::duration<double> &sleepTime) {
    std::shared_ptr<GDelayIndividual> p(new GDelayIndividual());

    p->setFixedSleepTime(sleepTime);
    p->setMayCrash(c.may_crash, c.throw_likelihood);
    p->setRandomSleep(
        c.sleep_randomly,
        std::tuple<double, double>(c.lower_rand_sleep_boundary, c.upper_rand_sleep_boundary)
    );

    gen::GGenomeBuilder gb;
    for(std::size_t var = 0; var < c.n_variables; var++) {
        gb.addDouble(0.5);
    }
    p->setGenome(gb.build());

    return p;
}

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
