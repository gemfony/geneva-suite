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

#include "geneva/oa/GNelderMead.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)

#include <algorithm>
#include <cmath>
#include <limits>

#include "common/GLogger.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GNelderMead_PersonalityTraits.hpp"
#include "geneva/genome/GGenome.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GNelderMead) // NOLINT

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * @brief The default constructor.
 *
 * Delegates to the parameterized constructor using DEFAULTNMSIMPLICES simultaneous simplices.
 */
GNelderMead::GNelderMead()
  : GNelderMead(DEFAULTNMSIMPLICES) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Initialization with the number of simplices.
 *
 * @param n_simplices The number of simultaneous Nelder-Mead simplices to maintain
 */
GNelderMead::GNelderMead(const std::size_t &n_simplices)
  : n_simplices_(n_simplices) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Retrieves the number of simultaneous simplices.
 *
 * @return The currently configured number of simplices
 */
std::size_t GNelderMead::getNSimplices() const {
    return n_simplices_;
}

/******************************************************************************/
/**
 * @brief Allows to set the number of simultaneous simplices.
 *
 * @param n_simplices The desired number of simplices; must be greater than 0 (a value of 0 throws)
 */
void GNelderMead::setNSimplices(std::size_t n_simplices) {
    if(n_simplices == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNelderMead::setNSimplices(std::size_t):" << '\n'
            << "Got invalid number of simplices (0)." << '\n'
        );
    }
    n_simplices_ = n_simplices;
}

/******************************************************************************/
/**
 * @brief Sets the reflection coefficient.
 *
 * @param alpha The Nelder-Mead reflection coefficient; must be strictly greater than 0 (throws otherwise)
 */
void GNelderMead::setAlpha(double alpha) {
    if(alpha <= 0.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNelderMead::setAlpha(double): Error!" << '\n'
            << "alpha must be > 0, got " << alpha << '\n'
        );
    }
    alpha_ = alpha;
}

/******************************************************************************/
/**
 * @brief Retrieves the reflection coefficient.
 *
 * @return The current reflection coefficient alpha
 */
double GNelderMead::getAlpha() const {
    return alpha_;
}

/******************************************************************************/
/**
 * @brief Sets the expansion coefficient.
 *
 * @param gamma The Nelder-Mead expansion coefficient; must be strictly greater than 1 (throws otherwise)
 */
void GNelderMead::setGamma(double gamma) {
    if(gamma <= 1.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNelderMead::setGamma(double): Error!" << '\n'
            << "gamma must be > 1, got " << gamma << '\n'
        );
    }
    gamma_ = gamma;
}

/******************************************************************************/
/**
 * @brief Retrieves the expansion coefficient.
 *
 * @return The current expansion coefficient gamma
 */
double GNelderMead::getGamma() const {
    return gamma_;
}

/******************************************************************************/
/**
 * @brief Sets the contraction coefficient.
 *
 * @param rho The Nelder-Mead contraction coefficient; must lie strictly in the open interval ]0,1[ (throws otherwise)
 */
void GNelderMead::setRho(double rho) {
    if(rho <= 0. || rho >= 1.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNelderMead::setRho(double): Error!" << '\n'
            << "rho must be in ]0,1[, got " << rho << '\n'
        );
    }
    rho_ = rho;
}

/******************************************************************************/
/**
 * @brief Retrieves the contraction coefficient.
 *
 * @return The current contraction coefficient rho
 */
double GNelderMead::getRho() const {
    return rho_;
}

/******************************************************************************/
/**
 * @brief Sets the shrink coefficient.
 *
 * @param sigma The Nelder-Mead shrink coefficient; must lie strictly in the open interval ]0,1[ (throws otherwise)
 */
void GNelderMead::setSigma(double sigma) {
    if(sigma <= 0. || sigma >= 1.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNelderMead::setSigma(double): Error!" << '\n'
            << "sigma must be in ]0,1[, got " << sigma << '\n'
        );
    }
    sigma_ = sigma;
}

/******************************************************************************/
/**
 * @brief Retrieves the shrink coefficient.
 *
 * @return The current shrink coefficient sigma
 */
double GNelderMead::getSigma() const {
    return sigma_;
}

/******************************************************************************/
/**
 * @brief Sets the relative size of the initial simplex.
 *
 * @param initial_edge The initial edge length expressed as a fraction of each parameter's value range;
 *                     must be strictly greater than 0 (throws otherwise)
 */
void GNelderMead::setInitialEdge(double initial_edge) {
    if(initial_edge <= 0.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNelderMead::setInitialEdge(double): Error!" << '\n'
            << "initial_edge must be > 0, got " << initial_edge << '\n'
        );
    }
    initial_edge_ = initial_edge;
}

/******************************************************************************/
/**
 * @brief Retrieves the relative size of the initial simplex.
 *
 * @return The current initial edge length (as a fraction of each parameter's value range)
 */
double GNelderMead::getInitialEdge() const {
    return initial_edge_;
}

/******************************************************************************/
/**
 * @brief Sets the stall count after which an oriented restart is performed (0 = disabled).
 *
 * @param restart_threshold The number of stalled iterations after which the simplices are restarted around
 *                          their best vertex; 0 disables restarts
 */
void GNelderMead::setRestartThreshold(std::uint32_t restart_threshold) {
    restart_threshold_ = restart_threshold;
}

/******************************************************************************/
/**
 * @brief Retrieves the stall count after which an oriented restart is performed.
 *
 * @return The current restart threshold (0 means restarts are disabled)
 */
std::uint32_t GNelderMead::getRestartThreshold() const {
    return restart_threshold_;
}

/******************************************************************************/
/**
 * @brief Number of population slots used per simplex (vertices + trial slots).
 *
 * @return The block size, i.e. (n_fp_parms_first_ + 1) vertices plus NM_NTRIALS trial slots
 */
std::size_t GNelderMead::simplexBlockSize() const {
    return n_fp_parms_first_ + 1 + NM_NTRIALS;
}

/******************************************************************************/
/**
 * @brief Population index of vertex v in simplex s.
 *
 * @param s The simplex index
 * @param v The vertex index within the simplex
 * @return The flat population position of the requested vertex
 */
std::size_t GNelderMead::vertexPos(std::size_t s, std::size_t v) const {
    return (s * simplexBlockSize()) + v;
}

/******************************************************************************/
/**
 * @brief Population index of trial slot t in simplex s.
 *
 * @param s The simplex index
 * @param t The trial-slot index within the simplex (e.g. NM_REFLECT, NM_EXPAND, NM_CONTRACT, NM_OCONTRACT)
 * @return The flat population position of the requested trial slot
 */
std::size_t GNelderMead::trialPos(std::size_t s, std::size_t t) const {
    return (s * simplexBlockSize()) + (n_fp_parms_first_ + 1) + t;
}

/******************************************************************************/
/**
 * @brief Resets transient state so a fresh optimization run can start.
 *
 * Clears the pending-trials flag, then delegates to the base class.
 */
void GNelderMead::resetToOptimizationStart_() {
    trials_pending_ = false;

    GOptimizationAlgorithmBase::resetToOptimizationStart_();
}

/******************************************************************************/
/**
 * @brief The actual business logic to be performed during each iteration.
 *
 * Ordering (mirrors the gradient-descent-style decision/propose/evaluate pattern):
 *   1. apply the Nelder-Mead acceptance rules using the trials proposed and
 *      evaluated in the previous iteration (skipped until real trials exist),
 *   2. propose new reflection / expansion / contraction trial points,
 *   3. (re-)evaluate the whole population.
 *
 * @return The value of the best vertex found in this iteration
 */
std::tuple<double, double> GNelderMead::cycleLogic_() {
    // Oriented restart on stagnation (opt-in). Fires once every restart_threshold_
    // stalled iterations: rebuild each simplex around its best vertex so a degenerate
    // collapse can be escaped. The restarted vertices invalidate any pending trials, so
    // the decision is skipped and fresh trials are proposed below. Done here (not in
    // actOnStalls_, which must not dirty individuals) because the whole population is
    // re-evaluated this iteration anyway.
    if(restart_threshold_ != 0 && afterFirstIteration() &&
       getStallCounter() >= restart_threshold_ && (getStallCounter() % restart_threshold_ == 0)) {
        if(this->restartSimplices()) {
            trials_pending_ = false;
        }
    }

    if(afterFirstIteration() && trials_pending_) {
        this->applyNelderMeadDecision();
    }

    if(afterFirstIteration()) {
        this->proposeTrials();
        trials_pending_ = true;
    }

    evaluatePopulation_();

    std::tuple<double, double> best_fitness =
        std::make_tuple(this->at(0)->getWorstCase(), this->at(0)->getWorstCase());
    std::tuple<double, double> fitness_candidate =
        std::make_tuple(this->at(0)->getWorstCase(), this->at(0)->getWorstCase());

    auto m = this->at(0)->getMaxMode();
    for(std::size_t s = 0; s < n_simplices_; s++) {
        for(std::size_t v = 0; v <= n_fp_parms_first_; v++) {
            auto &ind = this->at(vertexPos(s, v));
            std::get<G_RAW_FITNESS>(fitness_candidate) = ind->raw_fitness(0);
            std::get<G_TRANSFORMED_FITNESS>(fitness_candidate) = ind->transformed_fitness(0);

            if(isBetter(
                   std::get<G_TRANSFORMED_FITNESS>(fitness_candidate),
                   std::get<G_TRANSFORMED_FITNESS>(best_fitness),
                   m
               )) {
                best_fitness = fitness_candidate;
            }
        }
    }

    return best_fitness;
}

/******************************************************************************/
/**
 * @brief Proposes the reflection, expansion, inside-contraction and outside-contraction trial points for every simplex.
 *
 * The worst vertex and the centroid of the
 * remaining vertices are determined from the most recent evaluation; the four
 * candidates are written into the trial slots so they are evaluated in this
 * iteration. The acceptance rules (next iteration) then pick at most one of them.
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel: one Nelder-Mead trial-generation step per simplex (worst vertex, centroid, then the four reflect/expand/contract/ocontract candidates derived from that same centroid); splitting would scatter one tightly coupled formula set
void GNelderMead::proposeTrials() {
    for(std::size_t s = 0; s < n_simplices_; s++) {
        const std::size_t n_vert = n_fp_parms_first_ + 1;

        // Collect vertex parameter vectors and (minimization) fitnesses
        std::vector<std::vector<double>> vparm(n_vert);
        std::vector<double> vfit(n_vert);
        for(std::size_t v = 0; v < n_vert; v++) {
            auto &ind = this->at(vertexPos(s, v));
            ind->streamlineFPInternal(vparm[v], activityMode::ACTIVEONLY);
            // A vertex modified by a shrink in applyNelderMeadDecision() during
            // this same iteration has not been re-evaluated yet (its stored
            // result was invalidated). Parameters are always readable, but its
            // fitness is not; treat such a vertex as the worst so it becomes the
            // reflected point. It is re-evaluated by evaluatePopulation_()
            // later in this iteration, so proper ranking resumes next cycle.
            if(ind->is_due_for_processing() || ind->has_errors()) {
                vfit[v] = std::numeric_limits<double>::max();
            }
            else {
                vfit[v] = minOnly_transformed_fitness((*ind));
            }
        }

        // Worst vertex = largest minimization fitness
        std::size_t w = 0;
        for(std::size_t v = 1; v < n_vert; v++) {
            if(vfit[v] > vfit[w]) {
                w = v;
            }
        }

        // Centroid of all vertices except the worst
        std::vector<double> centroid(n_fp_parms_first_, 0.);
        for(std::size_t v = 0; v < n_vert; v++) {
            if(v == w) {
                continue;
            }
            for(std::size_t k = 0; k < n_fp_parms_first_; k++) {
                centroid[k] += vparm[v][k];
            }
        }
        const auto denom = static_cast<double>(n_vert - 1);
        for(std::size_t k = 0; k < n_fp_parms_first_; k++) {
            centroid[k] /= denom;
        }

        const std::vector<double> &xw = vparm[w];

        std::vector<double> reflect(n_fp_parms_first_);
        std::vector<double> expand(n_fp_parms_first_);
        std::vector<double> contract(n_fp_parms_first_);
        std::vector<double> ocontract(n_fp_parms_first_);
        for(std::size_t k = 0; k < n_fp_parms_first_; k++) {
            reflect[k] = centroid[k] + (alpha_ * (centroid[k] - xw[k]));
            expand[k] = centroid[k] + (gamma_ * (centroid[k] - xw[k]));
            contract[k] = centroid[k] + (rho_ * (xw[k] - centroid[k]));   // inside  contraction
            ocontract[k] = centroid[k] + (rho_ * (reflect[k] - centroid[k])); // outside contraction
        }

        this->at(trialPos(s, NM_REFLECT))
            ->assignFPValueVectorInternal(reflect, activityMode::ACTIVEONLY);
        this->at(trialPos(s, NM_EXPAND))
            ->assignFPValueVectorInternal(expand, activityMode::ACTIVEONLY);
        this->at(trialPos(s, NM_CONTRACT))
            ->assignFPValueVectorInternal(contract, activityMode::ACTIVEONLY);
        this->at(trialPos(s, NM_OCONTRACT))
            ->assignFPValueVectorInternal(ocontract, activityMode::ACTIVEONLY);
    }
}

/******************************************************************************/
/**
 * @brief Applies the standard Nelder-Mead acceptance rules.
 *
 * The reflection, expansion
 * and inside-contraction candidates were proposed in the previous iteration
 * (around the then-worst vertex) and have just been evaluated. Because the
 * vertices were not modified between proposal and this call, the worst vertex
 * recomputed here is the same one the trials were built for.
 *
 * Accepting a trial is done via load(): the vertex thereby also
 * inherits the trial's already-known fitness, which keeps proposeTrials() in
 * the same iteration consistent (no stale ranking except after a shrink).
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent decision procedure: the standard Nelder-Mead acceptance tree (best/worst/second-worst ranking feeding the reflect/expand/contract/shrink cascade) per simplex; splitting the cascade would scatter one textbook algorithm's decision logic
void GNelderMead::applyNelderMeadDecision() {
    for(std::size_t s = 0; s < n_simplices_; s++) {
        const std::size_t n_vert = n_fp_parms_first_ + 1;

        std::vector<double> vfit(n_vert);
        for(std::size_t v = 0; v < n_vert; v++) {
            vfit[v] = minOnly_transformed_fitness((*this->at(vertexPos(s, v))));
        }

        // Best, worst and second-worst vertices (minimization fitness)
        std::size_t b = 0;
        std::size_t w = 0;
        for(std::size_t v = 1; v < n_vert; v++) {
            if(vfit[v] < vfit[b]) {
                b = v;
            }
            if(vfit[v] > vfit[w]) {
                w = v;
            }
        }
        std::size_t sw = (w == 0) ? 1 : 0;
        for(std::size_t v = 0; v < n_vert; v++) {
            if(v == w) {
                continue;
            }
            if(vfit[v] > vfit[sw]) {
                sw = v;
            }
        }

        const double f_best = vfit[b];
        const double f_worst = vfit[w];
        const double f_second = vfit[sw];

        const double f_r = minOnly_transformed_fitness((*this->at(trialPos(s, NM_REFLECT))));
        const double f_e = minOnly_transformed_fitness((*this->at(trialPos(s, NM_EXPAND))));
        const double f_c = minOnly_transformed_fitness((*this->at(trialPos(s, NM_CONTRACT))));
        const double f_oc = minOnly_transformed_fitness((*this->at(trialPos(s, NM_OCONTRACT))));

        auto accept_trial_into_worst = [&](std::size_t trial_slot) {
            this->at(vertexPos(s, w))->load(this->at(trialPos(s, trial_slot)));
            this->at(vertexPos(s, w))
                ->getPersonalityTraits<GNelderMead_PersonalityTraits>()
                ->setPopulationPosition(vertexPos(s, w));
        };

        if(f_r < f_best) {
            // Reflection is the new best -> try to expand further
            if(f_e < f_r) {
                accept_trial_into_worst(NM_EXPAND);
            }
            else {
                accept_trial_into_worst(NM_REFLECT);
            }
        }
        else if(f_r < f_second) {
            // Reflection is an improvement (but not the best) -> accept it
            accept_trial_into_worst(NM_REFLECT);
        }
        else if(f_r < f_worst) {
            // f_second <= f_r < f_worst: try the OUTSIDE contraction (between the
            // centroid and the reflected point). Accept it if it is no worse than
            // the reflection, otherwise shrink.
            if(f_oc <= f_r) {
                accept_trial_into_worst(NM_OCONTRACT);
            }
            else {
                shrinkTowardsBest(s, b);
            }
        }
        else {
            // f_r >= f_worst: try the INSIDE contraction (between the centroid and
            // the worst vertex). Accept it if it improves on the worst vertex,
            // otherwise shrink the simplex towards the best vertex.
            if(f_c < f_worst) {
                accept_trial_into_worst(NM_CONTRACT);
            }
            else {
                shrinkTowardsBest(s, b);
            }
        }
    }
}

/******************************************************************************/
/**
 * @brief Shrinks simplex s by moving every non-best vertex a fraction sigma_ of the way towards the best vertex.
 *
 * The shrunk vertices are left unevaluated (their stored fitness is now stale); they are re-evaluated by
 * evaluatePopulation_() later in the same iteration.
 *
 * @param s The index of the simplex to shrink
 * @param b The index (within the simplex) of the best vertex, towards which the others are contracted
 */
void GNelderMead::shrinkTowardsBest(std::size_t s, std::size_t b) {
    const std::size_t n_vert = n_fp_parms_first_ + 1;
    std::vector<double> xb;
    this->at(vertexPos(s, b))->streamlineFPInternal(xb, activityMode::ACTIVEONLY);
    for(std::size_t v = 0; v < n_vert; v++) {
        if(v == b) {
            continue;
        }
        std::vector<double> xv;
        this->at(vertexPos(s, v))->streamlineFPInternal(xv, activityMode::ACTIVEONLY);
        for(std::size_t k = 0; k < n_fp_parms_first_; k++) {
            xv[k] = xb[k] + (sigma_ * (xv[k] - xb[k]));
        }
        this->at(vertexPos(s, v))->assignFPValueVectorInternal(xv, activityMode::ACTIVEONLY);
    }
}

/******************************************************************************/
/**
 * @brief Adds local configuration options to a GParserBuilder object.
 *
 * @param gpb The GParserBuilder to which the Nelder-Mead file-configuration options are added
 */
void GNelderMead::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    GOptimizationAlgorithmBase::addConfigurationOptions_(gpb);

    gpb.registerFileParameter<std::size_t>(
        "n_simplices",
        DEFAULTNMSIMPLICES,
        [this](std::size_t ns) { this->setNSimplices(ns); }
    ) << "The number of simultaneous Nelder-Mead simplices";

    gpb.registerFileParameter<double>(
        "alpha",
        DEFAULTNMALPHA,
        [this](double a) { this->setAlpha(a); }
    ) << "The Nelder-Mead reflection coefficient (> 0)";

    gpb.registerFileParameter<double>(
        "gamma",
        DEFAULTNMGAMMA,
        [this](double g) { this->setGamma(g); }
    ) << "The Nelder-Mead expansion coefficient (> 1)";

    gpb.registerFileParameter<double>(
        "rho",
        DEFAULTNMRHO,
        [this](double r) { this->setRho(r); }
    ) << "The Nelder-Mead contraction coefficient (in ]0,1[)";

    gpb.registerFileParameter<double>(
        "sigma",
        DEFAULTNMSIGMA,
        [this](double si) { this->setSigma(si); }
    ) << "The Nelder-Mead shrink coefficient (in ]0,1[)";

    gpb.registerFileParameter<double>(
        "initial_edge",
        DEFAULTNMINITIALEDGE,
        [this](double ie) { this->setInitialEdge(ie); }
    ) << "Relative size of the initial simplex,"
      << '\n'
      << "as a fraction of each parameter's value range";

    gpb.registerFileParameter<std::uint32_t>(
        "restart_threshold",
        DEFAULTNMRESTARTTHRESHOLD,
        [this](std::uint32_t rt) { this->setRestartThreshold(rt); }
    ) << "Stall count after which the simplices are restarted (oriented, around" << '\n'
      << "the best vertex) to escape a degenerate collapse. 0 disables restarts";
}

/******************************************************************************/
/**
 * @brief Triggers fitness calculation of all individuals via the consumer.
 *
 * Throws if the consumer does not return a complete set of results or reports errors in any individual.
 */
void GNelderMead::evaluatePopulation_() {
    using namespace Gem::Courtier;

    auto status = this->workOnPopulation(0, this->data_cnt_.size());

    this->requireCompleteEvaluation_(status, "GNelderMead::evaluatePopulation_()");
}

/******************************************************************************/
/**
 * @brief Does some preparatory work before the optimization starts.
 *
 * Clears the pending-trials flag, builds the initial simplices and records each individual's population
 * position. (The algorithm is boundary-agnostic: it works in the normalized internal coordinate.)
 */
void GNelderMead::init() {
    GOptimizationAlgorithmBase::init();

    trials_pending_ = false;
    buildInitialSimplices();
    markIndividualPositions();
}

/******************************************************************************/
/**
 * @brief Builds a non-degenerate initial simplex around each seed vertex.
 *
 * Vertex 0 of
 * every simplex is the (user-supplied or randomized) seed; the remaining n
 * vertices are obtained by perturbing one coordinate each by a dimensionless fraction
 * (initial_edge_) of the normalized unit interval.
 */
void GNelderMead::buildInitialSimplices() {
    for(std::size_t s = 0; s < n_simplices_; s++) {
        std::vector<double> p0;
        this->at(vertexPos(s, 0))->streamlineFPInternal(p0, activityMode::ACTIVEONLY);

        for(std::size_t v = 1; v <= n_fp_parms_first_; v++) {
            std::vector<double> p = p0;
            const std::size_t k = v - 1; // coordinate perturbed for this vertex

            // The simplex edge is a dimensionless fraction of the normalized unit interval (the same for
            // every parameter); positions live in the internal coordinate, so no per-parameter range.
            const double edge = initial_edge_;

            p[k] += edge;
            this->at(vertexPos(s, v))->assignFPValueVectorInternal(p, activityMode::ACTIVEONLY);
        }

        // The trial slots start as copies of the seed; they are overwritten by
        // proposeTrials() before they are first used for a decision.
        for(std::size_t t = 0; t < NM_NTRIALS; t++) {
            this->at(trialPos(s, t))->assignFPValueVectorInternal(p0, activityMode::ACTIVEONLY);
        }
    }
}

/******************************************************************************/
/**
 * @brief Rebuilds every simplex around its current best vertex to escape a degenerate collapse.
 *
 * A degenerate collapse occurs when vertices have become near-coplanar so the simplex can no longer
 * explore some directions). Called from cycleLogic_() once the run has stalled
 * for restart_threshold_ iterations.
 *
 * The restart is *oriented*: each of the n new vertices perturbs one coordinate
 * of the best vertex, and the sign of the perturbation follows the local descent
 * estimate -- the direction from the centroid of the other vertices towards the
 * best vertex (which, the best being the lowest, is the downhill direction). This
 * biases the fresh simplex down the slope instead of re-exploring symmetrically,
 * so it is far less likely to immediately re-collapse. The edge length is the same
 * fraction (initial_edge_) of the parameter range used by buildInitialSimplices().
 *
 * Only the n non-best vertices are moved (and thereby invalidated); the best
 * vertex -- and hence the best-so-far recorded globally -- is preserved, so a
 * restart can never worsen the reported result.
 *
 * @return true if a restart was performed (i.e. there is at least one simplex), false otherwise
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel: one oriented-restart step per simplex (best vertex, descent-orienting centroid, then the per-coordinate perturbation of every non-best vertex derived from that centroid); splitting would scatter one tightly coupled procedure
bool GNelderMead::restartSimplices() {
    const std::size_t n_vert = n_fp_parms_first_ + 1;
    for(std::size_t s = 0; s < n_simplices_; s++) {
        // Best vertex and the centroid of the remaining vertices (for the descent orientation).
        std::vector<std::vector<double>> vparm(n_vert);
        std::vector<double> vfit(n_vert);
        for(std::size_t v = 0; v < n_vert; v++) {
            auto &ind = this->at(vertexPos(s, v));
            ind->streamlineFPInternal(vparm[v], activityMode::ACTIVEONLY);
            vfit[v] = minOnly_transformed_fitness((*ind));
        }
        std::size_t b = 0;
        for(std::size_t v = 1; v < n_vert; v++) {
            if(vfit[v] < vfit[b]) {
                b = v;
            }
        }

        std::vector<double> centroid(n_fp_parms_first_, 0.);
        for(std::size_t v = 0; v < n_vert; v++) {
            if(v == b) {
                continue;
            }
            for(std::size_t k = 0; k < n_fp_parms_first_; k++) {
                centroid[k] += vparm[v][k];
            }
        }
        const auto denom = static_cast<double>(n_vert - 1);
        for(std::size_t k = 0; k < n_fp_parms_first_; k++) {
            centroid[k] /= denom;
        }

        const std::vector<double> &xb = vparm[b];

        // Rebuild the n non-best vertices around xb, one perturbed coordinate each.
        std::size_t k = 0; // coordinate assigned to the current non-best vertex
        for(std::size_t v = 0; v < n_vert; v++) {
            if(v == b) {
                continue; // keep the best vertex in place
            }
            std::vector<double> p = xb;

            // The simplex edge is a dimensionless fraction of the normalized unit interval (see
            // buildInitialSimplices); positions live in the internal coordinate, so no per-parameter range.
            const double edge = initial_edge_;

            // Orient the perturbation downhill: step in the direction leading from the
            // (worse) centroid towards the best vertex. Fall back to + when they coincide.
            const double descent = xb[k] - centroid[k];
            const double sign = (descent >= 0.) ? 1. : -1.;
            p[k] += sign * edge;

            this->at(vertexPos(s, v))->assignFPValueVectorInternal(p, activityMode::ACTIVEONLY);
            k++;
        }
    }
    return n_simplices_ > 0;
}

/******************************************************************************/
/**
 * @brief Performs any necessary finalization work after the optimization has ended.
 *
 * Currently just delegates to the base class.
 */
void GNelderMead::finalize() {
    GOptimizationAlgorithmBase::finalize();
}

/******************************************************************************/
/**
 * @brief Creates the personality traits object associated with this algorithm.
 *
 * @return A shared pointer to a freshly created GNelderMead_PersonalityTraits object
 */
std::shared_ptr<GPersonalityTraits> GNelderMead::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/
/**
 * @brief Resizes the population to the desired level and does some error checks.
 *
 * The layout is n_simplices_ blocks of (n_fp_parms_first_ + 1) vertices plus NM_NTRIALS speculative trial
 * slots each. The number of floating-point parameters is read from the first individual; any integer or
 * boolean parameters are left untouched (Nelder-Mead operates only on the continuous parameter space) and
 * merely logged. Each simplex gets a seed individual; simplices beyond the supplied individuals are seeded
 * with randomized clones of the first, and the remaining vertex slots are filled with clones; the real
 * initial simplex geometry is constructed later in init().
 */
void GNelderMead::adjustPopulation_() {
    // Non-empty population + at least one active FP parameter; logs a note for int/bool riders
    // (the shared precondition of the floating-point-only algorithms).
    n_fp_parms_first_ = this->requireFloatingPointGenome_("GNelderMead");
    const std::size_t n_start = this->size();

    const std::size_t block_size = n_fp_parms_first_ + 1 + NM_NTRIALS;
    const std::size_t total_size = n_simplices_ * block_size;

    GOptimizationAlgorithmBase::setDefaultPopulationSize(total_size);

    // Make sure we have one (randomized) seed individual per simplex first.
    if(n_start < n_simplices_) {
        for(std::size_t i = 0; i < (n_simplices_ - n_start); i++) {
            this->push_back(this->at(0)->clone());
            this->back()->randomInit(activityMode::ACTIVEONLY);
        }
    }
    else if(n_start > n_simplices_) {
        this->resize(n_simplices_);
    }

    // The seeds currently sit at positions 0 .. n_simplices_-1. Re-order them so
    // that seed s ends up at vertexPos(s,0) and fill the rest of every block
    // with clones (the real initial simplex is constructed in init()).
    std::vector<std::shared_ptr<gen::GGenome>> seeds;
    seeds.reserve(n_simplices_);
    for(std::size_t s = 0; s < n_simplices_; s++) {
        seeds.push_back(this->at(s)->clone<gen::GGenome>());
    }

    this->clear();
    for(std::size_t s = 0; s < n_simplices_; s++) {
        this->push_back(seeds[s]->clone()); // vertex 0 of simplex s
        for(std::size_t r = 1; r < block_size; r++) {
            this->push_back(seeds[s]->clone());
        }
    }

#ifdef DEBUG
    if(this->size() != total_size) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GNelderMead::adjustPopulation():" << '\n'
            << "Population size is " << this->size() << '\n'
            << "but expected " << total_size << '\n'
        );
    }
#endif /* DEBUG */
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
