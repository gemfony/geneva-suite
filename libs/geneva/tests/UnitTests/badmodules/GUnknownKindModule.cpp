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
 * @brief A deliberately broken module: it advertises a contribution kind this Geneva does not serve.
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

/** A kind number no Geneva has ever defined. */
constexpr std::uint32_t UNKNOWN_KIND = 99u;

const GenevaContribution g_contribution{
    UNKNOWN_KIND, "SomethingElse", []() -> void * { return nullptr; }};

const GenevaModuleManifest g_manifest{
    GENEVA_MODULE_ABI_STAMP, "GUnknownKindModule", GENEVA_VERSION_STRING, &g_contribution, 1u};

} // anonymous namespace

extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return &g_manifest;
}
