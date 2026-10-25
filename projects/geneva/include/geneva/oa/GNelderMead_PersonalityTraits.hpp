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
#include <string>
#include <string_view>
#include <tuple>

// Geneva headers go here
#include "geneva/oa/GAlgorithmPersonalityTraitsT.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * The Nelder-Mead downhill simplex's personality traits: every individual only needs to know its
 * position in the population (its simplex-vertex / trial-slot index), which the
 * GPositionPersonalityTraits base provides; the GAlgorithmPersonalityTraitsT scaffold supplies the
 * shared boilerplate, so this class contributes only the algorithm's identity.
 */
//! [geneva.oa.traits]
class GNelderMead_PersonalityTraits // NOLINT(cppcoreguidelines-special-member-functions)
  : public GAlgorithmPersonalityTraitsT<GNelderMead_PersonalityTraits> {
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceT base (via the generator) reach this
    // stateless class's explicit empty localMembers_() (required by the mixin's deleted fallback).
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief This traits class adds no own state (only its identity). @return An empty member tuple */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }

public:
    /** @brief An easy identifier for the class */
    static const std::string nickname; // Initialized in the .cpp definition file
    /** @brief The name emitted by name_() */
    static constexpr std::string_view class_name{"GNelderMead_PersonalityTraits"};
};
//! [geneva.oa.traits]

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

