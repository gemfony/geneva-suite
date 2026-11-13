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
#include <tuple>

// Boost headers go here

// Geneva headers go here
#include "common/GReflectiveInterfaceT.hpp"
#include "geneva/oa/GPositionPersonalityTraits.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Personality traits for populations comprising parents and children.
 *
 * Adds variables and functions to GPersonalityTraits that are specific to parent-child
 * populations: a parent re-election counter, the individual's position in the population,
 * and the id of the parent it descended from.
 */
class GBaseParChildPersonalityTraits // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Common::GReflectiveInterfaceT<GBaseParChildPersonalityTraits, GPositionPersonalityTraits> {
    ///////////////////////////////////////////////////////////////////////
    // Gem::Weft::access default-constructs this concrete type on load;
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceT base reach localMembers_().
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
     * @brief Single declaration of this class's local data members (mutable access, for serialization).
     *
     * @return A tuple of named member bindings for parent_counter_ and parent_id_ (the population
     * position lives on the GPositionPersonalityTraits base)
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("parent_counter_", self.parent_counter_),
            Gem::Common::make_member("parent_id_", self.parent_id_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GBaseParChildPersonalityTraits";

    /** @brief The default constructor */
    GBaseParChildPersonalityTraits() = default;
    /** @brief The copy constructor */
    GBaseParChildPersonalityTraits(const GBaseParChildPersonalityTraits &) = default;
    /** @brief The standard destructor */
    ~GBaseParChildPersonalityTraits() override = default;

    /**
     * @brief Marks the individual as a parent (incrementing its parent re-election counter).
     *
     * @return true if the individual was a child before this call (i.e. the status changed)
     */
    bool setIsParent();
    /**
     * @brief Marks the individual as a child (resetting its parent re-election counter).
     *
     * @return true if the individual was a parent before this call (i.e. the status changed)
     */
    bool setIsChild();

    /**
     * @brief Checks whether this is a parent individual.
     *
     * @return true if the individual is currently a parent, false if it is a child
     */
    [[nodiscard]] bool isParent() const;
    /**
     * @brief Retrieves the current value of the parent_counter_ variable.
     *
     * @return The number of consecutive generations this individual has been re-elected as parent (0 if it is a child)
     */
    [[nodiscard]] std::uint32_t getParentCounter() const;

    /**
     * @brief Stores the parent's id with this object.
     *
     * @param parent_id The population position of the parent this individual descended from
     */
    void setParentId(const std::size_t &parent_id);
    /**
     * @brief Retrieves the parent id's value.
     *
     * @return The stored parent id (throws if no parent id has been set)
     */
    [[nodiscard]] std::size_t getParentId() const;
    /**
     * @brief Checks whether a parent id has been set.
     *
     * @return true if a parent id has been recorded, false otherwise
     */
    [[nodiscard]] bool parentIdSet() const;
    /** @brief Marks the parent id as unset. */
    void unsetParentId();

    /**
     * @brief Retrieves the mnemonic of the optimization algorithm.
     *
     * @return The short mnemonic string identifying this personality's optimization algorithm
     */
    [[nodiscard]] std::string getMnemonic() const override;

protected:
    /***************************************************************************/
    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    /**
     * @brief Applies modifications to this object. This is needed for testing purposes.
     *
     * @return true if at least one modification was made
     */
    bool modify_GUnitTests_() override;
    /** @brief Performs self tests that are expected to succeed. This is needed for testing purposes. */
    void specificTestsNoFailureExpected_GUnitTests_() override;
    /** @brief Performs self tests that are expected to fail. This is needed for testing purposes. */
    void specificTestsFailuresExpected_GUnitTests_() override;

    /***************************************************************************/

private:

    /** @brief Allows populations to record how often an individual has been reelected as parent (0 if it is a child) */
    std::uint32_t parent_counter_ = 0;
    /** @brief The id of the old parent individual. This is intentionally a signed value. A negative value refers to an unset parent id */
    std::int16_t parent_id_ = -1;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */


