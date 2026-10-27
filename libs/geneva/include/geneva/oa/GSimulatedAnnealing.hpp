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
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GParChild.hpp"
#include "geneva/oa/GOptimizationAlgorithmT.hpp"
#include "geneva/oa/GSimulatedAnnealing_PersonalityTraits.hpp"
#include "common/GSelfTestable.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A population-based simulated annealing optimizer: a \f$(\mu,\lambda)\f$ evolutionary loop whose
 * parent/child replacement uses the Metropolis acceptance criterion with a cooling temperature.
 *
 * @details
 * Like the evolutionary algorithm this is a \f$(\mu,\lambda)\f$ scheme (\f$\mu\f$ parents produce
 * \f$\lambda\f$ children by adaption, "Geneva-style" with larger-than-textbook populations), but the
 * selection step differs: instead of always keeping the best, each parent \f$i\f$ is compared with its
 * child and the child is accepted with the @e Metropolis probability
 * \f[
 *   P(\text{accept child}_i) =
 *   \begin{cases}
 *     1 & \text{if } f_{\text{child}} \le f_{\text{parent}}\ (\text{child is at least as good}),\\[2pt]
 *     \exp\!\Bigl(-\dfrac{f_{\text{child}}-f_{\text{parent}}}{T}\Bigr) & \text{otherwise},
 *   \end{cases}
 * \f]
 * on the minimisation (min-only transformed) fitness. A worse child is thus accepted with a probability
 * that falls off with the fitness gap and rises with the temperature \f$T\f$ -- the mechanism that lets
 * the search climb out of local optima early on. After each generation the temperature is cooled
 * geometrically,
 * \f[
 *   T \leftarrow \max\bigl(\alpha\,T,\ T_{\min}\bigr),\qquad 0 < \alpha < 1,
 * \f]
 * starting from \f$T_0\f$, with \f$\alpha\f$ the degradation strength. The floor
 * \f$T_{\min}=\texttt{std::numeric\_limits<double>::min()}\f$ keeps \f$T\f$ strictly positive so the
 * acceptance probability never evaluates \f$0/0\f$; as \f$T\to T_{\min}\f$ the rule approaches pure
 * greedy (elitist) selection. The newly accepted parents are then re-sorted by fitness for the next
 * generation.
 */
class GSimulatedAnnealing // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GSimulatedAnnealing, GParChild>
  , public Gem::Common::GSelfTestable {
public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold.
    static constexpr std::string_view class_name = "GSimulatedAnnealing";
    static constexpr std::string_view oa_algorithm_name = "Simulated Annealing";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_SA";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GSimulatedAnnealing_PersonalityTraits;

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class'es local data members */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("t0_", self.t0_),
            Gem::Common::make_member("t_", self.t_),
            Gem::Common::make_member("alpha_", self.alpha_)
        );
    }

    // serialize(), load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base (via GOptimizationAlgorithmT) from class_name and localMembers_().
    ///////////////////////////////////////////////////////////////////////

public:
    /***************************************************************************/
    /** @brief The default constructor */
    GSimulatedAnnealing();
    /** @brief A standard copy constructor */
    GSimulatedAnnealing(const GSimulatedAnnealing &) = default;
    /** @brief The standard destructor */
    ~GSimulatedAnnealing() override = default;

    /**
     * @brief Determines the strength of the temperature degradation (cooling schedule).
     * @param alpha The new value of the temperature degradation strength (alpha_)
     */
    void setTDegradationStrength(double alpha);
    /**
     * @brief Retrieves the temperature degradation strength. This function is used for simulated annealing.
     * @return The current value of the temperature degradation strength (alpha_)
     */
    double getTDegradationStrength() const;

    /**
     * @brief Sets the start temperature. This function is used for simulated annealing.
     * @param t0 The new value of the start temperature (t0_)
     */
    void setT0(double t0);
    /**
     * @brief Retrieves the start temperature. This function is used for simulated annealing.
     * @return The current value of the start temperature (t0_)
     */
    double getT0() const;
    /**
     * @brief Retrieves the current temperature. This function is used for simulated annealing.
     * @return The current temperature (t_)
     */
    double getT() const;

protected:
    /***************************************************************************/
    /**
     * @brief Applies modifications to this object. This is needed for testing purposes
     *
     * This class contributes no checks of its own; it opts into the self-test facet so that the
     * category root's modify_GUnitTests_() keeps running for it under the standard tests.
     */
    bool modify_GUnitTests_() override {
#ifdef GEM_TESTING
        return GOptimizationAlgorithmBase::modify_GUnitTests_();
#else  /* GEM_TESTING */
        Gem::Common::condnotset("GSimulatedAnnealing::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif /* GEM_TESTING */
    }

    /***************************************************************************/
    // Virtual or overridden protected functions

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceT base.

    /** @brief Resets the settings of this population to what was configured when the optimize()-call was issued */
    void resetToOptimizationStart_() override;

    // name_(), clone_(), getAlgorithmName_(), getAlgorithmPersonalityType_() and the GUnitTests
    // stubs are generated by the GOptimizationAlgorithmT scaffold from the oa_* identifiers above.

    /***************************************************************************/

private:
    /***************************************************************************/
    // Virtual or overridden private functions

    /** @brief We submit individuals to the process consumer and wait for processed items. */
    void evaluatePopulation_() override;

    /**
     * @brief Retrieve a GPersonalityTraits object belonging to this algorithm.
     * @return A shared_ptr to a freshly created GSimulatedAnnealing_PersonalityTraits object
     */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;

    /** @brief Choose new parents, based on the SA selection scheme. */
    void selectBest_() override;

    /** @brief Some error checks related to population sizes */
    void populationSanityChecks_() const override;

    /***************************************************************************/

    /** @brief Performs a simulated annealing style sorting and selection */
    void sortSAMode();

    /**
     * @brief Calculates the simulated annealing probability for a child to replace a parent.
     * @param f_min_only_parent The fitness value of the parent
     * @param f_min_only_child The fitness value of the child
     * @return The acceptance weight for the child (a probability in [0,1] for a worse child; >= 1, i.e.
     *         always accept, for a child that is at least as good). Reads only the temperature, so const.
     */
    double saProb(const double &f_min_only_parent, const double &f_min_only_child) const;

    /** @brief Updates the temperature. This function is used for simulated annealing. */
    void updateTemperature();

    /***************************************************************************/
    // Data

    double t0_ = SA_T0;       ///< The start temperature, used in simulated annealing
    double t_ = t0_;         ///< The current temperature, used in simulated annealing
    double alpha_ = SA_ALPHA; ///< A constant used in the cooling schedule in simulated annealing
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */


