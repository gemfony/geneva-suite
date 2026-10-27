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
 * Options for the convergence error estimate.
 */
//! [hesse-options]
struct GHesseErrorOptions {
    /** @brief The error definition UP: the increase of the objective that defines one standard
     *  deviation. 1.0 for a chi^2-like objective, 0.5 for a negative-log-likelihood (the MINUIT
     *  convention). */
    double up = 1.;
    /** @brief When true and the dimension is at most max_full_dim, the full Hessian is built and
     *  inverted to a covariance matrix, giving correlation-aware (profiled) parameter errors.
     *  Otherwise only the cheap diagonal (parameter-fixed) estimate is computed. */
    bool full_covariance = false;
    /** @brief The dimension above which a full Hessian is never attempted (it costs O(n^2)
     *  evaluations), regardless of full_covariance. */
    std::size_t max_full_dim = 64;
    /** @brief When true (and dimension <= max_full_dim), compute MINUIT-style MINOS asymmetric errors:
     *  per parameter, the points where the PROFILE of the objective (re-minimised over all other
     *  parameters) rises by UP above the minimum. Captures non-parabolic / correlated minima. Opt-in
     *  and expensive -- each bound costs repeated re-minimisations. */
    bool minos = false;
};
//! [hesse-options]

/******************************************************************************/
/**
 * The result of a convergence error estimate at a minimum.
 */
struct GHesseErrorResult {
    /** @brief Whether a usable estimate was produced (a positive-curvature minimum). */
    bool valid = false;
    /** @brief The error definition UP used. */
    double up = 1.;
    /** @brief Per-parameter one-sigma errors. With full_covariance these are the profiled errors
     *  sqrt(UP * 2 * (H^-1)_jj); otherwise the parameter-fixed parabolic errors sqrt(2 * UP / H_jj),
     *  which ignore correlations. An entry is 0 for a parameter with non-positive curvature. */
    std::vector<double> parameter_errors;
    /** @brief The covariance matrix V = 2 * UP * H^-1 (only filled when full_covariance succeeded). */
    std::vector<std::vector<double>> covariance;
    /** @brief Whether covariance/profiled errors were produced (vs. the diagonal estimate only). */
    bool covariance_valid = false;
    /** @brief A conditioning proxy: the ratio of the largest to the smallest diagonal Hessian entry.
     *  A large value flags an ill-conditioned (flat-direction) minimum. */
    double condition_number = 0.;
    /** @brief MINOS asymmetric errors. Only meaningful when minos_valid is true: minos_low[j] /
     *  minos_high[j] are then the POSITIVE distances from x_min[j] to where the profiled objective rises
     *  by UP on the low / high side (both approach the symmetric parameter_errors[j] for a parabolic,
     *  uncorrelated minimum). When minos_valid is false these are not a reliable confidence interval and
     *  should be ignored (an entry is left 0 for any side/parameter that could not be bracketed). */
    std::vector<double> minos_low;
    std::vector<double> minos_high;
    /** @brief Whether usable MINOS asymmetric errors were produced -- true only if EVERY parameter was
     *  attempted (positive curvature) AND every attempted bound bracketed the UP crossing on both sides.
     *  A single flat/unbracketable direction sets this false; the symmetric parameter_errors then remain
     *  the usable fallback. */
    bool minos_valid = false;
    /** @brief The number of objective evaluations consumed. */
    std::size_t n_evaluations = 0;
};

/******************************************************************************/
/**
 * A reusable, MINUIT-inspired convergence error estimator. Given an objective, a minimum x*, and a
 * per-parameter finite step, it estimates parameter errors from the curvature of the objective:
 *
 *   - the diagonal of the Hessian H_jj = (f(x + h_j e_j) - 2 f(x) + f(x - h_j e_j)) / h_j^2 gives the
 *     cheap, always-affordable parameter-fixed (parabolic) error sigma_j = sqrt(2 * UP / H_jj);
 *   - optionally, for small dimension, the full finite-difference Hessian is inverted to the covariance
 *     matrix V = 2 * UP * H^-1, whose diagonal gives correlation-aware (profiled) errors.
 *
 * UP is the error definition (the increase of the objective defining one standard deviation: 1 for a
 * chi^2-like objective, 0.5 for a negative log-likelihood), exposed exactly as in MINUIT.
 *
 * This is an original, clean-room implementation of the published HESSE *method* (James & Roos,
 * "Minuit -- a system for function minimization and analysis of the parameter errors and
 * correlations", Comp. Phys. Comm. 10, 1975); it does not use or copy any MINUIT/Minuit2 code. The
 * matrix inverse is a self-contained Gauss-Jordan elimination with partial pivoting.
 *
 * The objective is supplied as a batch eval_fn(points) -> values, so the curvature probes are
 * evaluated through whatever consumer the calling algorithm uses (the estimator is consumer-agnostic).
 */
//! [hesse-estimate#1]
class GHesseError {
public:
    /** @brief Evaluates a batch of points and returns one (min-only) objective value per point. */
    using eval_fn_t =
        std::function<std::vector<double>(const std::vector<std::vector<double>> &)>;
//! [hesse-estimate#1]

    /***************************************************************************/
    /**
     * @brief Estimates the parameter errors at the minimum @p x_min from the curvature of the objective.
     *
     * Computes the cheap diagonal (parameter-fixed) errors always, and the covariance/profiled errors when
     * the dimension is small enough (see GHesseErrorOptions). All curvature probes are evaluated through
     * @p eval_fn, so the estimate runs on whatever consumer the calling algorithm uses.
     *
     * @param eval_fn The batch objective: maps a set of probe points to one (min-only) objective value each.
     * @param x_min The location of the minimum at which the errors are estimated.
     * @param f_min The objective value at @p x_min (the centre term of the second differences).
     * @param step_sizes The per-parameter finite step for the curvature differences (typically the
     *  algorithm's difference-quotient step); one entry per dimension of @p x_min.
     * @param opts The error definition UP and the gate controlling whether the full covariance is computed.
     * @return The estimate (validity flags, per-parameter errors, and -- when computed -- the covariance).
     */
    //! [hesse-estimate#2]
    static GHesseErrorResult estimate(
        eval_fn_t const &eval_fn,
        std::vector<double> const &x_min,
        double f_min,
        std::vector<double> const &step_sizes,
        GHesseErrorOptions const &opts = GHesseErrorOptions{}
    );
    //! [hesse-estimate#2]

private:
    /** @brief estimate() phase 1 (always): the diagonal Hessian, parameter-fixed errors, curvature
     *  validity and condition number. Fills @p result and the diagonal curvatures @p hessian_diag. */
    static void computeDiagonalHessian(
        GHesseErrorResult &result,
        eval_fn_t const &eval_fn,
        std::vector<double> const &x_min,
        double f_min,
        std::vector<double> const &step_sizes,
        GHesseErrorOptions const &opts,
        std::vector<double> &hessian_diag
    );
    /** @brief estimate() phase 2 (opt-in, low dimension): the off-diagonal Hessian, its inverse ->
     *  covariance, and the profiled (correlation-aware) errors. */
    static void computeCovariance(
        GHesseErrorResult &result,
        eval_fn_t const &eval_fn,
        std::vector<double> const &x_min,
        std::vector<double> const &step_sizes,
        GHesseErrorOptions const &opts,
        std::vector<double> const &hessian_diag
    );
    /** @brief computeCovariance() step 1: the full symmetric Hessian (diagonal + evaluated off-diagonal). */
    [[nodiscard]] static std::vector<std::vector<double>> buildFullHessian(
        GHesseErrorResult &result,
        eval_fn_t const &eval_fn,
        std::vector<double> const &x_min,
        std::vector<double> const &step_sizes,
        std::vector<double> const &hessian_diag
    );
    /** @brief computeCovariance() step 2: covariance = 2*UP*H^-1 and the profiled errors from its diagonal. */
    static void fillCovarianceFromHessian(
        GHesseErrorResult &result,
        std::vector<std::vector<double>> const &hessian,
        GHesseErrorOptions const &opts
    );
    /** @brief estimate() phase 3 (opt-in, low dimension): the MINOS asymmetric profiled bounds. */
    static void computeMinos(
        GHesseErrorResult &result,
        eval_fn_t const &eval_fn,
        std::vector<double> const &x_min,
        double f_min,
        std::vector<double> const &step_sizes,
        GHesseErrorOptions const &opts
    );
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
