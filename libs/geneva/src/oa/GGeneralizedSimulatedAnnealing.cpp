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

#include "geneva/oa/GGeneralizedSimulatedAnnealing.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <ranges>
#include <tuple>
#include <vector>

#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/genome/GGenome.hpp"

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#endif /* GEM_TESTING */

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GGeneralizedSimulatedAnnealing) // NOLINT

namespace Gem::Geneva::OptimizationAlgorithms {

using Gem::Geneva::activityMode;

/******************************************************************************/
/**
 * The default constructor.
 */
GGeneralizedSimulatedAnnealing::GGeneralizedSimulatedAnnealing() {
    this->setDefaultPopulationSize(n_chains_ * GSA_SLOTS_PER_CHAIN);
}

/******************************************************************************/
/**
 * Initialization with the number of chains.
 */
GGeneralizedSimulatedAnnealing::GGeneralizedSimulatedAnnealing(std::size_t n_chains)
  : n_chains_((n_chains >= 1) ? n_chains : DEFAULTGSANCHAINS) {
    this->setDefaultPopulationSize(n_chains_ * GSA_SLOTS_PER_CHAIN);
}

/******************************************************************************/
/** Retrieves the number of independent Markov chains. */
std::size_t GGeneralizedSimulatedAnnealing::getNChains() const {
    return n_chains_;
}

/******************************************************************************/
/** Allows to set the number of independent Markov chains. */
void GGeneralizedSimulatedAnnealing::setNChains(std::size_t n_chains) {
    if(n_chains == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGeneralizedSimulatedAnnealing::setNChains(std::size_t): Error!" << '\n'
            << "Got invalid number of chains (0)." << '\n'
        );
    }
    n_chains_ = n_chains;
    this->setDefaultPopulationSize(n_chains_ * GSA_SLOTS_PER_CHAIN);
}

/******************************************************************************/
/** Sets the visiting (distribution) parameter q_v (in ]1,3[). */
void GGeneralizedSimulatedAnnealing::setQv(double qv) {
    // The Tsallis visiting form is defined for 1 < q_v < 3
    if(qv <= 1. || qv >= 3.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGeneralizedSimulatedAnnealing::setQv(double): Error!" << '\n'
            << "q_v must be in ]1,3[, got " << qv << '\n'
        );
    }
    qv_ = qv;
}

/******************************************************************************/
double GGeneralizedSimulatedAnnealing::getQv() const {
    return qv_;
}

/******************************************************************************/
/** Sets the acceptance parameter q_a (!= 1). */
void GGeneralizedSimulatedAnnealing::setQa(double qa) {
    // The generalized acceptance form requires q_a != 1 (q_a == 1 is the
    // classical exponential limit and is intentionally excluded here).
    if(qa == 1.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGeneralizedSimulatedAnnealing::setQa(double): Error!" << '\n'
            << "q_a must not be exactly 1 (the classical exponential limit)." << '\n'
        );
    }
    qa_ = qa;
}

/******************************************************************************/
double GGeneralizedSimulatedAnnealing::getQa() const {
    return qa_;
}

/******************************************************************************/
/** Sets the initial visiting temperature (0 => derive from ranges). */
void GGeneralizedSimulatedAnnealing::setT0(double t0) {
    if(t0 < 0.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGeneralizedSimulatedAnnealing::setT0(double): Error!" << '\n'
            << "t0 must be >= 0 (0 means derive from parameter ranges), got " << t0 << '\n'
        );
    }
    t0_ = t0;
}

/******************************************************************************/
double GGeneralizedSimulatedAnnealing::getT0() const {
    return t0_;
}

/******************************************************************************/
/** Sets the per-chain reannealing stall threshold (0 = disabled). */
void GGeneralizedSimulatedAnnealing::setReannealingSteps(std::uint32_t reannealing_steps) {
    reannealing_steps_ = reannealing_steps;
}

/******************************************************************************/
std::uint32_t GGeneralizedSimulatedAnnealing::getReannealingSteps() const {
    return reannealing_steps_;
}

/******************************************************************************/
/**
 * Sets the cooling timescale. The schedule's step counter is divided by this value, so a larger
 * timescale stretches the cooling over proportionally more steps. The strict Tsallis schedule (the
 * faithful default) corresponds to a timescale of 1. Values below 1 are clamped to 1.
 */
void GGeneralizedSimulatedAnnealing::setCoolingTimescale(double cooling_timescale) {
    cooling_timescale_ = (cooling_timescale >= 1.) ? cooling_timescale : 1.;
}

/******************************************************************************/
double GGeneralizedSimulatedAnnealing::getCoolingTimescale() const {
    return cooling_timescale_;
}

/******************************************************************************/
/** Population index of the current point of chain c. */
std::size_t GGeneralizedSimulatedAnnealing::currentPos(std::size_t c) {
    return (c * GSA_SLOTS_PER_CHAIN) + GSA_CURRENT;
}

/******************************************************************************/
/** Population index of the proposal point of chain c. */
std::size_t GGeneralizedSimulatedAnnealing::proposalPos(std::size_t c) {
    return (c * GSA_SLOTS_PER_CHAIN) + GSA_PROPOSAL;
}

/******************************************************************************/
/**
 * Adds local configuration options to a GParserBuilder object.
 */
void GGeneralizedSimulatedAnnealing::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our parent class'es function
    GOptimizationAlgorithmT<GGeneralizedSimulatedAnnealing>::addConfigurationOptions_(gpb);

    gpb.registerFileParameter<std::size_t>(
        "n_chains",
        DEFAULTGSANCHAINS,
        [this](std::size_t nc) { this->setNChains(nc); }
    ) << "The number of independent Markov chains";

    gpb.registerFileParameter<double>(
        "qv",
        DEFAULTGSAQV,
        [this](double q) { this->setQv(q); }
    ) << "The Tsallis visiting (distribution) parameter q_v (in ]1,3[)";

    gpb.registerFileParameter<double>(
        "qa",
        DEFAULTGSAQA,
        [this](double q) { this->setQa(q); }
    ) << "The generalized acceptance parameter q_a (!= 1)";

    gpb.registerFileParameter<double>(
        "t0",
        DEFAULTGSAT0,
        [this](double t) { this->setT0(t); }
    ) << "The initial visiting temperature T_qv(1)"
      << '\n'
      << "(0 => derive from the parameter ranges)";

    gpb.registerFileParameter<std::uint32_t>(
        "reannealing_steps",
        DEFAULTGSAREANNEAL,
        [this](std::uint32_t r) { this->setReannealingSteps(r); }
    ) << "Per-chain stall steps before a cooling-clock restart (0 = disabled)";

    gpb.registerFileParameter<double>(
        "cooling_timescale",
        DEFAULTGSACOOLING,
        [this](double c) { this->setCoolingTimescale(c); }
    ) << "Cooling timescale stretching the schedule (1 = strict Tsallis; larger = slower cooling, better for long high-dimensional runs)";
}

/******************************************************************************/
/**
 * Resets the settings of this population to what was configured when optimize() was issued.
 */
void GGeneralizedSimulatedAnnealing::resetToOptimizationStart_() {
    n_fp_parms_ = 0;
    t0_effective_ = 0.;
    chain_step_.clear();
    chain_best_energy_.clear();
    chain_stall_.clear();

    GOptimizationAlgorithmT<GGeneralizedSimulatedAnnealing>::resetToOptimizationStart_();
}

/******************************************************************************/
/**
 * Sizes the population to 2 * n_chains_ (each chain occupies two consecutive slots:
 * [current | proposal]) and does error checks. Runs at setup, BEFORE setIndividualPersonalities() and
 * init(), so every slot (including the freshly cloned ones) receives a personality.
 *
 * Chain 0's current slot is seeded from the registered start individual; the remaining chains' current
 * slots are randomized within the parameter ranges (in init()). Each proposal slot starts as a clone of
 * its chain's current slot.
 */
void GGeneralizedSimulatedAnnealing::adjustPopulation_() {
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGeneralizedSimulatedAnnealing::adjustPopulation_(): Error!" << '\n'
            << "No individuals found in the population. You need to add at least" << '\n'
            << "one individual before the call to optimize()." << '\n'
        );
    }

    const std::size_t total_size = n_chains_ * GSA_SLOTS_PER_CHAIN;

    while(this->size() < total_size) {
        this->push_back(this->at(0)->clone());
    }
    if(this->size() > total_size) {
        this->resize(total_size);
    }
    this->setDefaultPopulationSize(total_size);
}

/******************************************************************************/
/**
 * Does some preparatory work before the optimization starts: determines the floating-point parameter
 * count, derives the effective initial visiting temperature, seeds the chains (chain 0's current slot keeps the
 * registered start individual; the other chains' current slots are randomized uniformly inside the box;
 * each proposal slot is initialized from its current slot) and initializes the per-chain cooling clocks.
 */
void GGeneralizedSimulatedAnnealing::init() {
    GOptimizationAlgorithmT<GGeneralizedSimulatedAnnealing>::init();

    // The chains move in the normalized internal coordinate, so no per-parameter bounds are needed; only
    // the count of active floating point parameters matters (the proposal jump is dimensionless).
    n_fp_parms_ = this->at(0)->countFPParameters(activityMode::ACTIVEONLY);

    if(n_fp_parms_ == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGeneralizedSimulatedAnnealing::init(): Error!" << '\n'
            << "The individual exposes no floating point parameters; Generalized Simulated" << '\n'
            << "Annealing requires a continuous (double / float) genome." << '\n'
        );
    }

    // Derive the effective initial visiting temperature. If the user supplied a positive t0_, use it
    // directly; otherwise the initial visiting *jump* scale is one normalized parameter range.
    //
    // The jump scale tracks the visiting temperature linearly: tau(t) = L * T_qv(t) / T_qv(1) (see
    // drawVisitingJump()), so the initial scale is tau(1) = L and then shrinks *polynomially* with the
    // power-law cooling toward local moves. Parameters live in the normalized internal coordinate (interval
    // width 1), so L is simply 1 (the full box width, a dimensionless fraction of the range) -- no
    // per-parameter span averaging. Tying the scale to T_qv(t) (rather than to the strictly faithful
    // Tsallis exponent T^{1/(3-qv)}, which collapses far too fast) is what keeps the step schedule
    // practical over a finite iteration budget -- broad early exploration, fine late refinement.
    t0_effective_ = (t0_ > 0.) ? t0_ : 1.;

    // Seed the chains. Chain 0's current slot keeps the registered start individual unchanged; the other
    // chains' current slots are randomized uniformly inside the box. Each proposal slot is initialized
    // from its chain's current slot.
    for(std::size_t c = 1; c < n_chains_; ++c) {
        this->at(currentPos(c))->randomInit(activityMode::ACTIVEONLY);
    }
    for(std::size_t c = 0; c < n_chains_; ++c) {
        this->at(proposalPos(c))->load((*this->at(currentPos(c))));
    }

    // Initialize the per-chain cooling clocks and stall trackers.
    chain_step_.assign(n_chains_, 1u);
    chain_best_energy_.assign(n_chains_, std::numeric_limits<double>::max());
    chain_stall_.assign(n_chains_, 0u);

    markIndividualPositions();
}

/******************************************************************************/
/**
 * Does any necessary finalization work.
 */
void GGeneralizedSimulatedAnnealing::finalize() {
    GOptimizationAlgorithmT<GGeneralizedSimulatedAnnealing>::finalize();
}

/******************************************************************************/
/**
 * Retrieve a GPersonalityTraits object belonging to this algorithm.
 */
std::shared_ptr<GPersonalityTraits> GGeneralizedSimulatedAnnealing::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/
/**
 * The Tsallis power-law visiting temperature at step t (t = 1, 2, ...):
 *
 *   T_qv(t) = T_qv(1) * (2^{qv-1} - 1) / ((1 + t_eff)^{qv-1} - 1),
 *
 * where t_eff = 1 + (t - 1) / cooling_timescale_ stretches the schedule. With the faithful default
 * timescale of 1, t_eff == t and the strict Tsallis schedule is recovered.
 */
double GGeneralizedSimulatedAnnealing::effectiveTime(std::uint32_t t) const {
    return 1. + ((static_cast<double>(t) - 1.) / cooling_timescale_);
}

/******************************************************************************/
double GGeneralizedSimulatedAnnealing::visitingTemperature(std::uint32_t t) const {
    const double exponent = qv_ - 1.;
    const double numerator = std::pow(2., exponent) - 1.;
    const double denominator = std::pow(1. + this->effectiveTime(t), exponent) - 1.;

    if(denominator <= 0.) {
        // Only happens at t == 1 (denominator == numerator) which is fine; this guards against any
        // pathological rounding. Fall back to the initial value.
        return t0_effective_;
    }
    return t0_effective_ * (numerator / denominator);
}

/******************************************************************************/
/**
 * Draws a single Tsallis-style heavy-tailed visiting jump of dimension n_fp_parms_ for a given visiting
 * temperature tqv. The construction is documented in the class header. The jump scale tracks the
 * temperature *linearly* (polynomially, since T_qv itself decays polynomially) so it does not collapse
 * within a finite iteration budget:
 *
 *   tau   = L * tqv / T_qv(1)   (== tqv / t0_effective_ scaled by L; here L == t0_effective_, so tau = tqv)
 *   dx_k  = tau * g_k / |z|^{(qv-1)/2},  g_k ~ N(0,1),  z ~ N(0,1), |z| >= eps
 *
 * Because the derived initial visiting temperature is set to L (the mean parameter span), tau == tqv,
 * i.e. tau(1) == L and tau(t) decays exactly with the visiting temperature. The unit-Gaussian vector g
 * supplies a well-behaved core; the shared random scalar divisor |z|^{-(qv-1)/2} injects the heavy tail
 * (heavier as qv -> 3, vanishing as qv -> 1). The whole jump scales with tau, so it shrinks as the
 * temperature cools. Reuses the algorithm's inherited RNG (gr_) and std::normal_distribution, exactly as
 * the sibling strategies draw their randomness.
 */
std::vector<double> GGeneralizedSimulatedAnnealing::drawVisitingJump(double tqv) {
    std::normal_distribution<double> gauss(0., 1.);

    // The scale tracks the visiting temperature linearly. With t0_effective_ == L (the mean span), this
    // is simply tau = tqv: tau(1) == L and tau decays polynomially with the power-law cooling.
    const double tau = std::max(tqv, 0.);

    // A single heavy-tail scalar shared across all coordinates of this jump.
    constexpr double eps = 1.e-12;
    double z = std::fabs(gauss(gr_));
    z = std::max(z, eps);
    const double tail = std::pow(z, (qv_ - 1.) / 2.); // |z|^{(qv-1)/2}

    std::vector<double> dx(n_fp_parms_);
    for(std::size_t k = 0; k < n_fp_parms_; ++k) {
        const double g = gauss(gr_);
        dx[k] = tau * g / tail;
    }
    return dx;
}

/******************************************************************************/
/**
 * The generalized (Tsallis) acceptance probability for an uphill move with energy difference delta_e
 * (> 0) and acceptance temperature tqa:
 *
 *   P_qa = [ 1 - (1 - qa) * delta_e / tqa ]^{1/(1-qa)}  if the bracket > 0,
 *        = 0                                            otherwise,
 *
 * clamped to [0,1].
 */
//! [gsa-acceptance]
double GGeneralizedSimulatedAnnealing::acceptanceProbability(double delta_e, double tqa) const {
    if(tqa <= 0.) {
        return 0.; // zero temperature => never accept an uphill move
    }

    const double one_minus_qa = 1. - qa_;
    const double bracket = 1. - (one_minus_qa * (delta_e / tqa));

    if(bracket <= 0.) {
        return 0.;
    }

    double p = std::pow(bracket, 1. / one_minus_qa);

    // Clamp to [0,1] to guard against rounding.
    if(p < 0.) {
        p = 0.;
    }
    else if(p > 1.) {
        p = 1.;
    }
    return p;
}
//! [gsa-acceptance]

/******************************************************************************/
/**
 * Proposes a new point for every chain by drawing a heavy-tailed Tsallis jump (scaled by the chain's
 * current visiting temperature) and adding it to the chain's current point. The jump is taken in the
 * normalized internal coordinate; the result is written into the proposal slot via the internal FP
 * channel, which folds a bounded parameter's overshooting jump back into the canonical [-0.5, 0.5).
 */
void GGeneralizedSimulatedAnnealing::proposeMoves() {
    for(std::size_t c = 0; c < n_chains_; ++c) {
        const double tqv = this->visitingTemperature(chain_step_[c]);

        std::vector<double> x;
        this->at(currentPos(c))->streamlineFPInternal(x, activityMode::ACTIVEONLY);

        const std::vector<double> dx = this->drawVisitingJump(tqv);

        std::vector<double> x_new(n_fp_parms_);
        for(std::size_t k = 0; k < n_fp_parms_; ++k) {
            x_new[k] = x[k] + dx[k];
        }

        this->at(proposalPos(c))->assignFPValueVectorInternal(x_new, activityMode::ACTIVEONLY);
        this->at(proposalPos(c))->mark_as_due_for_processing();
    }
}

/******************************************************************************/
/**
 * Applies the generalized acceptance rule to the proposals evaluated in the previous iteration. For each
 * chain: a downhill proposal is always accepted; an uphill proposal is accepted with probability
 * P_qa(delta_e, T_qa(t)). On acceptance the proposal is loaded into the current slot (carrying its
 * already known fitness with it). The chain's cooling clock then advances; a chain that has not improved
 * its own best energy for reannealing_steps_ steps has its clock reset toward T_qv(1).
 */
void GGeneralizedSimulatedAnnealing::applyAcceptance() {
    for(std::size_t c = 0; c < n_chains_; ++c) {
        const double e_cur = minOnly_transformed_fitness((*this->at(currentPos(c))));
        const double e_new = minOnly_transformed_fitness((*this->at(proposalPos(c))));
        const double delta_e = e_new - e_cur;

        bool accepted = false;
        if(delta_e <= 0.) {
            accepted = true;
        }
        else {
            const std::uint32_t t = chain_step_[c];
            const double tqv = this->visitingTemperature(t);
            const double tqa = tqv / this->effectiveTime(t); // coupled acceptance temperature (stretched time)
            const double p_pass = this->acceptanceProbability(delta_e, tqa);

            const double challenge = uniform_real_distribution_(
                gr_,
                std::uniform_real_distribution<double>::param_type(0., 1.)
            );
            if(challenge < p_pass) {
                accepted = true;
            }
        }

        if(accepted) {
            // Loading also transfers the proposal's already known fitness.
            this->at(currentPos(c))->load((*this->at(proposalPos(c))));
            this->at(currentPos(c))
                ->getPersonalityTraits<GGeneralizedSimulatedAnnealing_PersonalityTraits>()
                ->setPopulationPosition(currentPos(c));
        }

        // Track per-chain improvement for reannealing.
        const double e_after = minOnly_transformed_fitness((*this->at(currentPos(c))));
        if(e_after < chain_best_energy_[c]) {
            chain_best_energy_[c] = e_after;
            chain_stall_[c] = 0;
        }
        else {
            ++chain_stall_[c];
        }

        // Advance the cooling clock for this chain.
        ++chain_step_[c];

        // Reannealing: restart the cooling clock if the chain has stalled.
        if(reannealing_steps_ > 0 && chain_stall_[c] >= reannealing_steps_) {
            chain_step_[c] = 1;
            chain_stall_[c] = 0;
        }
    }
}

/******************************************************************************/
/**
 * Submits the whole population to the one process consumer and waits for processed items. Mirrors the
 * stock EA's submission path (workOnPopulation reconciles the contiguous range in place).
 */
void GGeneralizedSimulatedAnnealing::evaluatePopulation_() {
    auto status = this->workOnPopulation(0, this->size());

    this->requireCompleteEvaluation_(status, "GGeneralizedSimulatedAnnealing::evaluatePopulation_()");
}

/******************************************************************************/
/**
 * The actual business logic to be performed during each iteration.
 *
 * Ordering:
 *   1. After the first iteration, apply the generalized acceptance rule to the proposals evaluated in
 *      the previous iteration (cooling + reannealing).
 *   2. Propose a new point for every chain (into its proposal slot).
 *   3. (Re-)evaluate the whole population.
 *
 * The global best is retained across iterations by the base class's global priority queue, which is fed
 * the whole population every iteration; cycleLogic_ therefore reports the best (raw, transformed)
 * fitness among all the individuals evaluated this iteration.
 *
 * @return The best (current or proposal) energy found in this iteration
 */
//! [gsa-cycle-logic]
std::tuple<double, double> GGeneralizedSimulatedAnnealing::cycleLogic_() {
    if(this->afterFirstIteration()) {
        this->applyAcceptance();
    }

    this->proposeMoves();

    evaluatePopulation_();

    // Report the best (raw, transformed) fitness among all evaluated individuals this iteration, using
    // the standard EA/ES ranking helpers (isBetter / getMaxMode).
    const auto m = this->at(0)->getMaxMode();

    std::tuple<double, double> best_fitness = std::make_tuple(
        this->at(0)->getWorstCase(),
        this->at(0)->getWorstCase()
    );

    for(const auto & pos : *this) {
        auto &ind = (*pos);
        if(ind.is_due_for_processing() || ind.has_errors()) {
            continue;
        }
        if(isBetter(
               ind.transformed_fitness(0),
               std::get<G_TRANSFORMED_FITNESS>(best_fitness),
               m
           )) {
            best_fitness = ind.getFitnessTuple();
        }
    }

    return best_fitness;
}
//! [gsa-cycle-logic]

/******************************************************************************/
/**
 * Applies modifications to this object. This is needed for testing purposes.
 */
bool GGeneralizedSimulatedAnnealing::modify_GUnitTests_() {
#ifdef GEM_TESTING
    bool result = false;

    if(GOptimizationAlgorithmBase::modify_GUnitTests_()) {
        result = true;
    }

    this->setQv(this->getQv() + 0.01);
    result = true;

    return result;
#else  /* GEM_TESTING */
    Gem::Common::condnotset("GGeneralizedSimulatedAnnealing::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif /* GEM_TESTING */
}

/******************************************************************************/
/**
 * Performs self tests that are expected to succeed. This is needed for testing purposes.
 */
// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity) -- self-test entry point for GGeneralizedSimulatedAnnealing: a sequence of independent, self-scoped CHECK blocks, one per SA strategy-parameter/annealing scenario; same one-function-per-test-phase convention used identically across every OA self-test in this codebase
void GGeneralizedSimulatedAnnealing::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING

    { // Test setting and retrieval of basic strategy parameters
        auto const p_test = this->clone<GGeneralizedSimulatedAnnealing>();

        CHECK_NOTHROW(p_test->setNChains(4));
        CHECK(p_test->getNChains() == 4);

        CHECK_NOTHROW(p_test->setQv(2.0));
        CHECK(p_test->getQv() == 2.0);

        CHECK_NOTHROW(p_test->setQa(-3.0));
        CHECK(p_test->getQa() == -3.0);

        CHECK_NOTHROW(p_test->setT0(5.0));
        CHECK(p_test->getT0() == 5.0);

        CHECK_NOTHROW(p_test->setReannealingSteps(50));
        CHECK(p_test->getReannealingSteps() == 50);
    }

    { // Setting invalid strategy parameters must throw
        auto const p_test = this->clone<GGeneralizedSimulatedAnnealing>();
        CHECK_THROWS(p_test->setNChains(0)); // need at least 1 chain
        CHECK_THROWS(p_test->setQv(1.0));    // q_v must be in ]1,3[
        CHECK_THROWS(p_test->setQv(3.0));    // q_v must be in ]1,3[
        CHECK_THROWS(p_test->setQa(1.0));    // q_a must not be exactly 1
        CHECK_THROWS(p_test->setT0(-1.0));   // t0 must be >= 0
    }
#else  /* GEM_TESTING */
    Gem::Common::condnotset(
        "GGeneralizedSimulatedAnnealing::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif /* GEM_TESTING */
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
