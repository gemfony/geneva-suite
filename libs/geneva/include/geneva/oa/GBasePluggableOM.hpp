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
#include <tuple>

// Geneva headers go here
#include "common/GReflectiveInterfaceT.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "geneva/GOptimizationEnums.hpp" // infoMode

namespace Gem::Geneva::OptimizationAlgorithms {

// Forward declaration -- GBasePluggableOM only takes a pointer to the algorithm base, so it needs no
// definition. This is the seam that keeps the (large) GOptimizationAlgorithmBase definition out of
// the include set of monitor-only consumers.
class GOptimizationAlgorithmBase;

/*
 * This is a collection of simple pluggable modules suitable for emitting certain specialized
 * information from within optimization algorithms. They can be plugged into GOptimizationAlgorithmBase
 * derivatives. A requirement is that they implement a private function "informationFunction_"
 * according to the API of GBasePluggableOM .
 */

/*******************************************************************************/
/////////////////////////////////////////////////////////////////////////////////
/*******************************************************************************/
/**
 * The base class -- and the CRTP category root -- of all pluggable optimization
 * monitors.
 *
 * The pluggable-monitor hierarchy is its own category root: it derives directly from
 * Gem::Common::GCommonInterfaceT<GBasePluggableOM>, so its pointers are unrelated to
 * those of any other category. The common infrastructure
 * (clone/load/compare/name/IO/serialize) is supplied by the CRTP base, instantiated for
 * this root.
 */
class GBasePluggableOM
  : public Gem::Common::GReflectiveInterfaceBaseT<GBasePluggableOM, Gem::Common::GCommonInterfaceT<GBasePluggableOM>> {
    ///////////////////////////////////////////////////////////////////////
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceBaseT base reach this class's
    // localMembers_(); this abstract root is never reconstructed on load.
    friend struct Gem::Common::GReflectiveInterfaceAccess;

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GBasePluggableOM";

    /***************************************************************************/
    // Defaulted constructors, destructor and assignment operators

    GBasePluggableOM() = default;
    GBasePluggableOM(GBasePluggableOM const &cp) = default;
    GBasePluggableOM(GBasePluggableOM &&cp) = default;

    ~GBasePluggableOM() override = default;

    GBasePluggableOM &operator=(GBasePluggableOM const &) = default;
    GBasePluggableOM &operator=(GBasePluggableOM &&) = default;

    /***************************************************************************/
    /**
     * @brief Public entry point that forwards to the derived-class hook for emitting
     * information about the current iteration.
     *
     * @param infoMode Indicates which optimization phase is active (initialization, the
     *        per-iteration cycle, or finalization)
     * @param GOptimizationAlgorithmBase const *const Non-owning pointer to the algorithm
     *        currently being monitored; the monitor reads its state but does not own it
     */
    void informationFunction(infoMode im, GOptimizationAlgorithmBase const *const goa);

    /**
     * @brief Allows to set the use_raw_evaluation_ variable.
     *
     * @param use_raw If true, the true (unmodified) evaluation is used instead of any
     *        transformed value
     */
    void setUseRawEvaluation(bool use_raw);

    /**
     * @brief Allows to retrieve the value of the use_raw_evaluation_ variable.
     *
     * @return true if the true (unmodified) evaluation is requested, false otherwise
     */
    [[nodiscard]] bool getUseRawEvaluation() const;

protected:
    /************************************************************************/
    /**
     * @brief Single declaration of this class'es local data members.
     *
     * @return A tuple of named members used by the comparison and serialization framework
     */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(Gem::Common::make_member("use_raw_evaluation_", self.use_raw_evaluation_));
    }

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceBaseT base (clone_ stays pure -- this is the abstract
    // category root) from class_name and the localMembers_() declaration above.

    /**
     * @brief Applies modifications to this object. This is needed for testing purposes.
     *
     * @return true if the object was modified, false otherwise
     */
    bool modify_GUnitTests_();
    /***************************************************************************/
    // Data

    bool use_raw_evaluation_ =
        false; ///< Specifies whether the true (unmodified) evaluation should be used

private:
    /**
     * @brief Overload this function in derived classes, specifying actions for initialization,
     * the optimization cycles and finalization (pure virtual).
     *
     * @param infoMode Indicates which optimization phase is active (initialization, the
     *        per-iteration cycle, or finalization)
     * @param GOptimizationAlgorithmBase const *const Non-owning pointer to the algorithm
     *        currently being monitored
     */
    virtual void
    informationFunction_(infoMode, GOptimizationAlgorithmBase const *const) = 0;
};

/*******************************************************************************/
/////////////////////////////////////////////////////////////////////////////////
/*******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

