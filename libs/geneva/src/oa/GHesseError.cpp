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

#include "geneva/oa/GHesseError.hpp"

#include "geneva/oa/GLineSearch.hpp" // reused as the 1D step of the MINOS inner re-minimiser

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
namespace {

/**
 * @brief Returns x with x[idx] += delta (a single-coordinate perturbation).
 *
 * @param x The base parameter vector (taken by value and modified in place)
 * @param idx The index of the coordinate to perturb
 * @param delta The signed amount added to the selected coordinate
 * @return A copy of x with the single coordinate idx shifted by delta
 */
std::vector<double> perturb1(std::vector<double> x, std::size_t idx, double delta) {
    x[idx] += delta;
    return x;
}

/**
 * @brief Returns x with x[i] += di and x[j] += dj (a two-coordinate perturbation).
 *
 * @param x The base parameter vector (taken by value and modified in place)
 * @param i The index of the first coordinate to perturb
 * @param di The signed amount added to coordinate i
 * @param j The index of the second coordinate to perturb
 * @param dj The signed amount added to coordinate j
 * @return A copy of x with coordinates i and j shifted by di and dj respectively
 */
std::vector<double> perturb2(std::vector<double> x, std::size_t i, double di, std::size_t j, double dj) {
    x[i] += di;
    x[j] += dj;
    return x;
}

/**
 * @brief The PROFILED objective g(x_j) for MINOS: the minimum of F over all parameters EXCEPT @p jfix
 * (held at @p xjval). Implemented as finite-difference steepest descent whose 1D step REUSES GLineSearch
 * (the same line minimiser the CGD algorithm uses). The search direction always has a zero @p jfix
 * component, so the fixed parameter never moves. Accumulates evaluations into @p n_evals.
 *
 * @param eval_fn The objective-evaluation callback (maps a batch of points to their fitness values)
 * @param x The starting parameter vector for the inner minimisation (taken by value)
 * @param jfix The index of the parameter that is held fixed during profiling
 * @param xjval The fixed value imposed on parameter jfix
 * @param step_sizes Per-parameter finite-difference step sizes (a non-positive entry falls back to 1.e-6)
 * @param n_evals Running counter of objective evaluations, incremented by this call
 * @return The minimum of F over the free parameters with parameter jfix pinned to xjval
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel: finite-difference steepest-descent inner minimisation reusing GLineSearch's 1D step; the gradient assembly and line-search call are one tightly coupled iteration
double profileMin(
    GHesseError::eval_fn_t const &eval_fn,
    std::vector<double> x,
    std::size_t jfix,
    double xjval,
    std::vector<double> const &step_sizes,
    std::size_t &n_evals
) {
    const std::size_t n = x.size();
    x[jfix] = xjval;
    double f = eval_fn({x})[0];
    ++n_evals;
    if(n <= 1) {
        return f; // single parameter: nothing to minimise over
    }

    const GLineSearch line_search;
    const GLineSearchOptions ls_opts;
    constexpr std::size_t max_inner = 100;
    for(std::size_t it = 0; it < max_inner; ++it) {
        // Central finite-difference gradient over the FREE parameters (the jfix component stays 0).
        std::vector<std::vector<double>> pts;
        std::vector<std::size_t> idx;
        pts.reserve(2 * (n - 1));
        for(std::size_t k = 0; k < n; ++k) {
            if(k == jfix) {
                continue;
            }
            const double h = (step_sizes[k] > 0.) ? step_sizes[k] : 1.e-6;
            pts.push_back(perturb1(x, k, h));
            pts.push_back(perturb1(x, k, -h));
            idx.push_back(k);
        }
        const std::vector<double> gv = eval_fn(pts);
        n_evals += pts.size();

        std::vector<double> dir(n, 0.);
        double gnorm2 = 0.;
        for(std::size_t a = 0; a < idx.size(); ++a) {
            const std::size_t k = idx[a];
            const double h = (step_sizes[k] > 0.) ? step_sizes[k] : 1.e-6;
            const double gk = (gv[2 * a] - gv[(2 * a) + 1]) / (2. * h);
            dir[k] = -gk;
            gnorm2 += gk * gk;
        }
        if(gnorm2 <= 1.e-18) {
            break; // already at the free minimum
        }
        const GLineSearchResult r = line_search.search(eval_fn, x, dir, f, -gnorm2, ls_opts);
        n_evals += r.n_evaluations;
        if(not r.success || r.f_new >= f) {
            break; // no further progress
        }
        x = r.x_new; // dir[jfix] == 0, so x[jfix] stays == xjval
        f = r.f_new;
    }
    return f;
}

/**
 * @brief Brackets and bisects the MINOS bound on one side: given @p g(x_j) = profiled_objective - target
 * (so g(x0) < 0 at the minimum), find the distance from @p x0 to where g crosses 0, expanding from an
 * initial @p sigma_step (signed; the symmetric HESSE error on that side).
 *
 * @tparam G The callable type taking a double parameter value and returning the shifted profiled objective
 * @param g The callable g(x_j) = profiled_objective(x_j) - target, negative at the minimum
 * @param x0 The parameter value at the minimum (where g is negative), the origin of the bound
 * @param sigma_step The signed initial step (symmetric HESSE error) defining the search direction
 * @return The positive magnitude of the distance from x0 to the zero crossing, or std::nullopt if the
 *  crossing could not be bracketed within the expansion budget (a flat / unbounded direction) -- callers
 *  must treat nullopt as "no reliable MINOS bound on this side" and must NOT mark the result valid.
 */
template <typename G>
std::optional<double> minosBound(G &&g, double x0, double sigma_step) {
    double a = x0; // g(a) < 0 (the minimum lies below target)
    double b = x0 + sigma_step;
    double gb = g(b);
    std::size_t expand = 0;
    while(gb < 0. && expand < 25) {
        b = x0 + ((b - x0) * 1.6);
        gb = g(b);
        ++expand;
    }
    if(gb < 0.) {
        // The profiled objective never rose by UP within the budget: the zero crossing could not be
        // bracketed (a flat / unbounded direction). Signal failure rather than returning a fabricated
        // best-effort distance that the caller would otherwise report as a valid confidence bound.
        return std::nullopt;
    }
    for(std::size_t it = 0; it < 50; ++it) {
        const double mid = 0.5 * (a + b);
        if(g(mid) > 0.) {
            b = mid;
        }
        else {
            a = mid;
        }
        if(std::abs(b - a) <= 1.e-9 * (std::abs(x0) + 1.e-9)) {
            break;
        }
    }
    return std::abs((0.5 * (a + b)) - x0);
}

/**
 * @brief Inverts a symmetric matrix by Gauss-Jordan elimination with partial pivoting. Original,
 * self-contained implementation -- no external linear-algebra code.
 *
 * @param m The square matrix to invert (n x n)
 * @param inv Output parameter receiving the inverse; left untouched if m is singular
 * @return true if the inverse was computed, false if m is singular / too ill-conditioned to invert
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel: Gauss-Jordan elimination with partial pivoting on the augmented [m | I] system; the pivot/normalise/eliminate steps share the in-place augmented matrix and splitting would scatter one textbook algorithm
bool invertMatrix(std::vector<std::vector<double>> const &m, std::vector<std::vector<double>> &inv) {
    const std::size_t n = m.size();
    // Work on an augmented [m | I] system.
    std::vector<std::vector<double>> a(n, std::vector<double>(2 * n, 0.));
    for(std::size_t i = 0; i < n; ++i) {
        for(std::size_t j = 0; j < n; ++j) {
            a[i][j] = m[i][j];
        }
        a[i][n + i] = 1.;
    }

    for(std::size_t col = 0; col < n; ++col) {
        // Partial pivot: pick the row with the largest magnitude in this column.
        std::size_t pivot = col;
        double best = std::fabs(a[col][col]);
        for(std::size_t r = col + 1; r < n; ++r) {
            if(std::fabs(a[r][col]) > best) {
                best = std::fabs(a[r][col]);
                pivot = r;
            }
        }
        if(best < 1.e-300) {
            return false; // singular
        }
        if(pivot != col) {
            std::swap(a[pivot], a[col]);
        }

        // Normalise the pivot row.
        const double pivot_val = a[col][col];
        for(std::size_t j = 0; j < 2 * n; ++j) {
            a[col][j] /= pivot_val;
        }

        // Eliminate this column from every other row.
        for(std::size_t r = 0; r < n; ++r) {
            if(r == col) {
                continue;
            }
            const double factor = a[r][col];
            if(factor != 0.) {
                for(std::size_t j = 0; j < 2 * n; ++j) {
                    a[r][j] -= factor * a[col][j];
                }
            }
        }
    }

    inv.assign(n, std::vector<double>(n, 0.));
    for(std::size_t i = 0; i < n; ++i) {
        for(std::size_t j = 0; j < n; ++j) {
            inv[i][j] = a[i][n + j];
        }
    }
    return true;
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * @brief Estimates parameter errors at a located minimum via finite-difference curvature.
 *
 * Always computes the diagonal of the Hessian (parabolic per-parameter errors). Optionally
 * forms the full Hessian and the resulting covariance matrix (for small dimensions), and
 * optionally computes asymmetric MINOS errors by profiling the objective.
 *
 * @param eval_fn The objective-evaluation callback (maps a batch of points to their fitness values)
 * @param x_min The parameter vector at the located minimum
 * @param f_min The objective value at the minimum
 * @param step_sizes Per-parameter finite-difference step sizes (must have the same size as x_min)
 * @param opts Options controlling UP, full-covariance / MINOS computation and dimension limits
 * @return A GHesseErrorResult holding parameter errors, optional covariance and MINOS bounds,
 *         validity flags, condition number and the total number of evaluations performed
 */
GHesseErrorResult GHesseError::estimate(
    eval_fn_t const &eval_fn,
    std::vector<double> const &x_min,
    double f_min,
    std::vector<double> const &step_sizes,
    GHesseErrorOptions const &opts
) {
    GHesseErrorResult result;
    result.up = opts.up;

    const std::size_t n = x_min.size();
    if(n == 0 || step_sizes.size() != n) {
        return result; // invalid input
    }

    // --- diagonal of the Hessian (always; O(n) evaluations) -----------------------------------------
    std::vector<double> hessian_diag(n, 0.);
    computeDiagonalHessian(result, eval_fn, x_min, f_min, step_sizes, opts, hessian_diag);

    // --- optional full Hessian -> covariance (small dimension only) ---------------------------------
    if(opts.full_covariance && n <= opts.max_full_dim && result.valid) {
        computeCovariance(result, eval_fn, x_min, step_sizes, opts, hessian_diag);
    }

    // --- optional MINOS asymmetric errors (profiled, low dimension only) ----------------------------
    if(opts.minos && result.valid && n <= opts.max_full_dim) {
        computeMinos(result, eval_fn, x_min, f_min, step_sizes, opts);
    }

    return result;
}

/******************************************************************************/
/**
 * @brief estimate() phase 1 (always): the diagonal of the Hessian and the parameter-fixed errors.
 *
 * H_jj = (f(x + h_j e_j) - 2 f(x) + f(x - h_j e_j)) / h_j^2. Fills the parameter-fixed parabolic errors,
 * the validity flag (at least one positive curvature) and the condition number, and returns the diagonal
 * curvatures in @p hessian_diag for the optional covariance phase.
 */
void GHesseError::computeDiagonalHessian(
    GHesseErrorResult &result,
    eval_fn_t const &eval_fn,
    std::vector<double> const &x_min,
    double f_min,
    std::vector<double> const &step_sizes,
    GHesseErrorOptions const &opts,
    std::vector<double> &hessian_diag
) {
    const std::size_t n = x_min.size();

    std::vector<std::vector<double>> diag_points;
    diag_points.reserve(2 * n);
    for(std::size_t j = 0; j < n; ++j) {
        diag_points.push_back(perturb1(x_min, j, step_sizes[j]));
        diag_points.push_back(perturb1(x_min, j, -step_sizes[j]));
    }
    const std::vector<double> diag_values = eval_fn(diag_points);
    result.n_evaluations += diag_points.size();

    hessian_diag.assign(n, 0.);
    result.parameter_errors.assign(n, 0.);
    double min_curv = 0.;
    double max_curv = 0.;
    bool any_positive = false;
    for(std::size_t j = 0; j < n; ++j) {
        const double h = step_sizes[j];
        const double f_plus = diag_values[2 * j];
        const double f_minus = diag_values[(2 * j) + 1];
        const double curv = (h > 0.) ? (f_plus - 2. * f_min + f_minus) / (h * h) : 0.;
        hessian_diag[j] = curv;
        if(curv > 0.) {
            // Parameter-fixed parabolic error: 1/2 * H_jj * sigma_j^2 = UP -> sigma_j = sqrt(2 UP / H_jj).
            result.parameter_errors[j] = std::sqrt(2. * opts.up / curv);
            if(not any_positive) {
                min_curv = max_curv = curv;
                any_positive = true;
            }
            else {
                min_curv = (curv < min_curv) ? curv : min_curv;
                max_curv = (curv > max_curv) ? curv : max_curv;
            }
        }
    }
    result.valid = any_positive;
    result.condition_number = (min_curv > 0.) ? (max_curv / min_curv) : 0.;
}

/******************************************************************************/
/**
 * @brief estimate() phase 2 (opt-in, low dimension): the off-diagonal Hessian, its inverse -> covariance,
 * and the profiled (correlation-aware) errors.
 *
 * Off-diagonal mixed second differences:
 * H_ij = (f(x+hi+hj) - f(x+hi-hj) - f(x-hi+hj) + f(x-hi-hj)) / (4 hi hj).
 */
void GHesseError::computeCovariance(
    GHesseErrorResult &result,
    eval_fn_t const &eval_fn,
    std::vector<double> const &x_min,
    std::vector<double> const &step_sizes,
    GHesseErrorOptions const &opts,
    std::vector<double> const &hessian_diag
) {
    const std::vector<std::vector<double>> hessian =
        buildFullHessian(result, eval_fn, x_min, step_sizes, hessian_diag);
    fillCovarianceFromHessian(result, hessian, opts);
}

/******************************************************************************/
/**
 * @brief computeCovariance() step 1: assemble the full symmetric Hessian.
 *
 * The diagonal is @p hessian_diag (from computeDiagonalHessian); the off-diagonals are mixed second
 * differences H_ij = (f(x+hi+hj) - f(x+hi-hj) - f(x-hi+hj) + f(x-hi-hj)) / (4 hi hj), evaluated in one
 * batched eval_fn() call.
 */
std::vector<std::vector<double>> GHesseError::buildFullHessian(
    GHesseErrorResult &result,
    eval_fn_t const &eval_fn,
    std::vector<double> const &x_min,
    std::vector<double> const &step_sizes,
    std::vector<double> const &hessian_diag
) {
    const std::size_t n = x_min.size();

    std::vector<std::vector<double>> off_points;
    std::vector<std::pair<std::size_t, std::size_t>> off_index;
    for(std::size_t i = 0; i < n; ++i) {
        for(std::size_t j = i + 1; j < n; ++j) {
            const double hi = step_sizes[i];
            const double hj = step_sizes[j];
            off_index.emplace_back(i, j);
            off_points.push_back(perturb2(x_min, i, hi, j, hj));
            off_points.push_back(perturb2(x_min, i, hi, j, -hj));
            off_points.push_back(perturb2(x_min, i, -hi, j, hj));
            off_points.push_back(perturb2(x_min, i, -hi, j, -hj));
        }
    }

    std::vector<std::vector<double>> hessian(n, std::vector<double>(n, 0.));
    for(std::size_t j = 0; j < n; ++j) {
        hessian[j][j] = hessian_diag[j];
    }

    if(not off_points.empty()) {
        const std::vector<double> off_values = eval_fn(off_points);
        result.n_evaluations += off_points.size();
        for(std::size_t k = 0; k < off_index.size(); ++k) {
            const std::size_t i = off_index[k].first;
            const std::size_t j = off_index[k].second;
            const double hi = step_sizes[i];
            const double hj = step_sizes[j];
            const double v =
                (off_values[4 * k] - off_values[(4 * k) + 1] - off_values[(4 * k) + 2] +
                 off_values[(4 * k) + 3]) /
                (4. * hi * hj);
            hessian[i][j] = v;
            hessian[j][i] = v;
        }
    }

    return hessian;
}

/******************************************************************************/
/**
 * @brief computeCovariance() step 2: covariance V = 2*UP*H^-1 and the profiled (correlation-aware)
 * errors from its diagonal. Leaves the covariance empty (and covariance_valid false) if H is singular.
 *
 * The factor 2 follows from the 1/2 in the quadratic expansion F ~ F_min + 1/2 dx^T H dx; with UP it
 * matches the MINUIT chi^2 convention.
 */
void GHesseError::fillCovarianceFromHessian(
    GHesseErrorResult &result,
    std::vector<std::vector<double>> const &hessian,
    GHesseErrorOptions const &opts
) {
    const std::size_t n = hessian.size();

    std::vector<std::vector<double>> inv;
    if(not invertMatrix(hessian, inv)) {
        return;
    }

    result.covariance.assign(n, std::vector<double>(n, 0.));
    for(std::size_t i = 0; i < n; ++i) {
        for(std::size_t j = 0; j < n; ++j) {
            result.covariance[i][j] = 2. * opts.up * inv[i][j];
        }
    }

    // Profiled (correlation-aware) errors from the covariance diagonal.
    bool all_positive = true;
    for(std::size_t j = 0; j < n; ++j) {
        const double var = result.covariance[j][j];
        if(var > 0.) {
            result.parameter_errors[j] = std::sqrt(var);
        }
        else {
            all_positive = false;
        }
    }
    result.covariance_valid = all_positive;
}

/******************************************************************************/
/**
 * @brief estimate() phase 3 (opt-in, low dimension): the MINOS asymmetric profiled bounds.
 *
 * For each parameter j and each side, find the distance from x_min[j] to where the PROFILE of the
 * objective (re-minimised over all other parameters) rises by UP. Seeded from the symmetric error
 * parameter_errors[j]. Opt-in and expensive (each bound runs repeated re-minimisations).
 */
void GHesseError::computeMinos(
    GHesseErrorResult &result,
    eval_fn_t const &eval_fn,
    std::vector<double> const &x_min,
    double f_min,
    std::vector<double> const &step_sizes,
    GHesseErrorOptions const &opts
) {
    const std::size_t n = x_min.size();

    result.minos_low.assign(n, 0.);
    result.minos_high.assign(n, 0.);
    const double target = f_min + opts.up;
    bool any = false;     // at least one parameter was attempted (had positive curvature)
    bool all_ok = true;   // every attempted parameter bracketed BOTH sides
    for(std::size_t j = 0; j < n; ++j) {
        const double sigma = result.parameter_errors[j];
        if(sigma <= 0.) {
            all_ok = false; // a flat direction we could not even attempt -> result not fully valid
            continue;       // no curvature / flat direction -> cannot bracket
        }
        auto gdev = [&](double xj) {
            return profileMin(eval_fn, x_min, j, xj, step_sizes, result.n_evaluations) - target;
        };
        any = true;
        const std::optional<double> high = minosBound(gdev, x_min[j], sigma);
        const std::optional<double> low = minosBound(gdev, x_min[j], -sigma);
        if(high && low) {
            result.minos_high[j] = *high;
            result.minos_low[j] = *low;
        }
        else {
            // One or both sides could not be bracketed for this parameter: leave its bounds at 0
            // and invalidate the whole MINOS estimate (the symmetric parameter_errors remain the
            // usable fallback). We do NOT report a fabricated bound as a valid confidence interval.
            all_ok = false;
        }
    }
    // Valid only if every parameter was attempted AND every attempted bracket succeeded on both
    // sides. Otherwise the asymmetric errors are incomplete/unreliable and must not be trusted.
    result.minos_valid = any && all_ok;
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
