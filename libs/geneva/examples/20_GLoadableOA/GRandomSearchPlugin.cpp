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
 * The glue that turns GRandomSearch into a runtime-loadable Geneva module. It carries the two irreducible
 * pieces an OA plugin must provide:
 *   1. the GEM_REGISTER_ARCHIVABLE registrations for the algorithm and its personality traits (the same
 *      serialization registrations a compiled-in OA needs -- so a checkpoint written with this module loaded
 *      resumes with it loaded);
 *   2. the fixed, unmangled `geneva_module_manifest` entry point the loader resolves via dlsym, which
 *      delegates to the typed helper Gem::Geneva::oaManifest<>() to describe the single OA contribution.
 * The module links NO Geneva or Boost libraries; its symbols resolve at dlopen against the host process.
 */

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "geneva/oa/GOAPlugin.hpp" // Gem::Geneva::oaManifest
#include "geneva/oa/GOptimizationAlgorithmFactoryT.hpp" // the whole factory this module needs

// The algorithm shipped by this module
#include "GRandomSearch.hpp"
#include "GRandomSearch_PersonalityTraits.hpp"

// Serialization registrations for the algorithm and its personality traits (checkpoint payloads).
//! [modules-oa-glue#1]
GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GRandomSearch) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GRandomSearch_PersonalityTraits) // NOLINT
//! [modules-oa-glue#1]

/**
 * @brief The module manifest entry point. Resolved by the loader (Gem::Geneva::openModule / loadModule) via
 * a dlsym-style lookup of the fixed, unmangled symbol; delegates to the typed oaManifest<> helper.
 *
 * @return This module's process-lifetime manifest (a single OA contribution)
 */
//! [modules-oa-glue#2]
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    // The factory is the GOptimizationAlgorithmFactoryT scaffold instantiated for this algorithm, spelled
    // right here -- it generates the config path, getMnemonic(), getAlgorithmName(), getObject_() and the
    // constructors, and a factory IS the provider the algorithm store holds, so nothing wraps it. An
    // algorithm that needs more from its factory -- extra command-line options, a postProcess_() step --
    // derives a class from this instantiation instead (see GParameterScanFactory in the Geneva library).
    return Gem::Geneva::oaManifest<
        Gem::Geneva::OptimizationAlgorithms::GOptimizationAlgorithmFactoryT<
            Gem::Geneva::OptimizationAlgorithms::GRandomSearch>,
        "GRandomSearch">();
}
//! [modules-oa-glue#2]
