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

#include "geneva/oa/GSepCmaEvolutionStrategy.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <tuple>
#include <vector>

#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GParetoTools.hpp"

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#endif /* GEM_TESTING */

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GSepCmaEvolutionStrategy) // NOLINT

namespace Gem::Geneva::OptimizationAlgorithms {

using Gem::Geneva::activityMode;

/******************************************************************************/
/**
 * The default constructor. We start with a single individual; the population is grown to lambda
 * offspring in adjustPopulation_() once the dimension n is known.
 */
GSepCmaEvolutionStrategy::GSepCmaEvolutionStrategy() {
    // A sep-CMA-ES is not a parent/child population: there is just a search distribution sampled into
    // lambda offspring. We start small; adjustPopulation_() grows the population to lambda at setup.
    this->setDefaultPopulationSize(1);
}

/******************************************************************************/

void GSepCmaEvolutionStrategy::setLambda(std::size_t lambda) {
    lambda_ = lambda;
}

/******************************************************************************/

std::size_t GSepCmaEvolutionStrategy::getLambda() const {
    return lambda_;
}

/******************************************************************************/

void GSepCmaEvolutionStrategy::setMu(std::size_t mu) {
    mu_ = mu;
}

/******************************************************************************/

std::size_t GSepCmaEvolutionStrategy::getMu() const {
    return mu_;
}

/******************************************************************************/

void GSepCmaEvolutionStrategy::setUseDiagonalCMA(bool use_diagonal_cma) {
    use_diagonal_cma_ = use_diagonal_cma;
}

/******************************************************************************/

bool GSepCmaEvolutionStrategy::getUseDiagonalCMA() const {
    return use_diagonal_cma_;
}

/******************************************************************************/

void GSepCmaEvolutionStrategy::setParetoMode(bool pareto_mode) {
    pareto_mode_ = pareto_mode;
}

/******************************************************************************/

bool GSepCmaEvolutionStrategy::getParetoMode() const {
    return pareto_mode_;
}

/******************************************************************************/

void GSepCmaEvolutionStrategy::setInitialSigma(double initial_sigma) {
    if(initial_sigma <= 0.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSepCmaEvolutionStrategy::setInitialSigma(): Error!" << '\n'
            << "Got invalid initial sigma " << initial_sigma << '\n'
        );
    }
    initial_sigma_ = initial_sigma;
}

/******************************************************************************/

double GSepCmaEvolutionStrategy::getInitialSigma() const {
    return initial_sigma_;
}

/******************************************************************************/

double GSepCmaEvolutionStrategy::getSigma() const {
    return sigma_;
}

/******************************************************************************/
/**
 * Adds local configuration options to a GParserBuilder object.
 */
void GSepCmaEvolutionStrategy::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our parent class'es function
    GOptimizationAlgorithmT<GSepCmaEvolutionStrategy>::addConfigurationOptions_(gpb);

    gpb.registerFileParameter<std::size_t>(
        "lambda",
        DEFAULTSEPCMALAMBDA,
        [this](std::size_t l) { this->setLambda(l); }
    ) << "The number of offspring sampled per generation (lambda)." << '\n'
      << "A value of 0 selects the textbook default 4 + floor(3*ln(n)).";

    gpb.registerFileParameter<std::size_t>(
        "mu",
        static_cast<std::size_t>(0),
        [this](std::size_t m) { this->setMu(m); }
    ) << "The number of selected parents (mu). 0 means lambda/2.";

    gpb.registerFileParameter<bool>(
        "useDiagonalCMA",
        true,
        [this](bool b) { this->setUseDiagonalCMA(b); }
    ) << "Whether to run the separable (diagonal) covariance adaptation." << '\n'
      << "If false, the algorithm is a pure CSA-ES (step-size control only).";

    gpb.registerFileParameter<bool>(
        "paretoMode",
        false,
        [this](bool b) { this->setParetoMode(b); }
    ) << "Whether to use NSGA-II-style multi-objective selection (non-dominated" << '\n'
      << "sort + crowding distance) as the recombination ranking key.";

    gpb.registerFileParameter<double>(
        "initialSigma",
        0.3,
        [this](double s) { this->setInitialSigma(s); }
    ) << "The initial global step size sigma, as a fraction of the parameter range.";
}

/******************************************************************************/
/**
 * Resets the settings of this population to what was configured when optimize() was issued.
 */
void GSepCmaEvolutionStrategy::resetToOptimizationStart_() {
    n_ = 0;
    sigma_ = 0.;
    m_.clear();
    C_.clear();
    p_sigma_.clear();
    p_c_.clear();
    state_initialized_ = false;
    mu_eff_ = 0.;
    c_sigma_ = d_sigma_ = c_c_ = c_1_ = c_mu_ = chi_n_ = 0.;
    weights_.clear();

    GOptimizationAlgorithmT<GSepCmaEvolutionStrategy>::resetToOptimizationStart_();
}

/******************************************************************************/
/**
 * Determines the dimension n_ from the first individual. No parameter bounds are needed: the strategy
 * works in the normalized internal coordinate and an out-of-range sample is folded back into range by the
 * genome on assignment (no clamping in the algorithm).
 */
void GSepCmaEvolutionStrategy::determineDimension() {
    n_ = this->at(0)->countFPParameters(activityMode::ACTIVEONLY);

    if(n_ == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSepCmaEvolutionStrategy::determineDimension(): Error!" << '\n'
            << "The individual exposes no floating point parameters; sep-CMA-ES" << '\n'
            << "requires a continuous (double / float) genome." << '\n'
        );
    }
}

/******************************************************************************/
/**
 * Derives lambda, mu, the recombination weights and all dimension-scaled strategy constants once the
 * dimension n_ is known.
 *
 * Constant choices follow Hansen, "The CMA Evolution Strategy: A Tutorial" (2016) with the separable
 * (diagonal) modification of Ros & Hansen (PPSN 2008), which scales the rank-1/rank-mu learning rates by
 * the sep-CMA factor (n+2)/3 so that the whole update is O(n). These are the canonical defaults; they
 * carry the 1/n and 1/sqrt(n) dimension scalings the stock self-adaptive ES lacks.
 */
void GSepCmaEvolutionStrategy::setUpStrategyParameters() {
    const auto n = static_cast<double>(n_);

    // --- lambda (population size). Textbook default 4 + floor(3 ln n). -----------
    std::size_t lambda = lambda_;
    if(lambda == 0) {
        lambda = static_cast<std::size_t>(4 + std::floor(3. * std::log(n)));
    }
    lambda = std::max<std::size_t>(lambda, 4);
    lambda_ = lambda;

    // --- mu (parents) and the positive log recombination weights ----------------
    std::size_t mu = mu_;
    if(mu == 0) {
        mu = lambda_ / 2;
    }
    mu = std::max<std::size_t>(mu, 1);
    mu = std::min(mu, lambda_);
    mu_ = mu;

    // Preliminary (unnormalized) weights w_i' = ln(mu + 0.5) - ln(i), i = 1..mu .
    weights_.assign(mu_, 0.);
    double w_sum = 0.;
    double w_sq_sum = 0.;
    for(std::size_t i = 0; i < mu_; ++i) {
        double const w = std::log(static_cast<double>(mu_) + 0.5) - std::log(static_cast<double>(i + 1));
        weights_[i] = w;
        w_sum += w;
    }
    // Normalize so that sum(w_i) = 1 .
    for(double &w : weights_) {
        w /= w_sum;
    }
    for(double const w : weights_) {
        w_sq_sum += w * w;
    }
    // Variance-effective selection mass mu_eff = 1 / sum(w_i^2) .
    mu_eff_ = 1. / w_sq_sum;

    // --- step-size control (CSA) ------------------------------------------------
    // c_sigma ~ (mu_eff + 2) / (n + mu_eff + 5)  -- O(1/n) cumulation rate.
    c_sigma_ = (mu_eff_ + 2.) / (n + mu_eff_ + 5.);
    // damping d_sigma = 1 + 2*max(0, sqrt((mu_eff-1)/(n+1)) - 1) + c_sigma .
    d_sigma_ =
        1. + (2. * (std::max)(0., std::sqrt((mu_eff_ - 1.) / (n + 1.)) - 1.)) + c_sigma_;

    // --- covariance adaptation (separable / diagonal) ---------------------------
    // c_c ~ 4/n -- O(1/n) cumulation rate for the rank-1 path.
    c_c_ = 4. / (n + 4.);
    // Full-CMA learning rates ...
    double const c_1_full = 2. / ((n + 1.3) * (n + 1.3) + mu_eff_);
    double const c_mu_full =
        (std::min)(1. - c_1_full,
                   2. * (mu_eff_ - 2. + 1. / mu_eff_) / ((n + 2.) * (n + 2.) + mu_eff_));
    // ... scaled up by the sep-CMA factor (n+2)/3 (Ros & Hansen 2008). This makes the diagonal model
    // learn variances ~n times faster than full CMA would, which is exactly why sep-CMA converges fast
    // at very high n.
    const double sep_factor = (n + 2.) / 3.;
    c_1_ = (std::min)(1., c_1_full * sep_factor);
    c_mu_ = (std::min)(1. - c_1_, c_mu_full * sep_factor);

    // E[||N(0,I)||] ~ sqrt(n) (1 - 1/(4n) + 1/(21 n^2)) .
    chi_n_ = std::sqrt(n) * (1. - 1. / (4. * n) + 1. / (21. * n * n));
}

/******************************************************************************/
/**
 * Sizes the population to lambda offspring and seeds the search distribution. Runs at setup, BEFORE
 * setIndividualPersonalities() and init(), so every slot (including the freshly cloned ones) receives a
 * personality.
 */
void GSepCmaEvolutionStrategy::adjustPopulation_() {
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSepCmaEvolutionStrategy::adjustPopulation_(): Error!" << '\n'
            << "No individuals found in the population. You need to add at least" << '\n'
            << "one individual before the call to optimize()." << '\n'
        );
    }

    // Determine the dimension n, then derive lambda/mu/weights/constants.
    determineDimension();
    setUpStrategyParameters();

    // Grow the population to lambda offspring, all clones of the prototype.
    while(this->size() < lambda_) {
        this->push_back(this->at(0)->clone());
    }
    if(this->size() > lambda_) {
        this->resize(lambda_);
    }
    this->setDefaultPopulationSize(lambda_);
}

/******************************************************************************/
/**
 * Does any necessary initialization work before the optimization cycle starts. Seeds the distribution
 * mean from the registered start individual (unless resumed from a checkpoint).
 */
void GSepCmaEvolutionStrategy::init() {
    // To be performed before any other action.
    GOptimizationAlgorithmT<GSepCmaEvolutionStrategy>::init();

    // sep-CMA-ES is a real-valued evolution strategy: it samples a multivariate-normal distribution over
    // the floating-point (double/float) parameters only and never touches int32/bool parameters. Warn
    // (once, at setup) if the start individual carries active non-FP parameters, which would otherwise be
    // silently held at their start values for the whole run. Use a different algorithm (e.g. the EA) for
    // problems with integer or boolean parameters.
    if(not this->empty()) {
        const auto &ind = (*this->at(0));
        const std::size_t n_int = ind.countParameters<std::int32_t>(activityMode::ACTIVEONLY);
        const std::size_t n_bool = ind.countParameters<bool>(activityMode::ACTIVEONLY);
        if(n_int + n_bool > 0) {
            glogger << "In GSepCmaEvolutionStrategy::init(): Warning!" << '\n'
                    << "The individual has " << n_int << " active integer and " << n_bool
                    << " active boolean parameter(s)." << '\n'
                    << "sep-CMA-ES optimizes only floating-point parameters; these will be left at their"
                    << '\n'
                    << "start values. Use an algorithm that adapts all parameter types (e.g. the EA)"
                    << '\n'
                    << "if the integer/boolean parameters should be optimized." << '\n'
                    << GWARNING;
        }
    }

    // The dimension / constants were established in adjustPopulation_(); refresh if the dimension is
    // unset or no longer matches the genome (e.g. a resumed run carrying a stale value).
    if(n_ == 0 || n_ != this->at(0)->countFPParameters(activityMode::ACTIVEONLY)) {
        determineDimension();
        setUpStrategyParameters();
    }

    std::vector<double> mean;
    this->at(0)->streamlineFPInternal(mean, activityMode::ACTIVEONLY);

    if(not state_initialized_) {
        // Fresh start (not resumed from a checkpoint): seed the distribution.
        m_ = mean; // mean from the registered start individual

        // The initial step size is a dimensionless fraction of the parameter range: parameters live in
        // the normalized internal coordinate (interval width 1), so initial_sigma_ is used directly (no
        // per-parameter range scaling -- the physical scale is reapplied by the external transform).
        sigma_ = initial_sigma_;

        C_.assign(n_, 1.); // unit diagonal covariance
        p_sigma_.assign(n_, 0.);
        p_c_.assign(n_, 0.);
        state_initialized_ = true;
    }
    else {
        // Resumed from a checkpoint: keep the loaded distribution state but make sure the bookkeeping
        // vectors are dimensionally consistent.
        if(m_.size() != n_) {
            m_ = mean;
        }
        if(C_.size() != n_) {
            C_.assign(n_, 1.);
        }
        if(p_sigma_.size() != n_) {
            p_sigma_.assign(n_, 0.);
        }
        if(p_c_.size() != n_) {
            p_c_.assign(n_, 0.);
        }
    }
}

/******************************************************************************/
/**
 * Does any necessary finalization work.
 */
void GSepCmaEvolutionStrategy::finalize() {
    GOptimizationAlgorithmT<GSepCmaEvolutionStrategy>::finalize();
}

/******************************************************************************/
/**
 * Samples lambda offspring from the current distribution into the population.
 *   x_k = m + sigma * sqrt(C) o N(0,I)
 */
void GSepCmaEvolutionStrategy::sampleOffspring() {
    std::normal_distribution<double> norm(0., 1.);
    std::vector<double> z(n_, 0.); // current standard-normal draw; mirrored on the odd offspring

    for(std::size_t k = 0; k < this->size(); ++k) {
        // Mirrored sampling (Brockhoff et al. 2010): offspring come in antithetic pairs
        // x = m +/- sigma*sqrt(C)*z, which cancels the first-order sampling noise of the weighted
        // recombination and yields steadier, faster progress per generation at no extra cost.
        if(k % 2 == 0) {
            for(std::size_t i = 0; i < n_; ++i) {
                z[i] = norm(gr_);
            }
        } else {
            for(std::size_t i = 0; i < n_; ++i) {
                z[i] = -z[i];
            }
        }
        std::vector<double> x(n_);
        for(std::size_t i = 0; i < n_; ++i) {
            x[i] = m_[i] + (sigma_ * std::sqrt(C_[i]) * z[i]);
        }

        // Write the sampled values into the individual. A bounded coordinate that overshot its range is
        // folded back into range by the genome on assignment (no clamping needed here); an unbounded one
        // roams freely. Mark it for (re)evaluation.
        this->at(k)->assignFPValueVectorInternal(x, activityMode::ACTIVEONLY);
        this->at(k)->mark_as_due_for_processing();
    }
}

/******************************************************************************/
/**
 * Submits all offspring to the one process consumer and waits for processed items. Mirrors the stock
 * EA's submission path (workOnPopulation reconciles the range in place); unprocessed / errored items
 * are dropped.
 */
void GSepCmaEvolutionStrategy::evaluatePopulation_() {
    auto status = this->workOnPopulation(0, this->size());

    // Drop items a partial or errored return left unusable.
    this->discardUnusableItems_(status, "GSepCmaEvolutionStrategy::evaluatePopulation_()");
}

/******************************************************************************/
/**
 * Returns the indices of the population sorted best-first for selection.
 */
std::vector<std::size_t> GSepCmaEvolutionStrategy::rankPopulation() const {
    if(pareto_mode_) {
        return rankPopulationPareto();
    }

    const std::size_t sz = this->size();
    std::vector<std::size_t> idx(sz);
    std::iota(idx.begin(), idx.end(), 0);

    const bool maximize = (this->at(0)->getMaxMode() == maxMode::MAXIMIZE);
    std::stable_sort(idx.begin(), idx.end(), [this, maximize](std::size_t a, std::size_t b) -> bool {
        double const fa = this->at(a)->transformed_fitness();
        double const fb = this->at(b)->transformed_fitness();
        return maximize ? (fa > fb) : (fa < fb);
    });

    return idx;
}

/******************************************************************************/
/**
 * Computes an NSGA-II ranking order (best-first): individuals are first grouped into non-domination
 * fronts (fast non-dominated sort), then within each front ordered by decreasing crowding distance. The
 * leading mu indices are used as the recombination parents, so the distribution is pulled toward the
 * Pareto front.
 */
std::vector<std::size_t> GSepCmaEvolutionStrategy::rankPopulationPareto() const {
    std::vector<const gen::GGenome *> pop;
    pop.reserve(this->size());
    for(const auto & i : *this) {
        pop.push_back(&(*i));
    }
    return nonDominatedRank(pop);
}

/******************************************************************************/
/**
 * Performs the mean / sigma / C / path updates from the ranked offspring.
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel: the textbook (sep-)CMA-ES update sequence (weighted-mean recombination -> CSA evolution path -> rank-1 path with h_sigma stall guard -> step-size update -> diagonal covariance update), each phase consuming the previous phase's output (m_new, y_w, sigma_old, ...); splitting would scatter one tightly sequential algorithm
void GSepCmaEvolutionStrategy::updateDistribution(const std::vector<std::size_t> &ranked) {
    const std::size_t mu = (std::min)(mu_, ranked.size());
    if(mu == 0) {
        return;
    }

    // --- weighted recombination of the mu best -> new mean ----------------------
    std::vector<double> m_old = m_;
    std::vector<std::vector<double>> selected(mu);
    double w_used = 0.;
    for(std::size_t i = 0; i < mu; ++i) {
        this->at(ranked[i])->streamlineFPInternal(selected[i], activityMode::ACTIVEONLY);
        w_used += weights_[i];
    }
    if(not(w_used > 0.)) {
        return;
    }

    std::vector<double> m_new(n_, 0.);
    for(std::size_t i = 0; i < mu; ++i) {
        double const w = weights_[i] / w_used; // re-normalize in case mu was truncated
        for(std::size_t j = 0; j < n_; ++j) {
            m_new[j] += w * selected[i][j];
        }
    }
    m_ = m_new;

    // y_w = (m_new - m_old) / sigma  -- the weighted mean step in N(0,I) units.
    std::vector<double> y_w(n_, 0.);
    for(std::size_t j = 0; j < n_; ++j) {
        y_w[j] = (m_new[j] - m_old[j]) / sigma_;
    }

    // --- CSA step-size path: p_sigma = (1-c_sigma) p_sigma
    //                                 + sqrt(c_sigma(2-c_sigma) mu_eff) C^{-1/2} y_w
    // For the diagonal model C^{-1/2} y_w  =  y_w / sqrt(C) component-wise.
    const double cs_factor = std::sqrt(c_sigma_ * (2. - c_sigma_) * mu_eff_);
    double ps_norm_sq = 0.;
    for(std::size_t j = 0; j < n_; ++j) {
        p_sigma_[j] = ((1. - c_sigma_) * p_sigma_[j]) + (cs_factor * (y_w[j] / std::sqrt(C_[j])));
        ps_norm_sq += p_sigma_[j] * p_sigma_[j];
    }
    const double ps_norm = std::sqrt(ps_norm_sq);

    // --- rank-1 path p_c, with the h_sigma stalling switch ----------------------
    // h_sigma guards the rank-1 update against an over-long step early on.
    const std::uint32_t gen = this->getIteration() + 1;
    double const hsig_threshold =
        (1.4 + 2. / (static_cast<double>(n_) + 1.)) * chi_n_ *
        std::sqrt(1. - std::pow(1. - c_sigma_, 2. * static_cast<double>(gen)));
    bool const h_sigma = ps_norm < hsig_threshold;

    const double pc_factor = std::sqrt(c_c_ * (2. - c_c_) * mu_eff_);
    for(std::size_t j = 0; j < n_; ++j) {
        p_c_[j] = ((1. - c_c_) * p_c_[j]) + (h_sigma ? pc_factor * y_w[j] : 0.);
    }

    // --- step-size update (CSA): sigma *= exp( c_sigma/d_sigma * (||p_sigma||/chi_n - 1) )
    // Capture the sigma that actually generated this generation's samples BEFORE the
    // CSA update: the rank-mu covariance update below must normalize the sample steps
    // by that sigma_old (as the rank-1 path does via y_w), not by the just-updated value.
    const double sigma_old = sigma_;
    sigma_ *= std::exp((c_sigma_ / d_sigma_) * (ps_norm / chi_n_ - 1.));

    // --- diagonal covariance update (rank-1 + rank-mu) --------------------------
    if(use_diagonal_cma_) {
        // Loss-of-variance compensation when h_sigma == 0.
        double const delta_hsig = (1. - (h_sigma ? 1. : 0.)) * c_c_ * (2. - c_c_);
        for(std::size_t j = 0; j < n_; ++j) {
            double rank_mu = 0.;
            for(std::size_t i = 0; i < mu; ++i) {
                double const w = weights_[i] / w_used;
                double const yj = (selected[i][j] - m_old[j]) / sigma_old;
                rank_mu += w * yj * yj;
            }
            C_[j] = ((1. - c_1_ - c_mu_) * C_[j]) +
                    (c_1_ * (p_c_[j] * p_c_[j] + delta_hsig * C_[j])) +
                    (c_mu_ * rank_mu);
            // Numerical floor so sqrt(C) stays well-defined.
            if(not(C_[j] > 1.e-300)) {
                C_[j] = 1.e-300;
            }
        }
    }

    // Guard sigma against under-/overflow.
    if(not std::isfinite(sigma_) || sigma_ <= 0.) {
        sigma_ = initial_sigma_;
    }
}

/******************************************************************************/
/**
 * The actual business logic performed during each iteration.
 */
//! [algo-ea:sepcma-cycle]
std::tuple<double, double> GSepCmaEvolutionStrategy::cycleLogic_() {
    // 1) Sample lambda offspring from the current distribution.
    sampleOffspring();

    // 2) Evaluate them through the one process consumer.
    evaluatePopulation_();

    if(this->empty()) {
        // All items got dropped (errors / incomplete); report a worst-case value.
        double const wc = std::numeric_limits<double>::max();
        return std::make_tuple(wc, wc);
    }

    // 3) Rank and select; update the search distribution.
    std::vector<std::size_t> ranked = rankPopulation();
    updateDistribution(ranked);

    // 4) Record the rank in each individual's personality traits (informational), and move the best
    //    individual to the front so the base class's best-extraction (slot 0) picks it up.
    for(std::size_t r = 0; r < ranked.size(); ++r) {
        this->at(ranked[r])
            ->getPersonalityTraits<GSepCmaEvolutionStrategy_PersonalityTraits>()
            ->setRank(r);
    }
    if(ranked[0] != 0) {
        std::swap(this->data_cnt_[0], this->data_cnt_[ranked[0]]);
    }

    // 5) Report the best fitness of this iteration.
    return this->at(0)->getFitnessTuple();
}
//! [algo-ea:sepcma-cycle]

/******************************************************************************/
/**
 * Retrieve a GPersonalityTraits object belonging to this algorithm.
 */
std::shared_ptr<GPersonalityTraits> GSepCmaEvolutionStrategy::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/
/**
 * Applies modifications to this object. This is needed for testing purposes.
 */
bool GSepCmaEvolutionStrategy::modify_GUnitTests_() {
#ifdef GEM_TESTING
    bool result = false;

    if(GOptimizationAlgorithmBase::modify_GUnitTests_()) {
        result = true;
    }

    this->setInitialSigma(this->getInitialSigma() + 0.1);
    this->setUseDiagonalCMA(not this->getUseDiagonalCMA());
    result = true;

    return result;
#else  /* GEM_TESTING */
    Gem::Common::condnotset("GSepCmaEvolutionStrategy::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif /* GEM_TESTING */
}

/******************************************************************************/
/**
 * Performs self tests that are expected to succeed. This is needed for testing purposes.
 */
// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity) -- self-test entry point for GSepCmaEvolutionStrategy: a sequence of independent, self-scoped CHECK blocks, one per sep-CMA strategy-parameter scenario; same one-function-per-test-phase convention used identically across every OA self-test in this codebase
void GSepCmaEvolutionStrategy::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING

    { // Test setting and retrieval of basic strategy parameters
        auto const p_test = this->clone<GSepCmaEvolutionStrategy>();

        CHECK_NOTHROW(p_test->setLambda(20));
        CHECK(p_test->getLambda() == 20);

        CHECK_NOTHROW(p_test->setMu(7));
        CHECK(p_test->getMu() == 7);

        CHECK_NOTHROW(p_test->setUseDiagonalCMA(false));
        CHECK(p_test->getUseDiagonalCMA() == false);

        CHECK_NOTHROW(p_test->setParetoMode(true));
        CHECK(p_test->getParetoMode() == true);

        CHECK_NOTHROW(p_test->setInitialSigma(0.5));
        CHECK(std::abs(p_test->getInitialSigma() - 0.5) < 1.e-9);
    }

    { // Setting an invalid initial sigma must throw
        auto const p_test = this->clone<GSepCmaEvolutionStrategy>();
        CHECK_THROWS(p_test->setInitialSigma(-1.));
    }
#else  /* GEM_TESTING */
    Gem::Common::condnotset(
        "GSepCmaEvolutionStrategy::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif /* GEM_TESTING */
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
