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

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp" // GENEVA_VERSION (the ABI token)

// Standard headers go here
#include <cstddef>
#include <cstdint>
#include <memory>

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GFactoryT.hpp"
#include "common/GModuleManifest.hpp" // the unified module manifest (GenevaCompat + contributions)
#include "geneva/genome/GGenome.hpp"

namespace Gem::Geneva {

/******************************************************************************/
/** @brief The factory type a plugin hands back: the same content-creator type Go2::registerContentCreator
 *  accepts, so a loaded problem is indistinguishable from a compiled-in one downstream. */
using GIndividualFactoryPtr = std::shared_ptr<Gem::Common::GFactoryT<Genome::GGenome>>;

/** @brief The compile-time fixed string NTTP the manifest helpers use (shared, in common). */
using Gem::Common::GFixedString;

/******************************************************************************/
/**
 * @brief Builds the module manifest for a runtime-loadable individual, entirely in C++ (no macro).
 *
 * A loadable individual's source hand-writes ONE small entry point that delegates here:
 * @code
 *   extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
 *       return Gem::Geneva::individualManifest<
 *           Gem::Geneva::Genome::GIndividualFactory<MyProblem>,
 *           "./config/MyProblem.json", "MyProblem">();
 *   }
 * @endcode
 * The @c extern @c "C" wrapper is irreducible: the loader resolves the fixed, unmangled symbol
 * @c geneva_module_manifest via a dlsym-style lookup, which no template can synthesize. Everything else is
 * this typed template.
 *
 * The returned manifest carries this module's @c GenevaCompat fingerprint (which the loader validates BEFORE
 * touching any C++ contribution) and a single INDIVIDUAL contribution whose factory thunk hands back a
 * heap-allocated content-creator factory as a @c void* (so the boundary stays plain C); the loader
 * moves-from and deletes it. The statics live for the process; the .so is kept resident by the loader. This
 * function does NOT emit the individual's @c GEM_REGISTER_ARCHIVABLE -- the individual's own translation
 * unit carries it (the same registration a compiled-in individual needs), so there is no double
 * registration.
 *
 * @tparam FactoryType The content-creator factory (a GFactoryT<GGenome>, e.g.
 *         GIndividualFactory<MyProblem>)
 * @tparam Config The factory's configuration-file path (a string literal), passed to its constructor
 * @tparam Name   The module/contribution name, for diagnostics (a string literal)
 * @return A pointer to this module's process-lifetime manifest
 */
template <typename FactoryType, GFixedString Config, GFixedString Name>
const GenevaModuleManifest *individualManifest() {
    // Captureless thunk -> void*(*)(void): construct the factory (with its config path) on the heap; the
    // loader moves-from and deletes it. Config is a template-parameter object, usable without capture.
    static constexpr auto factory_thunk = +[]() -> void * {
        return new GIndividualFactoryPtr(std::make_shared<FactoryType>(Config.c_str()));
    };
    static const GenevaContribution contribution{GENEVA_CONTRIBUTION_INDIVIDUAL, Name.c_str(), factory_thunk};
    static const GenevaModuleManifest manifest{
        GENEVA_MODULE_ABI_STAMP, Name.c_str(), GENEVA_VERSION_STRING, &contribution, 1u};
    return &manifest;
}

/******************************************************************************/

} /* namespace Gem::Geneva */

/******************************************************************************/
