/**
 * @file GSumOfSquares.hpp
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

// What GSubClientOverhead's individual computes on its parameter values, alone (a group of one,
// the serial reference) or shared out over a group of processes (GSleepGroup.hpp): the sum of
// their squares. Standard C++ only, so that both the individual, which is free of MPI, and the
// halves of the group, which are free of Geneva, include it.

#pragma once

// Standard header files go here
#include <span>

namespace Gem::Geneva::SubClientOverhead {

/**
 * @brief What a process computes on its share of the parameters: the sum of their squares.
 *
 * @param share The share
 * @return The sum of the squares
 */
inline double sumOfSquares(std::span<const double> share) {
    double sum = 0.;
    for(const double x : share) {
        sum += x * x;
    }
    return sum;
}

} /* namespace Gem::Geneva::SubClientOverhead */
