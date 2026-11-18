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
#include <cstdint>
#include <filesystem>

// Geneva headers go here
#include "common/GModuleLoader.hpp" // Gem::Common::loadModule() -- the loader this one builds on
#include "geneva/genome/GIndividualPlugin.hpp" // the individual plugin contract (entry-point names + factory type)

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief What a loaded module contributed to this process.
 *
 * A module may carry several typed contributions. The store-backed kinds (optimization algorithms,
 * GPU marshallers and consumers) are registered into their process-global stores by @c loadModule()
 * as a side effect; the claim-once individual (at most one per process) is handed back here for the
 * caller to claim.
 */
struct LoadedModule {
    /** @brief The module's individual content-creator factory, or null if it contributes no
     *  individual. */
    GIndividualFactoryPtr individual;
    /** @brief How many optimization algorithms the module registered into @c oaFactoryStore(). */
    std::uint32_t oa_count = 0;
    /** @brief How many GPU marshallers the module registered into @c marshallerProviderStore(). */
    std::uint32_t marshaller_count = 0;
    /** @brief How many consumers the module registered into @c consumerProviderStore(). */
    std::uint32_t consumer_count = 0;
};

/**
 * @brief Loads a module into a Geneva program.
 *
 * Geneva's optional consumer modules (MPI) are loaded first, if they have not been: a module
 * given here cannot take the mnemonic of one. Then the module is loaded with
 * Gem::Common::loadModule() and Geneva's handlers for the four kinds it accepts:
 *  - an individual: at most one per module; its factory is returned in LoadedModule::individual
 *    for the caller to claim (the one content-creator slot is Go2's);
 *  - an optimization algorithm: registered through registerOptimizationAlgorithm() under its
 *    mnemonic;
 *  - a GPU marshaller: registered in marshallerProviderStore() under its device target;
 *  - a consumer: registered in consumerProviderStore() under its mnemonic.
 * A mnemonic or device target that is taken already is a refusal that names the module. A
 * module with a kind Geneva does not accept, with a contribution without factory entry point,
 * or with two individuals is refused before anything of it is taken; see
 * Gem::Common::loadModule() for the other refusals and for the one case in which part of a
 * module stays registered.
 */
LoadedModule loadModule(const std::filesystem::path &module_path);

/******************************************************************************/

} /* namespace Gem::Geneva */
