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
#include <sstream>
#include <vector>

// Boost header files go here

// Geneva header files go here

namespace Gem::Geneva::Interface {
/******************************************************************************/
/**
     * @brief A simple interface class for objects that can be evaluated.
     *
     * Defines the public accessors for raw and transformed fitness values (single value by id
     * or the full vector). The evaluation hook itself (the virtual evaluate()) lives on
     * GGenome, which derives this interface.
     */
class GRateableI {
public:
    /**
     * @brief Retrieves the stored raw fitness with a given id.
     *
     * @param pos The index of the fitness criterion to retrieve (default 0, the main criterion)
     * @return The raw (untransformed) fitness value for the requested criterion
     */
    //! [constraints:fitness-accessors#1]
    [[nodiscard]] double raw_fitness(std::size_t pos = 0) const;
    //! [constraints:fitness-accessors#1]
    /**
     * @brief Retrieves the stored transformed fitness with a given id.
     *
     * @param pos The index of the fitness criterion to retrieve (default 0, the main criterion)
     * @return The transformed fitness value for the requested criterion
     */
    //! [constraints:fitness-accessors#2]
    [[nodiscard]] double transformed_fitness(std::size_t pos = 0) const;
    //! [constraints:fitness-accessors#2]

    /**
     * @brief Returns all raw fitness results in a std::vector.
     *
     * @return A vector holding the raw (untransformed) fitness value of every criterion
     */
    //! [constraints:fitness-accessors#3]
    [[nodiscard]] std::vector<double> raw_fitness_vec() const;
    //! [constraints:fitness-accessors#3]
    /**
     * @brief Returns all transformed fitness results in a std::vector.
     *
     * @return A vector holding the transformed fitness value of every criterion
     */
    //! [constraints:fitness-accessors#4]
    [[nodiscard]] std::vector<double> transformed_fitness_vec() const;
    //! [constraints:fitness-accessors#4]

protected:
    /**************************************************************************/
    // Defaulted constructors / destructors / assignment operators

    /** @brief The default constructor. */
    GRateableI() = default;
    /** @brief The copy constructor. */
    GRateableI(GRateableI const &) = default;
    /** @brief The move constructor. */
    GRateableI(GRateableI &&) = default;
    /** @brief The destructor. */
    ~GRateableI() = default;

    /** @brief The copy assignment operator. */
    GRateableI &operator=(GRateableI const &) = default;
    /** @brief The move assignment operator. */
    GRateableI &operator=(GRateableI &&) = default;

private:
    /**
     * @brief Retrieves the stored raw fitness with a given id.
     *
     * @param id The index of the fitness criterion to retrieve
     * @return The raw (untransformed) fitness value for the requested criterion
     */
    [[nodiscard]] virtual double raw_fitness_(std::size_t) const = 0;
    /**
     * @brief Retrieves the stored transformed fitness with a given id.
     *
     * @param id The index of the fitness criterion to retrieve
     * @return The transformed fitness value for the requested criterion
     */
    [[nodiscard]] virtual double transformed_fitness_(std::size_t) const = 0;

    /**
     * @brief Returns all raw fitness results in a std::vector.
     *
     * @return A vector holding the raw (untransformed) fitness value of every criterion
     */
    [[nodiscard]] virtual std::vector<double> raw_fitness_vec_() const = 0;
    /**
     * @brief Returns all transformed fitness results in a std::vector.
     *
     * @return A vector holding the transformed fitness value of every criterion
     */
    [[nodiscard]] virtual std::vector<double> transformed_fitness_vec_() const = 0;
};

/******************************************************************************/
} /* namespace Gem::Geneva::Interface */
