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
 * @brief A deliberately broken module: a consumer contribution for an item type that is not
 * Geneva's.
 *
 * Part of the loader's negative-path coverage (see GGenomeTests.cpp, "[plugin][reject]"). The
 * loader casts what a factory returns without a check, so a consumer provider built for another
 * item type would be used as one for GGenome. Its manifest entry names the item type (the
 * subject), and a Geneva program must refuse the contribution by that name BEFORE it calls the
 * factory: the factory here ends the process, so a test that reaches it fails loudly.
 */

// Standard headers go here
#include <cstdlib>
#include <typeinfo>

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp"

namespace Gem::Tests {

/** @brief An item type of somebody else's: the subject of this module's consumer. */
struct GForeignItem {};

} /* namespace Gem::Tests */

namespace {

/** @brief The factory must never be called: the contribution is to be refused before.
 *  @return Nothing; the process ends */
void *abortingFactory() {
    std::abort();
}

const GemContribution CONTRIBUTION{
    .kind = "courtier.consumer",
    .name_or_mnemonic = "foreign",
    .make_factory = abortingFactory,
    .subject = typeid(Gem::Tests::GForeignItem).name()
};

// GEM_MODULE_ABI_STAMP is a positional initializer prefix, so the members cannot be designated.
// NOLINTNEXTLINE(modernize-use-designated-initializers)
const GemModuleManifest
    MANIFEST{GEM_MODULE_ABI_STAMP, "GForeignSubjectModule", GEM_VERSION_STRING, &CONTRIBUTION, 1u};

} // anonymous namespace

// The entry point's name is fixed by the module scheme (common/GModuleManifest.hpp).
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest();
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest() {
    return &MANIFEST;
}
