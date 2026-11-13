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
#include <memory>
#include <string_view>
#include <tuple>

// Boost headers go here

// Geneva headers go here
#include "common/GExpectationChecksT.hpp" // GToken / expectation / compare_base_t
#include "courtier/GCourtierEnums.hpp"    // GSubmissionPolicy
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOptimizationAlgorithmT.hpp"

#include "GRandomSearch_PersonalityTraits.hpp"

namespace Gem::Common {
class GParserBuilder;
} /* namespace Gem::Common */

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A minimal, real optimization algorithm: uniform random search.
 *
 * Each iteration it re-initializes every individual to a fresh random point in its parameter space, has the
 * whole population evaluated through the process consumer, and reports this iteration's best; the base class
 * tracks the best-ever across iterations. It is deliberately trivial -- its purpose is to be a genuine,
 * working algorithm that ships as a RUNTIME-LOADABLE module (a .so), demonstrating that an optimization
 * algorithm can be authored, compiled and distributed entirely outside the Geneva library and selected at
 * run time by its mnemonic ("rsearch"), exactly like a built-in.
 *
 * It derives directly from the CRTP scaffold @c GOptimizationAlgorithmT<GRandomSearch> (no parent/child
 * structure), which generates @c clone_() / @c name_() / @c getAlgorithmName_() /
 * @c getAlgorithmPersonalityType_() from the three @c oa_* identifiers below. The only state it adds beyond
 * the base is the population size (kept in the base via @c setDefaultPopulationSize()), so it carries no
 * local serialized data of its own.
 */
class GRandomSearch // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GRandomSearch> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief This algorithm carries no local data of its own; the explicit empty member list lets the
     *  Gem::Common::GReflectiveInterfaceT base generate serialize()/load_()/compare_()/name_()/clone_() (the base
     *  slice carries the population, its size and the halt criteria). */
    template <typename Self>
    auto localMembers_(this Self &self) {
        (void)self;
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold to generate name_(),
    // getAlgorithmName_() and getAlgorithmPersonalityType_().
    static constexpr std::string_view class_name = "GRandomSearch";
    static constexpr std::string_view oa_algorithm_name = "Random Search";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_RSEARCH";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GRandomSearch_PersonalityTraits;

    /** @brief The default constructor; sets a default population size. */
    GRandomSearch();
    /** @brief The copy constructor (no local data beyond the base).
     *  @param cp Another GRandomSearch object to be copied */
    GRandomSearch(const GRandomSearch &cp) = default;
    /** @brief The destructor */
    ~GRandomSearch() override = default;

protected:
    /** @brief Adds local configuration options to a GParserBuilder object (the population size).
     *  @param gpb The parser builder to which the configuration options are added */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceT base.

private:
    /** @brief Random search cannot tolerate a missing/failed evaluation: submit under full-success-or-fatal.
     *  @return The full-success-or-fatal submission policy */
    Gem::Courtier::GSubmissionPolicy getSubmissionPolicy_() const override;

    /** @brief The per-iteration business logic: re-randomize the whole population, evaluate it, report the
     *  iteration's best.
     *  @return A tuple holding the best raw and transformed fitness achieved this iteration */
    std::tuple<double, double> cycleLogic_() override;
    /** @brief Triggers fitness calculation of the whole population through the process consumer. */
    void evaluatePopulation_() override;
    /** @brief Retrieves the number of processable items for the current iteration (the whole population).
     *  @return The number of items that can be processed in the current iteration */
    std::size_t getNProcessableItems_() const override;
    /** @brief Resizes the population to the configured size by cloning the single seed individual. */
    void adjustPopulation_() override;
    /** @brief Retrieves a fresh personality-traits object for this algorithm.
     *  @return A shared pointer to a new GRandomSearch_PersonalityTraits object */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

