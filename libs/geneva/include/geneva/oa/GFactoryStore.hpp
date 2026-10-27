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

// Boost header files go here

// Geneva headers go here
#include "common/GProviderStoreT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOAFactoryT.hpp"

// A global store for optimization-algorithm providers. A provider here IS the algorithm's
// config-file-driven factory (GOAFactoryT implements Gem::Common::GProviderT), so every provide() call
// produces a freshly configured algorithm. This is the optimization-algorithm instantiation of the shared
// common/ provider-store template.
using GOAStore = Gem::Common::GProviderStoreT<oa::GOptimizationAlgorithmBase>;
/**
 * @brief Returns the global optimization-algorithm-provider store singleton.
 *
 * @return A shared pointer to the singleton store mapping keys to
 *         optimization-algorithm providers (never nullptr)
 */
[[nodiscard]] inline std::shared_ptr<GOAStore::STYPE> oaFactoryStore() {
    return GOAStore::instance();
}

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief The handle an optimization-algorithm factory is registered and stored under: a provider over the
 * algorithm base. Every GOAFactoryT<GOptimizationAlgorithmBase> subclass IS one, so a module hands the
 * loader its factory and the loader never learns the concrete algorithm type.
 */
using GOAProviderPtr = std::shared_ptr<Gem::Common::GProviderT<oa::GOptimizationAlgorithmBase>>;

/******************************************************************************/
/**
 * @brief THE registration seam for an optimization algorithm -- the single function through which an
 * algorithm becomes selectable by mnemonic.
 *
 * Both paths end here, with the same argument type and the same effect: a built-in algorithm registers at
 * static-init time through GInitializerT, and a runtime-loaded algorithm module registers at dlopen time
 * through GModuleLoader (which turns a false return into a diagnostic naming the module and the clashing
 * mnemonic). Nothing else may write to oaFactoryStore().
 *
 * @param provider The algorithm's factory, as the provider handle the store holds (must not be empty)
 * @return true if the algorithm was registered, false if its mnemonic was already taken
 */
//! [geneva.oa.registration#1]
[[nodiscard]] inline bool registerOptimizationAlgorithm(const GOAProviderPtr &provider) {
    return oaFactoryStore()->setOnce(provider->getMnemonic(), provider);
}
//! [geneva.oa.registration#1]

/******************************************************************************/

} /* namespace Gem::Geneva */
