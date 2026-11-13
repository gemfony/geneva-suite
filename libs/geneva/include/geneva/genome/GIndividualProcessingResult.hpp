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

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <functional>

// Boost header files go here

// Geneva header files go here
#include "common/GArchiveNamed.hpp" // archive_named (named-member emitter)
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Container for fitness and transformed fitness values, as produced by a candidate's evaluation. This
 * is the shared result currency of the individual hierarchy: a candidate stores one of these per fitness
 * criterion. It is a small value type (two doubles plus a flag) and is the processing_result_type of the
 * courtier processing machinery.
 */
class individual_processing_result {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /**
     * @brief Serializes this object to/from a GArchive codec
     * @tparam Archive The GArchive codec type
     * @param ar The archive to read from or write to
     * @param version The serialization version (unused)
     */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named(ar, "raw_fitness_", raw_fitness_);
        Gem::Common::archive_named(ar, "transformed_fitness_", transformed_fitness_);
        Gem::Common::archive_named(ar, "transformed_fitness_set_", transformed_fitness_set_);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The default constuctor */
    individual_processing_result() = default;

    /**
     * @brief Initialization with a raw fitness
     * @param raw_fitness The raw fitness value to store
     */
    explicit individual_processing_result(double raw_fitness);

    /**
     * @brief Initialization with a raw and transformed fitness
     * @param raw_fitness The raw fitness value to store
     * @param transformed_fitness The transformed fitness value to store
     */
    individual_processing_result(double raw_fitness, double transformed_fitness);

    /**
     * @brief Initialization with a raw fitness and recalculation of the transformed fitness
     * @param raw_fitness The raw fitness value to store
     * @param f The function used to derive the transformed fitness from the raw value
     */
    individual_processing_result(double raw_fitness, const std::function<double(double)>& f);

    /**
     * @brief Copy construction
     * @param cp The other object to copy from
     */
    individual_processing_result(individual_processing_result const &) = default;

    /**
     * @brief Move construction
     * @param cp The other object to move from
     */
    individual_processing_result(individual_processing_result &&) = default;

    /** @brief Destructor */
    ~individual_processing_result() = default;

    /**
     * @brief Assignment
     * @param cp The other object to copy-assign from
     * @return A reference to this object
     */
    individual_processing_result &operator=(individual_processing_result const &) = default;

    /**
     * @brief Move assignment
     * @param cp The other object to move-assign from
     * @return A reference to this object
     */
    individual_processing_result &operator=(individual_processing_result &&) = default;

    /**
     * @brief Access to the raw fitness
     * @return The stored raw fitness value
     */
    [[nodiscard]] double rawFitness() const;

    /**
     * @brief Access to the transformed fitness
     * @return The stored transformed fitness value
     */
    [[nodiscard]] double transformedFitness() const;

    /**
     * @brief Updates the transformed fitness using an external function
     * @param f The function applied to the raw fitness to obtain the transformed fitness
     */
    void setTransformedFitnessWith(const std::function<double(double)>& f);

    /**
     * @brief Sets the transformed fitness to a user-defined value
     * @param transformed_fitness The transformed fitness value to store
     */
    void setTransformedFitnessTo(double transformed_fitness);

    /** @brief Sets the transformed fitness to the same value as the raw fitness */
    void setTransformedFitnessToRaw();

    /**
     * @brief Checks whether the transformed fitness was set
     * @return true if a transformed fitness value is available, false otherwise
     */
    [[nodiscard]] bool transformedFitnessSet() const;

    /**
     * @brief Resets the object and stores a new raw value in the class
     * @param raw_fitness The new raw fitness value to store
     */
    void reset(double raw_fitness);

    /**
     * @brief Resets the object and stores a new raw and transformed value in the class
     * @param raw_fitness The new raw fitness value to store
     * @param transformed_fitness The new transformed fitness value to store
     */
    void reset(double raw_fitness, double transformed_fitness);

    /**
     * @brief Resets the object and stores a new raw value in the class and triggers recalculation of the transformed value
     * @param raw_fitness The new raw fitness value to store
     * @param f The function used to derive the transformed fitness from the raw value
     */
    void reset(double raw_fitness, const std::function<double(double)>& f);

private:
    /***************************************************************************/
    // Data

    double raw_fitness_ = 0.; ///< The fitness as it comes out of the evaluate() hook
    double transformed_fitness_ =
        0.; ///< The fitness as calculated from raw_fitness_ through
    bool transformed_fitness_set_ =
        false; ///< Indicates whether a suitable transformed_fitness_ value is available
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Genome */

/******************************************************************************/
/**
 * @brief Needed for GArchive serialization
 */
/******************************************************************************/
