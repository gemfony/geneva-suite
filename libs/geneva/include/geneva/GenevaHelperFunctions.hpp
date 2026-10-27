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

// Standard headers go here
#include <memory>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "courtier/GCourtierEnums.hpp"
#include "geneva/genome/GGenome.hpp"

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief Transforms the individual fitness so that the optimization algorithm always "sees" a
 * minimization problem.
 *
 * For minimization individuals the transformed fitness is returned unchanged; for maximization
 * individuals it is negated (with the numeric extremes max/lowest swapped) so that a smaller
 * returned value is always "better". Optimization algorithms should retrieve fitness only through
 * this function.
 *
 * @param item The work item whose fitness should be retrieved.
 * @param id The index of the fitness criterion to read (individuals may have more than one;
 *   defaults to the main criterion 0).
 * @return The fitness mapped onto an equivalent minimization value.
 */
double
minOnly_transformed_fitness(const gen::GGenome &item, std::size_t id = 0);

/******************************************************************************/
/**
 * @brief Checks whether the first individual is better than the second.
 *
 * The comparison uses the main (first) fitness criterion via minOnly_transformed_fitness(); both
 * individuals are assumed to share the same maxMode (cross-checked in DEBUG builds).
 *
 * @param x_ptr The first individual (the candidate tested for being better).
 * @param y_ptr The second individual (the reference compared against).
 * @return true if @p x_ptr is better than @p y_ptr, false otherwise.
 */
bool
isBetter(const std::shared_ptr<gen::GGenome> &x_ptr, const std::shared_ptr<gen::GGenome> &y_ptr);

/******************************************************************************/
/**
 * @brief Checks whether the first individual is worse than the second.
 *
 * Implemented as the logical negation of isBetter() on the main fitness criterion.
 *
 * @param x_ptr The first individual (the candidate tested for being worse).
 * @param y_ptr The second individual (the reference compared against).
 * @return true if @p x_ptr is not better than @p y_ptr, false otherwise.
 */
bool
isWorse(const std::shared_ptr<gen::GGenome> &x_ptr, const std::shared_ptr<gen::GGenome> &y_ptr);

/******************************************************************************/
/**
 * @brief Checks whether the first value is better than the second under the given optimization mode.
 *
 * @param x The first value (the candidate tested for being better).
 * @param y The second value (the reference compared against).
 * @param m The optimization mode: for MAXIMIZE larger is better, for MINIMIZE smaller is better.
 * @return true if @p x is better than @p y under mode @p m, false otherwise.
 */
bool isBetter(double x, double y, maxMode m);

/******************************************************************************/
/**
 * @brief Checks whether the first value is worse than the second under the given optimization mode.
 *
 * Implemented as the logical negation of isBetter().
 *
 * @param x The first value (the candidate tested for being worse).
 * @param y The second value (the reference compared against).
 * @param m The optimization mode: for MAXIMIZE larger is better, for MINIMIZE smaller is better.
 * @return true if @p x is not better than @p y under mode @p m, false otherwise.
 */
bool isWorse(double x, double y, maxMode m);

/******************************************************************************/

} /* namespace Gem::Geneva */
