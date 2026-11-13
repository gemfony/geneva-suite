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
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GParChild.hpp"
#include "geneva/oa/GTunableManifest.hpp"
#include "geneva/oa/GOptimizationAlgorithmT.hpp"

#ifdef GEM_TESTING
#include "geneva/individuals/GTestIndividual1.hpp"
#include "common/GSelfTestable.hpp"
#endif /* GEM_TESTING */

namespace Gem::Geneva::OptimizationAlgorithms {
/**
 * The default sorting mode (matches the classic EA)
 */
constexpr auto DEFAULTEASORTINGMODE = sortingMode::MUCOMMANU_SINGLEEVAL;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The Geneva evolutionary algorithm ("ea"): a \f$(\mu,\lambda)\f$ / \f$(\mu+\lambda)\f$
 * self-adaptive evolution strategy with a selectable step-size controller that repairs the classic
 * σSA-ES's poor @e fine convergence in high dimension.
 *
 * @details
 * This is the canonical Geneva EA. It evolves a population through the standard generational loop on the
 * GParChild spine and adds a @c stepControl strategy (read from the OA-owned GAdaptionConfig) selecting
 * @e how the mutation step size \f$\sigma\f$ is controlled. The legacy (pre-adaptive) EA is exactly the
 * \c SELF_ADAPT mode; the other controllers are opt-in improvements.
 *
 * @par Generational loop
 * With \f$\mu\f$ parents and \f$\lambda\f$ offspring (population size \f$\lambda\f$, @c n_parents
 * \f$=\mu\f$), each generation performs:
 * -# @b Recombination — every child is filled from the parents under the chosen duplication scheme
 *    (default / random / value-weighted-by-rank); the base scheme copies one parent's whole slot (genome
 *    and its \f$\sigma\f$ scratch) into each child.
 * -# @b Mutation — each child's genome is perturbed coordinate-wise and its per-individual \f$\sigma\f$
 *    self-adapts (below).
 * -# @b Evaluation — the whole offspring set is submitted to the single process consumer and scored.
 * -# @b Selection — the population is ranked under one of the \c sortingMode schemes and the best
 *    \f$\mu\f$ become the next parents.
 *
 * @par Selection schemes (\c sortingMode)
 * \c MUPLUSNU_SINGLEEVAL (elitist \f$\mu+\lambda\f$: parents compete with their children),
 * \c MUCOMMANU_SINGLEEVAL (non-elitist \f$\mu,\lambda\f$: parents are discarded — the default, and the
 * theoretically correct pairing for mutative \f$\sigma\f$ self-adaption), \c MUNU1PRETAIN_SINGLEEVAL
 * (comma selection, but the single best parent is retained), and the two multi-objective variants
 * \c MUPLUSNU_PARETO / \c MUCOMMANU_PARETO (see the Pareto selection below).
 *
 * @par Multi-objective (Pareto) selection
 * In the two PARETO sorting modes the \f$\mu\f$ survivors are chosen by the standard @b NSGA-II procedure
 * (Deb et al. 2002), via the shared OptimizationAlgorithms::nonDominatedRank helper: the eligible
 * individuals (parents + children for \c MUPLUSNU_PARETO, children only for \c MUCOMMANU_PARETO) are
 * partitioned into non-domination fronts (a candidate dominates another iff it is no worse on every
 * criterion and strictly better on at least one), and within a front that would overflow the \f$\mu\f$
 * budget the survivors are taken by @e decreasing crowding distance, so a well-spread subset of the
 * trade-off surface is retained rather than an arbitrary one. (On a single-criterion individual these
 * modes degenerate to the corresponding single-objective scheme.) The same helper backs the Pareto mode
 * of GSepCmaEvolutionStrategy, so every Pareto-capable algorithm in Geneva shares one dominance test and
 * one ranking.
 *
 * @par Mutation and classic step-size self-adaption (σSA)
 * Each coordinate \f$x_j\f$ of a child is perturbed, with per-group probability \f$p_{\mathrm{ad}}\f$, by
 * an isotropic Gaussian scaled by that parameter's range \f$r\f$, and \f$\sigma\f$ itself self-adapts
 * log-normally with learning rate \f$\tau\f$ (the config's @c sigma_sigma):
 * \f[
 *   x_j \leftarrow x_j + r\,\sigma\,\mathcal{N}(0,1),
 *   \qquad
 *   \sigma \leftarrow \sigma\,\exp\!\bigl(\tau\,\mathcal{N}(0,1)\bigr).
 * \f]
 *
 * @par Why a controller is needed (motivation)
 * Classic σSA uses a @e fixed \f$\tau\f$ (default 0.8) never scaled by the dimension \f$n\f$, whereas
 * self-adaptive-ES theory prescribes \f$\tau\propto 1/\sqrt{2n}\f$:
 * \f[
 *   \begin{array}{c|ccc}
 *     n & 10 & 10^{3} & 10^{4}\\\hline
 *     1/\sqrt{2n} & 0.22 & 0.022 & 0.007\\
 *     \text{fixed }\tau & 0.8 & 0.8 & 0.8
 *   \end{array}
 * \f]
 * At \f$n=10^{4}\f$ the fixed rate is \f$\sim\!100\times\f$ too large, so near the optimum the over-hot
 * log-normal update random-walks \f$\sigma\f$ instead of letting it settle and fine convergence stalls.
 *
 * @par Step-control modes (selected on the GAdaptionConfig)
 * The default is @b SELF_ADAPT_SCALED: the textbook dimension-scaled per-parameter self-adaption, which
 * converges cleanly across sorting modes and dimensions. @b SELF_ADAPT is the bit-for-bit legacy
 * behaviour. @b ONE_FIFTH and @b CSA are the single-global-\f$\sigma\f$ controllers; they are sound when
 * their success signal is the textbook Rechenberg quantity (below) and remain available for problems where
 * one robustly-controlled global step size is preferable to per-coordinate self-adaption.
 *
 * @par Tradeoff: per-parameter vs. global \f$\sigma\f$
 * @b SELF_ADAPT and @b SELF_ADAPT_SCALED keep the legacy structure of one self-adapting \f$\sigma\f$ @e per
 * adaption group (i.e. per parameter, for a genome built with one group per value), so each coordinate can
 * acquire its own step size. @b ONE_FIFTH and @b CSA instead control a @e single global \f$\sigma\f$ shared
 * by all parameters (the per-group self-adaption is switched off); the per-parameter @c range factor in the
 * step \f$ r\,\sigma\,\mathcal{N}(0,1) \f$ still scales by each parameter's bounds, but there is no longer an
 * independently-adapting \f$\sigma\f$ per coordinate. The global-\f$\sigma\f$ controllers can be preferable
 * where the per-coordinate @e mutative self-adaption is too noisy to learn useful per-axis scales (high
 * dimension), while @b SELF_ADAPT_SCALED — the default — is a robust all-round choice and is preferable on
 * low-dimensional or strongly heterogeneous-scale problems where per-coordinate \f$\sigma\f$ is both
 * reliable and useful. (Robust @e derandomized per-coordinate scaling is what GSepCmaEvolutionStrategy's
 * diagonal covariance provides, which is why it outperforms both.)
 * - @b SELF_ADAPT — the classic σSA above, unchanged; reproduces the legacy EA bit-for-bit.
 * - @b SELF_ADAPT_SCALED — the same log-normal rule with the textbook dimension-scaled rate
 *   \f[
 *     \tau=\frac{c}{\sqrt{2n}}\ \text{(one shared }\sigma\text{)},
 *     \qquad
 *     \tau'=\frac{c}{\sqrt{2\sqrt{n}}}\ \text{(per-coordinate }\sigma\text{)},
 *   \f]
 *   for the user constant \f$c=\,\f$@c learning_rate_c (default 1). (Schwefel 1981; Beyer & Schwefel 2002.)
 * - @b ONE_FIFTH — Rechenberg's \f$1/5\f$ success rule on a single global \f$\sigma\f$. With
 *   \f$p_{\mathrm{succ}}\f$ the fraction of the offspring that improved on THEIR OWN PARENT (the textbook
 *   per-offspring success rate, measured before selection reorders the population), and damping
 *   \f$d=0.2\f$,
 *   \f[ \sigma \leftarrow \sigma\,\exp\!\Bigl(\tfrac{p_{\mathrm{succ}}-1/5}{1+d}\Bigr). \f]
 *   (Rechenberg 1973.) Measuring against each child's own parent — rather than a moving population-best
 *   reference — is what keeps the controller stable under comma selection (where the reference would
 *   otherwise degrade as \f$\sigma\f$ overshoots, driving \f$\sigma\f$ to run away instead of annealing).
 * - @b CSA — a scalar cumulative step-size adaptation: an evolution-path proxy \f$p_\sigma\f$ accumulates
 *   the normalised success-rate deviation \f$s=(p_{\mathrm{succ}}-1/5)/(1-1/5)\f$ from the \f$1/5\f$
 *   target, with cumulation constant \f$c_\sigma=1/(1+\sqrt{n}/4)\f$:
 *   \f[
 *     p_\sigma \leftarrow (1-c_\sigma)\,p_\sigma + \sqrt{c_\sigma(2-c_\sigma)}\;s,
 *     \qquad
 *     \sigma \leftarrow \sigma\,\exp(0.3\,p_\sigma),
 *   \f]
 *   with \f$\sigma\f$ clamped to \f$[10^{-12},\sigma_{\max}]\f$, where \f$\sigma_{\max}\f$ is the authored
 *   per-group @c max_sigma (in the normalized model \f$\sigma\f$ is a fraction of the parameter range, so
 *   the config's ceiling is the meaningful bound). (The scalar analogue of the vector evolution-path
 *   step control of Hansen & Ostermeier 2001 — the same principle used at covariance level 0 in
 *   GSepCmaEvolutionStrategy.)
 *
 * @par Intermediate recombination of \f$\sigma\f$
 * For the per-individual-\f$\sigma\f$ modes (SELF_ADAPT / SELF_ADAPT_SCALED), when @c recombine_sigma is
 * set, every child's \f$\sigma\f$ is replaced after recombination by the mean of the \f$\mu\f$ parents'
 * \f$\sigma\f$,
 * \f[ \sigma_{\text{child}} \leftarrow \frac{1}{\mu}\sum_{i=1}^{\mu}\sigma_i, \f]
 * the standard σSA variance-reducing stabiliser that the classic GParChild::recombine (values only) omits.
 * It is a no-op for the global-\f$\sigma\f$ modes (every slot already shares one \f$\sigma\f$).
 *
 * @par Reproducing the legacy (pre-adaptive) EA exactly
 * Set @c stepControl = @c SELF_ADAPT and @c recombine_sigma = @c false, leaving the remaining EA settings
 * (@c size, @c n_parents, @c sorting_method, @c max_iteration) and the adaption config (@c sigma,
 * @c min/max_sigma, @c sigma_sigma, @c ad_prob, …) at the legacy values. Verified by code cross-check:
 * every step then performs the identical operations and consumes the RNG in the identical order, so the
 * results match the old EA up to the (non-deterministic) seed.
 *
 * @note Clean-room implementation from the published methods (the equations above); no third-party
 * optimizer source was consulted.
 *
 * @par References
 * - I. Rechenberg, "Evolutionsstrategie: Optimierung technischer Systeme nach Prinzipien der biologischen
 *   Evolution", Frommann-Holzboog, 1973 (the \f$1/5\f$ success rule).
 * - H.-P. Schwefel, "Numerical Optimization of Computer Models", Wiley, 1981 (mutative \f$\sigma\f$
 *   self-adaptation; the \f$1/\sqrt{2n}\f$ learning rate).
 * - H.-G. Beyer, H.-P. Schwefel, "Evolution Strategies: A Comprehensive Introduction", Natural Computing
 *   1(1):3-52, 2002.
 * - N. Hansen, A. Ostermeier, "Completely Derandomized Self-Adaptation in Evolution Strategies",
 *   Evolutionary Computation 9(2):159-195, 2001 (cumulative step-size adaptation).
 * - K. Deb, A. Pratap, S. Agarwal, T. Meyarivan, "A Fast and Elitist Multiobjective Genetic Algorithm:
 *   NSGA-II", IEEE Trans. Evolutionary Computation 6(2):182-197, 2002 (the Pareto selection).
 */
//! [geneva.oa.identifiers]
class GEvolutionaryAlgorithm // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GEvolutionaryAlgorithm, GParChild>
  , public Gem::Common::GSelfTestable {
public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold.
    static constexpr std::string_view class_name = "GEvolutionaryAlgorithm";
    static constexpr std::string_view oa_algorithm_name = "Evolutionary Algorithm";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_EA";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GEvolutionaryAlgorithm_PersonalityTraits;
//! [geneva.oa.identifiers]

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class'es local data members */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("sorting_mode_", self.sorting_mode_),
            Gem::Common::make_member("step_control_", self.step_control_),
            Gem::Common::make_member("learning_rate_c_", self.learning_rate_c_),
            Gem::Common::make_member("recombine_sigma_", self.recombine_sigma_)
        );
    }

    // serialize(), load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base (via GOptimizationAlgorithmT) from class_name and localMembers_().

    ///////////////////////////////////////////////////////////////////////

public:
    /***************************************************************************/
    /** @brief The default constructor */
    GEvolutionaryAlgorithm();
    /** @brief A standard copy constructor. @param The object to be copied */
    GEvolutionaryAlgorithm(const GEvolutionaryAlgorithm &) = default;
    /** @brief The standard destructor */
    ~GEvolutionaryAlgorithm() override = default;

    /** @brief Sets the sorting scheme. @param smode The selection/sorting scheme to use */
    void setSortingScheme(sortingMode smode);
    /** @brief Retrieves the current sorting scheme. @return The currently configured sorting scheme */
    sortingMode getSortingScheme() const;

    /** @brief Sets the step-size-control strategy. The default is SELF_ADAPT_SCALED.
     *  @param sc The strategy (SELF_ADAPT, SELF_ADAPT_SCALED, ONE_FIFTH or CSA) */
    void setStepControl(stepControl sc);
    /** @brief Retrieves the step-size-control strategy. @return The configured strategy. */
    stepControl getStepControl() const;

    /** @brief The current global step size of the ONE_FIFTH / CSA controllers (run scratch;
     *  seeded at init from the slots' representative sigma, updated once per generation).
     *  Primarily an observability hook for monitors and tests.
     *  @return The controllers' current global sigma (1.0 while no controller is active). */
    double getGlobalSigma() const noexcept { return global_sigma_; }

    /** @brief Sets the learning-rate constant c for SELF_ADAPT_SCALED (tau = c/sqrt(2n)).
     *  @param c The constant (≈1 by convention). */
    void setLearningRateConstant(double c);
    /** @brief Retrieves the learning-rate constant c. @return The configured constant. */
    double getLearningRateConstant() const;

    /** @brief Enables/disables intermediate recombination of the per-individual sigma.
     *  @param r true to average parents' sigma into children after recombination. */
    void setSigmaRecombination(bool r);
    /** @brief Whether intermediate sigma-recombination is enabled. @return true if enabled. */
    bool getSigmaRecombination() const;

    /** @brief Extracts all individuals on the pareto front.
     *  @param pareto_inds Output vector filled with freshly cloned copies of the individuals
     *         currently on the front; the caller takes sole ownership of them */
    void extractCurrentParetoIndividuals(
        std::vector<std::unique_ptr<gen::GGenome>> &pareto_inds
    );

    /** @brief The knobs a meta-optimizer may tune. @return The ordered list of tunable parameters. */
    static std::vector<TunableParam> tunableManifest();

    /** @brief Requests INLINE evaluation (see GEvolutionaryAlgorithm). @param inln true to evaluate inline */
    void setInlineEvaluation(bool inln) { inline_evaluation_ = inln; }
    /** @brief Whether inline (in-thread) evaluation is enabled. @return true if inline */
    [[nodiscard]] bool getInlineEvaluation() const { return inline_evaluation_; }

protected:
    /***************************************************************************/
    // Virtual or overridden protected functions

    /** @brief Adds local configuration options. @param gpb The parser-builder */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceT base.

    /** @brief Resets the settings to what was configured at the optimize()-call */
    void resetToOptimizationStart_() override;

    /** @brief Performs initialization work before the optimization loop starts (installs the step controller). */
    void init() override;

    /** @brief Recombination + (optionally) intermediate sigma-recombination of children. */
    void recombine() override;

    /** @brief Applies modifications to this object */
    bool modify_GUnitTests_() override;
    /** @brief Performs self tests that are expected to succeed */
    void specificTestsNoFailureExpected_GUnitTests_() override;
    /***************************************************************************/

private:
    /***************************************************************************/
    // Overloaded or virtual base functions (mirror GEvolutionaryAlgorithm).

    /** @brief We submit individuals to the process consumer and wait for processed items */
    void evaluatePopulation_() override;

    /** @brief Evaluates the population's [start, end) range and returns the executor status. */
    virtual Gem::Courtier::submission_status_t
    evaluatePopulationRange_(std::size_t start, std::size_t end);

    /** @brief Adds the iteration's individuals to the global-best priority queue. */
    void updateGlobalBestsPQ_(gen::GGenomeFixedSizePriorityQueue &best_individuals) override;
    /** @brief Adds the iteration's individuals to this iteration's best priority queue. */
    void updateIterationBestsPQ_(gen::GGenomeFixedSizePriorityQueue &best_individuals) override;

    /** @brief Retrieve a personality-traits object belonging to this algorithm. */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;

    /** @brief Choose new parents, based on the selection scheme set by the user (then drive the controller). */
    void selectBest_() override;

    /** @brief Some error checks related to population sizes */
    void populationSanityChecks_() const override;

    /***************************************************************************/
    // Step-control machinery (the addition over GEvolutionaryAlgorithm).

    /** @brief Installs the step-size controller onto the OA-owned adaption config at init() (rescales /
     *  suppresses self-adaption per the chosen mode) and seeds the controller's global-sigma state. */
    void installStepController();
    /** @brief Measures the offspring success rate (fraction of children whose fitness beats their OWN
     *  parent) BEFORE selection reorders the population, storing it in last_p_success_ for the global
     *  step controller to consume. The Rechenberg success signal; a no-op for the self-adaptive modes
     *  and in the first generation (no meaningful parents yet). Called at the top of selectBest_. */
    void measureOffspringSuccess_();
    /** @brief Drives the global-sigma controller (ONE_FIFTH / CSA) once per generation after selection,
     *  consuming the pre-selection success rate from measureOffspringSuccess_(), then pushes the updated
     *  global sigma into every population slot's scratch. A no-op for the self-adaptive modes. */
    void driveGlobalSigmaController();

    /***************************************************************************/
    // Selection helpers (verbatim from GEvolutionaryAlgorithm so all five sorting modes work, Pareto incl.).

    void sortMuPlusNuMode();
    void sortMuCommaNuMode();
    void sortMunu1pretainMode();
    void sortMuPlusNuParetoMode();
    void sortMuCommaNuParetoMode();
    /** @brief NSGA-II Pareto selection of the mu survivors (non-dominated front + crowding distance, via
     *  the shared OptimizationAlgorithms::nonDominatedRank). @param include_parents true for mu+nu (parents
     *  and children compete), false for mu,nu (only children). */
    void selectParetoParents(bool include_parents);

    void fillWithObjects(const std::size_t &n_individuals);

    /***************************************************************************/
    // Local data

    sortingMode sorting_mode_ = DEFAULTEASORTINGMODE; ///< The chosen sorting scheme

    /** @brief The step-size-control strategy applied to the adaption config (default SELF_ADAPT_SCALED). */
    stepControl step_control_ = stepControl::SELF_ADAPT_SCALED;
    /** @brief The learning-rate constant c for SELF_ADAPT_SCALED (tau = c/sqrt(2n)). */
    double learning_rate_c_ = 1.;
    /** @brief Whether to intermediate-recombine the per-individual sigma after recombination. */
    bool recombine_sigma_ = true;

    /** @brief Evaluate the population inline (transient; not serialized/compared). */
    bool inline_evaluation_ = false;

    /***************************************************************************/
    // Transient run scratch for the global-sigma controllers (ONE_FIFTH / CSA). NOT serialized / compared
    // / part of localMembers(): rebuilt at init() from the seed sigma in the adaption config.
    double global_sigma_ = 1.;       ///< the single global step size (ONE_FIFTH / CSA)
    double p_sigma_ = 0.;            ///< the CSA evolution-path accumulator (scalar proxy)
    double last_p_success_ = 0.;     ///< offspring success rate (fraction of children beating their own parent),
                                     ///< measured before selection reorders the population (see measureOffspringSuccess_)
    bool   controller_warmed_up_ = false; ///< whether at least one generation's success rate has been measured (warm-up guard)
    std::size_t controller_dim_ = 0; ///< the adapted dimension n the controller reasons about

    /***************************************************************************/
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/** @brief Streams a summary of the parent individuals' fitness values. */
std::ostream &operator<<(std::ostream &os, const GEvolutionaryAlgorithm &pop);

/******************************************************************************/
} /* namespace Gem::Geneva::OptimizationAlgorithms */

