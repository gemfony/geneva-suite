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
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GExceptions.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOptimizationAlgorithmT.hpp"
#include "geneva/oa/GNelderMead_PersonalityTraits.hpp"

#ifdef GEM_TESTING

#ifdef GEM_TESTING
#include "geneva/individuals/GTestIndividual1.hpp"
#include "common/GSelfTestable.hpp"
#endif /* GEM_TESTING */

#endif /* GEM_TESTING */

namespace Gem::Geneva::OptimizationAlgorithms {

/**
 * Default values for the Nelder-Mead downhill simplex.
 */
constexpr std::size_t DEFAULTNMSIMPLICES = 1;
constexpr double DEFAULTNMALPHA = 1.0;   ///< Reflection coefficient
constexpr double DEFAULTNMGAMMA = 2.0;   ///< Expansion coefficient
constexpr double DEFAULTNMRHO = 0.5;     ///< Contraction coefficient
constexpr double DEFAULTNMSIGMA = 0.5;   ///< Shrink coefficient
constexpr double DEFAULTNMINITIALEDGE = 0.1; ///< Initial simplex edge (fraction of range)
constexpr std::uint32_t DEFAULTNMRESTARTTHRESHOLD = 0; ///< Stall count triggering an oriented restart (0 = disabled)

//! [algo-nm-trial-slots]
/** @brief Number of speculative trial points evaluated per simplex and iteration
 *  (reflection, expansion, inside contraction, outside contraction). */
constexpr std::size_t NM_NTRIALS = 4;
/** @brief Trial slot offsets within a simplex block */
constexpr std::size_t NM_REFLECT = 0;
constexpr std::size_t NM_EXPAND = 1;
constexpr std::size_t NM_CONTRACT = 2;  ///< inside contraction
constexpr std::size_t NM_OCONTRACT = 3; ///< outside contraction
//! [algo-nm-trial-slots]

/******************************************************************************/
/**
 * @brief The n-dimensional downhill simplex method of Nelder and Mead (1965): a derivative-free local
 * optimizer for non-differentiable (and moderately noisy) objectives where a (conjugate) gradient
 * descent is inappropriate.
 *
 * @details
 * A simplex in \f$ n \f$-dimensional parameter space is a set of \f$ n+1 \f$ vertices
 * \f$ \mathbf{x}_0,\dots,\mathbf{x}_n \f$. Each iteration ranks them by fitness, identifies the best
 * \f$ \mathbf{x}_b \f$ and worst \f$ \mathbf{x}_w \f$, forms the centroid of all vertices @e except the
 * worst,
 * \f[
 *   \bar{\mathbf{x}} = \frac{1}{n}\sum_{i \neq w} \mathbf{x}_i,
 * \f]
 * and proposes four trial points by moving the worst vertex along the line through the centroid:
 * \f[
 *   \underbrace{\mathbf{x}_r=\bar{\mathbf{x}}+\alpha(\bar{\mathbf{x}}-\mathbf{x}_w)}_{\text{reflection}},
 *   \quad
 *   \underbrace{\mathbf{x}_e=\bar{\mathbf{x}}+\gamma(\bar{\mathbf{x}}-\mathbf{x}_w)}_{\text{expansion}},
 *   \quad
 *   \underbrace{\mathbf{x}_{ic}=\bar{\mathbf{x}}+\rho(\mathbf{x}_w-\bar{\mathbf{x}})}_{\text{inside contraction}},
 *   \quad
 *   \underbrace{\mathbf{x}_{oc}=\bar{\mathbf{x}}+\rho(\mathbf{x}_r-\bar{\mathbf{x}})}_{\text{outside contraction}},
 * \f]
 * with the standard coefficients \f$ \alpha=1 \f$ (reflection), \f$ \gamma=2 \f$ (expansion) and
 * \f$ \rho=\tfrac{1}{2} \f$ (contraction). Writing \f$ f_b \le f_{s} \le f_w \f$ for the best,
 * second-worst and worst fitnesses, the worst vertex is replaced according to the classical acceptance
 * rules:
 * \f[
 *   \mathbf{x}_w \leftarrow
 *   \begin{cases}
 *     \mathbf{x}_e & \text{if } f_r < f_b \text{ and } f_e < f_r,\\
 *     \mathbf{x}_r & \text{if } f_r < f_b \text{ (else)},\\
 *     \mathbf{x}_r & \text{if } f_b \le f_r < f_{s},\\
 *     \mathbf{x}_{oc} & \text{if } f_{s} \le f_r < f_w \text{ and } f_{oc} \le f_r,\\
 *     \mathbf{x}_{ic} & \text{if } f_r \ge f_w \text{ and } f_{ic} < f_w.
 *   \end{cases}
 * \f]
 * If neither contraction improves on the worst, the whole simplex @e shrinks towards the best vertex,
 * \f$ \mathbf{x}_i \leftarrow \mathbf{x}_b + \sigma(\mathbf{x}_i-\mathbf{x}_b) \f$ for all \f$ i\neq b \f$,
 * with \f$ \sigma=\tfrac{1}{2} \f$.
 *
 * @par Batch mapping (one-iteration lag)
 * Because Geneva evaluates a @e fixed population per iteration through the single process consumer, the
 * classically @e sequential simplex moves are mapped onto a batch scheme. The population layout per
 * simplex is
 * \f[
 *   [\,\underbrace{\mathbf{x}_0\,\dots\,\mathbf{x}_n}_{n+1\text{ vertices}}\;|\;
 *      \mathbf{x}_r\;|\;\mathbf{x}_e\;|\;\mathbf{x}_{ic}\;|\;\mathbf{x}_{oc}\,],
 * \f]
 * i.e. \f$ n+1 \f$ vertices plus \f$ 4 \f$ speculative trial slots (block size \f$ n+5 \f$). In every
 * iteration all four candidates are proposed and submitted @e together with the vertices, and the
 * acceptance rules above are applied in the @e next iteration once their fitnesses are known. This costs
 * a one-iteration evaluation lag (the same approximation style as GConjugateGradientDescent's difference
 * quotients) but lets the method exploit the batch evaluation model and converge to a local
 * optimum without any gradient information. \f$ n_\text{simplices} \f$ such blocks run simultaneously
 * (analogous to the multiple starting points of the gradient descents).
 *
 * @par Oriented restart
 * An optional oriented restart (setRestartThreshold(), config "restart_threshold") rebuilds every
 * simplex around its best vertex, with the edges oriented down the local descent direction, after the run
 * has stalled for that many iterations. It escapes a degenerate simplex collapse (where the vertices
 * become near-coplanar and progress stops); at a genuine optimum it simply re-converges.
 */
class GNelderMead // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GNelderMead>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold.
    static constexpr std::string_view class_name = "GNelderMead";
    static constexpr std::string_view oa_algorithm_name = "Nelder-Mead Simplex";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_NM";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GNelderMead_PersonalityTraits;

private:

    /** @brief Single declaration of this class'es local data members */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("n_simplices_", self.n_simplices_),
            Gem::Common::make_member("n_fp_parms_first_", self.n_fp_parms_first_),
            Gem::Common::make_member("alpha_", self.alpha_),
            Gem::Common::make_member("gamma_", self.gamma_),
            Gem::Common::make_member("rho_", self.rho_),
            Gem::Common::make_member("sigma_", self.sigma_),
            Gem::Common::make_member("initial_edge_", self.initial_edge_),
            Gem::Common::make_member("restart_threshold_", self.restart_threshold_)
        );
    }

    // serialize(), load_(), compare_(), name_() and clone_() are all generated by the
    // Gem::Common::GReflectiveInterfaceT base (via GOptimizationAlgorithmT) from class_name and
    // localMembers_().
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The default constructor */
    GNelderMead();
    /**
     * @brief Initialization with the number of simultaneous simplices.
     * @param n_simplices The number of simplices to run in parallel (one per starting region)
     */
    explicit GNelderMead(const std::size_t &n_simplices);
    /**
     * @brief A standard copy constructor.
     * @param cp Another GNelderMead object whose state is copied
     */
    GNelderMead(const GNelderMead &cp) = default;
    /** @brief The destructor */
    ~GNelderMead() override = default;

    /**
     * @brief Retrieves the number of simultaneous simplices.
     * @return The number of simplices run in parallel
     */
    std::size_t getNSimplices() const;
    /**
     * @brief Allows to set the number of simultaneous simplices.
     * @param n_simplices The number of simplices to run in parallel
     */
    void setNSimplices(std::size_t n_simplices);

    /**
     * @brief Sets the reflection coefficient (alpha).
     * @param alpha The reflection coefficient controlling how far the worst vertex is reflected through the centroid
     */
    void setAlpha(double alpha);
    /**
     * @brief Retrieves the reflection coefficient (alpha).
     * @return The current reflection coefficient
     */
    double getAlpha() const;
    /**
     * @brief Sets the expansion coefficient (gamma).
     * @param gamma The expansion coefficient controlling how far a successful reflection is extended
     */
    void setGamma(double gamma);
    /**
     * @brief Retrieves the expansion coefficient (gamma).
     * @return The current expansion coefficient
     */
    double getGamma() const;
    /**
     * @brief Sets the contraction coefficient (rho).
     * @param rho The contraction coefficient controlling how far a vertex is contracted towards the centroid
     */
    void setRho(double rho);
    /**
     * @brief Retrieves the contraction coefficient (rho).
     * @return The current contraction coefficient
     */
    double getRho() const;
    /**
     * @brief Sets the shrink coefficient (sigma).
     * @param sigma The shrink coefficient controlling how far non-best vertices are moved towards the best vertex
     */
    void setSigma(double sigma);
    /**
     * @brief Retrieves the shrink coefficient (sigma).
     * @return The current shrink coefficient
     */
    double getSigma() const;
    /**
     * @brief Sets the relative size of the initial simplex.
     * @param initial_edge The initial simplex edge length, expressed as a fraction of the parameter range
     */
    void setInitialEdge(double initial_edge);
    /**
     * @brief Retrieves the relative size of the initial simplex.
     * @return The initial simplex edge length (as a fraction of the parameter range)
     */
    double getInitialEdge() const;
    /**
     * @brief Sets the stall count after which an oriented restart is performed.
     * @param restart_threshold The number of stalled iterations triggering an oriented restart (0 = disabled)
     */
    void setRestartThreshold(std::uint32_t restart_threshold);
    /**
     * @brief Retrieves the oriented-restart stall threshold.
     * @return The stall count that triggers an oriented restart (0 = disabled)
     */
    std::uint32_t getRestartThreshold() const;

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
        Gem::Common::condnotset("GNelderMead::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif /* GEM_TESTING */
    }

    /***************************************************************************/
    // Virtual or overridden protected functions

private:
    /**
     * @brief Need-all algorithm: a missing or failed evaluation cannot be tolerated, so it submits
     *  through courtier under full-success-or-fatal (matches the legacy throw-on-error).
     * @return The full-success-or-fatal submission policy
     */
    Gem::Courtier::GSubmissionPolicy getSubmissionPolicy_() const override {
        return Gem::Courtier::GSubmissionPolicy::full_success_or_fatal();
    }

protected:
    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb A reference to the parser-builder that collects this algorithm's configuration options
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceT base.

    /** @brief Resets the settings of this population to what was configured when the optimize()-call was issued */
    void resetToOptimizationStart_() override;

    /** @brief Does some preparatory work before the optimization starts */
    void init() override;
    /** @brief Does any necessary finalization work */
    void finalize() override;

    /** @brief Applies the Nelder-Mead acceptance rules using the trials of the previous iteration */
    void applyNelderMeadDecision();
    /** @brief Proposes the reflection / expansion / contraction trial points */
    void proposeTrials();

    // name_(), clone_(), getAlgorithmName_(), getAlgorithmPersonalityType_() and the GUnitTests
    // stubs are generated by the GOptimizationAlgorithmT scaffold from the oa_* identifiers above.

    /***************************************************************************/

private:
    /***************************************************************************/
    // Virtual or overridden private functions

    /**
     * @brief The actual business logic to be performed during each iteration.
     * @return A tuple holding the best achieved fitness (raw, transformed) of this iteration
     */
    std::tuple<double, double> cycleLogic_() override;
    /** @brief Triggers fitness calculation of a number of individuals */
    void evaluatePopulation_() override;


    /**
     * @brief Retrieve a GPersonalityTraits object belonging to this algorithm.
     * @return A shared pointer to a freshly created Nelder-Mead personality-traits object
     */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;
    /** @brief Resizes the population to the desired level and does some error checks */
    void adjustPopulation_() override;

    /** @brief Builds the initial (non-degenerate) simplices around the seed vertices */
    void buildInitialSimplices();
    /**
     * @brief Rebuilds every simplex around its best vertex, oriented down the local descent
     *  direction. Used to escape a degenerate collapse once the run has stalled.
     * @return true if at least one simplex was restarted (its vertices then need re-evaluation),
     *  false otherwise
     */
    bool restartSimplices();
    /**
     * @brief Shrinks simplex s by moving every non-best vertex towards the best vertex b.
     * @param s The index of the simplex to shrink
     * @param b The index (within that simplex) of the best vertex towards which the others are moved
     */
    void shrinkTowardsBest(std::size_t s, std::size_t b);

    /**
     * @brief Convenience: population index of vertex v in simplex s.
     * @param s The simplex index
     * @param v The vertex index within the simplex
     * @return The corresponding position in the population
     */
    std::size_t vertexPos(std::size_t s, std::size_t v) const;
    /**
     * @brief Convenience: population index of trial slot t in simplex s.
     * @param s The simplex index
     * @param t The trial-slot index within the simplex (NM_REFLECT / NM_EXPAND / NM_CONTRACT / NM_OCONTRACT)
     * @return The corresponding position in the population
     */
    std::size_t trialPos(std::size_t s, std::size_t t) const;
    /**
     * @brief Number of population slots used per simplex.
     * @return The block size per simplex (n+1 vertices plus the speculative trial slots)
     */
    std::size_t simplexBlockSize() const;

    /***************************************************************************/
    // Data

    std::size_t n_simplices_ =
        DEFAULTNMSIMPLICES;         ///< The number of simultaneous simplices
    std::size_t n_fp_parms_first_ = 0; ///< The amount of active floating point values per individual

    double alpha_ = DEFAULTNMALPHA;             ///< Reflection coefficient
    double gamma_ = DEFAULTNMGAMMA;             ///< Expansion coefficient
    double rho_ = DEFAULTNMRHO;                 ///< Contraction coefficient
    double sigma_ = DEFAULTNMSIGMA;             ///< Shrink coefficient
    double initial_edge_ = DEFAULTNMINITIALEDGE; ///< Initial simplex edge (fraction of range)
    std::uint32_t restart_threshold_ =
        DEFAULTNMRESTARTTHRESHOLD; ///< Stall count triggering an oriented restart (0 = disabled)

    bool trials_pending_ =
        false; ///< True once real trial points have been proposed (transient; gates the first decision)
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

