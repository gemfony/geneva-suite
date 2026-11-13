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
 * @brief Turns GLoadableParaboloid into a runtime-loadable Geneva individual plugin.
 *
 * This whole translation unit is the "glue" a problem author writes to make an existing individual
 * loadable at runtime. It is exactly TWO pieces of registration:
 *
 *  1. GEM_REGISTER_ARCHIVABLE(GLoadableParaboloid): registers the individual's archive tag so it can
 *     cross the networked wire and a checkpoint (the SAME registration a compiled-in individual needs; it
 *     lives here so it is compiled into the .so). Every node that (de)serializes this individual must have
 *     this plugin loaded -- which, since server and client are the same binary loading the same .so, is
 *     automatic.
 *
 *  2. The fixed entry point geneva_module_manifest(): a small extern "C" wrapper (the loader resolves this
 *     unmangled symbol via dlsym) delegating to the typed helper individualManifest<Factory, Config, Name>(),
 *     which builds the module manifest -- the toolchain-compatibility fingerprint the loader validates first,
 *     plus one INDIVIDUAL contribution whose factory is the standard GIndividualFactory<Derived>. That is
 *     the entire author-facing surface: no Geneva macro, ordinary C++.
 */

#include <boost/config.hpp>              // BOOST_SYMBOL_EXPORT

#include "common/GModuleManifest.hpp" // GenevaModuleManifest
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GIndividualPlugin.hpp" // Gem::Geneva::individualManifest<>

#include "GLoadableParaboloid.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE

//! [modules-individual-glue#1]
GEM_REGISTER_ARCHIVABLE(GLoadableParaboloid) // NOLINT
//! [modules-individual-glue#1]

// (1) The archive tag for wire / checkpoint transport of this individual.

// (2) The module entry point. The extern "C" wrapper is the only irreducible boilerplate (fixed symbol name
// for the loader's dlsym); everything else is the typed individualManifest<> helper. The template arguments
// are the content-creator factory (the standard flat-individual factory on the problem type), the problem's
// config-file path (auto-created with the individual's defaults if absent), and a display name.
//! [modules-individual-glue#2]
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return Gem::Geneva::individualManifest<Gem::Geneva::Genome::GIndividualFactory<GLoadableParaboloid>,
                                           "./config/GLoadableParaboloid.json", "GLoadableParaboloid">();
}
//! [modules-individual-glue#2]
