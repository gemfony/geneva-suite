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

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp" // the unified module manifest (GenevaCompat + contributions)
#include "geneva/GConsumerSetup.hpp"  // GConsumerProviderT / consumerProviderStore()

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief The type a loaded consumer contribution hands back: a provider over the consumer base, the type
 * @c consumerProviderStore() holds. The module builds it (where the concrete consumer is known) and the
 * loader registers it under the provider's mnemonic, so the loader stays free of the concrete consumer.
 */
using GConsumerProviderPtr = std::shared_ptr<GConsumerProviderT>;

/******************************************************************************/
/**
 * @brief Builds the module manifest for a runtime-loadable consumer, entirely in C++ (no macro).
 *
 * The consumer analogue of @c marshallerManifest(). A consumer that needs a library not every machine has
 * -- MPI -- lives in a module of its own, so that no Geneva program depends on that library to start. The
 * module's source hand-writes ONE small entry point that delegates here:
 * @code
 *   extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
 *       return Gem::Geneva::consumerManifest<GMyConsumerProvider, "my">();
 *   }
 * @endcode
 * The loader registers the provider into @c consumerProviderStore() under its mnemonic; a mnemonic already
 * held (a built-in consumer or another module) is a hard error.
 *
 * @tparam Provider The concrete consumer provider (a @c GConsumerProviderT subclass), default-constructed
 * @tparam Mnemonic The consumer mnemonic (a string literal, e.g. "mpi"), as chosen with --consumer
 * @return A pointer to this module's process-lifetime manifest
 */
template <typename Provider, Gem::Common::GFixedString Mnemonic>
const GenevaModuleManifest *consumerManifest() {
    // Captureless thunk -> void*(*)(void): build the provider on the heap; the loader moves-from and deletes
    // it, then registers the provider in consumerProviderStore().
    static constexpr auto factory_thunk = +[]() -> void * {
        return new GConsumerProviderPtr(std::make_shared<Provider>());
    };
    static const GenevaContribution contribution{GENEVA_CONTRIBUTION_CONSUMER, Mnemonic.c_str(), factory_thunk};
    static const GenevaModuleManifest manifest{
        GENEVA_MODULE_ABI_STAMP, Mnemonic.c_str(), GENEVA_VERSION_STRING, &contribution, 1u};
    return &manifest;
}

/******************************************************************************/

} /* namespace Gem::Geneva */
