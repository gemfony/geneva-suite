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

#include "geneva/oa/GStandardPSO2011.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
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

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#endif /* GEM_TESTING */

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GStandardPSO2011) // NOLINT

namespace Gem::Geneva::OptimizationAlgorithms {

using Gem::Geneva::activityMode;

/******************************************************************************/
/** The SPSO-2011 inertia / acceleration constants */
namespace {
const double SPSO_W = 1. / (2. * std::log(2.)); ///< inertia weight w = 1/(2 ln 2)
const double SPSO_C = 0.5 + std::log(2.);       ///< acceleration c = 1/2 + ln 2
constexpr double SPSO_VMAX_FACTOR = 0.2; ///< velocity clamp as a fraction of the normalized unit interval
} // namespace

/******************************************************************************/
/**
 * The default constructor.
 */
GStandardPSO2011::GStandardPSO2011() {
    this->setDefaultPopulationSize(swarm_size_);
}

/******************************************************************************/
/**
 * Initialization with the swarm size.
 */
GStandardPSO2011::GStandardPSO2011(std::size_t swarm_size)
  : swarm_size_((swarm_size >= 2) ? swarm_size : DEFAULTSPSOSWARMSIZE) {
    this->setDefaultPopulationSize(swarm_size_);
}

/******************************************************************************/

std::size_t GStandardPSO2011::getSwarmSize() const {
    return swarm_size_;
}

/******************************************************************************/

void GStandardPSO2011::setSwarmSize(std::size_t swarm_size) {
    if(swarm_size < 2) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GStandardPSO2011::setSwarmSize(std::size_t): Error!" << '\n'
            << "Got invalid swarm size " << swarm_size << " (need at least 2)." << '\n'
        );
    }
    swarm_size_ = swarm_size;
    this->setDefaultPopulationSize(swarm_size_);
}

/******************************************************************************/

std::size_t GStandardPSO2011::getNInformants() const {
    return n_informants_;
}

/******************************************************************************/

void GStandardPSO2011::setNInformants(std::size_t n_informants) {
    if(n_informants == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GStandardPSO2011::setNInformants(std::size_t): Error!" << '\n'
            << "Got invalid number of informants (0)." << '\n'
        );
    }
    n_informants_ = n_informants;
}

/******************************************************************************/
/**
 * Adds local configuration options to a GParserBuilder object.
 */
void GStandardPSO2011::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our parent class'es function
    GOptimizationAlgorithmT<GStandardPSO2011>::addConfigurationOptions_(gpb);

    gpb.registerFileParameter<std::size_t>(
        "swarm_size",
        DEFAULTSPSOSWARMSIZE,
        [this](std::size_t s) { this->setSwarmSize(s); }
    ) << "The number of particles in the swarm (at least 2).";

    gpb.registerFileParameter<std::size_t>(
        "n_informants",
        DEFAULTSPSOK,
        [this](std::size_t k) { this->setNInformants(k); }
    ) << "The number of particles each particle informs (K, default 3).";
}

/******************************************************************************/
/**
 * Resets the settings of this population to what was configured when optimize() was issued.
 */
void GStandardPSO2011::resetToOptimizationStart_() {
    n_fp_parms_ = 0;
    velocities_.clear();
    personal_bests_.clear();
    personal_best_fitness_.clear();
    global_best_.clear();
    global_best_fitness_ = 0.;
    informs_.clear();
    global_best_improved_ = true;

    GOptimizationAlgorithmT<GStandardPSO2011>::resetToOptimizationStart_();
}

/******************************************************************************/
/**
 * Sizes the population to the swarm size and does error checks. Runs at setup, BEFORE
 * setIndividualPersonalities() and init(), so every slot (including the freshly cloned ones) receives a
 * personality. Missing particles are filled with clones of the seed individual; surplus particles are
 * removed.
 */
void GStandardPSO2011::adjustPopulation_() {
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GStandardPSO2011::adjustPopulation_(): Error!" << '\n'
            << "No individuals found in the population. You need to add at least" << '\n'
            << "one individual before the call to optimize()." << '\n'
        );
    }

    while(this->size() < swarm_size_) {
        this->push_back(this->at(0)->clone());
    }
    if(this->size() > swarm_size_) {
        this->resize(swarm_size_);
    }
    this->setDefaultPopulationSize(swarm_size_);
}

/******************************************************************************/
/**
 * Does some preparatory work before the optimization starts. It determines the floating-point parameter
 * count, randomly initializes all particles (keeping particle 0 as the registered start individual),
 * seeds the velocities via the SPSO-2011 half-diff rule, and sets each personal best to the start position.
 */
void GStandardPSO2011::init() {
    GOptimizationAlgorithmT<GStandardPSO2011>::init();

    // The swarm works in the normalized internal coordinate (boundary-agnostic): a bounded parameter
    // occupies the unit interval [-0.5, 0.5) and an unbounded one its perimeter image, so no per-parameter
    // bounds are needed -- only the count of active floating point parameters.
    n_fp_parms_ = this->at(0)->countFPParameters(activityMode::ACTIVEONLY);

    if(n_fp_parms_ == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GStandardPSO2011::init(): Error!" << '\n'
            << "The individual exposes no floating point parameters; Standard PSO 2011" << '\n'
            << "requires a continuous (double / float) genome." << '\n'
        );
    }

    velocities_.assign(swarm_size_, std::vector<double>(n_fp_parms_, 0.));
    personal_bests_.assign(swarm_size_, std::vector<double>(n_fp_parms_, 0.));
    personal_best_fitness_.assign(swarm_size_, std::numeric_limits<double>::max());
    global_best_.assign(n_fp_parms_, 0.);
    global_best_fitness_ = std::numeric_limits<double>::max();
    informs_.assign(swarm_size_, std::vector<bool>(swarm_size_, false));
    global_best_improved_ = true;

    for(std::size_t i = 0; i < swarm_size_; ++i) {
        // Particle index bookkeeping.
        this->at(i)
            ->getPersonalityTraits<GStandardPSO2011_PersonalityTraits>()
            ->setParticle(i);

        std::vector<double> pos;
        this->at(i)->streamlineFPInternal(pos, activityMode::ACTIVEONLY);

        // Particle 0 keeps the registered start individual; all others are randomized uniformly within the
        // normalized unit interval [-0.5, 0.5) (an out-of-range value is folded back by the genome).
        if(i != 0) {
            for(std::size_t d = 0; d < n_fp_parms_; ++d) {
                pos[d] = uniform_real_distribution_(
                    gr_,
                    std::uniform_real_distribution<double>::param_type(-0.5, 0.5)
                );
            }
            this->at(i)->assignFPValueVectorInternal(pos, activityMode::ACTIVEONLY);
        }

        // Half-diff velocity initialization: v_d = (U(-0.5, 0.5) - x_d) / 2 (SPSO-2011, normalized box).
        for(std::size_t d = 0; d < n_fp_parms_; ++d) {
            const double sample = uniform_real_distribution_(
                gr_,
                std::uniform_real_distribution<double>::param_type(-0.5, 0.5)
            );
            velocities_[i][d] = 0.5 * (sample - pos[d]);
        }

        // Personal best initialized to the start position.
        personal_bests_[i] = pos;
    }
}

/******************************************************************************/
/**
 * Does any necessary finalization work.
 */
void GStandardPSO2011::finalize() {
    GOptimizationAlgorithmT<GStandardPSO2011>::finalize();
}

/******************************************************************************/
/**
 * Retrieve a GPersonalityTraits object belonging to this algorithm.
 */
std::shared_ptr<GPersonalityTraits> GStandardPSO2011::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/
/**
 * Submits all particles to the one process consumer and waits for processed items. Mirrors the stock
 * EA's submission path (workOnPopulation reconciles the range in place).
 */
void GStandardPSO2011::evaluatePopulation_() {
    auto status = this->workOnPopulation(0, this->size());

    this->requireCompleteEvaluation_(status, "GStandardPSO2011::evaluatePopulation_()");
}

/******************************************************************************/
/**
 * (Re-)builds the random informant topology. Each particle informs K others chosen uniformly at random
 * with replacement, plus itself.
 */
void GStandardPSO2011::buildTopology() {
    for(auto &row : informs_) {
        std::fill(row.begin(), row.end(), false);
    }

    for(std::size_t i = 0; i < swarm_size_; ++i) {
        informs_[i][i] = true; // a particle always informs itself
        for(std::size_t k = 0; k < n_informants_; ++k) {
            // Draw a target uniformly at random in [0, swarm_size_) (with replacement).
            const double u = uniform_real_distribution_(
                gr_,
                std::uniform_real_distribution<double>::param_type(
                    0., static_cast<double>(swarm_size_)
                )
            );
            auto target = static_cast<std::size_t>(u);
            if(target >= swarm_size_) {
                target = swarm_size_ - 1; // guard against u == swarm_size_ (degenerate rounding)
            }
            informs_[i][target] = true;
        }
    }
}

/******************************************************************************/
/**
 * Determines the local best position of a particle: the best personal best among all particles that
 * inform it (including itself).
 */
std::vector<double> GStandardPSO2011::localBest(std::size_t particle) const {
    std::size_t best = particle;
    double best_fit = std::numeric_limits<double>::max();
    bool found = false;

    for(std::size_t j = 0; j < swarm_size_; ++j) {
        if(informs_[j][particle]) { // particle j informs 'particle'
            if(not found || personal_best_fitness_[j] < best_fit) {
                best = j;
                best_fit = personal_best_fitness_[j];
                found = true;
            }
        }
    }

    return personal_bests_[best];
}

/******************************************************************************/
/******************************************************************************/
/**
 * Updates the velocity and position of every particle using the SPSO-2011 hypersphere rule. Positions are
 * contained by the genome's write-fold on assignment (no explicit boundary clamping).
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel: the SPSO-2011 hypersphere update per particle (center-of-gravity G, sampling radius, uniform direction draw, in-ball sample x', velocity clamp/position update), each step consuming the previous step's output; splitting would scatter one textbook algorithm
void GStandardPSO2011::updatePositions() {
    std::normal_distribution<double> gauss(0., 1.);

    for(std::size_t i = 0; i < swarm_size_; ++i) {
        std::vector<double> x;
        this->at(i)->streamlineFPInternal(x, activityMode::ACTIVEONLY);

        const std::vector<double> &p = personal_bests_[i];
        const std::vector<double> l = localBest(i);

        // The particle is its own best informant iff its local best position equals its personal best.
        const bool p_is_local = (l == p);

        // Build the center of gravity G.
        std::vector<double> G(n_fp_parms_, 0.);
        if(p_is_local) {
            // 2-point center: G = (x + P) / 2, with P = x + c (p - x).
            for(std::size_t d = 0; d < n_fp_parms_; ++d) {
                const double P = x[d] + (SPSO_C * (p[d] - x[d]));
                G[d] = 0.5 * (x[d] + P);
            }
        }
        else {
            // 3-point center: G = (x + P + L) / 3.
            for(std::size_t d = 0; d < n_fp_parms_; ++d) {
                const double P = x[d] + (SPSO_C * (p[d] - x[d]));
                const double L = x[d] + (SPSO_C * (l[d] - x[d]));
                G[d] = (x[d] + P + L) / 3.;
            }
        }

        // Radius of the sampling hypersphere: r = || G - x || .
        double r2 = 0.;
        for(std::size_t d = 0; d < n_fp_parms_; ++d) {
            const double diff = G[d] - x[d];
            r2 += diff * diff;
        }
        const double r = std::sqrt(r2);

        // Draw a uniform direction on the unit sphere from a normal vector.
        std::vector<double> dir(n_fp_parms_, 0.);
        double norm2 = 0.;
        for(std::size_t d = 0; d < n_fp_parms_; ++d) {
            dir[d] = gauss(gr_);
            norm2 += dir[d] * dir[d];
        }
        const double norm = std::sqrt(norm2);

        // Uniform radius factor inside the ball: U(0,1)^(1/n) .
        const double u = uniform_real_distribution_(
            gr_,
            std::uniform_real_distribution<double>::param_type(0., 1.)
        );
        const double radius_factor = std::pow(u, 1. / static_cast<double>(n_fp_parms_));

        // Sample x' inside the ball around G of radius r.
        std::vector<double> x_prime(n_fp_parms_, 0.);
        if(norm > 0. && r > 0.) {
            for(std::size_t d = 0; d < n_fp_parms_; ++d) {
                x_prime[d] = G[d] + (r * radius_factor * (dir[d] / norm));
            }
        }
        else {
            // Degenerate sphere (r == 0): the trial point collapses to G.
            x_prime = G;
        }

        // Velocity and position update, with per-dimension velocity clamping (Vmax). Strict SPSO-2011
        // omits Vmax and relies on the constriction (w<1) to contract, but in HIGH dimension the
        // hypersphere radius factor U^(1/n) -> 1, so the trial point sits at distance ~||G-x|| from G
        // every step: the velocity never decays, the swarm cannot contract, and coordinates get pinned to
        // the box walls (it stagnates). Clamping the velocity to a fraction of the parameter range lets
        // the swarm settle and refine.
        std::vector<double> &v = velocities_[i];
        for(std::size_t d = 0; d < n_fp_parms_; ++d) {
            v[d] = (SPSO_W * v[d]) + (x_prime[d] - x[d]);
            // Velocity stabilization (not boundary clamping): cap each component to SPSO_VMAX_FACTOR, a
            // dimensionless fraction of the normalized unit interval (uniform across parameters).
            v[d] = std::clamp(v[d], -SPSO_VMAX_FACTOR, SPSO_VMAX_FACTOR);
            x[d] = x[d] + v[d];
        }

        // A coordinate that left its range is folded back by the genome on assignment (continuous
        // reflection); no explicit boundary confinement here.
        this->at(i)->assignFPValueVectorInternal(x, activityMode::ACTIVEONLY);
        this->at(i)->mark_as_due_for_processing();
    }
}

/******************************************************************************/
/**
 * Updates the personal bests of all particles and the global best, using the just-computed fitnesses.
 * Returns the best (raw, transformed) fitness tuple of this iteration and sets the global_best_improved_
 * flag for the next iteration's topology decision. The best individual is moved to the front so the base
 * class's best-extraction (slot 0) picks it up.
 */
std::tuple<double, double> GStandardPSO2011::updateBests() {
    const auto m = this->at(0)->getMaxMode();

    global_best_improved_ = false;

    std::size_t best_idx = 0;
    std::tuple<double, double> best_iteration_fitness = std::make_tuple(
        this->at(0)->getWorstCase(),
        this->at(0)->getWorstCase()
    );

    for(std::size_t i = 0; i < swarm_size_; ++i) {
        auto &ind = (*this->at(i));
        const double fit = minOnly_transformed_fitness(ind);

        // Update the personal best (lower minimization fitness is always better).
        if(fit < personal_best_fitness_[i]) {
            personal_best_fitness_[i] = fit;
            ind.streamlineFPInternal(personal_bests_[i], activityMode::ACTIVEONLY);
        }

        // Update the global best.
        if(fit < global_best_fitness_) {
            global_best_fitness_ = fit;
            ind.streamlineFPInternal(global_best_, activityMode::ACTIVEONLY);
            global_best_improved_ = true;
        }

        // Track the best fitness tuple seen in this iteration.
        if(isBetter(
               ind.transformed_fitness(0),
               std::get<G_TRANSFORMED_FITNESS>(best_iteration_fitness),
               m
           )) {
            best_iteration_fitness = ind.getFitnessTuple();
            best_idx = i;
        }
    }

    // Move the best individual to the front so the base class extracts it as the iteration best.
    if(best_idx != 0) {
        std::swap(this->data_cnt_[0], this->data_cnt_[best_idx]);
        // Keep the parallel swarm-state arrays in sync with the swapped population slots.
        std::swap(velocities_[0], velocities_[best_idx]);
        std::swap(personal_bests_[0], personal_bests_[best_idx]);
        std::swap(personal_best_fitness_[0], personal_best_fitness_[best_idx]);
        // The informant topology is keyed by particle index; swap the two rows/columns so the
        // adjacency stays consistent with the new slot order.
        std::swap(informs_[0], informs_[best_idx]);
        for(auto &row : informs_) {
            const bool tmp = row[0];
            row[0] = row[best_idx];
            row[best_idx] = tmp;
        }
        this->at(0)->getPersonalityTraits<GStandardPSO2011_PersonalityTraits>()->setParticle(0);
        this->at(best_idx)->getPersonalityTraits<GStandardPSO2011_PersonalityTraits>()->setParticle(best_idx);
    }

    return best_iteration_fitness;
}

/******************************************************************************/
/**
 * The actual business logic to be performed during each iteration.
 *
 *   1. (Re-)build the random informant topology if needed (always in the first iteration; thereafter
 *      only if the global best did not improve).
 *   2. In iterations after the first, update all velocities and positions.
 *   3. (Re-)evaluate the whole swarm.
 *   4. Update the personal bests and the global best.
 *
 * @return The best fitness found in this iteration
 */
//! [spso-cycle]
std::tuple<double, double> GStandardPSO2011::cycleLogic_() {
    if(inFirstIteration() || not global_best_improved_) {
        buildTopology();
    }

    if(afterFirstIteration()) {
        updatePositions();
    }

    evaluatePopulation_();

    return updateBests();
}
//! [spso-cycle]

/******************************************************************************/
/**
 * Applies modifications to this object. This is needed for testing purposes.
 */
bool GStandardPSO2011::modify_GUnitTests_() {
#ifdef GEM_TESTING
    bool result = false;

    if(GOptimizationAlgorithmBase::modify_GUnitTests_()) {
        result = true;
    }

    this->setNInformants(this->getNInformants() + 1);
    result = true;

    return result;
#else  /* GEM_TESTING */
    Gem::Common::condnotset("GStandardPSO2011::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif /* GEM_TESTING */
}

/******************************************************************************/
/**
 * Performs self tests that are expected to succeed. This is needed for testing purposes.
 */
// NOLINTNEXTLINE(readability-function-size) -- self-test entry point for GStandardPSO2011: a sequence of independent, self-scoped CHECK blocks, one per PSO strategy-parameter scenario; same one-function-per-test-phase convention used identically across every OA self-test in this codebase
void GStandardPSO2011::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING

    { // Test setting and retrieval of basic strategy parameters
        auto const p_test = this->clone<GStandardPSO2011>();

        CHECK_NOTHROW(p_test->setSwarmSize(25));
        CHECK(p_test->getSwarmSize() == 25);

        CHECK_NOTHROW(p_test->setNInformants(5));
        CHECK(p_test->getNInformants() == 5);
    }

    { // Setting an invalid swarm size / informant count must throw
        auto const p_test = this->clone<GStandardPSO2011>();
        CHECK_THROWS(p_test->setSwarmSize(1));
        CHECK_THROWS(p_test->setNInformants(0));
    }
#else  /* GEM_TESTING */
    Gem::Common::condnotset(
        "GStandardPSO2011::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif /* GEM_TESTING */
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
