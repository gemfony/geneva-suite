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
#include <functional>
#include <vector>

// Boost headers go here

// Geneva headers go here

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * Acceptance and termination options for the backtracking line search. The defaults follow the usual
 * textbook recommendations (Nocedal & Wright, "Numerical Optimization" 2e, ch. 3).
 */
//! [algo-gd-linesearch-options]
struct GLineSearchOptions {
    /** @brief Armijo sufficient-decrease constant c1, with 0 < c1 < 1 (typically 1e-4). */
    double c1 = 1.e-4;
    /** @brief The initial (largest) trial step length. */
    double alpha_init = 1.;
    /** @brief The step-reduction factor applied on each backtrack, with 0 < backtrack < 1. */
    double backtrack = 0.5;
    /** @brief k: the number of trial step lengths evaluated together in one batch (>= 1). */
    std::size_t probes_per_round = 4;
    /** @brief The total trial-evaluation budget before the search gives up. */
    std::size_t max_probes = 40;
    /** @brief The smallest step length still worth trying. */
    double alpha_min = 1.e-12;
};
//! [algo-gd-linesearch-options]

/******************************************************************************/
/**
 * The outcome of a line search along a direction.
 */
struct GLineSearchResult {
    /** @brief The accepted step length (0 if none was accepted). */
    double alpha = 0.;
    /** @brief x0 + alpha * dir (equal to x0 on failure). */
    std::vector<double> x_new;
    /** @brief The objective value at x_new. */
    double f_new = 0.;
    /** @brief The number of objective evaluations consumed. */
    std::size_t n_evaluations = 0;
    /** @brief Whether an Armijo-acceptable step was found within budget. */
    bool success = false;
};

/******************************************************************************/
/**
 * A reusable, derivative-free-acceptance backtracking line search for descent methods. Given a base
 * point x0, a search direction dir and the directional derivative
 *
 *     g0_dot_dir = grad f(x0) . dir   (which MUST be negative for a descent direction),
 *
 * it finds a step length alpha that sufficiently decreases phi(alpha) = f(x0 + alpha*dir) under the
 * Armijo sufficient-decrease condition (Armijo, "Minimization of functions having Lipschitz continuous
 * first partial derivatives", Pacific J. Math. 16, 1966):
 *
 *     f(x0 + alpha*dir) <= f(x0) + c1 * alpha * g0_dot_dir.
 *
 * Geneva submits a whole batch of parameter sets per courtier round, so the search probes
 * `probes_per_round` trial step lengths at once -- one round per batch -- and accepts the LARGEST trial
 * step in the batch that satisfies Armijo. This is an ordinary backtracking line search, batched to fit
 * the bulk-submission model. The objective is supplied as eval_fn(points) -> values, evaluating a batch
 * of points in a single call, so it can be wired straight to the optimization algorithm's
 * process consumer, so it runs on whatever consumer the algorithm uses (serial, multi-threaded, GPU
 * or networked) -- the line search itself is consumer-agnostic.
 *
 * The class is a transient numeric helper: it holds no optimization state and is NOT a registered
 * optimization algorithm. It is intended for reuse by any descent-style algorithm (conjugate gradient
 * descent today; others later). The mathematics is standard, published and unencumbered, and the
 * implementation is original to Geneva.
 */
class GLineSearch {
public:
    /** @brief Evaluates a batch of points and returns one (min-only) objective value per point. */
    using eval_fn_t =
        std::function<std::vector<double>(const std::vector<std::vector<double>> &)>;

    /***************************************************************************/
    /**
     * @brief Performs the backtracking line search along @p dir starting at @p x0.
     *
     * @param eval_fn The batch objective: maps a batch of points to one (min-only) value per point.
     * @param x0 The base point f(x0) is evaluated at.
     * @param dir The search direction (must be a descent direction, i.e. @p g0_dot_dir < 0).
     * @param f0 The objective value at the base point, f(x0).
     * @param g0_dot_dir The directional derivative grad f(x0) . dir (must be negative for descent).
     * @param opts Acceptance/termination options for the search (defaults to GLineSearchOptions{}).
     * @return The chosen step, the new point and its value, the number of evaluations, and whether the
     *         Armijo condition was met. On failure (non-descent direction or budget exhausted) the
     *         result carries x0 / f0 with success == false, so the caller can restart to steepest
     *         descent or stop.
     */
    static GLineSearchResult search(
        eval_fn_t const &eval_fn,
        std::vector<double> const &x0,
        std::vector<double> const &dir,
        double f0,
        double g0_dot_dir,
        GLineSearchOptions const &opts = GLineSearchOptions{}
    );
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
