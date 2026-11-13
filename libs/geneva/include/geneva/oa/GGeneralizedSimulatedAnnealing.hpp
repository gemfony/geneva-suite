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
#include <cstdint>
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
#include "geneva/oa/GGeneralizedSimulatedAnnealing_PersonalityTraits.hpp"

#ifdef GEM_TESTING
#include "geneva/individuals/GTestIndividual1.hpp"
#include "common/GSelfTestable.hpp"
#endif /* GEM_TESTING */

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * Default values for the Generalized (Dual) Simulated Annealing algorithm.
 */
constexpr std::size_t DEFAULTGSANCHAINS = 1; ///< Number of independent Markov chains
constexpr double DEFAULTGSAQV = 2.62;        ///< Visiting (distribution) parameter q_v
constexpr double DEFAULTGSAQA = -5.0;        ///< Acceptance parameter q_a
constexpr double DEFAULTGSAT0 = 0.;          ///< Initial visiting temperature (0 => derive from ranges)
constexpr std::uint32_t DEFAULTGSAREANNEAL =
    0; ///< Per-chain stall steps before a cooling-clock restart (0 = disabled)
constexpr double DEFAULTGSACOOLING =
    1.; ///< Cooling timescale: divides the step counter in the schedule (1 = strict/faithful schedule)

/** @brief Number of population slots used per chain (current + proposal) */
constexpr std::size_t GSA_SLOTS_PER_CHAIN = 2;
/** @brief Slot offset of the current ("incumbent") point within a chain block */
constexpr std::size_t GSA_CURRENT = 0;
/** @brief Slot offset of the proposal point within a chain block */
constexpr std::size_t GSA_PROPOSAL = 1;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * GGeneralizedSimulatedAnnealing implements Generalized (Dual) Simulated
 * Annealing (GSA), the Tsallis-statistics generalization of classical simulated
 * annealing introduced by Tsallis & Stariolo and popularized by Xiang, Sun, Fan
 * & Gong (1997). It runs \f$P\f$ independent Markov chains in parallel
 * (Geneva-style "larger population" simulated annealing). Each chain keeps a
 * current point \f$x \in \mathbb{R}^n\f$ with energy \f$E(x)\f$ (the min-only
 * transformed fitness); the global best over all chains and iterations is
 * tracked by the standard Geneva best-individual machinery.
 *
 * GSA differs from the classical geometric-cooling Metropolis algorithm
 * (GSimulatedAnnealing) in three ways: a heavy-tailed (Tsallis) **visiting**
 * distribution, a **generalized** acceptance probability, and a **power-law**
 * cooling schedule.
 *
 * \par Power-law cooling
 * The artificial visiting temperature at step \f$t=1,2,\dots\f$ follows the
 * Tsallis schedule
 * \f[
 *   T_{q_v}(t) \;=\; T_{q_v}(1)\,
 *      \frac{2^{\,q_v-1}-1}{(1+t)^{\,q_v-1}-1},
 * \f]
 * which decays far more slowly (polynomially) than the geometric law
 * \f$T(t)=\alpha^t T(0)\f$ of the classical algorithm, so long exploratory
 * jumps remain possible for many more steps. The acceptance temperature is
 * coupled to it by
 * \f[
 *   T_{q_a}(t) \;=\; \frac{T_{q_v}(t)}{t}.
 * \f]
 *
 * \par Cooling timescale (long, high-dimensional runs)
 * The strict schedule above advances its clock by one per step, so the visiting
 * temperature reaches near-local values within the first \f$\sim 10^2\f$ steps.
 * That is well matched to short or low-dimensional runs, but on a
 * high-dimensional landscape the chains freeze long before they have refined all
 * coordinates. A configurable \em cooling \em timescale \f$\kappa\ge 1\f$
 * therefore stretches the clock,
 * \f[
 *   t_{\mathrm{eff}} \;=\; 1 + \frac{t-1}{\kappa},
 * \f]
 * and both \f$T_{q_v}\f$ and \f$T_{q_a}\f$ are evaluated at \f$t_{\mathrm{eff}}\f$.
 * The faithful default \f$\kappa=1\f$ recovers the exact Tsallis schedule
 * (\f$t_{\mathrm{eff}}=t\f$); larger values cool proportionally more slowly so the
 * jumps stay useful across a long, time-bounded, high-dimensional search.
 * The initial visiting temperature \f$T_{q_v}(1)=T_0\f$ may be supplied
 * explicitly or, when left at \f$0\f$, defaults to one normalized parameter range
 * (the chains move in the normalized internal coordinate, whose interval has width 1).
 *
 * \par Tsallis visiting distribution
 * The exact generalized visiting density of Tsallis & Stariolo for a jump
 * \f$\Delta x \in \mathbb{R}^n\f$ is
 * \f[
 *   g_{q_v}(\Delta x) \;\propto\;
 *     \frac{\big[T_{q_v}(t)\big]^{-n/(3-q_v)}}
 *          {\Big\{1+(q_v-1)\,
 *             \dfrac{(\Delta x)^2}{\big[T_{q_v}(t)\big]^{2/(3-q_v)}}\Big\}^{
 *             \frac{1}{q_v-1}+\frac{n-1}{2}}}.
 * \f]
 * For \f$q_v\to 1\f$ this degenerates to a Gaussian; for \f$1<q_v<3\f$ it is a
 * heavy-tailed \f$n\f$-dimensional generalized-Cauchy/Student form whose tails
 * grow heavier (and whose long jumps become more frequent) as \f$q_v\f$
 * increases.
 *
 * \par The sampler implemented here (reproducible)
 * Rather than the exact (fiddly) inverse transform, this class draws a jump from
 * a Gaussian-core / heavy-tail mixture that reproduces the qualitative
 * behaviour of \f$g_{q_v}\f$ and -- crucially for a practical iteration budget --
 * makes the jump scale track the temperature \em polynomially. The strictly
 * faithful Tsallis exponent \f$1/(3-q_v)\f$ collapses the step size far too
 * quickly to be useful within a finite run, so the scale is instead tied
 * \em linearly to \f$T_{q_v}(t)\f$ (which itself decays polynomially):
 * \f[
 *   \tau(t) \;=\; \bar{L}\,\frac{T_{q_v}(t)}{T_{q_v}(1)} ,
 * \f]
 * where \f$\bar{L}\f$ is the jump scale. The derived initial visiting temperature
 * is set to \f$T_{q_v}(1)=\bar{L}=1\f$ (one normalized parameter range), so that the
 * \em initial jump scale is exactly \f$\tau(1)=1\f$ -- the full normalized range --
 * and \f$\tau(t)\f$ then decays exactly with the visiting
 * temperature, giving broad early exploration and fine late refinement within a
 * finite iteration budget. For each chain we then form the jump
 * \f[
 *   \Delta x \;=\; \tau(t)\;\frac{g}{|z|^{(q_v-1)/2}},
 *   \qquad g \sim \mathcal{N}(0,I_n),\;\; z \sim \mathcal{N}(0,1),
 * \f]
 * where \f$g\f$ is a fresh unit Gaussian vector and \f$z\f$ is a single scalar
 * Gaussian, clamped away from zero (\f$|z|\ge\varepsilon\f$) so the divisor is
 * finite. The Gaussian numerator gives a well-behaved core, while the random
 * scalar divisor \f$|z|^{-(q_v-1)/2}\f$ injects the heavy tail: for \f$q_v>1\f$
 * small \f$|z|\f$ occasionally produces very long jumps, with the tail growing
 * heavier as \f$q_v\to 3\f$ and vanishing (\f$\Delta x \to \tau\,g\f$, a pure
 * Gaussian) as \f$q_v\to 1\f$. Because every jump is multiplied by
 * \f$\tau(t)\f$, the step size shrinks as the temperature cools: at high
 * \f$T_{q_v}\f$ the algorithm makes large exploratory moves on the order of the
 * search box, and as \f$T_{q_v}\to 0\f$ the proposals collapse toward local
 * refinement. The proposal is \f$x_{\mathrm{new}} = x + \Delta x\f$, after which
 * the genome's constrained fold/clamp is applied (unbounded coordinates are
 * left unclamped).
 *
 * \par Generalized acceptance
 * With \f$\Delta E = E(x_{\mathrm{new}}) - E(x)\f$ a downhill move
 * (\f$\Delta E \le 0\f$) is always accepted. An uphill move is accepted with the
 * generalized-Metropolis probability
 * \f[
 *   P_{q_a} \;=\;
 *     \Big[\,1-(1-q_a)\,\frac{\Delta E}{T_{q_a}(t)}\,\Big]^{\frac{1}{1-q_a}},
 * \f]
 * evaluated only where the bracket is positive (otherwise \f$P_{q_a}=0\f$) and
 * clamped to \f$[0,1]\f$. For \f$q_a<1\f$ this is a heavy-tailed acceptance rule
 * that, unlike the exponential Metropolis criterion, still occasionally accepts
 * markedly worse moves, aiding escape from local optima. As \f$q_a\to 1\f$ it
 * recovers the classical \f$\exp(-\Delta E/T_{q_a})\f$.
 *
 * \par Reannealing
 * If a chain fails to improve its own best energy for a configurable number of
 * steps (\c reannealing_steps, 0 = disabled), that chain's cooling clock is
 * reset toward \f$T_{q_v}(1)\f$ (its step index \f$t\f$ is sent back to 1),
 * re-enabling large exploratory jumps -- a per-chain restart of the schedule.
 *
 * \par Population layout
 * Each chain occupies two consecutive population slots,
 * \f$[\,x_{\mathrm{cur}} \mid x_{\mathrm{prop}}\,]\f$, so the population holds
 * \f$2P\f$ individuals. In every iteration a proposal is written into each
 * chain's proposal slot, the whole population is evaluated through the one
 * process consumer, and the generalized acceptance rule then (conditionally)
 * copies the proposal into the current slot.
 *
 * \par Reuse of Geneva facilities
 * Candidate parameters are read and written through the standard flat genome FP
 * channels (streamlineFPInternal, assignFPValueVectorInternal) with
 * activityMode::ACTIVEONLY, and the whole population is evaluated through the one
 * process consumer via workOnPopulation(), mirroring the other from-scratch
 * flat-genome optimizers (e.g. GSepCmaEvolutionStrategy, GStandardPSO2011,
 * GAntColonyOptimization). Ranking uses the min-only transformed fitness helper
 * (Gem::Geneva::minOnly_transformed_fitness) together with isBetter()/getMaxMode()
 * so that maximisation problems are handled transparently. All randomness is drawn
 * from the algorithm's inherited generator (gr_) and inherited
 * uniform_real_distribution_, exactly as the sibling strategies draw theirs.
 *
 * The algorithm is floating-point only and needs no adaption configuration; it
 * mutates by sampling, not by per-parameter adaptors.
 *
 * @par References
 * - C. Tsallis and D. A. Stariolo, "Generalized simulated annealing",
 *   Physica A, 233(1-2):395-406, 1996.
 * - Y. Xiang, D. Y. Sun, W. Fan and X. G. Gong, "Generalized simulated annealing
 *   algorithm and its application to the Thomson model", Physics Letters A,
 *   233(3):216-220, 1997.
 */
class GGeneralizedSimulatedAnnealing // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GGeneralizedSimulatedAnnealing>
  , public Gem::Common::GSelfTestable {
public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold.
    static constexpr std::string_view class_name = "GGeneralizedSimulatedAnnealing";
    static constexpr std::string_view oa_algorithm_name = "Generalized Simulated Annealing";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_GSA";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GGeneralizedSimulatedAnnealing_PersonalityTraits;

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class'es local data members (drives serialize/load/compare).
     *  Only the scalar configuration (n_chains, q_v, q_a, t0, reannealing) is persisted; all per-chain
     *  state (current points/energies, cooling clocks, stall counters, parameter bounds) is transient
     *  and rebuilt in init(). */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("n_chains_", self.n_chains_),
            Gem::Common::make_member("qv_", self.qv_),
            Gem::Common::make_member("qa_", self.qa_),
            Gem::Common::make_member("t0_", self.t0_),
            Gem::Common::make_member("reannealing_steps_", self.reannealing_steps_),
            Gem::Common::make_member("cooling_timescale_", self.cooling_timescale_)
        );
    }

    // serialize(), load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base (via GOptimizationAlgorithmT) from class_name and localMembers_().
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The default constructor */
    GGeneralizedSimulatedAnnealing();
    /** @brief Initialization with the number of chains */
    explicit GGeneralizedSimulatedAnnealing(std::size_t n_chains);
    /** @brief A standard copy constructor */
    GGeneralizedSimulatedAnnealing(const GGeneralizedSimulatedAnnealing &) = default;
    /** @brief The destructor */
    ~GGeneralizedSimulatedAnnealing() override = default;

    /** @brief Retrieves the number of independent Markov chains */
    std::size_t getNChains() const;
    /** @brief Allows to set the number of independent Markov chains */
    void setNChains(std::size_t n_chains);

    /** @brief Sets the visiting (distribution) parameter q_v (in ]1,3[) */
    void setQv(double qv);
    /** @brief Retrieves the visiting (distribution) parameter q_v */
    double getQv() const;

    /** @brief Sets the acceptance parameter q_a (!= 1) */
    void setQa(double qa);
    /** @brief Retrieves the acceptance parameter q_a */
    double getQa() const;

    /** @brief Sets the initial visiting temperature (0 => derive from ranges) */
    void setT0(double t0);
    /** @brief Retrieves the initial visiting temperature */
    double getT0() const;

    /** @brief Sets the per-chain reannealing stall threshold (0 = disabled) */
    void setReannealingSteps(std::uint32_t reannealing_steps);
    /** @brief Retrieves the per-chain reannealing stall threshold */
    std::uint32_t getReannealingSteps() const;

    /** @brief Sets the cooling timescale (stretches the schedule; 1 = strict/faithful, larger = slower cooling) */
    void setCoolingTimescale(double cooling_timescale);
    /** @brief Retrieves the cooling timescale */
    double getCoolingTimescale() const;

protected:
    /***************************************************************************/
    // Virtual or overridden protected functions

    /** @brief Adds local configuration options to a GParserBuilder object */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceT base.

    /** @brief Resets the settings of this population to what was configured when optimize() was issued */
    void resetToOptimizationStart_() override;

    /** @brief Does some preparatory work before the optimization starts */
    void init() override;
    /** @brief Does any necessary finalization work */
    void finalize() override;

    /** @brief Proposes a new point for every chain into its proposal slot */
    virtual void proposeMoves();
    /** @brief Applies the generalized acceptance rule, cools and reanneals each chain */
    virtual void applyAcceptance();

    /** @brief Applies modifications to this object. This is needed for testing purposes */
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
    /** @brief Triggers fitness calculation of the relevant individuals via the one process consumer */
    void evaluatePopulation_() override;


    /** @brief Retrieve a GPersonalityTraits object belonging to this algorithm */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;

    /** @brief Resizes the population to the desired level and does some error checks */
    void adjustPopulation_() override;


    /***************************************************************************/
    // Algorithm-internal helpers

    /** @brief Maps a raw step counter to the cooling-timescale-stretched effective time */
    double effectiveTime(std::uint32_t t) const;

    /** @brief Computes the Tsallis power-law visiting temperature at step t */
    double visitingTemperature(std::uint32_t t) const;
    /** @brief Draws a single Tsallis-style heavy-tailed jump vector for a given visiting temperature */
    std::vector<double> drawVisitingJump(double tqv);
    /** @brief Generalized acceptance probability for an uphill move */
    double acceptanceProbability(double delta_e, double tqa) const;

    /** @brief Convenience: population index of the current point of chain c */
    static std::size_t currentPos(std::size_t c);
    /** @brief Convenience: population index of the proposal point of chain c */
    static std::size_t proposalPos(std::size_t c);

    /***************************************************************************/
    // User-facing strategy parameters (configured by the user / factory)

    std::size_t n_chains_ = DEFAULTGSANCHAINS; ///< The number of independent Markov chains
    double qv_ = DEFAULTGSAQV;                 ///< Visiting (distribution) parameter q_v
    double qa_ = DEFAULTGSAQA;                 ///< Acceptance parameter q_a
    double t0_ = DEFAULTGSAT0;                 ///< Initial visiting temperature (0 => derived in init())
    std::uint32_t reannealing_steps_ =
        DEFAULTGSAREANNEAL; ///< Per-chain stall steps before a cooling-clock restart (0 = disabled)
    double cooling_timescale_ =
        DEFAULTGSACOOLING; ///< Divides the step counter in the schedule (1 = strict; larger slows cooling)

    /***************************************************************************/
    // Internal per-chain state (transient; rebuilt in init(), excluded from serialize/compare)

    std::size_t n_fp_parms_ = 0; ///< The number of active floating point parameters (set in init())

    double t0_effective_ = 0.; ///< The visiting temperature actually used (== t0_ or derived)

    std::vector<std::uint32_t>
        chain_step_; ///< Per-chain cooling-clock step index t (reset on reanneal)
    std::vector<double> chain_best_energy_; ///< Per-chain best energy seen so far
    std::vector<std::uint32_t> chain_stall_; ///< Per-chain steps since the last improvement
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

