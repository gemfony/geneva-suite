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
 * @brief Module "glue" that makes GLineFitIndividual a runtime-loadable Geneva individual.
 *
 * This translation unit is compiled into the module .so ONLY (never the object library): it holds the fixed
 * extern "C" entry point geneva_module_manifest(), whose symbol name is fixed for the loader's dlsym and so
 * must be unique per process. It deliberately does NOT re-emit the individual's GEM_REGISTER_ARCHIVABLE --
 * that lives once in GLineFitIndividual.cpp (the individual's own, compiled into both the module and any
 * compile-in consumer), so there is exactly one archive-tag registration per type.
 *
 * The manifest itself is built by the typed helper individualManifest<Factory, Config, Name>(): the factory
 * is the standard config-driven flat-individual factory on GLineFitIndividual (which reads its data-point
 * file from the named config), the config path is auto-created with the individual's defaults if absent, and
 * the name is used for diagnostics.
 */

#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

#include "common/GModuleManifest.hpp" // GenevaModuleManifest
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GIndividualPlugin.hpp" // Gem::Geneva::individualManifest<>

#include "geneva/individuals/GLineFitIndividual.hpp"

// The module entry point. The extern "C" wrapper is the only irreducible boilerplate (fixed symbol name for
// the loader's dlsym); everything else is the typed individualManifest<> helper.
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return Gem::Geneva::individualManifest<
        Gem::Geneva::Genome::GIndividualFactory<Gem::Geneva::Individuals::GLineFitIndividual>,
        "./config/GLineFitIndividual.json", "GLineFitIndividual">();
}
