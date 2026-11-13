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

#include "geneva/oa/GLineSearch.hpp"

#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

#include <algorithm>

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
namespace {

/**
 * @brief Computes the trial point x0 + alpha*dir.
 *
 * @param x0 The base point (origin of the line search)
 * @param dir The search direction; must have the same dimension as x0
 * @param alpha The step length scaling the direction
 * @return A new parameter vector equal to x0 + alpha*dir
 */
std::vector<double>
stepPoint(std::vector<double> const &x0, std::vector<double> const &dir, double alpha) {
    std::vector<double> point(x0.size());
    for(std::size_t i = 0; i < x0.size(); ++i) {
        point[i] = x0[i] + (alpha * dir[i]);
    }
    return point;
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * @brief Performs a backtracking Armijo line search, batched to one courtier round per group of probes.
 *
 * The trial step lengths form a decreasing geometric sequence alpha, alpha*backtrack, alpha*backtrack^2, ...;
 * within a batch we accept the LARGEST step that satisfies the Armijo sufficient-decrease condition. If none in
 * a batch passes, we continue backtracking from just past the smallest step probed, until the budget or
 * the minimum step length is reached.
 *
 * @param eval_fn Callback that evaluates a batch of candidate points and returns one fitness per point,
 *                in the same order (the single courtier round per probe group)
 * @param x0 The current (base) point from which the search starts
 * @param dir The search direction along which trial steps are taken
 * @param f0 The objective value at x0 (used as the reference for the Armijo test)
 * @param g0_dot_dir The directional derivative at x0 along dir; must be strictly negative for a descent
 *                   direction, otherwise the search reports failure immediately
 * @param opts The line-search tuning options (initial/minimum step, backtracking factor, probe budget,
 *             probes per round, Armijo constant c1)
 * @return A GLineSearchResult holding the accepted step length, new point and fitness, the number of
 *         evaluations performed, and a success flag (false if no acceptable step was found)
 */
GLineSearchResult GLineSearch::search(
    eval_fn_t const &eval_fn,
    std::vector<double> const &x0,
    std::vector<double> const &dir,
    double f0,
    double g0_dot_dir,
    GLineSearchOptions const &opts
) {
    GLineSearchResult result;
    result.x_new = x0;
    result.f_new = f0;

    // A descent direction has a strictly negative directional derivative. If it is not negative (a bad
    // search direction, e.g. a stale conjugate direction), the Armijo test can never accept a positive
    // step, so we report failure at once and let the caller restart to steepest descent.
    if(not(g0_dot_dir < 0.)) {
        return result; // success == false
    }

    const std::size_t k = std::max<std::size_t>(1, opts.probes_per_round);
    double alpha = opts.alpha_init;
    std::size_t n_evals = 0;

    while(n_evals < opts.max_probes && alpha >= opts.alpha_min) {
        // Build a decreasing geometric batch of up to k trial step lengths.
        std::vector<double> alphas;
        std::vector<std::vector<double>> points;
        double a = alpha;
        for(std::size_t j = 0; j < k && a >= opts.alpha_min; ++j) {
            alphas.push_back(a);
            points.push_back(stepPoint(x0, dir, a));
            a *= opts.backtrack;
        }
        if(points.empty()) {
            break;
        }

        // ONE batched evaluation -- this is the single courtier round for this group of probes.
        std::vector<double> values = eval_fn(points);
        n_evals += points.size();

        if(values.size() != points.size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GLineSearch::search(): Error!" << '\n'
                << "The evaluation function returned " << values.size() << " values for "
                << points.size() << " points." << '\n'
            );
        }

        // Accept the largest step (alphas are in decreasing order) that satisfies Armijo.
        for(std::size_t j = 0; j < alphas.size(); ++j) {
            const double armijo_rhs = f0 + (opts.c1 * alphas[j] * g0_dot_dir);
            if(values[j] <= armijo_rhs) {
                result.alpha = alphas[j];
                result.x_new = points[j];
                result.f_new = values[j];
                result.n_evaluations = n_evals;
                result.success = true;
                return result;
            }
        }

        // Nothing in this batch passed: keep backtracking from just past the smallest step probed.
        alpha = a;
    }

    result.n_evaluations = n_evals;
    return result; // success == false
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
