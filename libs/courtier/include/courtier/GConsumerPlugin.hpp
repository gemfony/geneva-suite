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
#include <string>
#include <typeinfo>

// Geneva header files go here
#include "common/GModuleLoader.hpp" // GContributionHandler
#include "common/GModuleManifest.hpp"
#include "courtier/GProcessable.hpp" // ProcessableItem

namespace Gem::Courtier {

template <ProcessableItem processable_type>
class GConsumerProviderT; // courtier/GConsumerProviderT.hpp

/******************************************************************************/
/**
 * @brief The kind of a module contribution that carries a consumer provider: a name with the
 * prefix of the library that handles it (common/GModuleManifest.hpp).
 */
inline constexpr const char *CONSUMER_CONTRIBUTION_KIND = "courtier.consumer";

/******************************************************************************/
/**
 * @brief The manifest of a consumer module: one consumer contribution, the provider
 * @p provider_type for its item type. A consumer module -- a shipped one or a third party's -- is
 * one source file with one entry point:
 * @code
 *   extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest() {
 *       return Gem::Courtier::consumerManifest<
 *           Gem::Courtier::GWebsocketConsumerProviderT<MyItem>>();
 *   }
 * @endcode
 * The contribution names the item type (its subject, typeid(item type).name()), so that a program
 * for another item type refuses it before anything of the module is called, and the mnemonic,
 * which is the provider's MNEMONIC (stated once, by the configuration struct of its family). A
 * module links no library of the collection it does not need: it resolves them in the program,
 * which is linked with exported symbols (CMake ENABLE_EXPORTS), so that the program and its
 * modules share one catalogue and one registry per item type.
 *
 * @tparam provider_type The provider (derived from GConsumerProviderT<its item_type>,
 *         default-constructible, with a static MNEMONIC)
 * @return The module's process-lifetime manifest
 */
template <typename provider_type>
const GemModuleManifest *consumerManifest() {
    using item_type = typename provider_type::item_type;
    static constexpr auto factory_thunk = +[]() -> void * {
        return new std::shared_ptr<GConsumerProviderT<item_type>>(
            std::make_shared<provider_type>()
        );
    };
    static const std::string mnemonic(provider_type::MNEMONIC);
    static const GemContribution contribution{
        CONSUMER_CONTRIBUTION_KIND,
        mnemonic.c_str(),
        factory_thunk,
        typeid(item_type).name()
    };
    static const GemModuleManifest
        manifest{GEM_MODULE_ABI_STAMP, mnemonic.c_str(), GEM_VERSION_STRING, &contribution, 1u};
    return &manifest;
}

/******************************************************************************/
/**
 * @brief The handler of consumer contributions for a program whose item type is
 * @p processable_type (Gem::Common::loadModule()). It refuses, before anything of the module is
 * called, a contribution for another item type; then it refuses a provider whose mnemonic is not
 * the one of its manifest entry, one that does not share the program's catalogue (the program
 * lacks exported symbols), and a mnemonic that is taken; otherwise the provider joins the
 * catalogue of the item type (consumerCatalog()).
 * @tparam processable_type The program's item type
 * @return The handler
 * (Defined in courtier/GConsumerProviderT.hpp, beside the catalogue.)
 */
template <ProcessableItem processable_type>
Gem::Common::GContributionHandler consumerContributionHandler();

/******************************************************************************/

} /* namespace Gem::Courtier */
