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

#include "geneva/GenevaInitializer.hpp"
#include "hap/GRandomFactory.hpp"

#include <tuple>

namespace Gem::Geneva {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The default constructor. Touching the factory is all that is needed: its producer threads
 * start at the first draw. The order of start and end: the class comment.
 */
GenevaInitializer::GenevaInitializer() {
    // Deliberately discard the handle: constructing the singleton is the point
    std::ignore = Gem::Hap::randomFactory();
}

/******************************************************************************/
/**
 * @brief The destructor. It does not finalize the factory: every Go2 holds a GenevaInitializer, and
 * a short-lived one would otherwise end the factory for every later draw (the regression test
 * "destroying a GenevaInitializer keeps the process RNG alive"). hap's lifecycle guard finalizes it
 * at the end of the process (the class comment).
 */
GenevaInitializer::~GenevaInitializer() = default;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */
