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
 * @brief The default constructor; brings the Hap random-number factory singleton online.
 *
 * Touching the singleton is all that is needed -- the factory's producer
 * threads start lazily on the first container request.
 */
GenevaInitializer::GenevaInitializer() {
    // Deliberately discard the handle: constructing the singleton is the point
    std::ignore = Gem::Hap::randomFactory();
}

/******************************************************************************/
/**
 * @brief The destructor.
 *
 * Deliberately does NOT finalize the Hap random-number factory. The factory is a process-global
 * singleton (GSingletonT<GRandomFactory>) shared by every Geneva facility, and its finalize() is
 * TERMINAL -- it closes the producer/return buffers (a terminal close()) and joins the producer
 * threads, after which getNewRandomContainer() can never succeed again. A GenevaInitializer is,
 * however, NOT the exclusive owner of that singleton: it is embedded in every Go2 (as Go2::gi_), so
 * a short-lived Go2 (or any scoped GenevaInitializer) would otherwise finalize the shared factory the
 * moment it is destroyed, permanently starving all subsequent random-number consumers -- an
 * unbounded 100%-CPU spin in GRandomT::getNewRandomContainer(). The factory is instead torn down
 * exactly once, by its own singleton destructor at process exit (which joins the producers cleanly);
 * that is precisely the destruction-order guarantee GSingletonT's shared_ptr hand-out exists to
 * provide. See the regression test "destroying a GenevaInitializer keeps the process RNG alive".
 */
GenevaInitializer::~GenevaInitializer() = default;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */
