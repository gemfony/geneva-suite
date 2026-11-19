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
 * @brief A test module with two good contributions of kinds no library of the collection defines.
 *
 * Used by GModuleLoaderTests to watch common's loader without Geneva: which of the two
 * contributions is "bad" is decided by the handlers a test passes (no handler for its kind, or a
 * check that refuses it). Its stamp and fingerprint are correct, so both gates pass. Each factory
 * hands over a GTestPayloadPtr that holds the number of the contribution (1 or 2).
 */

// Standard header files go here
#include <memory>

// Boost header files go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva header files go here
#include "common/GModuleManifest.hpp"
#include "modules/GTwoContributionsModule.hpp"

namespace {

// A plain-C array, as the manifest takes one
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
const GemContribution CONTRIBUTIONS[] = {
    {.kind = Gem::Tests::TEST_KIND_FIRST,
     .name_or_mnemonic = "first",
     .make_factory = []() -> void * {
         return new Gem::Tests::GTestPayloadPtr(std::make_shared<int>(1));
     }},
    {.kind = Gem::Tests::TEST_KIND_SECOND,
     .name_or_mnemonic = "second",
     .make_factory = []() -> void * {
         return new Gem::Tests::GTestPayloadPtr(std::make_shared<int>(2));
     }},
};

// Written as the other hand-written test manifests are: GEM_MODULE_ABI_STAMP is a positional
// initializer prefix, so the members cannot be designated.
// NOLINTNEXTLINE(modernize-use-designated-initializers)
const GemModuleManifest MANIFEST{
    GEM_MODULE_ABI_STAMP,
    "GTwoContributionsModule",
    GEM_VERSION_STRING,
    CONTRIBUTIONS,
    2u
};

} // anonymous namespace

// The entry point's name is fixed by the module scheme (common/GModuleManifest.hpp).
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest();
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest() {
    return &MANIFEST;
}
