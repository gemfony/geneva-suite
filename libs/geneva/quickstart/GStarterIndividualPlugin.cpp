/**
 * @file
 * @brief Module "glue" that makes GStarterIndividual a runtime-loadable Geneva individual.
 *
 * This is the entire author-facing surface for making an existing individual loadable at run time: the fixed
 * extern "C" entry point gem_module_manifest(), delegating to the typed helper
 * individualManifest<Factory, Config, Name>() (from the INSTALLED Geneva header geneva/genome/GIndividualPlugin.hpp).
 * The individual's archive tag (GEM_REGISTER_ARCHIVABLE) lives in GStarterIndividual.cpp, which is
 * the DECLARE's SOURCES (compiled into both the compiled-in object library and the loadable module), so this
 * translation unit carries ONLY the manifest -- no archive registration here, or it would be a double
 * registration.
 *
 * Compiled into the module .so only (never the object library): the manifest symbol name is fixed, so a
 * compile-in binary that links several individuals must not contain two definitions of it.
 */

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

#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

#include "common/GModuleManifest.hpp" // GemModuleManifest
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GIndividualPlugin.hpp" // Gem::Geneva::individualManifest<>

#include "GStarterIndividual.hpp"

extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest() {
    return Gem::Geneva::individualManifest<
        Gem::Geneva::Genome::GIndividualFactory<Gem::Geneva::GStarterIndividual>,
        "./config/GStarterIndividual.json",
        "GStarterIndividual">();
}
