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

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <ranges>
#include <vector>

#include "geneva/oa/GLineSearch.hpp"

using namespace Gem::Geneva::OptimizationAlgorithms;

namespace {

/** @brief A sphere objective f(x) = sum x_i^2 and a batch evaluator over it. */
double sphere(std::vector<double> const &x) {
    double s = 0.;
    for(double const v : x) {
        s += v * v;
    }
    return s;
}

/** @brief The 2D Rosenbrock objective. */
double rosenbrock(std::vector<double> const &x) {
    const double a = 1. - x[0];
    const double b = x[1] - x[0] * x[0];
    return a * a + 100. * b * b;
}

template <typename F>
GLineSearch::eval_fn_t batchOf(F f) {
    return [f](std::vector<std::vector<double>> const &points) {
        return points | std::views::transform(f) | std::ranges::to<std::vector<double>>();
    };
}

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("GLineSearch: backtracks to a sufficient-decrease step on a sphere", "[geneva][linesearch]") {
    // f(x) = x0^2 + x1^2 ; at (1,1) the gradient is (2,2) and the steepest-descent direction is (-2,-2).
    const std::vector<double> x0{1., 1.};
    const std::vector<double> dir{-2., -2.};
    const double f0 = sphere(x0);                       // = 2
    const double g0_dot_dir = 2. * -2. + 2. * -2.;      // grad . dir = -8

    GLineSearch const ls;
    auto r = ls.search(batchOf(sphere), x0, dir, f0, g0_dot_dir);

    REQUIRE(r.success);
    CHECK(r.alpha > 0.);
    CHECK(r.f_new < f0); // strictly decreased
    // The exact 1D minimum along this ray is at alpha = 0.5 (the origin); the initial alpha = 1
    // overshoots and fails Armijo, so the search must have backtracked at least once.
    CHECK(r.alpha < 1.);
    CHECK(r.n_evaluations >= 1u);
}

TEST_CASE("GLineSearch: rejects a non-descent direction", "[geneva][linesearch]") {
    // grad . dir > 0 -- an ascent direction; no positive step can satisfy Armijo.
    GLineSearch const ls;
    auto r = ls.search(batchOf(sphere), {1., 1.}, {2., 2.}, sphere({1., 1.}), +8.);

    CHECK_FALSE(r.success);
    CHECK(r.alpha == 0.);
    CHECK(r.f_new == sphere({1., 1.})); // unchanged
}

TEST_CASE("GLineSearch: finds a decreasing step along a Rosenbrock ray", "[geneva][linesearch]") {
    // At (-1, 1): grad = (-4, 0); steepest-descent direction (4, 0).
    const std::vector<double> x0{-1., 1.};
    const std::vector<double> dir{4., 0.};
    const double f0 = rosenbrock(x0); // = 4
    const double g0_dot_dir = -4. * 4. + 0. * 0.; // = -16

    GLineSearch const ls;
    auto r = ls.search(batchOf(rosenbrock), x0, dir, f0, g0_dot_dir);

    REQUIRE(r.success);
    CHECK(r.f_new < f0);
    // The accepted point really is x0 + alpha*dir with the reported value.
    const std::vector<double> expected{x0[0] + r.alpha * dir[0], x0[1] + r.alpha * dir[1]};
    CHECK(r.x_new == expected);
    CHECK(std::abs(r.f_new - rosenbrock(r.x_new)) < 1.e-9);
}

TEST_CASE("GLineSearch: accepts the full initial step when it already satisfies Armijo", "[geneva][linesearch]") {
    // A gently sloped 1D ray where alpha = 1 already gives sufficient decrease, so no backtracking.
    // f(t) = (t)^2 around x0 = 10 with a small unit direction so the quadratic stays in the basin.
    const std::vector<double> x0{10.};
    const std::vector<double> dir{-1.};
    const double f0 = sphere(x0);               // 100
    const double g0_dot_dir = 2. * 10. * -1.;   // grad . dir = -20

    GLineSearchOptions const opts; // alpha_init = 1
    GLineSearch const ls;
    auto r = ls.search(batchOf(sphere), x0, dir, f0, g0_dot_dir, opts);

    REQUIRE(r.success);
    // f(10 - 1) = 81 <= 100 + 1e-4 * 1 * (-20) = 99.998 -> the full step passes immediately.
    CHECK(r.alpha == 1.);
    CHECK(r.f_new < f0);
}
