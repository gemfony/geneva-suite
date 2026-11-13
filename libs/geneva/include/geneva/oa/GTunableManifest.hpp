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

#include "common/GGlobalDefines.hpp"

// Standard headers
#include <string>
#include <vector>

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * One tunable knob of an optimization algorithm, as published by its tunableManifest(). It is the
 * single, self-describing source of truth a meta-optimizer uses to build the genome that searches an
 * algorithm's parameters: each descriptor names a knob, says which value channel carries it, and gives a
 * default initial value and search range. The genome is then built one labelled group per descriptor
 * (the label IS @c name), so parameters are addressed by name rather than by hand-maintained index --
 * inserting or reordering a knob can no longer silently misread a value.
 */
struct TunableParam {
    std::string name;        ///< The knob's name; also the genome group label (name <-> position map)
    bool is_integer = false; ///< true: the int32 value channel; false: the double value channel
    double init = 0.;        ///< Default initial value
    double lower = 0.;       ///< Lower search bound
    double upper = 1.;       ///< Upper search bound
};

/******************************************************************************/
/**
 * The canonical knob names an evolutionary algorithm publishes for meta-optimization. Defined once here
 * (rather than as free strings at every use site) so the manifest, the genome labels and the readers all
 * agree on the spelling. The sigma / ad_prob knobs are RAW: a meta-optimizer encodes them so the genome
 * always carries valid values, and derives the actual adaption settings from them (e.g.
 * max_sigma = min_sigma + sigma_range; start_sigma = min_sigma + sigma_range_pct * sigma_range).
 */
namespace ea_tunable {
inline constexpr const char *n_parents = "n_parents";                 ///< Number of parents (int)
inline constexpr const char *n_children = "n_children";               ///< Number of children (int)
inline constexpr const char *amalgamation = "amalgamation";           ///< Cross-over likelihood
inline constexpr const char *min_ad_prob = "min_ad_prob";             ///< Lower bound of ad_prob
inline constexpr const char *ad_prob_range = "ad_prob_range";         ///< Range of ad_prob above its lower bound
inline constexpr const char *ad_prob_start_pct = "ad_prob_start_pct"; ///< Start of ad_prob within its range [0,1]
inline constexpr const char *adapt_ad_prob = "adapt_ad_prob";         ///< Strength of ad_prob self-adaption
inline constexpr const char *min_sigma = "min_sigma";                 ///< Lower bound of sigma
inline constexpr const char *sigma_range = "sigma_range";             ///< Range of sigma above its lower bound
inline constexpr const char *sigma_range_pct = "sigma_range_pct";     ///< Start of sigma within its range [0,1]
inline constexpr const char *sigma_sigma = "sigma_sigma";             ///< Strength of sigma self-adaption
} /* namespace ea_tunable */

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
