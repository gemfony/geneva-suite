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

#include "geneva/genome/GIndividualProcessingResult.hpp"

#include <functional>

#include "common/GErrorStreamer.hpp" // g_error_streamer, DO_LOG, timeAndPlace
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"


namespace Gem::Geneva::Genome {
/******************************************************************************/
/**
 * @brief Initializes the result object with a raw fitness value only.
 *
 * The transformed fitness is set to the same value as the raw fitness, and the flag indicating that
 * the transformed fitness has been set is left false.
 *
 * @param raw_fitness The raw fitness value.
 */
individual_processing_result::individual_processing_result(const double raw_fitness)
  : raw_fitness_(raw_fitness)
  , transformed_fitness_(raw_fitness_)
  , transformed_fitness_set_(false) {
    /* nothing */
}

/******************************************************************************/
/**
 * @brief Initializes the result object with both raw and transformed fitness values.
 *
 * Also sets the flag indicating that the transformed fitness has been set.
 *
 * @param raw_fitness The raw fitness value.
 * @param transformed_fitness The transformed fitness value.
 */
individual_processing_result::individual_processing_result(
    const double raw_fitness,
    const double transformed_fitness
)
  : raw_fitness_(raw_fitness)
  , transformed_fitness_(transformed_fitness)
  , transformed_fitness_set_(true) {
    /* nothing */
}
/******************************************************************************/
/**
 * @brief Initializes the result object with a raw fitness value and a transformation function.
 *
 * The transformed fitness is calculated by applying the provided function to the raw fitness. Throws
 * if the function object is empty.
 *
 * @param raw_fitness The raw fitness value.
 * @param f A function mapping the raw fitness value to a transformed fitness value; must not be empty.
 */
individual_processing_result::individual_processing_result(
    const double raw_fitness,
    const std::function<double(double)>& f
)
  : raw_fitness_(raw_fitness) {
    if(f) {
        transformed_fitness_ = f(raw_fitness_);
        transformed_fitness_set_ = true;
    }
    else {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In individual_processing_result(double, std::function<double(double)>): Error!" << '\n'
            << "Received an empty transformation function." << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Access to the raw fitness.
 *
 * @return The stored raw fitness value
 */
double individual_processing_result::rawFitness() const {
    return raw_fitness_;
}

/******************************************************************************/
/**
 * @brief Access to the transformed fitness.
 *
 * @return The stored transformed fitness value
 */
double individual_processing_result::transformedFitness() const {
    return transformed_fitness_;
}

/******************************************************************************/
/**
     * @brief Updates the transformed fitness using an external function.
     *
     * Applies the function to the stored raw fitness and stores the result. Throws if f is empty.
     *
     * @param f A function mapping the raw fitness to a transformed fitness; must not be empty.
     */
void individual_processing_result::setTransformedFitnessWith(const std::function<double(double)>& f) {
    if(f) {
        transformed_fitness_ = f(raw_fitness_);
        transformed_fitness_set_ = true;
    }
    else {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In individual_processing_result::setTransformedFitnessWith():" << '\n'
            << "Function object f is empty." << '\n'
        );
    }
}

/******************************************************************************/
/**
     * @brief Sets the transformed fitness to a user-defined value.
     *
     * @param transformed_fitness The transformed fitness value to store
     */
void individual_processing_result::setTransformedFitnessTo(const double transformed_fitness) {
    transformed_fitness_ = transformed_fitness;
    transformed_fitness_set_ = true;
}

/******************************************************************************/
/**
     * @brief Sets the transformed fitness to the same value as the raw fitness.
     */
void individual_processing_result::setTransformedFitnessToRaw() {
    transformed_fitness_ = raw_fitness_;
    transformed_fitness_set_ = true;
}

/******************************************************************************/
/**
     * @brief Checks whether the transformed fitness was set.
     *
     * @return true if a transformed fitness has been explicitly set, false otherwise
     */
bool individual_processing_result::transformedFitnessSet() const {
    return transformed_fitness_set_;
}

/******************************************************************************/
/**
     * @brief Resets the object and stores a new raw value in the class.
     *
     * The transformed fitness is set equal to the raw fitness and the "transformed set" flag is cleared.
     *
     * @param raw_fitness The new raw fitness value to store
     */
void individual_processing_result::reset(const double raw_fitness) {
    raw_fitness_ = raw_fitness;
    transformed_fitness_ = raw_fitness_;
    transformed_fitness_set_ = false;
}

/******************************************************************************/
/**
 * @brief Resets the object and stores a new raw and transformed value in the class.
 *
 * @param raw_fitness The new raw fitness value to store
 * @param transformed_fitness The new transformed fitness value to store
 */
void individual_processing_result::reset(
    const double raw_fitness,
    const double transformed_fitness
) {
    raw_fitness_ = raw_fitness;
    transformed_fitness_ = transformed_fitness;
    transformed_fitness_set_ = true;
}

/******************************************************************************/
/**
 * @brief Resets the object, stores a new raw value and recalculates the transformed value.
 *
 * The transformed fitness is recomputed by applying f to the new raw fitness. Throws if f is empty.
 *
 * @param raw_fitness The new raw fitness value to store
 * @param f A function mapping the raw fitness to a transformed fitness; must not be empty.
 */
void individual_processing_result::reset(
    const double raw_fitness,
    const std::function<double(double)>& f
) {
    if(f) {
        raw_fitness_ = raw_fitness;
        transformed_fitness_ = f(raw_fitness_);
        transformed_fitness_set_ = true;
    }
    else {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In individual_processing_result::reset():" << '\n'
            << "Function object f is empty." << '\n'
        );
    }
}

/******************************************************************************/
} /* namespace Gem::Geneva::Genome */
