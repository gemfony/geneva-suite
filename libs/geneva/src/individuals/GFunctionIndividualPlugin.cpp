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
 * @brief Module "glue" that makes GFunctionIndividual a runtime-loadable Geneva individual.
 *
 * Compiled into the module .so ONLY (never the object library): it holds the fixed extern "C" entry point
 * geneva_module_manifest(), whose symbol name must be unique per process. It does NOT re-emit the
 * individual's GEM_REGISTER_ARCHIVABLE -- those (GFunctionIndividual and the three demo constraints)
 * live once in GFunctionIndividual.cpp, compiled into both the module and any compile-in consumer.
 * GFunctionIndividual is the canonical Tier-2 (config-driven) demo individual: the manifest hands back the
 * standard flat-individual factory on it.
 */

#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

#include "common/GModuleManifest.hpp" // GenevaModuleManifest
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GIndividualPlugin.hpp" // Gem::Geneva::individualManifest<>

#include "geneva/individuals/GFunctionIndividual.hpp"

//! [ind-module-manifest]
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return Gem::Geneva::individualManifest<
        Gem::Geneva::Genome::GIndividualFactory<Gem::Geneva::Individuals::GFunctionIndividual>,
        "./config/GFunctionIndividual.json", "GFunctionIndividual">();
}
//! [ind-module-manifest]
