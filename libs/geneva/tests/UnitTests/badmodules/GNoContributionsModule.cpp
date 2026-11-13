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
 * @brief A deliberately broken module: a valid manifest that advertises nothing.
 *
 * Part of the loader's negative-path coverage (see GGenomeTests.cpp, "[plugin][reject]"). This is what a
 * manifest whose contribution list was never filled in looks like -- a build accident that used to load
 * cleanly and then leave the caller wondering why its problem or algorithm was missing.
 */

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp"

namespace {

const GenevaModuleManifest g_manifest{
    GENEVA_MODULE_ABI_STAMP, "GNoContributionsModule", GENEVA_VERSION_STRING, nullptr, 0u};

} // anonymous namespace

extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return &g_manifest;
}
