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

#include "geneva/GenevaHelperFunctions.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include <cstddef>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief Returns a fitness value transformed so the optimizer always "sees" a minimization problem.
 *
 * Optimization algorithms should only use this function to retrieve the fitness of individuals.
 * For minimization the raw transformed fitness is returned unchanged; for maximization it is negated
 * (with the numeric extremes swapped so the largest/smallest representable doubles remain finite).
 *
 * @param item The work item whose fitness should be retrieved
 * @param id The id of the fitness criterion (individuals may have more than one)
 * @return The fitness expressed as a quantity to be minimized
 */
double minOnly_transformed_fitness(
    const gen::GGenome &item,
    const std::size_t id // NOLINT(misc-unused-parameters)
) {
    const double f = item.transformed_fitness(id); // NOLINT(cppcoreguidelines-init-variables)
    const maxMode m = item.getMaxMode();           // NOLINT(cppcoreguidelines-init-variables)

    if(maxMode::MINIMIZE == m) {
        return f;
    }
    // MAXIMIZE
    // Negation will transform maximization problems into minimization problems
    if(std::numeric_limits<double>::max() == f) {
        return std::numeric_limits<double>::lowest();
    }
    if(std::numeric_limits<double>::lowest() == f) {
        return std::numeric_limits<double>::max();
    }
    return -f;
}

/******************************************************************************/
/**
 * @brief Checks whether the first individual is better than the second.
 *
 * The comparison is done with the first (main) fitness criterion via minOnly_transformed_fitness().
 * In a DEBUG build both items are cross-checked to share the same maxMode.
 *
 * @param x_ptr The work item tested for being the better one
 * @param y_ptr The work item it is compared against
 * @return true if x_ptr is better than y_ptr, false otherwise
 */
bool isBetter(
    const std::shared_ptr<gen::GGenome> &x_ptr,
    const std::shared_ptr<gen::GGenome> &y_ptr
) {
#ifdef DEBUG
    const auto x_mode = x_ptr->getMaxMode();
    const auto y_mode = y_ptr->getMaxMode();
    // Cross-check that both work items have the same maxMode
    if(x_mode != y_mode) {
        // Throw an exception
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In isBetterThan(x_ptr, y_ptr):" << '\n'
            << "Got different maxMode-settings: " << x_mode << " / " << y_mode << '\n'
        );
    }
#endif

    // We assume that both items have the same maxMode and simply compare the "minOnly-Fitness"
    return minOnly_transformed_fitness(*x_ptr) < minOnly_transformed_fitness(*y_ptr);
   
}

/******************************************************************************/
/**
 * @brief Checks whether the first individual is worse than the second.
 *
 * Defined as the negation of isBetter(); the comparison uses the first (main) fitness criterion.
 *
 * @param x_ptr The work item tested for being the worse one
 * @param y_ptr The work item it is compared against
 * @return true if x_ptr is worse than (or not better than) y_ptr, false otherwise
 */
bool isWorse(
    const std::shared_ptr<gen::GGenome> &x_ptr,
    const std::shared_ptr<gen::GGenome> &y_ptr
) {
    return not isBetter(x_ptr, y_ptr);
}

/******************************************************************************/
/**
 * @brief Checks whether the first value is better than the second for a given optimization direction.
 *
 * @param x The value tested for being the better one
 * @param y The value it is compared against
 * @param m The optimization direction (MAXIMIZE: larger is better; MINIMIZE: smaller is better)
 * @return true if x is better than y under the given maxMode, false otherwise
 */
bool isBetter(const double x, const double y, const maxMode m) {
    if(maxMode::MAXIMIZE == m) {
        return x > y;
       
    }
            // maxMode::MINIMIZE
        if(x < y) {
            return true;
        }
                    return false;
       
   
}

/******************************************************************************/
/**
 * @brief Checks whether the first value is worse than the second for a given optimization direction.
 *
 * Defined as the negation of isBetter().
 *
 * @param x The value tested for being the worse one
 * @param y The value it is compared against
 * @param m The optimization direction (MAXIMIZE: larger is better; MINIMIZE: smaller is better)
 * @return true if x is worse than (or not better than) y under the given maxMode, false otherwise
 */
bool isWorse(const double x, const double y, const maxMode m) {
    return not isBetter(x, y, m);
}

/******************************************************************************/

} /* namespace Gem::Geneva */
