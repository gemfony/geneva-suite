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
#include <type_traits>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GReflectiveInterfaceT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "geneva/GOptimizationEnums.hpp"

namespace Gem::Geneva {

// Forward declaration
namespace Genome { class GGenome; }

/******************************************************************************/
/**
 * This is the base class of a hierarchy of classes dealing with inter-parameter
 * constraints. Objects representing the template parameter are evaluated for their
 * validity. Note that the classes in this hierarchy are meant to be used PRIOR
 * to the evaluation.
 *
 * @tparam ind_type The individual type to be checked; must derive from Genome::GGenome
 */
template <typename ind_type>
class GPreEvaluationValidityCheckT // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Common::GReflectiveInterfaceBaseT<
        GPreEvaluationValidityCheckT<ind_type>,
        Gem::Common::GCommonInterfaceT<GPreEvaluationValidityCheckT<ind_type>>
    > {
    ///////////////////////////////////////////////////////////////////////
    // GReflectiveInterfaceAccess lets the mixin reach localMembers_(); this abstract root is
    // never reconstructed on load.
    friend struct Gem::Common::GReflectiveInterfaceAccess;
    ///////////////////////////////////////////////////////////////////////

    // A constraint reads the candidate's parameter values via the genome value API (streamlineFP), which
    // lives on the algorithm-facing base GGenome. Every individual derives that base, so the
    // constraint is parameterised on it.
    static_assert(
        std::is_base_of_v<Genome::GGenome, ind_type>,
        "ind_type must derive from Gem::Geneva::Genome::GGenome"
    );

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GPreEvaluationValidityCheckT<ind_type>";

    /***************************************************************************/
    /**
     * The default constructor
     */
    GPreEvaluationValidityCheckT() = default;

    /***************************************************************************/
    /**
     * The copy constructor
     */
    GPreEvaluationValidityCheckT(const GPreEvaluationValidityCheckT<ind_type> &cp) = default;

    /***************************************************************************/
    /**
     * The destructor
     */
    ~GPreEvaluationValidityCheckT() override = default;

    /***************************************************************************/
    /**
     * @brief Checks whether a given parameter set is valid.
     *
     * The function returns a double value which is expected to be >= 0. Values in the range [0,1]
     * indicate valid parameters (according to this constraint). Values above 1
     * indicate invalid parameters. The size of the return value can thus
     * be used to indicate the extent of the invalidity. Two policies are implemented
     * when check_() returns a value < 0: If allow_negative_ is set to true, such
     * evaluations are considered to be valid, and the function returns 0. If
     * allow_negative_ is set to false, an invalidity is calculated, and the return
     * value will be > 1.
     *
     * @param cp A pointer to the individual whose parameters are to be checked
     * @return 0 if the parameters are valid, otherwise a value > 1 indicating the extent of the invalidity
     */
    double check(const ind_type *cp) const {
        double const result = check_(cp);

        if(allow_negative_) {
            if(result <= 1.) { // valid
                return 0.;
            }
            return result;
        }
        if(result >= 0. && result <= 1.) { // valid
            return 0.;
        }
        // invalid
        if(result < 0.) { // we need to calculate a replacement value
            // Will be the more invalid the further below 0 "result" is
            return 1. + std::abs(result);
        }
        // result > 1, we may just return the unmodified value
        return result;
    }

    /***************************************************************************/
    /**
     * @brief Checks whether the constraint is valid for the given individual.
     *
     * @param cp A pointer to the individual to be checked
     * @param validity_level Output parameter, filled with the computed validity level of this individual
     * @return A boolean indicating whether the constraint is valid
     */
    bool isValid(const ind_type *cp, double &validity_level) const {
        // Set the external validity level
        validity_level = this->check(cp);

        if(std::numeric_limits<double>::max() == validity_level ||
           std::numeric_limits<double>::lowest() == validity_level) {
            return false;
        }

        if(allow_negative_) {
            return (validity_level <= 1.);
        }
                    return (validity_level >= 0. && validity_level <= 1.);
       
    }

    /***************************************************************************/
    /**
     * @brief Checks whether the constraint is invalid for the given individual.
     *
     * @param cp A pointer to the individual to be checked
     * @param validity_level Output parameter, filled with the computed validity level of this individual
     * @return A boolean indicating whether the constraint is invalid
     */
    bool isInvalid(const ind_type *cp, double &validity_level) const {
        return not this->isValid(cp, validity_level);
    }

    /***************************************************************************/
    /**
     * @brief Retrieves whether negative check values are considered to be valid.
     *
     * @return True if negative values are treated as valid, false otherwise
     */
    [[nodiscard]] bool getAllowNegative() const {
        return allow_negative_;
    }

    /***************************************************************************/
    /**
     * @brief Specifies whether negative check values are considered to be valid.
     *
     * @param allow_negative If true, negative values returned by check_() are treated as valid
     */
    void setAllowNegative(bool allow_negative) {
        allow_negative_ = allow_negative;
    }

protected:
    /***************************************************************************/
    /**
     * @brief Checks whether a given parameter set is valid.
     *
     * The function returns a double value which is expected to be >= 0., giving a level of confidence
     * that this is a valid solution. This function must be overloaded in derived classes.
     *
     * @param cp A pointer to the individual whose parameters are to be checked
     * @return A raw validity level (>= 0. expected), with values in [0,1] indicating validity
     */
    virtual double check_(const ind_type *) const = 0;

    /***************************************************************************/
    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     *
     * TODO: Check whether it makes sense to provide custom configuration files -- if so, add allow_negative_ here
     *
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override {
        // Call our parent class'es function
        Gem::Common::GCommonInterfaceT<GPreEvaluationValidityCheckT<ind_type>>::addConfigurationOptions_(gpb);
    }

    /***************************************************************************/
    /**
     * @brief Returns this class's local data members as a tuple (mutable overload).
     *
     * The single declaration of this class's local data members. load_() and
     * compare_() are derived from it, so the member list lives in one place.
     *
     * @return A tuple of named, mutable references to the local data members
     */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("allow_negative_", self.allow_negative_)
        );
    }

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceBaseT base (clone_ stays pure -- this is the abstract
    // category root) from class_name and the localMembers_() declaration above.

    /***************************************************************************/
    /**
     * @brief Applies modifications to this object. This is needed for testing purposes.
     *
     * @return A boolean indicating whether this object was actually modified
     */
    bool modify_GUnitTests_() {
#ifdef GEM_TESTING
        // This is the category root; there is no modifiable parent class above it.
        bool result = false;

        if(not this->getAllowNegative()) {
            this->setAllowNegative(true);
            result = true;
        }

        return result;

#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
        Gem::Common::condnotset("GPreEvaluationValidityCheckT<>::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif                  /* GEM_TESTING */
    }

private:
    /***************************************************************************/

    bool allow_negative_ = false; ///< Set to true if negative values are considered to be valid
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A collection of validity checks with the GPreEvaluationValidityCheckT interface
 *
 * @tparam ind_type The individual type to be checked; must derive from Genome::GGenome
 */
template <typename ind_type>
class GValidityCheckContainerT
  : public Gem::Common::GReflectiveInterfaceBaseT<GValidityCheckContainerT<ind_type>, GPreEvaluationValidityCheckT<ind_type>> {
    ///////////////////////////////////////////////////////////////////////
    // GReflectiveInterfaceAccess lets the mixin reach localMembers_(); this abstract class is
    // never reconstructed on load.
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class's local data (the validity checks), feeding the
     *  GReflectiveInterfaceBaseT-generated serialize()/load_()/compare_() from one source. The checks are
     *  std::shared_ptr<...> that must be deep-cloned on load (make_cloneable_container_member). */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_cloneable_container_member("validity_checks_", self.validity_checks_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GValidityCheckContainerT<ind_type>";

    /***************************************************************************/
    /**
     * The default constructor
     */
    GValidityCheckContainerT() = default;

    /***************************************************************************/
    /**
     * @brief Initialization from a vector of validity checks.
     *
     * @param validity_checks A vector of validity checks; each one is cloned into this container
     */
    explicit GValidityCheckContainerT(
        const std::vector<std::shared_ptr<GPreEvaluationValidityCheckT<ind_type>>> &validity_checks
    ) {
        Gem::Common::copyCloneableSmartPointerContainer(validity_checks, validity_checks_);
    }

    /***************************************************************************/
    /**
     * @brief The copy constructor.
     *
     * @param cp Another GValidityCheckContainerT object whose contained checks are deep-copied
     */
    GValidityCheckContainerT(const GValidityCheckContainerT<ind_type> &cp)
      : Gem::Common::GReflectiveInterfaceBaseT<GValidityCheckContainerT<ind_type>, GPreEvaluationValidityCheckT<ind_type>>(cp) {
        Gem::Common::copyCloneableSmartPointerContainer(cp.validity_checks_, validity_checks_);
    }

    /***************************************************************************/
    /**
     * The destructor
     */
    ~GValidityCheckContainerT() override = default;

    /***************************************************************************/
    /**
     * @brief The standard assignment operator.
     *
     * @param cp Another GValidityCheckContainerT object whose data is loaded into this one
     * @return A reference to this object
     */
    GValidityCheckContainerT<ind_type> &operator=(const GValidityCheckContainerT<ind_type> &cp) {
        if(this == &cp) {
            return *this;
        }
        this->load_(&cp);
        return *this;
    }

    /***************************************************************************/
    /**
     * @brief Adds a validity check to this object.
     *
     * Note that the check is cloned so that it can be used multiple times. A null pointer triggers an exception.
     *
     * @param vc_ptr A shared pointer to the validity check to add (must not be null)
     */
    void addCheck(const std::shared_ptr<GPreEvaluationValidityCheckT<ind_type>>& vc_ptr) {
        if(not vc_ptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GValidityCheckContainerT<>::addCheck(): Error!" << '\n'
                << "Got empty check pointer" << '\n'
            );
        }

        validity_checks_.push_back(
            vc_ptr->template clone<GPreEvaluationValidityCheckT<ind_type>>()
        );
    }

protected:
    /***************************************************************************/
    /**
     * @brief Checks whether a given parameter set is valid. To be specified in derived classes.
     *
     * @param cp A pointer to the individual whose parameters are to be checked
     * @return A raw validity level (>= 0. expected), with values in [0,1] indicating validity
     */
    double check_(const ind_type *) const override = 0;

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceBaseT base (clone_ stays pure -- this is abstract)
    // from class_name and the validity_checks_ localMembers_() declaration above.

    /***************************************************************************/
    /** @brief Holds all registered validity checks */
    std::vector<std::shared_ptr<GPreEvaluationValidityCheckT<ind_type>>> validity_checks_;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A class which combines all values (i.e. values > 1) according to a
 * user-defined policy or returns 0, if all checks are valid.
 *
 * @tparam ind_type The individual type to be checked; must derive from Genome::GGenome
 */
template <typename ind_type>
class GCheckCombinerT
  : public Gem::Common::GReflectiveInterfaceT<GCheckCombinerT<ind_type>, GValidityCheckContainerT<ind_type>> {
    ///////////////////////////////////////////////////////////////////////
    // Gem::Weft::access default-constructs this concrete type on load;
    // GReflectiveInterfaceAccess lets the mixin reach localMembers_().
    friend struct Gem::Common::GReflectiveInterfaceAccess;
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GCheckCombinerT<ind_type>";

    /***************************************************************************/
    /**
     * The default constructor
     */
    GCheckCombinerT() = default;

    /***************************************************************************/
    /**
     * @brief Initialization from a vector of validity checks.
     *
     * @param validity_checks A vector of validity checks; forwarded to the base container, which clones each one
     */
    explicit GCheckCombinerT(
        const std::vector<std::shared_ptr<GPreEvaluationValidityCheckT<ind_type>>> &validity_checks
    )
      : Gem::Common::GReflectiveInterfaceT<GCheckCombinerT<ind_type>, GValidityCheckContainerT<ind_type>>(validity_checks) { /* nothing */
    }

    /***************************************************************************/
    /**
     * @brief The copy constructor.
     */
    GCheckCombinerT(const GCheckCombinerT<ind_type> &) = default;

    /***************************************************************************/
    /**
     * The destructor
     */
    ~GCheckCombinerT() override = default;

    /***************************************************************************/
    /**
     * @brief The standard assignment operator.
     *
     * @param cp Another GCheckCombinerT object whose data is loaded into this one
     * @return A reference to this object
     */
    GCheckCombinerT<ind_type> &operator=(const GCheckCombinerT<ind_type> &cp) {
        if(this == &cp) {
            return *this;
        }
        this->load_(&cp);
        return *this;
    }

    /***************************************************************************/
    /**
     * @brief Allows to set the combiner policy.
     *
     * @param combiner_policy The policy used to combine the individual invalidities (e.g. MULTIPLYINVALID, ADDINVALID)
     */
    void setCombinerPolicy(validityCheckCombinerPolicy combiner_policy) {
        combiner_policy_ = combiner_policy;
    }

    /***************************************************************************/
    /**
     * @brief Allows to retrieve the combiner policy.
     *
     * @return The currently set policy used to combine the individual invalidities
     */
    [[nodiscard]] validityCheckCombinerPolicy getCombinerPolicy() const {
        return combiner_policy_;
    }

protected:
    /***************************************************************************/
    /**
     * @brief Combines all invalidities according to the user-defined policy.
     *
     * Note that we DO have to take care here of a situation where the invalidity equals MIN- or MAX_DOUBLE.
     *
     * @param cp A pointer to the individual whose parameters are checked against all registered checks
     * @return 0 if all checks are valid, otherwise the combined invalidity (or MAX_DOUBLE at the numeric boundaries)
     */
    double check_(const ind_type *cp) const override {
        // First identify invalid checks
        std::vector<double> invalid_checks;
        double validity_level = 0.;
        for(const auto &validity_check : GValidityCheckContainerT<ind_type>::validity_checks_) {
            if(not validity_check->isValid(cp, validity_level)) {
                invalid_checks.push_back(validity_level);
            }
        }

        // We can leave now, if no invalid checks were found
        if(invalid_checks.empty()) { // All checks were valid
            return 0.;
        }

        // Now act on the invalid tests
        using enum Gem::Geneva::validityCheckCombinerPolicy;
        switch(combiner_policy_) {
        // --------------------------------------------------------------------
        // Multiply all invalidities
        case MULTIPLYINVALID: {
            double result = 1.;
            for(const auto &invalidity : invalid_checks) {
                // If we encounter an invalidity at the numeric boundaries, we simply
                // return MAX_DOUBLE
                if(std::numeric_limits<double>::max() == invalidity ||
                   std::numeric_limits<double>::lowest() == invalidity) {
                    return std::numeric_limits<double>::max();
                }

                result *= invalidity;
            }
            return result;
        } break;

            // --------------------------------------------------------------------
            // Add all invalidities
        case ADDINVALID: {
            double result = 0.;
            for(const auto &invalidity : invalid_checks) {
                // If we encounter an invalidity at the numeric boundaries, we simply
                // return MAX_DOUBLE
                if(std::numeric_limits<double>::max() == invalidity ||
                   std::numeric_limits<double>::lowest() == invalidity) {
                    return std::numeric_limits<double>::max();
                }

                result += invalidity;
            }
            return result;
        } break;

            // --------------------------------------------------------------------
        default: {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GCheckCombinerT<ind_type>::check_(): Error!" << '\n'
                << "Got invalid combiner_policy_ value: " << combiner_policy_ << '\n'
            );
        }
        }
    }

    /***************************************************************************/
    /**
     * @brief Returns this class's local data members as a tuple (mutable overload).
     *
     * The single declaration of this class's local data members. load_() and
     * compare_() are derived from it, so the member list lives in one place.
     *
     * @return A tuple of named, mutable references to the local data members
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("combiner_policy_", self.combiner_policy_)
        );
    }

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and the combiner_policy_
    // localMembers_() declaration above.

private:
    /***************************************************************************/
    // Local data

    validityCheckCombinerPolicy combiner_policy_ = Gem::Geneva::validityCheckCombinerPolicy::
        MULTIPLYINVALID; ///< Indicates how validity checks should be combined
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */

