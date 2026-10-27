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
#include <memory>
#include <type_traits>

// Boost header files go here

// Geneva headers go here
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GFactoryStore.hpp"
#include "geneva/oa/GOAFactoryT.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Registers one built-in optimization-algorithm factory with the global algorithm store. A static object
 * of this type, one per algorithm, lives in the single registration translation unit
 * (src/oa/GBuiltinAlgorithms.cpp), so the algorithms Geneva itself ships become selectable by mnemonic at
 * library-load time and Go2 needs no explicit registration call.
 *
 * The factory IS the provider (GOAFactoryT implements Gem::Common::GProviderT), and registration goes
 * through Gem::Geneva::registerOptimizationAlgorithm() -- the same function the module loader calls for a
 * runtime-loaded algorithm. There is exactly one registration seam, whatever the algorithm's origin.
 *
 * @tparam oaf_type The concrete optimization-algorithm factory type to register.
 */
template <typename oaf_type>
class GInitializerT {
    // Make sure oaf_type is an optimization-algorithm factory over the algorithm base
    static_assert(
        std::is_base_of_v<GOAFactoryT<GOptimizationAlgorithmBase>, oaf_type>,
        "GOAFactoryT<GOptimizationAlgorithmBase> is not a base of oaf_type"
    );

public:
    /**
     * @brief The initializing constructor; registers the factory with the global algorithm store.
     *
     * A false return means the mnemonic was already taken. For the built-ins that cannot happen (their
     * mnemonics are distinct by construction, and this runs before any module can load); a module's clash
     * is the loader's business and is reported there, so the result is deliberately dropped here rather
     * than turned into a throw during static initialization.
     */
    GInitializerT() {
        (void)registerOptimizationAlgorithm(std::make_shared<oaf_type>());
    }
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

