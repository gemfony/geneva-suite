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
 * @brief A deliberately broken module: its stamp claims a contribution size that is not this
 * Geneva's.
 *
 * Part of the loader's negative-path coverage (see GGenomeTests.cpp, "[plugin][reject]"). Toolchain
 * fingerprint, ABI version and manifest size are this build's; only the size of a contribution is
 * not, which is what a module built against a GenevaContribution of another layout under the same
 * ABI version would look like. The loader must refuse it with the ABI gate, before it reads a
 * contribution: the factory here ends the process, so a test that reaches it fails loudly.
 */

// Standard headers go here
#include <cstdint>
#include <cstdlib>

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp"

namespace {

/** @brief The factory must never be called: the module is to be refused before.
 *  @return Nothing; the process ends */
void *abortingFactory() {
    std::abort();
}

const GenevaContribution CONTRIBUTION{
    .kind = GENEVA_CONTRIBUTION_INDIVIDUAL,
    .name_or_mnemonic = "WrongSize",
    .make_factory = abortingFactory
};

// Written field by field, as GStaleAbiModule is, so that one of the stamp's fields can be wrong.
const GenevaModuleManifest MANIFEST{
    .compat = GENEVA_BUILD_FINGERPRINT,
    .abi_version = GENEVA_MODULE_ABI_VERSION,
    .manifest_size = static_cast<std::uint32_t>(sizeof(GenevaModuleManifest)),
    .contribution_size = static_cast<std::uint32_t>(sizeof(GenevaContribution) + 8u), // the defect
    .module_name = "GContributionSizeModule",
    .module_version = GENEVA_VERSION_STRING,
    .contributions = &CONTRIBUTION,
    .contributions_count = 1u
};

} // anonymous namespace

// The entry point's name is fixed by the module scheme (common/GModuleManifest.hpp).
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return &MANIFEST;
}
