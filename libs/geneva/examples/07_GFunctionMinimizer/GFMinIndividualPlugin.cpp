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
 * @brief Module "glue" that makes GFMinIndividual a runtime-loadable Geneva individual.
 *
 * This example ships its problem as a loadable module and runs it with the generic optimizer
 * (example 18's GGenericOptimizer) via --individual, instead of a bespoke main -- the load-from-disk model.
 * This translation unit holds only the fixed extern "C" entry point geneva_module_manifest(); it does NOT
 * re-emit GFMinIndividual's archive registration (that lives in GFMinIndividual.cpp).
 */

#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

#include "common/GModuleManifest.hpp" // GenevaModuleManifest
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GIndividualPlugin.hpp" // Gem::Geneva::individualManifest<>

#include "GFMinIndividual.hpp"

extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return Gem::Geneva::individualManifest<
        Gem::Geneva::Genome::GIndividualFactory<Gem::Geneva::GFMinIndividual>,
        "./config/GFMinIndividual.json", "GFMinIndividual">();
}
