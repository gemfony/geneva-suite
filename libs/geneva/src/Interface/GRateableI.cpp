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

#include "geneva/Interface/GRateableI.hpp"
#include <cstddef>
#include <vector>

namespace Gem::Geneva::Interface {

/******************************************************************************/
/**
 * @brief Retrieves the stored raw fitness with a given id
 *
 * @param pos The position / id of the raw fitness to be retrieved
 * @return The stored raw fitness at the requested position
 */
double GRateableI::raw_fitness(std::size_t pos) const {
    return raw_fitness_(pos);
}

/******************************************************************************/
/**
 * @brief Retrieves the stored transformed fitness with a given id
 *
 * @param pos The position / id of the transformed fitness to be retrieved
 * @return The stored transformed fitness at the requested position
 */
double GRateableI::transformed_fitness(std::size_t pos) const {
    return transformed_fitness_(pos);
}

/******************************************************************************/
/**
 * @brief Returns all raw fitness results in a std::vector
 *
 * @return A std::vector holding all stored raw fitness results
 */
std::vector<double> GRateableI::raw_fitness_vec() const {
    return raw_fitness_vec_();
}

/******************************************************************************/
/**
 * @brief Returns all transformed fitness results in a std::vector
 *
 * @return A std::vector holding all stored transformed fitness results
 */
std::vector<double> GRateableI::transformed_fitness_vec() const {
    return transformed_fitness_vec_();
}

/******************************************************************************/

} /* namespace Gem::Geneva::Interface */
