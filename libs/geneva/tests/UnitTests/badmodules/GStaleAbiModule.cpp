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
 * @brief A deliberately broken module: its ABI stamp is one ahead of this Geneva's.
 *
 * Part of the loader's negative-path coverage (see GGenomeTests.cpp, "[plugin][reject]"). It is a real
 * shared object that really is dlopened, so the rejection is proved through the whole loader path rather
 * than against a hand-made in-process struct. Everything about it is valid -- its toolchain fingerprint is
 * this build's -- EXCEPT the module-ABI version, which is what a module built against a different
 * common/GModuleManifest.hpp would look like. Nothing here is ever reached: the loader must refuse it
 * before it reads a single field behind the stamp.
 */

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp"

namespace {

const GenevaContribution g_contribution{
    GENEVA_CONTRIBUTION_INDIVIDUAL, "StaleAbi", []() -> void * { return nullptr; }};

const GenevaModuleManifest g_manifest{
    GENEVA_BUILD_FINGERPRINT,
    GENEVA_MODULE_ABI_VERSION + 1u, // the defect: built against a later module ABI
    static_cast<std::uint32_t>(sizeof(GenevaModuleManifest)),
    "GStaleAbiModule",
    GENEVA_VERSION_STRING,
    &g_contribution,
    1u};

} // anonymous namespace

extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return &g_manifest;
}
