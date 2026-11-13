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

#include "geneva/oa/GHesseError.hpp"

using namespace Gem::Geneva::OptimizationAlgorithms;

namespace {

template <typename F>
GHesseError::eval_fn_t batchOf(F f) {
    return [f](std::vector<std::vector<double>> const &points) {
        return points | std::views::transform(f) | std::ranges::to<std::vector<double>>();
    };
}

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("GHesseError: parameter-fixed errors on a separable quadratic", "[geneva][hesse]") {
    // f(x) = 0.5*(a x0^2 + b x1^2); Hessian = diag(a, b); minimum at the origin.
    const double a = 2.;
    const double b = 8.;
    auto f = [a, b](std::vector<double> const &x) { return 0.5 * (a * x[0] * x[0] + b * x[1] * x[1]); };

    const std::vector<double> x_min{0., 0.};
    GHesseError const he;
    const auto r = he.estimate(batchOf(f), x_min, f(x_min), {1.e-2, 1.e-2});

    REQUIRE(r.valid);
    // sigma_j = sqrt(2 * UP / H_jj), UP defaults to 1.
    CHECK(std::abs(r.parameter_errors[0] - std::sqrt(2. / a)) < 1.e-6);
    CHECK(std::abs(r.parameter_errors[1] - std::sqrt(2. / b)) < 1.e-6);
    // condition number = max/min curvature = b/a.
    CHECK(std::abs(r.condition_number - b / a) < 1.e-6);
    CHECK_FALSE(r.covariance_valid); // diagonal estimate only by default
}

TEST_CASE("GHesseError: full covariance captures correlations", "[geneva][hesse]") {
    // f(x) = 0.5*(x0^2 + x1^2 + x0 x1); Hessian H = [[1, 0.5], [0.5, 1]].
    // H^-1 = (4/3) * [[1, -0.5], [-0.5, 1]]; V = 2 * H^-1 = [[8/3, -4/3], [-4/3, 8/3]].
    auto f = [](std::vector<double> const &x) {
        return 0.5 * (x[0] * x[0] + x[1] * x[1] + x[0] * x[1]);
    };

    const std::vector<double> x_min{0., 0.};
    GHesseErrorOptions opts;
    opts.full_covariance = true;
    const auto r = GHesseError{}.estimate(batchOf(f), x_min, f(x_min), {1.e-2, 1.e-2}, opts);

    REQUIRE(r.valid);
    REQUIRE(r.covariance_valid);
    CHECK(std::abs(r.covariance[0][0] - 8. / 3.) < 1.e-4);
    CHECK(std::abs(r.covariance[1][1] - 8. / 3.) < 1.e-4);
    CHECK(std::abs(r.covariance[0][1] - (-4. / 3.)) < 1.e-4);
    // The profiled (correlation-aware) error sqrt(V_jj) exceeds the parameter-fixed sqrt(2/H_jj)=sqrt(2).
    CHECK(std::abs(r.parameter_errors[0] - std::sqrt(8. / 3.)) < 1.e-4);
    CHECK(r.parameter_errors[0] > std::sqrt(2.));
}

TEST_CASE("GHesseError: UP scales the errors", "[geneva][hesse]") {
    // For -logL-like objectives UP = 0.5; the error scales as sqrt(UP).
    auto f = [](std::vector<double> const &x) { return 0.5 * (x[0] * x[0]); };
    GHesseErrorOptions up_half;
    up_half.up = 0.5;
    const auto r1 = GHesseError{}.estimate(batchOf(f), {0.}, f({0.}), {1.e-2});
    const auto r2 = GHesseError{}.estimate(batchOf(f), {0.}, f({0.}), {1.e-2}, up_half);
    REQUIRE(r1.valid);
    REQUIRE(r2.valid);
    CHECK(std::abs(r2.parameter_errors[0] / r1.parameter_errors[0] - std::sqrt(0.5)) < 1.e-6);
}

TEST_CASE("GHesseError: flat / non-minimum directions are flagged", "[geneva][hesse]") {
    // A saddle in x1 (negative curvature): no usable error for that parameter.
    auto f = [](std::vector<double> const &x) { return 0.5 * (x[0] * x[0] - x[1] * x[1]); };
    const auto r = GHesseError{}.estimate(batchOf(f), {0., 0.}, f({0., 0.}), {1.e-2, 1.e-2});
    REQUIRE(r.valid);                       // x0 has positive curvature
    CHECK(r.parameter_errors[0] > 0.);
    CHECK(r.parameter_errors[1] == 0.);     // x1 curvature non-positive -> no error
}

/******************************************************************************/

TEST_CASE("GHesseError(MINOS): symmetric bounds on a separable quadratic", "[geneva][hesse][minos]") {
    // f = 0.5*(a x0^2 + b x1^2). Separable -> the profile along each axis equals the axis itself, so
    // the MINOS bounds equal the symmetric HESSE error sqrt(2*UP/H_jj) on both sides.
    const double a = 2.;
    const double b = 8.;
    auto f = [a, b](std::vector<double> const &x) { return 0.5 * (a * x[0] * x[0] + b * x[1] * x[1]); };

    GHesseErrorOptions opts;
    opts.minos = true;
    const auto r = GHesseError{}.estimate(batchOf(f), {0., 0.}, 0., {1.e-2, 1.e-2}, opts);

    REQUIRE(r.valid);
    REQUIRE(r.minos_valid);
    CHECK(std::abs(r.minos_low[0] - std::sqrt(2. / a)) < 3.e-2);
    CHECK(std::abs(r.minos_high[0] - std::sqrt(2. / a)) < 3.e-2);
    CHECK(std::abs(r.minos_low[1] - std::sqrt(2. / b)) < 3.e-2);
    CHECK(std::abs(r.minos_high[1] - std::sqrt(2. / b)) < 3.e-2);
    CHECK(std::abs(r.minos_low[0] - r.minos_high[0]) < 1.e-2); // symmetric for a quadratic
}

TEST_CASE("GHesseError(MINOS): profiles correlations (re-minimises the other parameter)",
          "[geneva][hesse][minos]") {
    // f = 0.5*(x0^2 + x1^2 + x0 x1). The PROFILED error along x0 (re-minimising x1) is sqrt(V_00) =
    // sqrt(8/3) ~ 1.633, strictly larger than the parameter-fixed sqrt(2). MINOS must recover the
    // profiled bound -- this verifies the inner re-minimiser actually runs.
    auto f = [](std::vector<double> const &x) {
        return 0.5 * (x[0] * x[0] + x[1] * x[1] + x[0] * x[1]);
    };

    GHesseErrorOptions opts;
    opts.minos = true;
    const auto r = GHesseError{}.estimate(batchOf(f), {0., 0.}, 0., {1.e-2, 1.e-2}, opts);

    REQUIRE(r.valid);
    REQUIRE(r.minos_valid);
    const double profiled = std::sqrt(8. / 3.);
    CHECK(std::abs(r.minos_high[0] - profiled) < 4.e-2);
    CHECK(std::abs(r.minos_low[0] - profiled) < 4.e-2);
    CHECK(r.minos_high[0] > std::sqrt(2.)); // strictly above the parameter-fixed error
}

TEST_CASE("GHesseError(MINOS): captures an asymmetric (non-parabolic) minimum", "[geneva][hesse][minos]") {
    // f(x) = 0.5 x^2 + 0.1 x^3 (single parameter). The cubic makes f rise FASTER for x>0 than for x<0,
    // so the high-side MINOS bound is smaller than the low-side one (symmetric HESSE would miss this).
    auto f = [](std::vector<double> const &x) { return 0.5 * x[0] * x[0] + 0.1 * x[0] * x[0] * x[0]; };

    GHesseErrorOptions opts;
    opts.minos = true;
    const auto r = GHesseError{}.estimate(batchOf(f), {0.}, 0., {1.e-2}, opts);

    REQUIRE(r.valid);
    REQUIRE(r.minos_valid);
    CHECK(r.minos_low[0] > r.minos_high[0] + 0.2); // clearly asymmetric
    CHECK(std::abs(r.minos_high[0] - 1.263) < 2.e-2);
    CHECK(std::abs(r.minos_low[0] - 1.757) < 2.e-2);
}

TEST_CASE("GHesseError(MINOS): an unbracketable direction is reported invalid, not fabricated",
          "[geneva][hesse][minos]") {
    // Regression test for the silent-wrong-result bug: f(x) = c x^2 / (1 + x^2) has POSITIVE curvature at
    // the minimum (so a symmetric HESSE error exists and the parameter is attempted) but is bounded above
    // by c. With c < UP (=1) the profiled objective can NEVER rise by UP, so neither MINOS side can bracket
    // the crossing. Previously minosBound() returned a best-effort distance and the caller marked the
    // result minos_valid == true (a fabricated confidence interval). It must now be reported INVALID.
    const double c = 0.3;
    auto f = [c](std::vector<double> const &x) { return c * x[0] * x[0] / (1. + x[0] * x[0]); };

    GHesseErrorOptions opts;
    opts.minos = true;
    const auto r = GHesseError{}.estimate(batchOf(f), {0.}, 0., {1.e-2}, opts);

    REQUIRE(r.valid);                 // positive curvature at the minimum -> a symmetric error exists
    CHECK(r.parameter_errors[0] > 0.);
    CHECK_FALSE(r.minos_valid);       // ... but the UP crossing is unbracketable -> MINOS is not usable
    CHECK(r.minos_low[0] == 0.);      // a failed side is left at the 0 sentinel, not a fabricated bound
    CHECK(r.minos_high[0] == 0.);
}
