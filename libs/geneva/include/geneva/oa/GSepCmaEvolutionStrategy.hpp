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
#include <cstddef>
#include <string_view>
#include <tuple>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOptimizationAlgorithmT.hpp"
#include "geneva/oa/GSepCmaEvolutionStrategy_PersonalityTraits.hpp"

#ifdef GEM_TESTING
#include "geneva/individuals/GTestIndividual1.hpp"
#include "common/GSelfTestable.hpp"
#endif /* GEM_TESTING */

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * The default offspring count (lambda) per generation. 0 means "use the textbook
 * default 4 + floor(3*ln(n))".
 */
constexpr std::size_t DEFAULTSEPCMALAMBDA = 0;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A from-scratch, high-dimensional derandomized evolution strategy: separable CMA-ES with
 * cumulative step-size adaptation (CSA), usable at \f$ n \gtrsim 10^4 \f$ parameters thanks to an
 * \f$ O(n) \f$ per-generation cost.
 *
 * @details
 * Unlike the classic \f$(\mu,\lambda)\f$ self-adaptive ES (GEvolutionaryAlgorithm), which mutates a
 * @e per-individual step size, this strategy maintains a single multivariate-normal search distribution
 * \f$ \mathcal{N}\!\bigl(\mathbf{m},\,\sigma^{2}\mathbf{C}\bigr) \f$ inside the algorithm object and
 * adapts its moments from the ranked offspring. The state is
 * \f[
 *   \mathbf{m}\in\mathbb{R}^{n},\quad \sigma>0,\quad
 *   \mathbf{C}=\operatorname{diag}(c_{1},\dots,c_{n}),\quad
 *   \mathbf{p}_{\sigma}\in\mathbb{R}^{n},\quad \mathbf{p}_{c}\in\mathbb{R}^{n},
 * \f]
 * i.e. the mean, a global step size, a @e diagonal covariance (the separable simplification, giving
 * \f$O(n)\f$ storage/time instead of the \f$O(n^{2})\f$ of full CMA-ES), and two evolution paths.
 *
 * @par Sampling and recombination
 * Each generation draws \f$\lambda\f$ offspring (\f$\odot\f$ is the element-wise product) using
 * @b mirrored @b sampling (Brockhoff et al. 2010) — antithetic pairs that share one Gaussian draw,
 * \f[
 *   \mathbf{x}_{2k-1}=\mathbf{m}+\sigma\,\sqrt{\mathbf{C}}\odot\mathbf{z}_{k},\qquad
 *   \mathbf{x}_{2k}=\mathbf{m}-\sigma\,\sqrt{\mathbf{C}}\odot\mathbf{z}_{k},\qquad
 *   \mathbf{z}_{k}\sim\mathcal{N}(\mathbf{0},\mathbf{I}),
 * \f]
 * which cancels the first-order sampling noise of the weighted recombination (steadier, faster progress
 * at no extra cost). It then
 * evaluates them through the single process consumer (transport-agnostic, mirroring the stock EA's
 * submission path), ranks them, and recombines the best \f$\mu\f$ with positive logarithmic weights
 * \f[
 *   w_{i}=\frac{w_{i}'}{\sum_{j=1}^{\mu}w_{j}'},\qquad
 *   w_{i}'=\ln\!\Bigl(\mu+\tfrac{1}{2}\Bigr)-\ln i,\qquad
 *   \mu_{\mathrm{eff}}=\Bigl(\textstyle\sum_{i=1}^{\mu}w_{i}^{2}\Bigr)^{-1}.
 * \f]
 * With the sorted, normalised steps \f$\mathbf{y}_{i}=(\mathbf{x}_{i:\lambda}-\mathbf{m})/\sigma\f$ and
 * \f$\langle\mathbf{y}\rangle_{w}=\sum_{i=1}^{\mu}w_{i}\mathbf{y}_{i}\f$, the mean moves as
 * \f$ \mathbf{m}\leftarrow\mathbf{m}+\sigma\,\langle\mathbf{y}\rangle_{w} \f$.
 *
 * @par Step-size control (CSA)
 * \f[
 *   \mathbf{p}_{\sigma}\leftarrow(1-c_{\sigma})\,\mathbf{p}_{\sigma}
 *     +\sqrt{c_{\sigma}(2-c_{\sigma})\,\mu_{\mathrm{eff}}}\;
 *     \mathbf{C}^{-1/2}\langle\mathbf{y}\rangle_{w},\qquad
 *   \sigma\leftarrow\sigma\,\exp\!\Biggl(\frac{c_{\sigma}}{d_{\sigma}}
 *     \biggl(\frac{\lVert\mathbf{p}_{\sigma}\rVert}{\chi_{n}}-1\biggr)\Biggr),
 * \f]
 * where \f$\chi_{n}=\mathbb{E}\lVert\mathcal{N}(\mathbf{0},\mathbf{I})\rVert
 * \approx\sqrt{n}\,\bigl(1-\tfrac{1}{4n}+\tfrac{1}{21n^{2}}\bigr)\f$ and \f$\mathbf{C}^{-1/2}\f$ is
 * element-wise on the diagonal.
 *
 * @par Covariance update (rank-1 + optional rank-\f$\mu\f$, diagonal)
 * With the Heaviside stall switch \f$h_{\sigma}\f$ on \f$\lVert\mathbf{p}_{\sigma}\rVert\f$,
 * \f[
 *   \mathbf{p}_{c}\leftarrow(1-c_{c})\,\mathbf{p}_{c}
 *     +h_{\sigma}\sqrt{c_{c}(2-c_{c})\,\mu_{\mathrm{eff}}}\;\langle\mathbf{y}\rangle_{w},
 *   \qquad
 *   c_{j}\leftarrow(1-c_{1}-c_{\mu})\,c_{j}
 *     +c_{1}\,p_{c,j}^{2}
 *     +c_{\mu}\sum_{i=1}^{\mu}w_{i}\,y_{i,j}^{2},\quad j=1,\dots,n.
 * \f]
 * Setting @c useDiagonalCMA = @c false disables this block, leaving a pure CSA-ES (covariance level 0).
 *
 * @par Dimension-scaled constants (the property the stock self-adaptive EA lacks)
 * \f[
 *   \lambda=4+\lfloor 3\ln n\rfloor,\quad \mu=\lfloor\lambda/2\rfloor,\quad
 *   c_{\sigma}=\frac{\mu_{\mathrm{eff}}+2}{n+\mu_{\mathrm{eff}}+5},\quad
 *   d_{\sigma}=1+2\max\!\Bigl(0,\sqrt{\tfrac{\mu_{\mathrm{eff}}-1}{n+1}}-1\Bigr)+c_{\sigma},\quad
 *   c_{c}\sim\frac{4}{n}.
 * \f]
 * The rank-1 / rank-\f$\mu\f$ rates are the full-CMA values
 * \f$ c_{1}^{\text{full}}=\tfrac{2}{(n+1.3)^{2}+\mu_{\mathrm{eff}}} \f$,
 * \f$ c_{\mu}^{\text{full}}=\min\!\bigl(1-c_{1}^{\text{full}},\,
 * \tfrac{2(\mu_{\mathrm{eff}}-2+1/\mu_{\mathrm{eff}})}{(n+2)^{2}+\mu_{\mathrm{eff}}}\bigr) \f$
 * multiplied by the separable factor \f$\tfrac{n+2}{3}\f$ (Ros & Hansen 2008), which lets the diagonal
 * model adapt fast enough to be worthwhile at large \f$n\f$.
 *
 * A multi-objective mode replaces the scalar ranking with an NSGA-II fast-non-dominated-sort + crowding
 * distance key (Deb et al. 2002), used as the selection order for recombination.
 *
 * @note Clean-room implementation: derived solely from the cited publications (the equations above), not
 * adapted from any existing CMA-ES codebase — keeping it free of copyleft entanglement with Geneva's
 * Apache-2.0 licence. Behaviour may be cross-checked black-box against a reference (e.g. pycma on BBOB),
 * but no third-party source was consulted.
 *
 * @par References
 * - N. Hansen, A. Ostermeier, "Completely Derandomized Self-Adaptation in Evolution Strategies",
 *   Evolutionary Computation 9(2):159-195, 2001.
 * - N. Hansen, "The CMA Evolution Strategy: A Tutorial", arXiv:1604.00772, 2016.
 * - R. Ros, N. Hansen, "A Simple Modification in CMA-ES Achieving Linear Time and Space Complexity",
 *   Parallel Problem Solving from Nature (PPSN X), LNCS 5199:296-305, 2008.
 * - K. Deb, A. Pratap, S. Agarwal, T. Meyarivan, "A Fast and Elitist Multiobjective Genetic Algorithm:
 *   NSGA-II", IEEE Trans. Evolutionary Computation 6(2):182-197, 2002.
 * - D. Brockhoff, A. Auger, N. Hansen, D. V. Arnold, T. Hohm, "Mirrored Sampling and Sequential
 *   Selection for Evolution Strategies", Parallel Problem Solving from Nature (PPSN XI), 2010.
 */
class GSepCmaEvolutionStrategy // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GSepCmaEvolutionStrategy>
  , public Gem::Common::GSelfTestable {
public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold.
    static constexpr std::string_view class_name = "GSepCmaEvolutionStrategy";
    static constexpr std::string_view oa_algorithm_name = "Separable CMA / CSA Evolution Strategy";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_SEPCMA";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GSepCmaEvolutionStrategy_PersonalityTraits;

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class'es local data members (drives serialize/load/compare). */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("lambda_", self.lambda_),
            Gem::Common::make_member("mu_", self.mu_),
            Gem::Common::make_member("use_diagonal_cma_", self.use_diagonal_cma_),
            Gem::Common::make_member("pareto_mode_", self.pareto_mode_),
            Gem::Common::make_member("initial_sigma_", self.initial_sigma_),
            Gem::Common::make_member("n_", self.n_),
            Gem::Common::make_member("sigma_", self.sigma_),
            Gem::Common::make_member("m_", self.m_),
            Gem::Common::make_member("C_", self.C_),
            Gem::Common::make_member("p_sigma_", self.p_sigma_),
            Gem::Common::make_member("p_c_", self.p_c_),
            Gem::Common::make_member("state_initialized_", self.state_initialized_),
            Gem::Common::make_member("mu_eff_", self.mu_eff_),
            Gem::Common::make_member("c_sigma_", self.c_sigma_),
            Gem::Common::make_member("d_sigma_", self.d_sigma_),
            Gem::Common::make_member("c_c_", self.c_c_),
            Gem::Common::make_member("c_1_", self.c_1_),
            Gem::Common::make_member("c_mu_", self.c_mu_),
            Gem::Common::make_member("chi_n_", self.chi_n_),
            Gem::Common::make_member("weights_", self.weights_)
        );
    }

    // serialize(), load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base (via GOptimizationAlgorithmT) from class_name and localMembers_().
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The default constructor */
    GSepCmaEvolutionStrategy();
    /** @brief A standard copy constructor */
    GSepCmaEvolutionStrategy(const GSepCmaEvolutionStrategy &) = default;
    /** @brief The standard destructor */
    ~GSepCmaEvolutionStrategy() override = default;

    /**
     * @brief Sets the number of offspring sampled per generation (lambda); 0 selects the textbook default.
     * @param lambda The number of offspring per generation (0 == 4 + floor(3*ln(n)))
     */
    void setLambda(std::size_t lambda);
    /** @brief Retrieves the number of offspring sampled per generation (lambda). @return lambda */
    std::size_t getLambda() const;

    /**
     * @brief Sets the number of selected parents (mu); 0 means lambda/2.
     * @param mu The number of recombination parents (0 == lambda/2)
     */
    void setMu(std::size_t mu);
    /** @brief Retrieves the number of selected parents (mu). @return mu */
    std::size_t getMu() const;

    /**
     * @brief Enables / disables the diagonal (sep-CMA) covariance update.
     * @param use_diagonal_cma true to run the diagonal covariance adaptation; false for pure CSA-ES
     */
    void setUseDiagonalCMA(bool use_diagonal_cma);
    /** @brief Checks whether the diagonal (sep-CMA) covariance update is active. @return the flag */
    bool getUseDiagonalCMA() const;

    /**
     * @brief Enables / disables NSGA-II-style multi-objective selection.
     * @param pareto_mode true to rank offspring by non-dominated sort + crowding distance
     */
    void setParetoMode(bool pareto_mode);
    /** @brief Checks whether NSGA-II-style multi-objective selection is active. @return the flag */
    bool getParetoMode() const;

    /**
     * @brief Sets the initial global step size (sigma) as a fraction of the parameter range.
     * @param initial_sigma The initial sigma fraction (must be > 0)
     */
    void setInitialSigma(double initial_sigma);
    /** @brief Retrieves the initial global step size fraction. @return initial_sigma */
    double getInitialSigma() const;

    /** @brief Retrieves the current global step size (sigma). @return the current sigma */
    double getSigma() const;

protected:
    /***************************************************************************/
    // Virtual or overridden protected functions

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb The parser-builder to which this algorithm's configuration options are added
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceT base.

    /** @brief Resets the settings of this population to what was configured when optimize() was issued */
    void resetToOptimizationStart_() override;

    /** @brief Does any necessary initialization work before the optimization cycle starts */
    void init() override;
    /** @brief Does any necessary finalization work */
    void finalize() override;

    /** @brief Applies modifications to this object */
    bool modify_GUnitTests_() override;
    /** @brief Performs self tests that are expected to succeed. This is needed for testing purposes */
    void specificTestsNoFailureExpected_GUnitTests_() override;
    /***************************************************************************/

private:
    /***************************************************************************/
    // Overloaded or virtual base functions. name_(), clone_(), getAlgorithmName_(),
    // getAlgorithmPersonalityType_() are generated by the GOptimizationAlgorithmT scaffold.

    /** @brief The actual business logic performed during each iteration */
    std::tuple<double, double> cycleLogic_() override;
    /** @brief Submits offspring to the one process consumer and waits for processed items */
    void evaluatePopulation_() override;


    /** @brief Retrieve a GPersonalityTraits object belonging to this algorithm */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;


    /** @brief Sizes the population to lambda offspring (and derives n / constants) and does error checks */
    void adjustPopulation_() override;

    /***************************************************************************/
    // Algorithm-internal helpers

    /** @brief Determines the dimension n from the first individual */
    void determineDimension();
    /** @brief Derives lambda/mu/weights/constants once the dimension n is known */
    void setUpStrategyParameters();
    /** @brief Samples lambda offspring from the current distribution into the population */
    void sampleOffspring();
    /** @brief Returns the indices of the population sorted best-first for selection */
    std::vector<std::size_t> rankPopulation() const;
    /** @brief Computes the NSGA-II crowding/non-dominated ranking order (best-first) via the shared
     *  OptimizationAlgorithms::nonDominatedRank helper */
    std::vector<std::size_t> rankPopulationPareto() const;
    /** @brief Performs the mean / sigma / C / path updates from the ranked offspring */
    void updateDistribution(const std::vector<std::size_t> &ranked);

    /***************************************************************************/
    // User-facing strategy parameters (configured by the user / factory)

    std::size_t lambda_ = DEFAULTSEPCMALAMBDA; ///< Number of offspring per generation (0 = auto)
    std::size_t mu_ = 0;                        ///< Number of selected parents (0 = lambda/2)
    bool use_diagonal_cma_ = true;             ///< Whether to run the diagonal covariance update
    bool pareto_mode_ = false;                 ///< Whether to use NSGA-II-style MO selection
    double initial_sigma_ =
        0.3; ///< Initial global step size as a fraction of the parameter range

    /***************************************************************************/
    // Internal distribution state (initialized at setup, evolves each cycle)

    std::size_t n_ = 0;                ///< The number of (double) parameters
    double sigma_ = 0.;               ///< The current global step size
    std::vector<double> m_;          ///< The distribution mean
    std::vector<double> C_;          ///< The diagonal covariance (per-coordinate variance)
    std::vector<double> p_sigma_;    ///< The conjugate evolution path (CSA)
    std::vector<double> p_c_;        ///< The anisotropic evolution path (rank-1)
    bool state_initialized_ = false; ///< Whether the distribution state has been set up

    /***************************************************************************/
    // Derived strategy constants (computed once in setUpStrategyParameters())

    double mu_eff_ = 0.;             ///< Variance-effective selection mass
    double c_sigma_ = 0.;           ///< CSA step-size cumulation rate
    double d_sigma_ = 0.;           ///< CSA step-size damping
    double c_c_ = 0.;               ///< Cumulation rate for the rank-1 path
    double c_1_ = 0.;               ///< Rank-1 covariance learning rate
    double c_mu_ = 0.;              ///< Rank-mu covariance learning rate
    double chi_n_ = 0.;            ///< Expectation of ||N(0,I)||
    std::vector<double> weights_; ///< Positive recombination weights (length mu)
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

