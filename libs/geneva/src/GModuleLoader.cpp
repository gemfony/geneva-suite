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

#include "geneva/GModuleLoader.hpp"

// Standard headers go here
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

// Geneva headers go here
#include "GModuleLoaderInternal.hpp" // loadModuleWithGenevaHandlers(), loadOptionalConsumerModules
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GModuleLoader.hpp"
#include "common/GModuleManifest.hpp"
#include "common/GProviderT.hpp"
#include "geneva/GConsumerPlugin.hpp"   // GConsumerProviderPtr -- a consumer contribution's holder
#include "geneva/GMarshallerPlugin.hpp" // GMarshallerProviderPtr -- a marshaller's holder
#include "geneva/GMarshallerSetup.hpp"  // marshallerProviderStore() -- the marshallers' store
#include "geneva/oa/GFactoryStore.hpp"  // oaFactoryStore() -- the algorithms' store
#include "geneva/oa/GOAPlugin.hpp"      // GOAProviderPtr -- the OA contribution's void* holder type

namespace Gem::Geneva {

namespace {

using Gem::Common::GContributionContext;
using Gem::Common::GContributionHandler;
using Gem::Common::takeContribution;

/** @brief Registers one OA contribution's factory through registerOptimizationAlgorithm() -- the
 *  same seam a built-in algorithm's GInitializerT uses -- under the algorithm's own mnemonic. A
 *  mnemonic already held (a built-in or another module) is a hard error -- a module cannot shadow
 *  one.
 *  @param contribution The contribution whose factory is called
 *  @param context The module, for the messages
 *  @param noun What the contribution is, for the messages */
void registerOAContribution(
    const GenevaContribution &contribution,
    const GContributionContext &context,
    std::string_view noun
) {
    auto const provider = takeContribution<GOAProviderPtr>(contribution, context, noun);
    const std::string mnemonic = provider->getMnemonic();
    if(not registerOptimizationAlgorithm(provider)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::loadModule(): Error!" << '\n'
            << "The module " << context.description
            << " contributes an optimization algorithm with mnemonic '" << mnemonic << "'," << '\n'
            << "but that mnemonic is already registered (a built-in or another module owns it). A"
            << " module cannot shadow an existing algorithm; give the module's algorithm a distinct"
            << " mnemonic." << '\n'
        );
    }
}

/** @brief Registers one MARSHALLER contribution's provider into marshallerProviderStore() under the
 *  marshaller's device target. A device target already held (a compiled-in marshaller or another
 *  module) is a hard error -- one problem per process means one marshaller per target.
 *  @param contribution The contribution whose factory is called
 *  @param context The module, for the messages
 *  @param noun What the contribution is, for the messages */
void registerMarshallerContribution(
    const GenevaContribution &contribution,
    const GContributionContext &context,
    std::string_view noun
) {
    auto const provider = takeContribution<GMarshallerProviderPtr>(contribution, context, noun);
    const std::string device_target = provider->getMnemonic();
    if(not marshallerProviderStore()->setOnce(device_target, provider)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::loadModule(): Error!" << '\n'
            << "The module " << context.description
            << " contributes a GPU marshaller for device target '" << device_target << "'," << '\n'
            << "but that target is already registered (a compiled-in marshaller or another module"
            << " owns it)." << '\n'
            << "One problem per process means at most one marshaller per target." << '\n'
        );
    }
}

/** @brief Registers one CONSUMER contribution's provider into consumerProviderStore() under the
 *  consumer's mnemonic. A mnemonic already held (a built-in consumer or another module) is a hard
 *  error.
 *  @param contribution The contribution whose factory is called
 *  @param context The module, for the messages
 *  @param noun What the contribution is, for the messages */
void registerConsumerContribution(
    const GenevaContribution &contribution,
    const GContributionContext &context,
    std::string_view noun
) {
    auto const provider = takeContribution<GConsumerProviderPtr>(contribution, context, noun);
    const std::string mnemonic = provider->getMnemonic();
    if(not consumerProviderStore()->setOnce(mnemonic, provider)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::loadModule(): Error!" << '\n'
            << "The module " << context.description << " contributes a consumer with mnemonic '"
            << mnemonic << "'," << '\n'
            << "but that mnemonic is already registered (a built-in consumer or another module"
            << " owns it)." << '\n'
        );
    }
}

} // namespace

/******************************************************************************/
/**
 * Loads a module with Geneva's four handlers. The claim-once individual and the three counters are
 * captures of handlers that live for this one call.
 *
 * @param module_path The module
 * @return What it contributed
 */
LoadedModule loadModuleWithGenevaHandlers(const std::filesystem::path &module_path) {
    LoadedModule result;
    std::uint32_t n_individuals = 0;

    const std::array<GContributionHandler, 4> handlers{{
        {.kind = GENEVA_CONTRIBUTION_INDIVIDUAL,
         .noun = "individual",
         .check =
             [&n_individuals](
                 const GenevaContribution & /*contribution*/,
                 const GContributionContext &context
             ) {
                 if(++n_individuals > 1) {
                     throw geneva_exception(
                         g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                         << "In Gem::Geneva::loadModule(): Error!" << '\n'
                         << "The module " << context.description
                         << " contributes more than one individual;"
                         << " a module may contribute at most one (one problem per process)."
                         << '\n'
                     );
                 }
             },
         .take =
             [&result](
                 const GenevaContribution &contribution,
                 const GContributionContext &context
             ) {
                 result.individual =
                     takeContribution<GIndividualFactoryPtr>(contribution, context, "individual");
             }},
        {.kind = GENEVA_CONTRIBUTION_OA,
         .noun = "optimization algorithm",
         .check = {},
         .take =
             [&result](
                 const GenevaContribution &contribution,
                 const GContributionContext &context
             ) {
                 registerOAContribution(contribution, context, "optimization algorithm");
                 ++result.oa_count;
             }},
        {.kind = GENEVA_CONTRIBUTION_MARSHALLER,
         .noun = "GPU marshaller",
         .check = {},
         .take =
             [&result](
                 const GenevaContribution &contribution,
                 const GContributionContext &context
             ) {
                 registerMarshallerContribution(contribution, context, "GPU marshaller");
                 ++result.marshaller_count;
             }},
        {.kind = GENEVA_CONTRIBUTION_CONSUMER,
         .noun = "consumer",
         .check = {},
         .take = [&result](
                     const GenevaContribution &contribution,
                     const GContributionContext &context
                 ) {
             registerConsumerContribution(contribution, context, "consumer");
             ++result.consumer_count;
         }},
    }};

    Gem::Common::loadModule(module_path, handlers);
    return result;
}

/******************************************************************************/
/**
 * Loads a module into a Geneva program: the optional consumer modules first, then the module itself
 * with Geneva's handlers.
 *
 * @param module_path The module
 * @return What it contributed
 */
LoadedModule loadModule(const std::filesystem::path &module_path) {
    // Before the loader's lock is taken, and never from inside it: see GModuleLoaderInternal.hpp.
    loadOptionalConsumerModules();
    return loadModuleWithGenevaHandlers(module_path);
}

/******************************************************************************/

} /* namespace Gem::Geneva */
