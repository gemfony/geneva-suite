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
#include "common/GGlobalDefines.hpp"

// Standard headers go here
#include <string_view>

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief The uniform "emit my configuration files" entry point for every config-owning binary.
 *
 * A Geneva binary's registered parameters and their defaults are the single source of truth for its
 * configuration; the shipped JSON files under `config/` are just a materialized cache of what the
 * code already knows. To materialize them at build/install time, a binary is run with the
 * @c --update-configs command-line switch: it puts GParserBuilder into update-in-place mode (so every
 * config it subsequently parses is created-if-absent and rewritten in canonical form), constructs
 * each of its config owners so their parse fires, and exits without optimizing.
 *
 * A driver that owns its program's configs may do this internally (Geneva's @c Go2, for example,
 * consumes @c --update-configs and refreshes every config it owns). These helpers give every other
 * main() (in Geneva: the direct-OA examples, the parameter-object demo, the benchmarks, the manual
 * tests) the exact same behaviour behind the exact same switch, so a single build step can drive
 * @c "<binary> --update-configs" over every config-owning target uniformly.
 *
 * Usage in a main(), once the libraries it uses are initialized and before the binary's own option
 * parser runs:
 * @code
 *   if(Gem::Common::configEmissionRequested(argc, argv)) {
 *       Gem::Common::beginConfigEmission();
 *       // Construct (and, if the parse is lazy, exercise) every config owner this binary uses:
 *       MyConfiguredComponent component("./config/MyConfiguredComponent.json");
 *       Gem::Common::finishConfigEmission(); // logs and exits(0)
 *   }
 * @endcode
 */

/******************************************************************************/
/**
 * @brief The command-line switch that requests configuration emission.
 *
 * The same token a driver such as Geneva's @c Go2 recognises, so a build/install materialization
 * step drives every config-owning binary -- driver-based or not -- through one uniform invocation.
 */
inline constexpr std::string_view CONFIG_EMISSION_FLAG = "--update-configs";

/******************************************************************************/
/**
 * @brief The environment variable that switches configuration emission into "reference" mode.
 *
 * When this variable is set in the environment, beginConfigEmission() additionally suppresses the header's
 * creation timestamp, so the emitted configs are byte-stable across regenerations. The config-reference
 * build target sets it to generate the single, version-controlled reference config tree (a changing
 * timestamp would otherwise show as a spurious diff every time it is regenerated). It is a build-internal
 * mechanism -- users never set it.
 */
inline constexpr std::string_view CONFIG_REFERENCE_ENV = "GEM_CONFIG_REFERENCE";

/******************************************************************************/
/**
 * @brief Reports whether the configuration-emission switch appears in the command line.
 *
 * This is a side-effect-free scan of @p argv for CONFIG_EMISSION_FLAG, meant to be called before the
 * binary's own program-options parser runs (so an emit-only invocation need not carry a full, valid
 * option set).
 */
bool configEmissionRequested(int argc, char **argv);

/******************************************************************************/
/**
 * @brief Enters configuration-emission mode by enabling GParserBuilder update-in-place globally.
 *
 * Call once, before constructing any config owner: every configuration subsequently parsed is then
 * created-if-absent and rewritten in canonical form.
 */
void beginConfigEmission();

/******************************************************************************/
/**
 * @brief Concludes a configuration-emission run: logs completion and exits the process cleanly.
 *
 * Call after every config owner has been constructed (and its parse exercised). Like Geneva's Go2
 * on @c --update-configs, this exits via std::exit(0) so the caller's remaining work is not
 * reached; registered atexit/static teardown (logger flush, RNG factory guard) still runs.
 */
[[noreturn]] void finishConfigEmission();

/******************************************************************************/

} /* namespace Gem::Common */
