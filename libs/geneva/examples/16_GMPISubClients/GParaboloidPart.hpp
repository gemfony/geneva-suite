/**
 * @file GParaboloidPart.hpp
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

// What a process of example 16 computes on its part of the parameter values: it squares each of
// them, sleeping a while for each to stand for an expensive calculation, and sums the squares.
// Standard C++ only, so that both the individual, which is free of MPI, and the halves of a group,
// which are free of Geneva, include it.

#pragma once

// Standard header files go here
#include <chrono>
#include <span>
#include <thread>

namespace Gem::Geneva {

/**
 * @brief Squares every value of @p part, sleeping @p delay_per_parameter for each, and sums the
 * squares.
 *
 * @param part The parameter values
 * @param delay_per_parameter The simulated time of the calculation of one square
 * @return The sum of the squares
 */
inline double
sumOfSquares(std::span<const double> part, std::chrono::milliseconds delay_per_parameter) {
    double sum = 0.;
    for(const double x : part) {
        std::this_thread::sleep_for(delay_per_parameter);
        sum += x * x;
    }
    return sum;
}

} /* namespace Gem::Geneva */
