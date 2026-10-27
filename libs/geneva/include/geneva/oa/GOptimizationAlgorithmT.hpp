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

// Geneva headers go here
#include "common/GReflectiveInterfaceT.hpp"           // Gem::Common::GReflectiveInterfaceT
#include "common/GCommonHelperFunctions.hpp"  // Gem::Common::condnotset
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * A CRTP scaffold that generates the boilerplate every concrete optimization algorithm otherwise
 * repeats by hand. The generic quartet -- clone_(), name_(), load_(), compare_() plus the folded
 * serialize() -- comes from Gem::Common::GReflectiveInterfaceT (single-sourced from the class's
 * localMembers_() and class_name, exactly as everywhere else in the tree); this layer only adds the
 * three algorithm-specific overriders (getAlgorithmName_(), getAlgorithmPersonalityType_()) and the
 * three default GUnitTests stubs. A concrete algorithm derives as
 *
 *     class GFoo : public GOptimizationAlgorithmT<GFoo>             // direct-from-base algorithms
 *     class GBar : public GOptimizationAlgorithmT<GBar, GParChild>  // mu/lambda algorithms
 *
 * and supplies its member list (localMembers_()) plus three static string identifiers and the alias
 * naming its personality traits (all public, so this layer, the mixin and the algorithm factory
 * scaffold can read them):
 *
 *     static constexpr std::string_view class_name          = "GFoo";  // read by GReflectiveInterfaceT
 *     static constexpr std::string_view oa_algorithm_name   = "Foo Optimizer";
 *     static constexpr std::string_view oa_personality_type = "PERSONALITY_FOO";
 *     using personality_traits_type = GFoo_PersonalityTraits;  // what getPersonalityTraits_() hands out
 *
 * The mixin holds NO data of its own; clone_() returns a GOptimizationAlgorithmBase pointer (the
 * hierarchy root, the default GReflectiveInterfaceT clone-return). The GUnitTests stubs are ordinary
 * virtuals, so an algorithm with real algorithm-specific tests simply overrides them. An algorithm
 * whose load_() is more than a member-wise copy (e.g. GSwarmAlgorithm's iteration-dependent
 * neighbourhood-best handling) overrides the generated load_() in the usual way.
 *
 * @tparam Derived The concrete algorithm (CRTP).
 * @tparam Parent  The class to derive from -- GOptimizationAlgorithmBase by default, or an
 *                 intermediate such as GParChild for the mu/lambda algorithms.
 */
template <typename Derived, typename Parent = GOptimizationAlgorithmBase>
class GOptimizationAlgorithmT : public Gem::Common::GReflectiveInterfaceT<Derived, Parent> {
    /** @brief The mixin base that generates clone_()/name_()/load_()/compare_()/serialize(). */
    using reflective_interface_t = Gem::Common::GReflectiveInterfaceT<Derived, Parent>;

public:
    /** @brief Inherit the mixin's (and thereby the parent's) constructors. */
    using reflective_interface_t::reflective_interface_t;

    /** @brief The default constructor */
    GOptimizationAlgorithmT() = default;
    /**
     * @brief The copy constructor
     *
     * @param The object to be copied (default member-wise copy).
     */
    GOptimizationAlgorithmT(const GOptimizationAlgorithmT &) = default;
    /** @brief The destructor */
    ~GOptimizationAlgorithmT() override = default;

private:
    /**
     * @brief Returns the human-readable name of this optimization algorithm
     *
     * @return The algorithm's human-readable name, taken from Derived::oa_algorithm_name.
     */
    [[nodiscard]] std::string getAlgorithmName_() const override { return std::string(Derived::oa_algorithm_name); }

    /**
     * @brief Returns the personality-type tag of this optimization algorithm
     *
     * @return The algorithm's personality-type tag, taken from Derived::oa_personality_type.
     */
    [[nodiscard]] std::string getAlgorithmPersonalityType_() const override {
        return std::string(Derived::oa_personality_type);
    }
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
