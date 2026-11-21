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
 * @brief A deliberately broken module: it advertises a contribution kind no program accepts.
 *
 * Part of the loader's negative-path coverage (see GGenomeTests.cpp, "[plugin][reject]"). Its stamp and
 * fingerprint are correct, so both gates pass and the defect is found during the contribution walk. The
 * point of the test is that such a contribution is REFUSED by name, not skipped: a silently dropped
 * contribution leaves the user hunting for a mnemonic that never appears.
 */

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp"

namespace {

/** A kind no library of the collection defines. */
constexpr const char *UNKNOWN_KIND = "nowhere.something";

const GemContribution CONTRIBUTION{
    .kind = UNKNOWN_KIND,
    .name_or_mnemonic = "SomethingElse",
    .make_factory = []() -> void * { return nullptr; }
};

// GEM_MODULE_ABI_STAMP is a positional initializer prefix, so the members cannot be designated.
// NOLINTNEXTLINE(modernize-use-designated-initializers)
const GemModuleManifest
    MANIFEST{GEM_MODULE_ABI_STAMP, "GUnknownKindModule", GEM_VERSION_STRING, &CONTRIBUTION, 1u};

} // anonymous namespace

// The entry point's name is fixed by the module scheme (common/GModuleManifest.hpp).
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest();
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest() {
    return &MANIFEST;
}
