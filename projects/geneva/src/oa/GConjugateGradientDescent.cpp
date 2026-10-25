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

#include "geneva/oa/GConjugateGradientDescent.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)

#include <algorithm>
#include <istream>
#include <limits>
#include <ostream>
#include <sstream>

#include "common/GLogger.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GConjugateGradientDescent_PersonalityTraits.hpp"
#include "geneva/genome/GGenome.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <tuple>
#include <span>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GConjugateGradientDescent) // NOLINT

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * The auxiliary-store keys under which each starting point keeps its conjugate-gradient memory on the
 * OA scratch of its CENTRAL individual (the individual at population position == starting
 * point). This per-starting-point scratch (previously parallel vectors on the
 * algorithm) rides on the individual as plain POD blocks. The two double blocks hold g_{k-1} / d_{k-1}
 * (n_fp_parms entries each); the one-byte block flags whether a previous gradient/direction exists. The
 * values are distinct from the adaption AuxKeys 1-7 and the swarm velocity key 8.
 */
constexpr Gem::Geneva::Genome::AuxKey AUXKEY_CGD_PREV_GRADIENT = 9;
constexpr Gem::Geneva::Genome::AuxKey AUXKEY_CGD_PREV_DIRECTION = 10;
constexpr Gem::Geneva::Genome::AuxKey AUXKEY_CGD_HISTORY_VALID = 11;
// L-BFGS quasi-Newton state (only installed in LBFGS mode): the last m (s, y) curvature pairs, the
// previous parameter vector, and the live pair count.
constexpr Gem::Geneva::Genome::AuxKey AUXKEY_CGD_LBFGS_S = 12;     // m*n doubles (s = x_k - x_{k-1})
constexpr Gem::Geneva::Genome::AuxKey AUXKEY_CGD_LBFGS_Y = 13;     // m*n doubles (y = g_k - g_{k-1})
constexpr Gem::Geneva::Genome::AuxKey AUXKEY_CGD_LBFGS_PREV_X = 14; // n doubles (x_{k-1})
constexpr Gem::Geneva::Genome::AuxKey AUXKEY_CGD_LBFGS_COUNT = 15;  // 1 (number of stored pairs, 0..m)

/******************************************************************************/
/**
 * @brief The default constructor
 *
 * Delegates to the parameterized constructor using the compiled-in default number of starting points,
 * finite-difference step and step size.
 */
GConjugateGradientDescent::GConjugateGradientDescent()
  : GConjugateGradientDescent(
        DEFAULTCGDSTARTINGPOINTS,
        DEFAULTCGDFINITESTEP,
        DEFAULTCGDSTEPSIZE
    ) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Initialization with the number of starting points and step parameters
 *
 * @param n_starting_points The number of simultaneous, independent conjugate gradient descents
 * @param finite_step The size of the difference-quotient step (in per mill of the parameter value range)
 * @param step_size The multiplicative factor for the (initial trial) step along the search direction
 */
GConjugateGradientDescent::GConjugateGradientDescent(
    const std::size_t &n_starting_points,
    const double &finite_step,
    const double &step_size
)
  : n_starting_points_(n_starting_points)
  , finite_step_(finite_step)
  , step_size_(step_size) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Retrieves the number of starting points of the algorithm
 *
 * @return The number of simultaneous starting points
 */
std::size_t GConjugateGradientDescent::getNStartingPoints() const {
    return n_starting_points_;
}

/******************************************************************************/
/**
 * @brief Allows to set the number of starting points for the conjugate gradient descent
 *
 * @param n_starting_points The desired number of simultaneous starting points; throws if 0
 */
void GConjugateGradientDescent::setNStartingPoints(std::size_t n_starting_points) {
    if(n_starting_points == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GConjugateGradientDescent::setNStartingPoints(const std::size_t&):" << '\n'
            << "Got invalid number of starting points." << '\n'
        );
    }

    n_starting_points_ = n_starting_points;
}

/******************************************************************************/
/**
 * @brief Set the size of the finite step of the difference quotient
 *
 * @param finite_step The finite step size in per mill of the parameter value range; must be in ]0.:1000.] or the call throws
 */
void GConjugateGradientDescent::setFiniteStep(double finite_step) {
    if(finite_step <= 0. ||
       finite_step > 1000.) { // Specified in per mill of the allowed or preferred value range
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GConjugateGradientDescent::setFiniteStep(double): Error!" << '\n'
            << "Invalid value of finite_step: " << finite_step << '\n'
            << "Must be in the range ]0.:1000.]" << '\n'
        );
    }

    finite_step_ = finite_step;

    // Keep adjusted_finite_step_ consistent if called after init()
    updateDerivedQuantities();
}

/******************************************************************************/
/**
 * @brief Retrieve the size of the finite step of the difference quotient
 *
 * @return The finite step size (in per mill of the parameter value range)
 */
double GConjugateGradientDescent::getFiniteStep() const {
    return finite_step_;
}

/******************************************************************************/
/**
 * @brief Sets a multiplier for the step along the search direction
 *
 * @param step_size The (initial trial) step size in per mill of the parameter value range; must be in ]0.:1000.] or the call throws
 */
void GConjugateGradientDescent::setStepSize(double step_size) {
    if(step_size <= 0. ||
       step_size > 1000.) { // Specified in per mill of the allowed or preferred value range
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GConjugateGradientDescent::setStepSize(double): Error!" << '\n'
            << "Invalid value of step_size: " << step_size << '\n'
            << "Must be in the range ]0.:1000.]" << '\n'
        );
    }

    step_size_ = step_size;
}

/******************************************************************************/
/**
 * @brief Retrieves the current step size
 *
 * @return The (initial trial) step size (in per mill of the parameter value range)
 */
double GConjugateGradientDescent::getStepSize() const {
    return step_size_;
}

/******************************************************************************/
/**
 * @brief Selects the search-direction rule. STEEPEST_DESCENT (beta == 0) reproduces the former, separate
 * gradient-descent algorithm; CONJUGATE_PR_PLUS (the default) is the Polak-Ribiere+ nonlinear CG.
 *
 * @param gm The search-direction rule to use (steepest descent, a conjugate-gradient variant, or L-BFGS)
 */
void GConjugateGradientDescent::setGradientMethod(gradientMethod gm) {
    gradient_method_ = gm;
}

/******************************************************************************/
/**
 * @brief Retrieves the search-direction rule currently in use.
 *
 * @return The currently selected search-direction rule
 */
gradientMethod GConjugateGradientDescent::getGradientMethod() const {
    return gradient_method_;
}

/******************************************************************************/
/**
 * @brief Enables/disables the O(h^2) central-difference gradient (doubles the probes per direction).
 *
 * @param central true to use the central-difference gradient, false for the default forward difference
 */
void GConjugateGradientDescent::setCentralDifferences(bool central) {
    central_differences_ = central;
}

/******************************************************************************/
/**
 * @brief Whether the central-difference gradient is in use.
 *
 * @return true if the central-difference gradient is enabled, false for the forward difference
 */
bool GConjugateGradientDescent::getCentralDifferences() const {
    return central_differences_;
}

/******************************************************************************/
/**
 * @brief Sets the L-BFGS history size m (clamped to >= 1).
 *
 * @param m The number of (s, y) curvature pairs to keep per starting point; a value of 0 is clamped to 1
 */
void GConjugateGradientDescent::setLBFGSMemory(std::size_t m) {
    lbfgs_memory_ = (m == 0) ? 1 : m;
}

/******************************************************************************/
/**
 * @brief Retrieves the L-BFGS history size m.
 *
 * @return The number of (s, y) curvature pairs kept per starting point
 */
std::size_t GConjugateGradientDescent::getLBFGSMemory() const {
    return lbfgs_memory_;
}

/******************************************************************************/
/**
 * @brief Selects whether/how a MINUIT-style parameter-error estimate is computed at convergence.
 *
 * @param em The error-estimation mode (none, diagonal, full Hessian, or MINOS asymmetric)
 */
void GConjugateGradientDescent::setErrorEstimation(errorEstimationMode em) {
    error_estimation_ = em;
}

/******************************************************************************/
/**
 * @brief Retrieves the error-estimation mode currently in use.
 *
 * @return The currently selected error-estimation mode
 */
errorEstimationMode GConjugateGradientDescent::getErrorEstimation() const {
    return error_estimation_;
}

/******************************************************************************/
/**
 * @brief Sets the error definition UP (the objective increase defining one standard deviation).
 *
 * @param up The MINUIT error definition UP (e.g. 1 for chi^2, 0.5 for -logL); must be positive or the call throws
 */
void GConjugateGradientDescent::setErrorDefinition(double up) {
    if(up <= 0.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GConjugateGradientDescent::setErrorDefinition(double): Error!" << '\n'
            << "UP must be positive, got " << up << '\n'
        );
    }
    error_up_ = up;
}

/******************************************************************************/
/**
 * @brief Retrieves the error definition UP.
 *
 * @return The MINUIT error definition UP
 */
double GConjugateGradientDescent::getErrorDefinition() const {
    return error_up_;
}

/******************************************************************************/
/**
 * @brief Retrieves the most recent convergence error estimate.
 *
 * @return The error estimate produced by the last finalize() pass (its validity flags indicate whether it is usable)
 */
GHesseErrorResult GConjugateGradientDescent::getLastErrorEstimate() const {
    return last_error_estimate_;
}

/******************************************************************************/
/**
 * @brief Resets the settings of this population to what was configured when
 * the optimize()-call was issued
 */
void GConjugateGradientDescent::resetToOptimizationStart_() {
    adjusted_finite_step_ = 0.;
    // The per-starting-point conjugate-gradient memory now lives on the central slots' OA scratch and is
    // dropped at the optimization-algorithm boundary (resetIndividualPersonalities -> clearScratch).

    GOptimizationAlgorithmBase::resetToOptimizationStart_();
}

/******************************************************************************/
/**
 * The actual business logic to be performed during each iteration.
 *
 * The structure mirrors GGradientDescent: the conjugate-gradient step uses
 * the difference quotients evaluated in the *previous* iteration (they are
 * still stored on the individuals), then the children are rebuilt around the
 * stepped parents and the whole population is re-evaluated.
 *
 * @return The value of the best individual found in this iteration
 */
std::tuple<double, double> GConjugateGradientDescent::cycleLogic_() {
    if(afterFirstIteration()) {
        // Perform a conjugate-gradient step for every starting point. This
        // only makes sense once the difference quotients have been evaluated.
        this->updateParentIndividuals();
    }

    // Rebuild the difference-quotient children around the (stepped) parents
    this->updateChildParameters();

    // Trigger value calculation for all individuals (parents + children)
    evaluatePopulation_();

    std::tuple<double, double> best_fitness =
        std::make_tuple(this->at(0)->getWorstCase(), this->at(0)->getWorstCase());
    std::tuple<double, double> fitness_candidate =
        std::make_tuple(this->at(0)->getWorstCase(), this->at(0)->getWorstCase());

    GConjugateGradientDescent::iterator it;
    auto m = this->at(0)->getMaxMode(); // All individuals share the same max mode
    for(it = this->begin(); it != this->begin() + this->getNStartingPoints(); ++it) {
        std::get<G_RAW_FITNESS>(fitness_candidate) = (*it)->raw_fitness(0);
        std::get<G_TRANSFORMED_FITNESS>(fitness_candidate) = (*it)->transformed_fitness(0);

        if(isBetter(
               std::get<G_TRANSFORMED_FITNESS>(fitness_candidate),
               std::get<G_TRANSFORMED_FITNESS>(best_fitness),
               m
           )) {
            best_fitness = fitness_candidate;
        }
    }

    return best_fitness;
}

/******************************************************************************/
/**
 * @brief Rebuilds the difference-quotient children of every starting point. For starting point i and
 * direction j the child(ren) at
 *
 *   n_starting_points_ + i * children_per_sp + j * n_probes + probe   (children_per_sp = n_fp * n_probes)
 *
 * are copies of parent i with the j-th active parameter stepped by +h (probe 0, forward) and, for the
 * central-difference gradient, also -h (probe 1, backward). n_probes is 1 (forward) or 2 (central).
 */
void GConjugateGradientDescent::updateChildParameters() {
    // One probe per direction (forward) by default, two (forward + backward) for central differences.
    // child_pos = n_starting_points_ + i*children_per_sp + j*n_probes + probe; with n_probes == 1 this
    // collapses to the historical layout n_starting_points_ + i*n_fp + j (the default path is unchanged).
    const std::size_t n_probes = central_differences_ ? 2 : 1;
    const std::size_t children_per_sp = n_fp_parms_first_ * n_probes;
    for(std::size_t i = 0; i < n_starting_points_; i++) {
        std::vector<double> parm_vec;
        this->at(i)->streamlineFPInternal(parm_vec, activityMode::ACTIVEONLY);

        for(std::size_t j = 0; j < n_fp_parms_first_; j++) {
            const double orig_parm_val = parm_vec[j];
            for(std::size_t probe = 0; probe < n_probes; probe++) {
                const std::size_t child_pos =
                    n_starting_points_ + (i * children_per_sp) + (j * n_probes) + probe;

                // Load the current "parent" into the "child"
                this->at(child_pos)->load(this->at(i));

                // Update the child's position in the population
                this->at(child_pos)
                    ->getPersonalityTraits<GConjugateGradientDescent_PersonalityTraits>()
                    ->setPopulationPosition(child_pos);

                // probe 0 = forward (+h); probe 1 (central only) = backward (-h)
                const double sign = (probe == 0) ? 1. : -1.;
                parm_vec[j] = orig_parm_val + (sign * adjusted_finite_step_);
                this->at(child_pos)->assignFPValueVectorInternal(parm_vec, activityMode::ACTIVEONLY);
            }
            // Restore the original value for the next direction
            parm_vec[j] = orig_parm_val;
        }
    }
}

/******************************************************************************/
/**
 * @brief Performs a non-linear conjugate-gradient step for every starting point.
 *
 * For each starting point the (proxy) gradient component in direction j is the
 * forward difference
 *
 *   g_j = (f(x + h_j e_j) - f(x)) / h_j
 *
 * (a normalised O(h) finite-difference gradient; the central-difference variant uses
 * (f(x+h_j e_j) - f(x-h_j e_j)) / (2 h_j) when central_differences_ is set). The Polak-Ribière+
 * coefficient and the new conjugate search direction are
 *
 *   beta = max(0, g . (g - g_prev) / (g_prev . g_prev))
 *   d    = -g + beta * d_prev          (with d = -g on the first step or on a restart)
 *
 * and the parameter vector is moved by step_ratio * d, with
 * step_ratio = step_size_ / finite_step_ (identical to GGradientDescent). A
 * non-positive / numerically unstable denominator triggers an automatic
 * restart (beta = 0), which keeps the method globally convergent.
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel implementing the documented 6-step nonlinear-CG/line-search algorithm per starting point; the steps share loop-carried gradient/direction/scratch state and splitting them would scatter one tightly coupled numeric procedure
void GConjugateGradientDescent::updateParentIndividuals() {
    // The line search's first trial step reproduces the former fixed step (step_size_/finite_step_) and
    // backtracks from there, so the method is never worse than the old fixed step and -- by the Armijo
    // sufficient-decrease test -- never moves a starting point uphill.
    const double step_ratio = step_size_ / finite_step_;

    // A representative parameter-space scale for the initial trial step. The gradient below is
    // normalised (units of 1/parameter), so a bare step_ratio would be mis-scaled; multiplying by the
    // (now uniform) difference-quotient step restores the old fixed step's magnitude as the first trial.
    const double mean_h = adjusted_finite_step_;

    // A conjugate direction loses accuracy after about n steps, so restart to steepest descent every
    // n_fp_parms iterations (the classical periodic restart) to keep the nonlinear CG globally
    // convergent.
    const bool periodic_restart =
        (n_fp_parms_first_ > 0) && (this->getIteration() % n_fp_parms_first_ == 0);

    GLineSearch const line_search;

    for(std::size_t i = 0; i < n_starting_points_; i++) {
        std::vector<double> parm_vec;
        this->at(i)->streamlineFPInternal(parm_vec, activityMode::ACTIVEONLY);

#ifdef DEBUG
        if(this->at(i)->is_due_for_processing() || (this->at(i)->has_errors())) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GConjugateGradientDescent::updateParentIndividuals():" << '\n'
                << "Found individual in position " << i
                << " which is unprocessed or has errors" << '\n'
            );
        }
#endif /* DEBUG */

        const double parent_fitness = minOnly_transformed_fitness((*this->at(i)));

        // The conjugate-gradient memory for this starting point lives on its central individual's slot
        // scratch (position i); ensureGradientScratch_() installs any blocks a freshly spliced-in slot
        // is missing, so it restarts cleanly with steepest descent.
        auto &cg_scratch = this->at(i)->scratch();
        this->ensureGradientScratch_(cg_scratch);
        std::span<double> const prev_gradient = cg_scratch.metaRecords<double>(AUXKEY_CGD_PREV_GRADIENT);
        std::span<double> const prev_direction = cg_scratch.metaRecords<double>(AUXKEY_CGD_PREV_DIRECTION);
        auto &cg_valid = cg_scratch.metaScalar<std::uint8_t>(AUXKEY_CGD_HISTORY_VALID);

        // 1) Normalised finite-difference (proxy) gradient of this starting point.
        const std::vector<double> gradient = this->computeProxyGradient_(i, parent_fitness);

        // 2) Build the search direction: the L-BFGS quasi-Newton direction, or the plain
        //    steepest-descent / conjugate-gradient-family direction.
        const bool history_valid = (cg_valid != 0);
        std::vector<double> direction =
            (gradient_method_ == gradientMethod::LBFGS)
                ? this->lbfgsDirection_(cg_scratch, parm_vec, gradient, history_valid, periodic_restart)
                : this->conjugateDirection_(
                      gradient, prev_gradient, prev_direction, history_valid, periodic_restart
                  );

        // 3) The directional derivative grad f . d must be negative for a descent direction. A stale
        //    conjugate direction occasionally fails this; fall back to steepest descent so the line
        //    search has an acceptable direction.
        double g_dot_d = 0.;
        for(std::size_t j = 0; j < n_fp_parms_first_; j++) {
            g_dot_d += gradient[j] * direction[j];
        }
        if(not(g_dot_d < 0.)) {
            g_dot_d = 0.;
            for(std::size_t j = 0; j < n_fp_parms_first_; j++) {
                direction[j] = -gradient[j];
                g_dot_d += gradient[j] * direction[j]; // == -||g||^2 <= 0
            }
        }

        // 4) Line search along the direction. The probes are evaluated through the same consumer the
        //    algorithm uses (so they run on the GPU when the GPU consumer is active).
        GLineSearchOptions opts;
        opts.alpha_init = (mean_h > 0.) ? step_ratio * mean_h : step_ratio;
        const std::size_t starting_point = i;
        const GLineSearchResult lr = line_search.search(
            [this, starting_point](std::vector<std::vector<double>> const &points) {
                return this->evaluateProbes(starting_point, points);
            },
            parm_vec,
            direction,
            parent_fitness,
            g_dot_d,
            opts
        );

        // 5) Apply the accepted step. A starting point for which no step satisfied Armijo is left in
        //    place: it has effectively converged (zero gradient) or sits where the current direction
        //    cannot improve it. It still re-joins the next population evaluation (its position is
        //    unchanged, so the recomputed fitness is identical), so mark it due either way -- the
        //    per-iteration evaluation contract requires every individual to be due for processing.
        if(lr.success) {
            this->at(i)->assignFPValueVectorInternal(lr.x_new, activityMode::ACTIVEONLY);
        }
        else {
            this->at(i)->mark_as_due_for_processing();
        }

        // 6) Remember gradient/direction for the next conjugate step (on the slot's scratch).
        std::copy(gradient.begin(), gradient.end(), prev_gradient.begin());
        std::copy(direction.begin(), direction.end(), prev_direction.begin());
        cg_valid = 1;
    }
}

/******************************************************************************/
/**
 * @brief Installs any missing conjugate-gradient / L-BFGS scratch blocks on a central slot's store.
 *
 * Robustness: a central slot spliced in after a lost return has no CG block -- install it (history
 * invalid) so it restarts cleanly with steepest descent. In L-BFGS mode each starting point also
 * keeps the last m (s, y) curvature pairs, the previous parameter vector and a live pair count,
 * installed just as lazily so a spliced-in slot picks up a clean (empty) history.
 *
 * @param cg_scratch The central slot's scratch store the blocks are installed on
 */
void GConjugateGradientDescent::ensureGradientScratch_(gen::GAuxiliaryStore &cg_scratch) const {
    if(not cg_scratch.hasAux(AUXKEY_CGD_HISTORY_VALID)) {
        cg_scratch.installAuxBlock<double>(AUXKEY_CGD_PREV_GRADIENT, n_fp_parms_first_, gen::AuxScope::PerIndividual);
        cg_scratch.installAuxBlock<double>(AUXKEY_CGD_PREV_DIRECTION, n_fp_parms_first_, gen::AuxScope::PerIndividual);
        cg_scratch.installAuxBlock<std::uint8_t>(AUXKEY_CGD_HISTORY_VALID, 1, gen::AuxScope::PerIndividual);
    }
    if(gradient_method_ == gradientMethod::LBFGS && not cg_scratch.hasAux(AUXKEY_CGD_LBFGS_COUNT)) {
        cg_scratch.installAuxBlock<double>(AUXKEY_CGD_LBFGS_S, lbfgs_memory_ * n_fp_parms_first_, gen::AuxScope::PerIndividual);
        cg_scratch.installAuxBlock<double>(AUXKEY_CGD_LBFGS_Y, lbfgs_memory_ * n_fp_parms_first_, gen::AuxScope::PerIndividual);
        cg_scratch.installAuxBlock<double>(AUXKEY_CGD_LBFGS_PREV_X, n_fp_parms_first_, gen::AuxScope::PerIndividual);
        cg_scratch.installAuxBlock<std::uint32_t>(AUXKEY_CGD_LBFGS_COUNT, 1, gen::AuxScope::PerIndividual);
    }
}

/******************************************************************************/
/**
 * @brief Computes the normalised finite-difference (proxy) gradient of one starting point.
 *
 * FORWARD: g_j = (f(x+h_j e_j) - f(x)) / h_j (O(h), one probe). CENTRAL:
 * g_j = (f(x+h_j e_j) - f(x-h_j e_j)) / (2 h_j) (O(h^2), two probes). Both are proper gradients
 * (normalised by the step), so the line search's Armijo test and the conjugate-gradient beta are
 * correctly scaled either way.
 *
 * @param starting_point The index of the starting point whose gradient is computed
 * @param parent_fitness The (minimization-oriented) fitness of that starting point
 * @return The proxy gradient, one component per floating-point parameter
 */
//! [algo-gd-proxy-gradient]
std::vector<double> GConjugateGradientDescent::computeProxyGradient_(
    std::size_t starting_point,
    double parent_fitness
) const {
    const std::size_t n_probes = central_differences_ ? 2 : 1;
    const std::size_t children_per_sp = n_fp_parms_first_ * n_probes;
    std::vector<double> gradient(n_fp_parms_first_, 0.);
    const double h = adjusted_finite_step_; // uniform across parameters (normalized coordinate)
    for(std::size_t j = 0; j < n_fp_parms_first_; j++) {
        if(h <= 0.) {
            continue;
        }
        const std::size_t fwd = n_starting_points_ + (starting_point * children_per_sp) + (j * n_probes);
        const double f_fwd = minOnly_transformed_fitness((*this->at(fwd)));
        if(central_differences_) {
            const double f_bwd = minOnly_transformed_fitness((*this->at(fwd + 1)));
            gradient[j] = (f_fwd - f_bwd) / (2. * h);
        }
        else {
            gradient[j] = (f_fwd - parent_fitness) / h;
        }
    }
    return gradient;
}
//! [algo-gd-proxy-gradient]

/******************************************************************************/
/**
 * @brief Builds the L-BFGS quasi-Newton direction -H_k g_k, updating the curvature history.
 *
 * L-BFGS (Nocedal & Wright, "Numerical Optimization", 2nd ed., Algorithm 7.4/7.5). The last
 * m (s, y) pairs implicitly define an inverse-Hessian approximation H_k; the direction is
 * -H_k g_k, recovered by the two-loop recursion without ever forming H_k.
 *
 * @param cg_scratch The central slot's scratch store holding the (s, y) history
 * @param parm_vec The starting point's current parameter vector
 * @param gradient The current proxy gradient
 * @param history_valid Whether the stored previous gradient/parameters are valid
 * @param periodic_restart Whether this iteration performs the classical periodic restart
 * @return The search direction
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel implementing the textbook L-BFGS two-loop recursion (Nocedal & Wright, Algorithm 7.4/7.5); the curvature update and the two loops operate on the same ring-buffer history and splitting them would scatter one numeric algorithm
std::vector<double> GConjugateGradientDescent::lbfgsDirection_(
    gen::GAuxiliaryStore &cg_scratch,
    const std::vector<double> &parm_vec,
    const std::vector<double> &gradient,
    bool history_valid,
    bool periodic_restart
) const {
    std::span<double> const s_hist = cg_scratch.metaRecords<double>(AUXKEY_CGD_LBFGS_S);
    std::span<double> const y_hist = cg_scratch.metaRecords<double>(AUXKEY_CGD_LBFGS_Y);
    std::span<double> const prev_x = cg_scratch.metaRecords<double>(AUXKEY_CGD_LBFGS_PREV_X);
    std::span<double> const prev_gradient = cg_scratch.metaRecords<double>(AUXKEY_CGD_PREV_GRADIENT);
    auto &count = cg_scratch.metaScalar<std::uint32_t>(AUXKEY_CGD_LBFGS_COUNT);
    const std::size_t n = n_fp_parms_first_;

    // 2a) Curvature update from the previous step (skipped on the first iteration / after a
    //     restart, where there is no valid previous gradient or x). s = x_k - x_{k-1},
    //     y = g_k - g_{k-1}; accept the pair only when s.y > 0 (the curvature condition that keeps
    //     H_k positive definite), otherwise drop it. Newest pair is stored at slot count-1.
    if(history_valid && not periodic_restart) {
        long double s_dot_y = 0.L;
        for(std::size_t j = 0; j < n; j++) {
            s_dot_y += static_cast<long double>(parm_vec[j] - prev_x[j]) *
                       static_cast<long double>(gradient[j] - prev_gradient[j]);
        }
        if(s_dot_y > std::numeric_limits<long double>::min()) {
            // The curvature history is a fixed-capacity ring laid out in a flat GAuxiliaryStore
            // block, kept bespoke: no common/ facility fits (GSPSCStagingRingT is a lock-free RNG
            // staging ring, unrelated), and the two-loop recursion below wants the pairs in
            // oldest-to-newest order in contiguous memory.
            // Eviction is therefore a shift rather than a modular head index: O(m*n) instead of
            // O(1) per accepted pair. Deliberate -- m is the L-BFGS memory (single digits) and the
            // shift happens at most once per iteration, against a two-loop recursion that is
            // already O(m*n), so a head index would buy nothing and would spread modular
            // arithmetic across both loops and the gamma scaling.
            if(count == lbfgs_memory_) { // ring is full: drop the oldest pair (shift down by one)
                std::copy(s_hist.begin() + n, s_hist.end(), s_hist.begin());
                std::copy(y_hist.begin() + n, y_hist.end(), y_hist.begin());
                count = static_cast<std::uint32_t>(lbfgs_memory_ - 1);
            }
            const std::size_t slot = count * n;
            for(std::size_t j = 0; j < n; j++) {
                s_hist[slot + j] = parm_vec[j] - prev_x[j];
                y_hist[slot + j] = gradient[j] - prev_gradient[j];
            }
            count++;
        }
    }
    else {
        count = 0; // a (re)start clears the curvature history
    }

    // 2b) Two-loop recursion. With no pairs yet this yields plain steepest descent.
    std::vector<double> q(gradient.begin(), gradient.end());
    std::vector<double> rho(count, 0.);
    std::vector<double> alpha(count, 0.);
    for(std::size_t k = count; k-- > 0;) {
        const std::size_t slot = k * n;
        long double y_dot_s = 0.L, s_dot_q = 0.L;
        for(std::size_t j = 0; j < n; j++) {
            y_dot_s += static_cast<long double>(y_hist[slot + j]) * s_hist[slot + j];
            s_dot_q += static_cast<long double>(s_hist[slot + j]) * q[j];
        }
        rho[k] = (y_dot_s > std::numeric_limits<long double>::min()) ? Gem::Common::narrow<double>(1.L / y_dot_s) : 0.;
        alpha[k] = Gem::Common::narrow<double>(rho[k] * s_dot_q);
        for(std::size_t j = 0; j < n; j++) {
            q[j] -= alpha[k] * y_hist[slot + j];
        }
    }
    // Initial inverse-Hessian scaling gamma = (s_newest . y_newest) / (y_newest . y_newest);
    // 1 when no pair is available, which makes the direction plain steepest descent.
    double gamma = 1.;
    if(count > 0) {
        const std::size_t slot = (count - 1) * n;
        long double s_dot_y = 0.L, y_dot_y = 0.L;
        for(std::size_t j = 0; j < n; j++) {
            s_dot_y += static_cast<long double>(s_hist[slot + j]) * y_hist[slot + j];
            y_dot_y += static_cast<long double>(y_hist[slot + j]) * y_hist[slot + j];
        }
        if(y_dot_y > std::numeric_limits<long double>::min()) {
            gamma = Gem::Common::narrow<double>(s_dot_y / y_dot_y);
        }
    }
    std::vector<double> r(n, 0.);
    for(std::size_t j = 0; j < n; j++) {
        r[j] = gamma * q[j];
    }
    for(std::size_t k = 0; k < count; k++) {
        const std::size_t slot = k * n;
        long double y_dot_r = 0.L;
        for(std::size_t j = 0; j < n; j++) {
            y_dot_r += static_cast<long double>(y_hist[slot + j]) * r[j];
        }
        const auto beta_k = Gem::Common::narrow<double>(rho[k] * y_dot_r);
        for(std::size_t j = 0; j < n; j++) {
            r[j] += (alpha[k] - beta_k) * s_hist[slot + j];
        }
    }
    std::vector<double> direction(n, 0.);
    for(std::size_t j = 0; j < n; j++) {
        direction[j] = -r[j]; // -H_k g_k
    }
    // Remember x_k for the next curvature pair (prev_gradient is stored in step 6 as for CG).
    std::copy(parm_vec.begin(), parm_vec.end(), prev_x.begin());
    return direction;
}

/******************************************************************************/
/**
 * @brief Builds the steepest-descent / conjugate-gradient-family search direction.
 *
 * @param gradient The current proxy gradient
 * @param prev_gradient The previous iteration's gradient (scratch-backed)
 * @param prev_direction The previous iteration's search direction (scratch-backed)
 * @param history_valid Whether the previous gradient/direction are valid
 * @param periodic_restart Whether this iteration performs the classical periodic restart
 * @return The search direction
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel computing the conjugate-gradient beta (Powell restart guard, per-variant FR/HS/DY/PR+ formula switch, numerical-stability clamp); the dot-product accumulation, variant switch and stability guard are tightly coupled around one beta value
std::vector<double> GConjugateGradientDescent::conjugateDirection_(
    const std::vector<double> &gradient,
    std::span<const double> prev_gradient,
    std::span<const double> prev_direction,
    bool history_valid,
    bool periodic_restart
) const {
    double beta = 0.;
    const bool want_conjugate = (gradient_method_ != gradientMethod::STEEPEST_DESCENT) &&
                                history_valid && not periodic_restart;
    if(want_conjugate) {
        long double g_dot_g = 0.L;         // g_k . g_k
        long double g_dot_gprev = 0.L;     // g_k . g_{k-1}
        long double gprev_dot_gprev = 0.L; // g_{k-1} . g_{k-1}
        long double g_dot_y = 0.L;         // g_k . (g_k - g_{k-1})   (Polak-Ribiere / Hestenes-Stiefel num)
        long double d_dot_y = 0.L;         // d_{k-1} . (g_k - g_{k-1}) (Hestenes-Stiefel / Dai-Yuan den)
        for(std::size_t j = 0; j < n_fp_parms_first_; j++) {
            const long double g = gradient[j];
            const long double gp = prev_gradient[j];
            const long double y = g - gp;
            g_dot_g += g * g;
            g_dot_gprev += g * gp;
            gprev_dot_gprev += gp * gp;
            g_dot_y += g * y;
            d_dot_y += static_cast<long double>(prev_direction[j]) * y;
        }

        // Powell restart (Powell, "Restart procedures for the conjugate gradient method", Math.
        // Prog. 12, 1977): restart to steepest descent when successive gradients are insufficiently
        // orthogonal, i.e. |g_k . g_{k-1}| / ||g_k||^2 >= 0.1.
        const long double abs_overlap = (g_dot_gprev >= 0.L) ? g_dot_gprev : -g_dot_gprev;
        const bool powell_restart = (g_dot_g > 0.L) && (abs_overlap / g_dot_g >= 0.1L);

        // Per-formula numerator / denominator (the conjugate-gradient family differs only here):
        //   PR+ : g.(g-g_prev) / g_prev.g_prev    FR : g.g / g_prev.g_prev
        //   HS+ : g.(g-g_prev) / d_prev.(g-g_prev) DY : g.g / d_prev.(g-g_prev)
        long double num = 0.L;
        long double den = 0.L;
        switch(gradient_method_) {
        case gradientMethod::CONJUGATE_FR:
            num = g_dot_g;
            den = gprev_dot_gprev;
            break;
        case gradientMethod::CONJUGATE_HS:
            num = g_dot_y;
            den = d_dot_y;
            break;
        case gradientMethod::CONJUGATE_DY:
            num = g_dot_g;
            den = d_dot_y;
            break;
        case gradientMethod::CONJUGATE_PR_PLUS:
        default:
            num = g_dot_y;
            den = gprev_dot_gprev;
            break;
        }

        // Numerical-stability guard: the denominator vanishes near convergence (and d.(g-g_prev) can
        // change sign), so divide only when |den| is above the representable floor and large enough
        // relative to the numerator that |beta| stays below a finite cap. The "+" clamp (beta >= 0,
        // i.e. an automatic restart when beta would be negative) keeps every variant globally
        // convergent and matches PR+/HS+.
        constexpr long double beta_max = 1.0e4L;
        const long double abs_num = (num >= 0.L) ? num : -num;
        const long double abs_den = (den >= 0.L) ? den : -den;
        if(not powell_restart && abs_den > std::numeric_limits<long double>::min() &&
           abs_den * beta_max > abs_num) {
            const long double beta_cg = num / den;
            beta = (beta_cg > 0.L) ? Gem::Common::narrow<double>(beta_cg) : 0.;
        }
    }

    std::vector<double> direction(n_fp_parms_first_, 0.);
    for(std::size_t j = 0; j < n_fp_parms_first_; j++) {
        direction[j] =
            -gradient[j] + (history_valid ? beta * prev_direction[j] : 0.);
    }
    return direction;
}

/******************************************************************************/
/**
 * @brief Evaluates a batch of trial parameter vectors (the line-search probes) by cloning the given starting
 * point, assigning each probe's floating point values, and submitting the lot through the same
 * span+policy consumer path the main algorithm uses (this->workOn). Because submission goes through the
 * process consumer, the probes are evaluated on whatever consumer is active -- serial, multi-threaded, GPU
 * or networked -- so the line search is fully decoupled from where evaluation happens.
 *
 * @param starting_point The population position of the starting-point individual to clone for each probe
 * @param points The trial parameter vectors to evaluate (each is one set of active floating point values)
 * @return One min-only transformed fitness per probe, in the same order as points
 */
std::vector<double> GConjugateGradientDescent::evaluateProbes(
    std::size_t starting_point,
    std::vector<std::vector<double>> const &points
) {
    std::vector<std::unique_ptr<gen::GGenome>> probes;
    probes.reserve(points.size());
    for(auto const &pt : points) {
        auto probe = this->at(starting_point)->clone();
        probe->assignFPValueVectorInternal(pt, activityMode::ACTIVEONLY);
        probes.push_back(std::move(probe));
    }

    this->workOn(probes, 0, probes.size());

    std::vector<double> values;
    values.reserve(probes.size());
    for(auto const &probe : probes) {
        values.push_back(minOnly_transformed_fitness(*probe));
    }
    return values;
}

/******************************************************************************/
/**
 * @brief Adds local configuration options to a GParserBuilder object
 *
 * @param gpb The GParserBuilder object to which the configuration options are added
 */
void GConjugateGradientDescent::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our parent class'es function
    GOptimizationAlgorithmBase::addConfigurationOptions_(gpb);

    gpb.registerFileParameter<std::size_t>(
        "n_starting_points",
        DEFAULTCGDSTARTINGPOINTS,
        [this](std::size_t nsp) { this->setNStartingPoints(nsp); }
    ) << "The number of simultaneous conjugate gradient descents";

    gpb.registerFileParameter<double>(
        "finite_step",
        DEFAULTCGDFINITESTEP,
        [this](double fs) { this->setFiniteStep(fs); }
    ) << "The size of the adjustment in the difference quotient,"
      << '\n'
      << "specified in per mill of the allowed or expected value" << '\n'
      << "range of a parameter";

    gpb.registerFileParameter<double>(
        "step_size",
        DEFAULTCGDSTEPSIZE,
        [this](double ss) { this->setStepSize(ss); }
    ) << "The size of the INITIAL trial step along the search direction"
      << '\n'
      << "(in per mill of the value range); the line search then refines it";

    gpb.registerFileParameter<int>(
        "gradient_method",
        static_cast<int>(gradientMethod::CONJUGATE_PR_PLUS),
        [this](int gm) { this->setGradientMethod(static_cast<gradientMethod>(gm)); }
    ) << "The search-direction rule: 0 = Polak-Ribiere+ conjugate gradient (the default)," << '\n'
      << "1 = plain steepest descent (the former \"gd\"), 2 = Fletcher-Reeves," << '\n'
      << "3 = Hestenes-Stiefel+, 4 = Dai-Yuan, 5 = L-BFGS (limited-memory quasi-Newton)";

    gpb.registerFileParameter<std::size_t>(
        "lbfgs_memory",
        DEFAULTCGDLBFGSMEMORY,
        [this](std::size_t m) { this->setLBFGSMemory(m); }
    ) << "The L-BFGS history size m (number of (s, y) curvature pairs kept per starting" << '\n'
      << "point); only used when gradient_method = 5. Larger m approximates the inverse" << '\n'
      << "Hessian better at the cost of m*n storage and O(m*n) work per iteration";

    gpb.registerFileParameter<bool>(
        "central_differences",
        false,
        [this](bool cd) { this->setCentralDifferences(cd); }
    ) << "Use the O(h^2) central-difference gradient (two probe evaluations per direction)" << '\n'
      << "instead of the default O(h) forward difference (one probe). More accurate," << '\n'
      << "but doubles the per-iteration evaluation cost";

    gpb.registerFileParameter<int>(
        "error_estimation",
        static_cast<int>(errorEstimationMode::NONE),
        [this](int em) { this->setErrorEstimation(static_cast<errorEstimationMode>(em)); }
    ) << "MINUIT-style parameter-error estimate at convergence:" << '\n'
      << "0 = none (default), 1 = diagonal (parabolic) errors," << '\n'
      << "2 = full Hessian -> covariance (small dimension only)," << '\n'
      << "3 = MINOS asymmetric (profiled) errors (small dimension only)";

    gpb.registerFileParameter<double>(
        "error_definition",
        1.,
        [this](double up) { this->setErrorDefinition(up); }
    ) << "The MINUIT error definition UP: the objective increase" << '\n'
      << "defining one standard deviation (1 = chi^2, 0.5 = -logL)";
}

/******************************************************************************/
/**
 * @brief Triggers fitness calculation of all individuals through the process consumer.
 *
 * Throws a geneva_exception if no complete set of evaluated items is received or if any individual reports errors.
 */
void GConjugateGradientDescent::evaluatePopulation_() {
    using namespace Gem::Courtier;

#ifdef DEBUG
    for(auto const &[pos, item_ptr] : *this | std::views::enumerate) {
        if(this->afterFirstIteration() && !item_ptr->is_due_for_processing()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GConjugateGradientDescent::evaluatePopulation_():" << '\n'
                << "Found individual on position " << pos
                << " which is not due for processing" << '\n'
            );
        }
    }
#endif /* DEBUG */

    auto status = this->workOnPopulation(0, this->data_cnt_.size());

    // A conjugate-gradient method needs a complete set of evaluated solutions.
    this->requireCompleteEvaluation_(status, "GConjugateGradientDescent::evaluatePopulation_()");
}

/******************************************************************************/
/**
 * @brief Does some preparatory work before the optimization starts
 *
 * Extracts the active-parameter boundaries, recomputes the per-parameter finite step, and (re-)initialises
 * the conjugate-gradient state and per-individual population positions.
 */
void GConjugateGradientDescent::init() {
    // To be performed before any other action
    GOptimizationAlgorithmBase::init();

#ifdef DEBUG
    if(step_size_ <= 0. || step_size_ > 1000.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GConjugateGradientDescent::init(): Error!" << '\n'
            << "Invalid value of step_size_: " << step_size_ << '\n'
            << "Must be in the range ]0.:1000.]" << '\n'
        );
    }

    if(finite_step_ <= 0. || finite_step_ > 1000.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GConjugateGradientDescent::init(): Error!" << '\n'
            << "Invalid value of finite_step_: " << finite_step_ << '\n'
            << "Must be in the range ]0.:1000.]" << '\n'
        );
    }
#endif /* DEBUG */

    updateDerivedQuantities();
    resetCGState();
    markIndividualPositions();
}

/******************************************************************************/
/**
 * @brief Recomputes the dimensionless difference-quotient step from finite_step_. Parameters live in the
 * normalized internal coordinate (interval width 1), so the step is simply finite_step_/1000 (a fraction
 * of each parameter's range, applied uniformly) -- no per-parameter boundary scaling.
 */
void GConjugateGradientDescent::updateDerivedQuantities() {
    adjusted_finite_step_ = finite_step_ / 1000.;
}

/******************************************************************************/
/**
 * @brief (Re-)initialises the per-starting-point conjugate-gradient memory. Called
 * from init() once n_fp_parms_first_ is known (it is set in adjustPopulation_,
 * which runs before init()).
 */
void GConjugateGradientDescent::resetCGState() {
    // On a checkpoint resume the central slots already carry their restored conjugate-gradient memory --
    // preserve it (skip the zero-reset) so a resumed run continues the conjugate sequence.
    if(this->resumedFromCheckpoint()) {
        return;
    }

    // The per-starting-point conjugate-gradient memory lives on the OA scratch of each starting point's
    // central individual slot (position == starting point). Install a zeroed g_{k-1} / d_{k-1} double
    // block and a "history invalid" flag on each, so the first cgStep falls back to steepest descent.
    for(std::size_t i = 0; i < n_starting_points_ && i < this->size(); ++i) {
        auto &scratch = this->at(i)->scratch();
        scratch.installAuxBlock<double>(AUXKEY_CGD_PREV_GRADIENT, n_fp_parms_first_, gen::AuxScope::PerIndividual);
        scratch.installAuxBlock<double>(AUXKEY_CGD_PREV_DIRECTION, n_fp_parms_first_, gen::AuxScope::PerIndividual);
        scratch.installAuxBlock<std::uint8_t>(AUXKEY_CGD_HISTORY_VALID, 1, gen::AuxScope::PerIndividual);
        // installAuxBlock zero-initialises, so the gradient/direction are 0 and the flag is false.
        if(gradient_method_ == gradientMethod::LBFGS) {
            // The L-BFGS curvature history (last m (s, y) pairs, previous x, live pair count) starts empty.
            scratch.installAuxBlock<double>(AUXKEY_CGD_LBFGS_S, lbfgs_memory_ * n_fp_parms_first_, gen::AuxScope::PerIndividual);
            scratch.installAuxBlock<double>(AUXKEY_CGD_LBFGS_Y, lbfgs_memory_ * n_fp_parms_first_, gen::AuxScope::PerIndividual);
            scratch.installAuxBlock<double>(AUXKEY_CGD_LBFGS_PREV_X, n_fp_parms_first_, gen::AuxScope::PerIndividual);
            scratch.installAuxBlock<std::uint32_t>(AUXKEY_CGD_LBFGS_COUNT, 1, gen::AuxScope::PerIndividual);
        }
    }
}

/******************************************************************************/
/**
 * @brief Does any necessary finalization work
 *
 * When error estimation is enabled, computes an optional MINUIT-style parameter-error estimate at the
 * best (converged) starting point and logs it, then delegates to the base class.
 */
void GConjugateGradientDescent::finalize() {
    // Optional MINUIT-style parameter-error estimate at the converged minimum (opt-in). The curvature
    // probes are evaluated through the same consumer the algorithm uses, exactly like the line search.
    if(error_estimation_ != errorEstimationMode::NONE && n_fp_parms_first_ > 0 && not this->empty()) {
        // Pick the best starting point (the lowest min-only fitness).
        std::size_t best = 0;
        double best_fitness = minOnly_transformed_fitness((*this->at(0)));
        for(std::size_t i = 1; i < n_starting_points_ && i < this->size(); ++i) {
            const double f = minOnly_transformed_fitness((*this->at(i)));
            if(f < best_fitness) {
                best_fitness = f;
                best = i;
            }
        }

        std::vector<double> x_min;
        this->at(best)->streamlineFPInternal(x_min, activityMode::ACTIVEONLY);

        GHesseErrorOptions opts;
        opts.up = error_up_;
        // FULL and MINOS both build the covariance (MINOS seeds its profile brackets from the
        // correlation-aware sigma); MINOS additionally computes the asymmetric profiled bounds.
        opts.full_covariance = (error_estimation_ == errorEstimationMode::FULL ||
                                error_estimation_ == errorEstimationMode::MINOS);
        opts.minos = (error_estimation_ == errorEstimationMode::MINOS);

        const std::size_t best_point = best;
        GHesseError const estimator;
        // The difference-quotient step is uniform across parameters now; the Hesse estimator still takes a
        // per-parameter step vector, so expand the scalar to x_min's dimension.
        const std::vector<double> hesse_step(x_min.size(), adjusted_finite_step_);
        last_error_estimate_ = estimator.estimate(
            [this, best_point](std::vector<std::vector<double>> const &points) {
                return this->evaluateProbes(best_point, points);
            },
            x_min,
            best_fitness,
            hesse_step,
            opts
        );

        if(last_error_estimate_.valid) {
            std::ostringstream oss;
            oss << "GConjugateGradientDescent: parameter-error estimate at the minimum (UP = "
                << error_up_ << ", "
                << (last_error_estimate_.covariance_valid ? "profiled" : "parameter-fixed") << "):\n";
            const std::size_t shown =
                std::min<std::size_t>(last_error_estimate_.parameter_errors.size(), 20u);
            for(std::size_t j = 0; j < shown; ++j) {
                oss << "  parameter[" << j << "] +/- " << last_error_estimate_.parameter_errors[j]
                    << '\n';
            }
            if(last_error_estimate_.parameter_errors.size() > shown) {
                oss << "  ... (" << (last_error_estimate_.parameter_errors.size() - shown)
                    << " more parameters)\n";
            }
            oss << "  curvature condition number = " << last_error_estimate_.condition_number
                << " (a large value flags a flat / ill-conditioned minimum)";
            glogger << oss.str() << '\n' << GLOGGING;
        }
        else {
            glogger << "GConjugateGradientDescent: no usable error estimate "
                       "(no positive curvature at the stopping point)."
                    << '\n'
                    << GLOGGING;
        }
    }

    GOptimizationAlgorithmBase::finalize();
}

/******************************************************************************/
/**
 * @brief Retrieve a GPersonalityTraits object belonging to this algorithm
 *
 * @return A new GConjugateGradientDescent_PersonalityTraits object, camouflaged as a GPersonalityTraits
 */
std::shared_ptr<GPersonalityTraits> GConjugateGradientDescent::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/
/**
 * @brief Resizes the population to the desired level and does some error checks. The population is
 * n_starting_points_ * (n_fp_parms_first_ * n_probes + 1) individuals, where n_probes is 1 (forward
 * difference) or 2 (central difference): each starting point plus its perturbed difference-quotient
 * children.
 */
void GConjugateGradientDescent::adjustPopulation_() {
    // Non-empty population + at least one active FP parameter; logs a note for int/bool riders
    // (the shared precondition of the floating-point-only algorithms).
    n_fp_parms_first_ = this->requireFloatingPointGenome_("GConjugateGradientDescent");
    const std::size_t n_start = this->size();

#ifdef DEBUG
    for(std::size_t i = 1; i < this->size(); i++) {
        if(this->at(i)->countFPParameters(activityMode::ACTIVEONLY) != n_fp_parms_first_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GConjugateGradientDescent::adjustPopulation():" << '\n'
                << "Found individual in position " << i << " with different" << '\n'
                << "number of floating point parameters than the first one: "
                << this->at(i)->countFPParameters(activityMode::ACTIVEONLY) << "/"
                << n_fp_parms_first_ << '\n'
            );
        }
    }
#endif

    // Each starting point needs one perturbed child per probe per direction: 1 (forward difference) or
    // 2 (central difference, forward + backward) probes. Population = starting points + their children.
    const std::size_t children_per_sp = n_fp_parms_first_ * (central_differences_ ? 2 : 1);
    GOptimizationAlgorithmBase::setDefaultPopulationSize(
        n_starting_points_ * (children_per_sp + 1)
    );

    // Create the requested number of (randomized) starting points
    if(n_start < n_starting_points_) {
        for(std::size_t i = 0; i < (n_starting_points_ - n_start); i++) {
            this->push_back(this->at(0)->clone());
            this->back()->randomInit(activityMode::ACTIVEONLY);
        }
    }
    else {
        this->resize(n_starting_points_);
    }

    // Add the difference-quotient children for every starting point
    for(std::size_t i = 0; i < n_starting_points_; i++) {
        for(std::size_t k = 0; k < children_per_sp; k++) {
            this->push_back(this->at(i)->clone());
        }
    }

#ifdef DEBUG
    if(this->size() != n_starting_points_ * (children_per_sp + 1)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GConjugateGradientDescent::adjustPopulation():" << '\n'
            << "Population size is " << this->size() << '\n'
            << "but expected " << n_starting_points_ * (children_per_sp + 1) << '\n'
        );
    }
#endif /* DEBUG */
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
