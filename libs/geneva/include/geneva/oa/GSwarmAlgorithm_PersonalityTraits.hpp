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

// Boost headers go here

// Geneva headers go here
#include "geneva/genome/GGenome.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "common/GSelfTestable.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * This class adds variables and functions to GPersonalityTraits that are specific
 * to swarm optimization.
 */
class GSwarmAlgorithm_PersonalityTraits // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Common::GReflectiveInterfaceT<GSwarmAlgorithm_PersonalityTraits, GPersonalityTraits>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    // Gem::Weft::access default-constructs this concrete type on load;
    // GReflectiveInterfaceAccess lets the base reach localMembers_() for the generated
    // serialize()/load_()/compare_()/name_()/clone_().
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /***************************************************************************/
    /**
     * Single declaration of this class'es local data members. It drives the
     * GReflectiveInterfaceT-generated serialize()/load_()/compare_() from one place.
     * personal_best_ (a std::shared_ptr<gen::GGenome>) is a deep-cloned
     * pointer with no post-load transform, so make_cloneable_member handles it
     * exactly as the former manual tail did (deep clone on load, deep compare).
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("neighborhood_", self.neighborhood_),
            Gem::Common::make_member("no_position_update_", self.no_position_update_),
            Gem::Common::make_member("personal_best_quality_", self.personal_best_quality_),
            Gem::Common::make_cloneable_member("personal_best_", self.personal_best_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GSwarmAlgorithm_PersonalityTraits";

    /** @brief An easy identifier for the class */
    static const std::string nickname; // Initialized in the .cpp definition file

    /** @brief The default constructor */
    GSwarmAlgorithm_PersonalityTraits() = default;
    /**
     * @brief The copy constructor
     *
     * @param cp A constant reference to another GSwarmAlgorithm_PersonalityTraits object to be copied
     */
    GSwarmAlgorithm_PersonalityTraits(const GSwarmAlgorithm_PersonalityTraits & cp);
    /** @brief The standard destructor */
    ~GSwarmAlgorithm_PersonalityTraits() override = default;

    /**
     * @brief Specifies in which neighborhood the individual is at present
     *
     * @param neighborhood The id of the neighborhood the individual is to be assigned to
     */
    void setNeighborhood(const std::size_t & neighborhood);
    /**
     * @brief Retrieves the id of the neighborhood the individual is in at present
     *
     * @return The id of the neighborhood the individual currently belongs to
     */
    [[nodiscard]] std::size_t getNeighborhood() const;

    /** @brief Sets the no_position_update_ flag */
    void setNoPositionUpdate();
    /**
     * @brief Retrieves the current value of the no_position_update_ flag
     *
     * @return true if the individual's position is not to be updated, false otherwise
     */
    [[nodiscard]] bool noPositionUpdate() const;
    /**
     * @brief Retrieves and resets the current value of the no_position_update_ flag
     *
     * @return The value of the no_position_update_ flag prior to being reset to false
     */
    bool checkNoPositionUpdateAndReset();

    /**
     * @brief Allows to add a new personal best to the individual
     *
     * @param p A shared pointer to the GGenome representing the individual's new personal best
     */
    void registerPersonalBest(const std::shared_ptr<gen::GGenome>& p);
    /**
     * @brief Allows to retrieve the personal best individual
     *
     * @return A shared pointer to the GGenome holding the individual's personal best
     */
    [[nodiscard]] std::shared_ptr<gen::GGenome> getPersonalBest() const;
    /** @brief Resets the personal best individual */
    void resetPersonalBest();
    /**
     * @brief Retrieve quality of personally best individual
     *
     * @return A tuple holding the raw and transformed fitness of the personally best individual
     */
    [[nodiscard]] std::tuple<double, double> getPersonalBestQuality() const;

    /**
     * @brief Retrieves the mnemonic of the optimization algorithm
     *
     * @return The mnemonic (short identifier) associated with the swarm optimization algorithm
     */
    [[nodiscard]] std::string getMnemonic() const override;

protected:
    /***************************************************************************/
    // Virtual or overridden protected functions

    /** @brief Applies modifications to this object. This is needed for testing purposes */
    bool modify_GUnitTests_() override;
    /** @brief Performs self tests that are expected to succeed. This is needed for testing purposes */
    void specificTestsNoFailureExpected_GUnitTests_() override;
    /***************************************************************************/

private:
    // serialize(), load_(), compare_(), name_() and clone_() are all generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    /** @brief Stores the neighborhood id the individual currently belongs to */
    std::size_t neighborhood_ = 0;

    /** @brief Determines whether the individual's position should not be updated */
    bool no_position_update_ = false;

    /** @brief Holds the personally best GGenome */
    std::shared_ptr<gen::GGenome> personal_best_;
    /** @brief The quality of the personally best individual */
    std::tuple<double, double> personal_best_quality_{0., 0.};
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */


