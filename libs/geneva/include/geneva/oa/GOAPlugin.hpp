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
#include <type_traits>

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp" // the unified module manifest (GenevaCompat + contributions)
#include "geneva/oa/GFactoryStore.hpp" // GOAProviderPtr -- the handle the algorithm store holds
#include "geneva/oa/GOAFactoryT.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief Builds the module manifest for a runtime-loadable optimization algorithm, entirely in C++ (no macro).
 *
 * The OA analogue of @c individualManifest(). A loadable OA's source hand-writes ONE small entry point that
 * delegates here:
 * @code
 *   extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
 *       return Gem::Geneva::oaManifest<MyOAFactory, "MyOA">();
 *   }
 * @endcode
 * The @c extern @c "C" wrapper is irreducible (the loader resolves the fixed, unmangled symbol
 * @c geneva_module_manifest via dlsym); everything else is this typed template.
 *
 * Unlike an individual (a claim-once single slot), an OA is resolved by mnemonic against a shared store, so
 * the contribution's factory thunk hands back the algorithm's factory itself (as a @c GOAProviderPtr on the
 * heap, the plain-C @c void* boundary -- a @c GOAFactoryT IS the provider the store holds); the loader moves
 * it out and hands it to @c registerOptimizationAlgorithm(), the very function a built-in algorithm's
 * @c GInitializerT uses, under the factory's own mnemonic (the algorithm's personality nickname).
 * Registering a mnemonic a built-in (or another module) already holds is a hard error -- a module cannot
 * shadow one.
 *
 * This function does NOT emit the OA's serialization registrations -- the OA's own translation unit carries
 * the @c GEM_REGISTER_ARCHIVABLE for the algorithm and its personality traits (the same registrations a
 * compiled-in OA needs), so a checkpoint written with the module loaded resumes with it loaded.
 *
 * @tparam FactoryType The concrete OA factory (a @c GOAFactoryT<GOptimizationAlgorithmBase> subclass, e.g.
 *         @c GOptimizationAlgorithmFactoryT<MyOA>); default-constructed here
 * @tparam Name The module/contribution name, for diagnostics (a string literal); the store key is the
 *         factory's mnemonic, not this
 * @return A pointer to this module's process-lifetime manifest
 */
template <typename FactoryType, Gem::Common::GFixedString Name>
const GenevaModuleManifest *oaManifest() {
    static_assert(
        std::is_base_of_v<
            OptimizationAlgorithms::GOAFactoryT<OptimizationAlgorithms::GOptimizationAlgorithmBase>,
            FactoryType>,
        "oaManifest(): FactoryType must derive from GOAFactoryT<GOptimizationAlgorithmBase>"
    );
    // Captureless thunk -> void*(*)(void): default-construct the OA factory (which is the store's provider)
    // on the heap; the loader moves-from and deletes the holder, then registers the factory.
    static constexpr auto factory_thunk = +[]() -> void * {
        return new GOAProviderPtr(std::make_shared<FactoryType>());
    };
    static const GenevaContribution contribution{GENEVA_CONTRIBUTION_OA, Name.c_str(), factory_thunk};
    static const GenevaModuleManifest manifest{
        GENEVA_MODULE_ABI_STAMP, Name.c_str(), GENEVA_VERSION_STRING, &contribution, 1u};
    return &manifest;
}

/******************************************************************************/

} /* namespace Gem::Geneva */
