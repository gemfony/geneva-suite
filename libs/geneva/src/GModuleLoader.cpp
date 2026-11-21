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
#include <typeinfo>

// Boost headers go here
#include <boost/core/demangle.hpp>

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GModuleLoader.hpp"
#include "common/GModuleManifest.hpp"
#include "common/GProviderT.hpp"
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
    const GemContribution &contribution,
    const GContributionContext &context,
    std::string_view noun
) {
    auto const provider = takeContribution<GOAProviderPtr>(contribution, context, noun);
    const std::string mnemonic = provider->getMnemonic();
    if(not registerOptimizationAlgorithm(provider)) {
        throw gem_exception(
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
    const GemContribution &contribution,
    const GContributionContext &context,
    std::string_view noun
) {
    auto const provider = takeContribution<GMarshallerProviderPtr>(contribution, context, noun);
    const std::string device_target = provider->getMnemonic();
    if(not Gem::Courtier::GPU::marshallerProviderStore()->setOnce(device_target, provider)) {
        throw gem_exception(
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

} // namespace

/******************************************************************************/
/**
 * The record of what was taken is a capture of the handlers, which live as long as their caller
 * keeps them.
 */
std::vector<GContributionHandler> genevaContributionHandlers(LoadedModule &taken) {
    return {
        {.kind = INDIVIDUAL_CONTRIBUTION_KIND,
         .noun = "individual",
         .check =
             [&taken, seen = std::make_shared<std::filesystem::path>()](
                 const GemContribution & /*contribution*/,
                 const GContributionContext &context
             ) {
                 // a second individual: in another module, or twice in this one
                 const bool twice = *seen == context.path;
                 *seen = context.path;
                 if(taken.individual or twice) {
                     throw gem_exception(
                         g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                         << "In Gem::Geneva::loadModule(): Error!" << '\n'
                         << "The module " << context.description
                         << " contributes a second individual;"
                         << " a program has one optimization problem." << '\n'
                     );
                 }
             },
         .take =
             [&taken](const GemContribution &contribution, const GContributionContext &context) {
                 taken.individual =
                     takeContribution<GIndividualFactoryPtr>(contribution, context, "individual");
                 taken.individual_module = context.path;
             }},
        {.kind = OA_CONTRIBUTION_KIND,
         .noun = "optimization algorithm",
         .check = {},
         .take =
             [&taken](const GemContribution &contribution, const GContributionContext &context) {
                 registerOAContribution(contribution, context, "optimization algorithm");
                 ++taken.oa_count;
             }},
        {.kind = MARSHALLER_CONTRIBUTION_KIND,
         .noun = "GPU marshaller",
         .check = {},
         .take =
             [&taken](const GemContribution &contribution, const GContributionContext &context) {
                 registerMarshallerContribution(contribution, context, "GPU marshaller");
                 ++taken.marshaller_count;
             }},
    };
}

/******************************************************************************/
/**
 * Loads a module into a Geneva program with Geneva's handlers.
 */
LoadedModule loadModule(const std::filesystem::path &module_path) {
    LoadedModule result;
    Gem::Common::loadModule(module_path, genevaContributionHandlers(result));
    return result;
}

/******************************************************************************/

} /* namespace Gem::Geneva */
