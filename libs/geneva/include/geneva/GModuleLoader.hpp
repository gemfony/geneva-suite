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
#include <vector>

// Geneva headers go here
#include "common/GModuleLoader.hpp" // Gem::Common::loadModule() -- the loader this one builds on
#include "geneva/genome/GIndividualPlugin.hpp" // the individual plugin contract (entry-point names + factory type)

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief What a loaded module contributed to this process.
 *
 * A module may carry several typed contributions. The store-backed kinds (optimization algorithms
 * and GPU marshallers) are registered into their process-global stores by @c loadModule() as a side
 * effect; the claim-once individual (at most one per process) is handed back here for the caller to
 * claim. Consumers are courtier's: a consumer module is loaded by the launcher.
 */
struct LoadedModule {
    /** @brief The module's individual content-creator factory, or null if it contributes no
     *  individual. */
    GIndividualFactoryPtr individual;
    /** @brief How many optimization algorithms the module registered into @c oaFactoryStore(). */
    std::uint32_t oa_count = 0;
    /** @brief How many GPU marshallers the module registered into @c marshallerProviderStore(). */
    std::uint32_t marshaller_count = 0;
    /** @brief The module the individual came from; empty if none contributed one. */
    std::filesystem::path individual_module;
};

/**
 * @brief Geneva's handlers for the contributions of modules, for Gem::Common::loadModule() or the
 * launcher (GOptimizationDescription::moduleHandlers()):
 *  - an individual: its factory and module are recorded in @p taken for the caller to claim (the
 *    one content-creator slot is GOptimizer's); a second individual among the modules these
 *    handlers load is refused (one problem per process);
 *  - an optimization algorithm: registered through registerOptimizationAlgorithm() under its
 *    mnemonic;
 *  - a GPU marshaller: registered in courtier's marshaller store under its device target.
 * A mnemonic or device target that is taken already is a refusal that names the module.
 * @param taken What the handlers took; it must outlive their use
 * @return The three handlers
 */
std::vector<Gem::Common::GContributionHandler> genevaContributionHandlers(LoadedModule &taken);

/**
 * @brief Loads a module into a Geneva program, with genevaContributionHandlers(). A module with a
 * kind Geneva does not accept (a consumer: the launcher loads those), with a contribution without
 * factory entry point, or with two individuals is refused before anything of it is taken; see
 * Gem::Common::loadModule() for the other refusals and for the one case in which part of a module
 * stays registered.
 * @param module_path The module
 * @return What it contributed
 */
LoadedModule loadModule(const std::filesystem::path &module_path);

/******************************************************************************/

} /* namespace Gem::Geneva */
