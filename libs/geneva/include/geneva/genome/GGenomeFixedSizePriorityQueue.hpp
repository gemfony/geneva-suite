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

// Standard header files go here
#include <memory>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GReflectiveInterfaceT.hpp"
#include "common/GFixedSizePriorityQueueT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/GenevaHelperFunctions.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
/**
 * @brief The archive of best individuals an optimization algorithm keeps.
 *
 * A fixed-size priority queue over GGenome objects, ordered by the min-only
 * transformed fitness (optimization algorithms only understand minimization, so
 * "lower is better" is this queue's only mode of operation).
 *
 * The archive **solely owns** its individuals: it is a
 * Gem::Common::GUniquePtrFixedSizePriorityQueueT, matching the unique_ptr-owned
 * population it is fed from, so no handle ever crosses an ownership boundary. An
 * algorithm that wants its population recorded calls addClone() — the archive then
 * holds independent copies that later generations cannot mutate underneath it — and
 * an algorithm that genuinely hands an individual over calls add() with an rvalue.
 *
 * Only individuals that have actually been processed are admitted; that rule lives
 * in the isValid() override, which is the base class's single admission gate.
 */
class GGenomeFixedSizePriorityQueue // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Common::GReflectiveInterfaceT<
        GGenomeFixedSizePriorityQueue,
        Gem::Common::GUniquePtrFixedSizePriorityQueueT<GGenome>> {
    ///////////////////////////////////////////////////////////////////////
    // Gem::Weft::access default-constructs this concrete type on load;
    // GReflectiveInterfaceAccess lets the mixin reach the (empty) localMembers_().
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief This class adds no own members; the explicit empty declaration is required
     *  (the mixin's deleted fallback rejects a missing one). */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GGenomeFixedSizePriorityQueue";

    /** @brief The default constructor */
    GGenomeFixedSizePriorityQueue() = default;

    /**
     * @brief Initialization with the maximum size
     *
     * @param max_size The maximum number of items the priority queue is allowed to hold
     */
    explicit GGenomeFixedSizePriorityQueue(const std::size_t & max_size);
    /**
     * @brief The copy constructor
     *
     * @param cp A constant reference to another GGenomeFixedSizePriorityQueue object to be copied
     */
    GGenomeFixedSizePriorityQueue(const GGenomeFixedSizePriorityQueue &cp) = default;
    /** @brief The destructor */
    ~GGenomeFixedSizePriorityQueue() override = default;

    /**
     * @brief Checks whether no item has the dirty flag set
     *
     * @param pos If a dirty item is found, this is set to the position of the first such item
     * @return true if no item has its dirty flag set, false otherwise
     */
    bool allClean(std::size_t & pos) const;
    /**
     * @brief Emits information about the "dirty flag" of all items
     *
     * @return A string describing the dirty-flag status of all items held by the queue
     */
    [[nodiscard]] std::string getCleanStatus() const;

protected:
    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and the empty localMembers_().

    /***************************************************************************/
    /**
     * @brief The archive's admission rule: only individuals that carry a result belong in it
     *
     * @param item_ptr A constant reference to the individual to be checked
     * @return true if the individual exists and has been processed, false otherwise
     */
    [[nodiscard]] bool isValid(const std::unique_ptr<GGenome> & item_ptr) const override;
    /**
     * @brief Evaluates a single work item, so that it can be sorted
     *
     * @param item_ptr A constant reference to the individual to be evaluated
     * @return The fitness value of the item used as the sorting criterion within the priority queue
     */
    [[nodiscard]] double evaluation(const std::unique_ptr<GGenome> & item_ptr) const override;

};

/******************************************************************************/

} /* namespace Gem::Geneva::Genome */

