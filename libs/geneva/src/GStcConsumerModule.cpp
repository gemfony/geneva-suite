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

/**
 * @file
 * Geneva's consumer module "stc": courtier's shipped provider, built for Geneva's item type GGenome
 * and installed next to libgemfony-geneva, which the launcher loads it from when the consumer is
 * chosen (Gem::Geneva::consumerModules()). It is the default consumer, used when
 * no other is named.
 */

#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

#include "courtier/GConsumerPlugin.hpp"
#include "courtier/consumers/GStdThreadConsumerProviderT.hpp"
#include "geneva/genome/GGenome.hpp"

// The entry point's name is fixed by the module scheme (common/GModuleManifest.hpp).
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest();
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest() {
    return Gem::Courtier::consumerManifest<
        Gem::Courtier::GStdThreadConsumerProviderT<Gem::Geneva::Genome::GGenome>>();
}
