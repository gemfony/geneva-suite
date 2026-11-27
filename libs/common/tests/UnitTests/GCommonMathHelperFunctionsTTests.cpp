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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"

using Catch::Approx;

// --- grational_sigmoid ---------------------------------------------------

TEST_CASE(
    "grational_sigmoid<double>: zero input yields zero output",
    "[common][math][grational_sigmoid]"
) {
    REQUIRE(Gem::Common::grational_sigmoid(0., 10., 1.) == Approx(0.));
    REQUIRE(Gem::Common::grational_sigmoid(0., -5., 2.) == Approx(0.));
}

TEST_CASE(
    "grational_sigmoid<float>: zero input yields zero output",
    "[common][math][grational_sigmoid]"
) {
    REQUIRE(Gem::Common::grational_sigmoid(0.f, 10.f, 1.f) == Approx(0.f));
}

TEST_CASE(
    "grational_sigmoid<double>: output is bounded by ±barrier",
    "[common][math][grational_sigmoid]"
) {
    const double barrier = 10.;
    const double steepness = 1.;
    double const large_pos = Gem::Common::grational_sigmoid(1e6, barrier, steepness);
    REQUIRE(large_pos > 0.);
    REQUIRE(large_pos < barrier);
    REQUIRE(large_pos == Approx(barrier).epsilon(0.01));

    double const large_neg = Gem::Common::grational_sigmoid(-1e6, barrier, steepness);
    REQUIRE(large_neg < 0.);
    REQUIRE(large_neg > -barrier);
    REQUIRE(large_neg == Approx(-barrier).epsilon(0.01));
}

TEST_CASE(
    "grational_sigmoid<float>: output is bounded by ±barrier",
    "[common][math][grational_sigmoid]"
) {
    const float barrier = 5.f;
    const float steepness = 1.f;
    float const large_pos = Gem::Common::grational_sigmoid(1e5f, barrier, steepness);
    REQUIRE(large_pos > 0.f);
    REQUIRE(large_pos < barrier);
    REQUIRE(large_pos == Approx(barrier).epsilon(0.01));

    float const large_neg = Gem::Common::grational_sigmoid(-1e5f, barrier, steepness);
    REQUIRE(large_neg < 0.f);
    REQUIRE(large_neg > -barrier);
    REQUIRE(large_neg == Approx(-barrier).epsilon(0.01));
}

TEST_CASE(
    "grational_sigmoid<double>: antisymmetry f(-x) == -f(x)",
    "[common][math][grational_sigmoid]"
) {
    const double barrier = 10.;
    const double steepness = 1.;
    for(double const v : {0.1, 1.0, 5.0, 100.0}) {
        REQUIRE(
            Gem::Common::grational_sigmoid(-v, barrier, steepness) ==
            Approx(-Gem::Common::grational_sigmoid(v, barrier, steepness))
        );
    }
}

TEST_CASE(
    "grational_sigmoid<float>: antisymmetry f(-x) == -f(x)",
    "[common][math][grational_sigmoid]"
) {
    const float barrier = 3.f;
    const float steepness = 2.f;
    for(float const v : {0.1f, 1.0f, 5.0f}) {
        REQUIRE(
            Gem::Common::grational_sigmoid(-v, barrier, steepness) ==
            Approx(-Gem::Common::grational_sigmoid(v, barrier, steepness))
        );
    }
}

TEST_CASE(
    "grational_sigmoid<double>: steeper curve converges faster",
    "[common][math][grational_sigmoid]"
) {
    const double barrier = 10.;
    const double v = 5.;
    double const slow = Gem::Common::grational_sigmoid(v, barrier, 10.);
    double const fast = Gem::Common::grational_sigmoid(v, barrier, 1.);
    REQUIRE(slow > 0.);
    REQUIRE(fast > 0.);
    REQUIRE(slow < barrier);
    REQUIRE(fast < barrier);
    REQUIRE(fast > slow);
}

TEST_CASE(
    "grational_sigmoid<double>: known value at v==steepness",
    "[common][math][grational_sigmoid]"
) {
    for(double const s : {1., 2., 5.}) {
        REQUIRE(Gem::Common::grational_sigmoid(s, 10., s) == Approx(5.));
    }
}

TEST_CASE("grational_sigmoid: zero steepness throws", "[common][math][grational_sigmoid]") {
    REQUIRE_THROWS_AS(Gem::Common::grational_sigmoid(1., 10., 0.), gem_exception);
    REQUIRE_THROWS_AS(Gem::Common::grational_sigmoid(1., 10., -1.), gem_exception);
}

// --- enforceRangeConstraint ----------------------------------------------

TEST_CASE(
    "enforceRangeConstraint: value in range is unchanged",
    "[common][math][enforceRangeConstraint]"
) {
    double v = 5.;
    Gem::Common::enforceRangeConstraint(v, 0., 10.);
    REQUIRE(v == Approx(5.));
}

TEST_CASE(
    "enforceRangeConstraint: value below lower is clamped",
    "[common][math][enforceRangeConstraint]"
) {
    double v = -3.;
    Gem::Common::enforceRangeConstraint(v, 0., 10.);
    REQUIRE(v == Approx(0.));
}

TEST_CASE(
    "enforceRangeConstraint: value above upper is clamped",
    "[common][math][enforceRangeConstraint]"
) {
    double v = 15.;
    Gem::Common::enforceRangeConstraint(v, 0., 10.);
    REQUIRE(v == Approx(10.));
}

TEST_CASE(
    "enforceRangeConstraint: lower > upper throws",
    "[common][math][enforceRangeConstraint]"
) {
    double v = 5.;
    REQUIRE_THROWS_AS(Gem::Common::enforceRangeConstraint(v, 10., 0.), gem_exception);
}

TEST_CASE(
    "enforceRangeConstraint: boundary values are kept",
    "[common][math][enforceRangeConstraint]"
) {
    double lo = 0.;
    double hi = 10.;
    Gem::Common::enforceRangeConstraint(lo, 0., 10.);
    Gem::Common::enforceRangeConstraint(hi, 0., 10.);
    REQUIRE(lo == Approx(0.));
    REQUIRE(hi == Approx(10.));
}

// --- checkRangeCompliance ------------------------------------------------

TEST_CASE(
    "checkRangeCompliance<fp>: value in range returns true",
    "[common][math][checkRangeCompliance]"
) {
    REQUIRE(Gem::Common::checkRangeCompliance(5., 0., 10.));
    REQUIRE(Gem::Common::checkRangeCompliance(0., 0., 10.));
    REQUIRE(Gem::Common::checkRangeCompliance(10., 0., 10.));
}

TEST_CASE(
    "checkRangeCompliance<fp>: value out of range returns false",
    "[common][math][checkRangeCompliance]"
) {
    REQUIRE_FALSE(Gem::Common::checkRangeCompliance(-1., 0., 10.));
    REQUIRE_FALSE(Gem::Common::checkRangeCompliance(11., 0., 10.));
}

TEST_CASE(
    "checkRangeCompliance<fp>: lower > upper throws",
    "[common][math][checkRangeCompliance]"
) {
    REQUIRE_THROWS_AS(Gem::Common::checkRangeCompliance(5., 10., 0.), gem_exception);
}

TEST_CASE(
    "checkRangeCompliance<int>: value in range returns true",
    "[common][math][checkRangeCompliance]"
) {
    REQUIRE(Gem::Common::checkRangeCompliance(5, 0, 10));
    REQUIRE(Gem::Common::checkRangeCompliance(0, 0, 10));
    REQUIRE(Gem::Common::checkRangeCompliance(10, 0, 10));
}

TEST_CASE(
    "checkRangeCompliance<int>: value out of range returns false",
    "[common][math][checkRangeCompliance]"
) {
    REQUIRE_FALSE(Gem::Common::checkRangeCompliance(-1, 0, 10));
    REQUIRE_FALSE(Gem::Common::checkRangeCompliance(11, 0, 10));
}

TEST_CASE(
    "checkRangeCompliance<int>: lower > upper throws",
    "[common][math][checkRangeCompliance]"
) {
    REQUIRE_THROWS_AS(Gem::Common::checkRangeCompliance(5, 10, 0), gem_exception);
}

// --- getWorstCase / getBestCase ------------------------------------------

TEST_CASE("getWorstCase/getBestCase bool overloads", "[common][math][worstBestCase]") {
    REQUIRE(Gem::Common::getWorstCase<double>(true) < 0.);
    REQUIRE(Gem::Common::getBestCase<double>(true) > 0.);
    REQUIRE(Gem::Common::getWorstCase<double>(false) > 0.);
    REQUIRE(Gem::Common::getBestCase<double>(false) < 0.);
}

TEST_CASE("getWorstCase/getBestCase sortOrder overloads", "[common][math][worstBestCase]") {
    using so = Gem::Common::sortOrder;
    REQUIRE(Gem::Common::getWorstCase<double>(so::HIGHERISBETTER) < 0.);
    REQUIRE(Gem::Common::getBestCase<double>(so::HIGHERISBETTER) > 0.);
    REQUIRE(Gem::Common::getWorstCase<double>(so::LOWERISBETTER) > 0.);
    REQUIRE(Gem::Common::getBestCase<double>(so::LOWERISBETTER) < 0.);
}

TEST_CASE(
    "isBetter: strict under both sort orders and NaN worse than every number",
    "[common][math][isBetter]"
) {
    using so = Gem::Common::sortOrder;
    double const nan = std::numeric_limits<double>::quiet_NaN();
    double const inf = std::numeric_limits<double>::infinity();

    CHECK(Gem::Common::isBetter(1., 2., so::LOWERISBETTER));
    CHECK_FALSE(Gem::Common::isBetter(2., 1., so::LOWERISBETTER));
    CHECK(Gem::Common::isBetter(2., 1., so::HIGHERISBETTER));
    CHECK_FALSE(Gem::Common::isBetter(1., 2., so::HIGHERISBETTER));

    for(auto const order : {so::LOWERISBETTER, so::HIGHERISBETTER}) {
        CHECK_FALSE(Gem::Common::isBetter(3., 3., order)); // equal is not better
        CHECK(Gem::Common::isBetter(inf, nan, order));
        CHECK(Gem::Common::isBetter(-inf, nan, order));
        CHECK(Gem::Common::isBetter(0., nan, order));
        CHECK_FALSE(Gem::Common::isBetter(nan, 0., order));
        CHECK_FALSE(Gem::Common::isBetter(nan, inf, order));
        CHECK_FALSE(Gem::Common::isBetter(nan, nan, order)); // two NaN are equal
    }
}

// --- checkValueRange -----------------------------------------------------

TEST_CASE(
    "checkValueRange<fp>: value in closed range is returned",
    "[common][math][checkValueRange]"
) {
    REQUIRE(Gem::Common::checkValueRange(5., 0., 10.) == Approx(5.));
    REQUIRE(Gem::Common::checkValueRange(0., 0., 10.) == Approx(0.));
    REQUIRE(Gem::Common::checkValueRange(10., 0., 10.) == Approx(10.));
}

TEST_CASE(
    "checkValueRange<fp>: value out of range throws by default",
    "[common][math][checkValueRange]"
) {
    REQUIRE_THROWS_AS(Gem::Common::checkValueRange(-1., 0., 10.), gem_exception);
    REQUIRE_THROWS_AS(Gem::Common::checkValueRange(11., 0., 10.), gem_exception);
}

TEST_CASE("checkValueRange<fp>: warnOnly does not throw", "[common][math][checkValueRange]") {
    REQUIRE_NOTHROW(Gem::Common::checkValueRange(-1., 0., 10., false, false, true));
}

TEST_CASE(
    "checkValueRange<fp>: open boundaries exclude endpoints",
    "[common][math][checkValueRange]"
) {
    REQUIRE_THROWS_AS(
        Gem::Common::checkValueRange(
            0.,
            0.,
            10.,
            Gem::Common::GFPLOWEROPEN,
            Gem::Common::GFPUPPERCLOSED
        ),
        gem_exception
    );
    REQUIRE_THROWS_AS(
        Gem::Common::checkValueRange(
            10.,
            0.,
            10.,
            Gem::Common::GFPLOWERCLOSED,
            Gem::Common::GFPUPPEROPEN
        ),
        gem_exception
    );
}

TEST_CASE("checkValueRange<int>: value in range is returned", "[common][math][checkValueRange]") {
    REQUIRE(Gem::Common::checkValueRange(5, 0, 10) == 5);
    REQUIRE(Gem::Common::checkValueRange(0, 0, 10) == 0);
    REQUIRE(Gem::Common::checkValueRange(10, 0, 10) == 10);
}

TEST_CASE("checkValueRange<int>: value out of range throws", "[common][math][checkValueRange]") {
    REQUIRE_THROWS_AS(Gem::Common::checkValueRange(-1, 0, 10), gem_exception);
    REQUIRE_THROWS_AS(Gem::Common::checkValueRange(11, 0, 10), gem_exception);
}

TEST_CASE(
    "checkValueRange<int>: open boundaries exclude endpoints",
    "[common][math][checkValueRange]"
) {
    REQUIRE_THROWS_AS(Gem::Common::checkValueRange(0, 0, 10, true /*lowerOpen*/), gem_exception);
    REQUIRE_THROWS_AS(
        Gem::Common::checkValueRange(10, 0, 10, false, true /*upperOpen*/),
        gem_exception
    );
}

TEST_CASE(
    "checkValueRange: lower > upper throws also when only a warning is asked for",
    "[common][math][checkValueRange]"
) {
    CHECK_THROWS_AS(Gem::Common::checkValueRange(5., 10., 0.), gem_exception);
    CHECK_THROWS_AS(Gem::Common::checkValueRange(5., 10., 0., false, false, true), gem_exception);
    CHECK_THROWS_AS(Gem::Common::checkValueRange(5, 10, 0, false, false, true), gem_exception);
}

TEST_CASE(
    "checkValueRange: the named flags select the boundaries and the warning",
    "[common][math][checkValueRange]"
) {
    using namespace Gem::Common;
    CHECK(checkValueRange(0., 0., 1., GFPLOWERCLOSED, GFPUPPEROPEN, GFNOWARNING, "x") == 0.);
    CHECK_THROWS_AS(
        checkValueRange(1., 0., 1., GFPLOWERCLOSED, GFPUPPEROPEN, GFNOWARNING, "x"),
        gem_exception
    );
    // A warning instead of an exception returns the value unchanged
    CHECK(checkValueRange(2, 0, 1, GFPLOWERCLOSED, GFPUPPERCLOSED, true, "n") == 2);
}

// --- NaN in the range checks ---------------------------------------------

TEST_CASE("Range checks: NaN lies in no range", "[common][math][rangeNaN]") {
    double const nan = std::numeric_limits<double>::quiet_NaN();
    float const nan_f = std::numeric_limits<float>::quiet_NaN();

    CHECK_FALSE(Gem::Common::checkRangeCompliance(nan, 0., 1.));
    CHECK_FALSE(Gem::Common::checkRangeCompliance(nan_f, 0.f, 1.f));

    // Every combination of open and closed boundaries, and also when only a warning is asked for
    for(bool const lower_open : {false, true}) {
        for(bool const upper_open : {false, true}) {
            CHECK_THROWS_AS(
                Gem::Common::checkValueRange(nan, 0., 1., lower_open, upper_open),
                gem_exception
            );
            CHECK_THROWS_AS(
                Gem::Common::checkValueRange(nan, 0., 1., lower_open, upper_open, true),
                gem_exception
            );
        }
    }

    double v = nan;
    CHECK_THROWS_AS(Gem::Common::enforceRangeConstraint(v, 0., 1.), gem_exception);
    CHECK_THROWS_AS(Gem::Common::enforceRangeConstraint(v, 0., 1., "caller", true), gem_exception);
}

TEST_CASE("Range checks: a NaN boundary is refused", "[common][math][rangeNaN]") {
    double const nan = std::numeric_limits<double>::quiet_NaN();

    CHECK_THROWS_AS(Gem::Common::checkRangeCompliance(0.5, nan, 1.), gem_exception);
    CHECK_THROWS_AS(Gem::Common::checkRangeCompliance(0.5, 0., nan), gem_exception);

    double v = 0.5;
    CHECK_THROWS_AS(Gem::Common::enforceRangeConstraint(v, nan, 1.), gem_exception);
    CHECK_THROWS_AS(Gem::Common::enforceRangeConstraint(v, 0., nan), gem_exception);

    CHECK_THROWS_AS(Gem::Common::checkValueRange(0.5, nan, 1., false, false, true), gem_exception);
    CHECK_THROWS_AS(Gem::Common::checkValueRange(0.5, 0., nan, false, false, true), gem_exception);
}

TEST_CASE("Range checks: infinities are ordinary values", "[common][math][rangeNaN]") {
    double const inf = std::numeric_limits<double>::infinity();

    CHECK(Gem::Common::checkRangeCompliance(inf, 0., inf));
    CHECK_FALSE(Gem::Common::checkRangeCompliance(inf, 0., 1.));
    CHECK(Gem::Common::checkValueRange(-inf, -inf, 0.) == -inf);
    CHECK_THROWS_AS(Gem::Common::checkValueRange(-inf, -inf, 0., true, false), gem_exception);

    double v = inf;
    CHECK(Gem::Common::enforceRangeConstraint(v, 0., 1.) == 1.);
    CHECK(v == 1.);
}

// --- getMinMax -----------------------------------------------------------

TEST_CASE("getMinMax 1D: returns correct min and max", "[common][math][getMinMax]") {
    std::vector<double> const v{3., 1., 4., 1., 5., 9., 2., 6.};
    auto [lo, hi] = Gem::Common::getMinMax(v);
    REQUIRE(lo == Approx(1.));
    REQUIRE(hi == Approx(9.));
}

TEST_CASE("getMinMax 1D: two-element vector", "[common][math][getMinMax]") {
    std::vector<int> const v{7, 3};
    auto [lo, hi] = Gem::Common::getMinMax(v);
    REQUIRE(lo == 3);
    REQUIRE(hi == 7);
}

TEST_CASE("getMinMax 1D: size < 2 throws", "[common][math][getMinMax]") {
    REQUIRE_THROWS_AS(Gem::Common::getMinMax(std::vector<double>{1.}), gem_exception);
    REQUIRE_THROWS_AS(Gem::Common::getMinMax(std::vector<double>{}), gem_exception);
}

TEST_CASE(
    "getMinMax 2D: returns correct extremes for both dimensions",
    "[common][math][getMinMax]"
) {
    using T = std::tuple<double, double>;
    std::vector<T> const v{{1., 10.}, {3., 2.}, {2., 8.}};
    auto [minX, maxX, minY, maxY] = Gem::Common::getMinMax(v);
    REQUIRE(minX == Approx(1.));
    REQUIRE(maxX == Approx(3.));
    REQUIRE(minY == Approx(2.));
    REQUIRE(maxY == Approx(10.));
}

TEST_CASE("getMinMax 2D: size < 2 throws", "[common][math][getMinMax]") {
    using T = std::tuple<double, double>;
    REQUIRE_THROWS_AS(Gem::Common::getMinMax(std::vector<T>{{1., 2.}}), gem_exception);
}

TEST_CASE(
    "getMinMax 3D: returns correct extremes for all three dimensions",
    "[common][math][getMinMax]"
) {
    using T = std::tuple<double, double, double>;
    std::vector<T> const v{{1., 10., 100.}, {3., 2., 50.}, {2., 8., 200.}};
    auto [minX, maxX, minY, maxY, minZ, maxZ] = Gem::Common::getMinMax(v);
    REQUIRE(minX == Approx(1.));
    REQUIRE(maxX == Approx(3.));
    REQUIRE(minY == Approx(2.));
    REQUIRE(maxY == Approx(10.));
    REQUIRE(minZ == Approx(50.));
    REQUIRE(maxZ == Approx(200.));
}

TEST_CASE(
    "getMinMax 4D: returns correct extremes for all four dimensions",
    "[common][math][getMinMax]"
) {
    using T = std::tuple<double, double, double, double>;
    std::vector<T> const v{{1., 10., 100., 1000.}, {3., 2., 50., 500.}, {2., 8., 200., 2000.}};
    auto [minX, maxX, minY, maxY, minZ, maxZ, minW, maxW] = Gem::Common::getMinMax(v);
    REQUIRE(minX == Approx(1.));
    REQUIRE(maxX == Approx(3.));
    REQUIRE(minY == Approx(2.));
    REQUIRE(maxY == Approx(10.));
    REQUIRE(minZ == Approx(50.));
    REQUIRE(maxZ == Approx(200.));
    REQUIRE(minW == Approx(500.));
    REQUIRE(maxW == Approx(2000.));
}

// --- GMean ---------------------------------------------------------------

TEST_CASE("GMean: single element returns that element", "[common][math][GMean]") {
    std::vector<double> const v{7.5};
    REQUIRE(Gem::Common::GMean(v) == Approx(7.5));
}

TEST_CASE("GMean: known arithmetic mean", "[common][math][GMean]") {
    std::vector<double> const v{1., 2., 3., 4., 5.};
    REQUIRE(Gem::Common::GMean(v) == Approx(3.));
}

TEST_CASE("GMean: constant vector returns the constant", "[common][math][GMean]") {
    std::vector<double> const v(10, 4.2);
    REQUIRE(Gem::Common::GMean(v) == Approx(4.2));
}

TEST_CASE("GMean: empty vector throws", "[common][math][GMean]") {
    REQUIRE_THROWS_AS(Gem::Common::GMean(std::vector<double>{}), gem_exception);
}

// --- GStandardDeviation --------------------------------------------------

TEST_CASE(
    "GStandardDeviation: single element gives sigma=0",
    "[common][math][GStandardDeviation]"
) {
    std::vector<double> const v{3.7};
    auto [mean, sigma] = Gem::Common::GStandardDeviation(v);
    REQUIRE(mean == Approx(3.7));
    REQUIRE(sigma == Approx(0.));
}

TEST_CASE(
    "GStandardDeviation: constant vector gives sigma=0",
    "[common][math][GStandardDeviation]"
) {
    std::vector<double> const v(5, 2.0);
    auto [mean, sigma] = Gem::Common::GStandardDeviation(v);
    REQUIRE(mean == Approx(2.0));
    REQUIRE(sigma == Approx(0.));
}

TEST_CASE("GStandardDeviation: known values 1 to 5", "[common][math][GStandardDeviation]") {
    std::vector<double> const v{1., 2., 3., 4., 5.};
    auto [mean, sigma] = Gem::Common::GStandardDeviation(v);
    REQUIRE(mean == Approx(3.));
    REQUIRE(sigma == Approx(std::sqrt(2.5)));
}

// --- PowSmallPosInt ------------------------------------------------------

TEST_CASE("PowSmallPosInt: compile-time integer powers", "[common][math][PowSmallPosInt]") {
    static_assert(Gem::Common::PowSmallPosInt<2, 0>() == 1);
    static_assert(Gem::Common::PowSmallPosInt<5, 0>() == 1);
    static_assert(Gem::Common::PowSmallPosInt<7, 1>() == 7);
    static_assert(Gem::Common::PowSmallPosInt<2, 1>() == 2);
    static_assert(Gem::Common::PowSmallPosInt<2, 2>() == 4);
    static_assert(Gem::Common::PowSmallPosInt<2, 8>() == 256);
    static_assert(Gem::Common::PowSmallPosInt<3, 3>() == 27);
    static_assert(Gem::Common::PowSmallPosInt<3, 4>() == 81);
    static_assert(Gem::Common::PowSmallPosInt<10, 3>() == 1000);
    SUCCEED();
}

// --- subtractVec / addVec ------------------------------------------------

TEST_CASE("subtractVec: element-wise subtraction", "[common][math][subtractVec]") {
    std::vector<double> a{5., 7., 9.};
    std::vector<double> const b{1., 2., 3.};
    Gem::Common::subtractVec(a, b);
    REQUIRE(a[0] == Approx(4.));
    REQUIRE(a[1] == Approx(5.));
    REQUIRE(a[2] == Approx(6.));
}

TEST_CASE("addVec: element-wise addition", "[common][math][addVec]") {
    std::vector<double> a{1., 2., 3.};
    std::vector<double> const b{4., 5., 6.};
    Gem::Common::addVec(a, b);
    REQUIRE(a[0] == Approx(5.));
    REQUIRE(a[1] == Approx(7.));
    REQUIRE(a[2] == Approx(9.));
}

TEST_CASE(
    "subtractVec/addVec: inverse operations cancel out",
    "[common][math][subtractVec][addVec]"
) {
    std::vector<double> original{3., 1., 4., 1., 5.};
    std::vector<double> a = original;
    std::vector<double> const b{2., 7., 1., 8., 2.};
    Gem::Common::addVec(a, b);
    Gem::Common::subtractVec(a, b);
    for(std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i] == Approx(original[i]));
    }
}

TEST_CASE("subtractVec: size mismatch throws", "[common][math][subtractVec]") {
    std::vector<double> a{1., 2.};
    std::vector<double> b{3., 4., 5.};
    REQUIRE_THROWS_AS(Gem::Common::subtractVec(a, b), gem_exception);
}

TEST_CASE("addVec: size mismatch throws", "[common][math][addVec]") {
    std::vector<double> a{1., 2.};
    std::vector<double> b{3.};
    REQUIRE_THROWS_AS(Gem::Common::addVec(a, b), gem_exception);
}

// --- multVecConst / assignVecConst ---------------------------------------

TEST_CASE(
    "multVecConst: each element is multiplied by the constant",
    "[common][math][multVecConst]"
) {
    std::vector<double> a{1., 2., 3.};
    Gem::Common::multVecConst(a, 3.);
    REQUIRE(a[0] == Approx(3.));
    REQUIRE(a[1] == Approx(6.));
    REQUIRE(a[2] == Approx(9.));
}

TEST_CASE("multVecConst: multiply by 0 gives zero vector", "[common][math][multVecConst]") {
    std::vector<double> a{1., 2., 3.};
    Gem::Common::multVecConst(a, 0.);
    for(auto v : a) {
        REQUIRE(v == Approx(0.));
    }
}

TEST_CASE("assignVecConst: each element is set to the constant", "[common][math][assignVecConst]") {
    std::vector<double> a{1., 2., 3., 4.};
    Gem::Common::assignVecConst(a, 7.);
    for(auto v : a) {
        REQUIRE(v == Approx(7.));
    }
}

TEST_CASE("assignVecConst: empty vector is a no-op", "[common][math][assignVecConst]") {
    std::vector<double> a;
    REQUIRE_NOTHROW(Gem::Common::assignVecConst(a, 1.));
    REQUIRE(a.empty());
}

// --- sumTupleVec / squareSumTupleVec / productSumTupleVec ---------------

TEST_CASE("sumTupleVec: sums x- and y-components independently", "[common][math][sumTupleVec]") {
    using T = std::tuple<double, double>;
    std::vector<T> const v{{1., 2.}, {3., 4.}, {5., 6.}};
    auto [sx, sy] = Gem::Common::sumTupleVec(v);
    REQUIRE(sx == Approx(9.));
    REQUIRE(sy == Approx(12.));
}

TEST_CASE(
    "squareSumTupleVec: sums squares of x- and y-components",
    "[common][math][squareSumTupleVec]"
) {
    using T = std::tuple<double, double>;
    std::vector<T> const v{{1., 2.}, {3., 4.}};
    auto [sqx, sqy] = Gem::Common::squareSumTupleVec(v);
    REQUIRE(sqx == Approx(10.));
    REQUIRE(sqy == Approx(20.));
}

TEST_CASE("productSumTupleVec: sums x*y products", "[common][math][productSumTupleVec]") {
    using T = std::tuple<double, double>;
    std::vector<T> const v{{1., 2.}, {3., 4.}};
    double const ps = Gem::Common::productSumTupleVec(v);
    REQUIRE(ps == Approx(14.));
}

// --- squareDeviation -----------------------------------------------------

TEST_CASE(
    "squareDeviation: points exactly on a line give zero deviation",
    "[common][math][squareDeviation]"
) {
    using T = std::tuple<double, double>;
    std::vector<T> const pts{{0., 1.}, {1., 3.}, {2., 5.}, {3., 7.}};
    REQUIRE(Gem::Common::squareDeviation(pts, 1., 2.) == Approx(0.).margin(1e-10));
}

TEST_CASE(
    "squareDeviation: offset points give positive deviation",
    "[common][math][squareDeviation]"
) {
    using T = std::tuple<double, double>;
    std::vector<T> const pts{{0., 1.}, {1., 2.}, {2., 3.}};
    REQUIRE(Gem::Common::squareDeviation(pts, 0., 1.) == Approx(3.));
}

// --- getRegressionParameters ---------------------------------------------

TEST_CASE(
    "getRegressionParameters: empty data returns zeros",
    "[common][math][getRegressionParameters]"
) {
    using T = std::tuple<double, double>;
    auto [a, sa, b, sb] = Gem::Common::getRegressionParameters(std::vector<T>{});
    REQUIRE(a == Approx(0.));
    REQUIRE(sa == Approx(0.));
    REQUIRE(b == Approx(0.));
    REQUIRE(sb == Approx(0.));
}

TEST_CASE(
    "getRegressionParameters: points on y=2x+1 recover a=1 and b=2",
    "[common][math][getRegressionParameters]"
) {
    using T = std::tuple<double, double>;
    std::vector<T> const pts{{0., 1.}, {1., 3.}, {2., 5.}, {3., 7.}};
    auto [a, sa, b, sb] = Gem::Common::getRegressionParameters(pts);
    REQUIRE(a == Approx(1.).margin(1e-10));
    REQUIRE(b == Approx(2.).margin(1e-10));
    REQUIRE(sa == Approx(0.).margin(1e-10));
    REQUIRE(sb == Approx(0.).margin(1e-10));
}

// --- ratioWithError ------------------------------------------------------

TEST_CASE("ratioWithError: known ratio and propagated error", "[common][math][ratioWithError]") {
    auto [ratio, err] = Gem::Common::ratioWithError(6., 0.3, 2., 0.1);
    REQUIRE(ratio == Approx(3.));
    REQUIRE(err == Approx(0.15 * std::sqrt(2.)));
}

TEST_CASE("ratioWithError: each error contributes on its own", "[common][math][ratioWithError]") {
    // Only the numerator is uncertain: the error is s_error / p
    auto [r1, e1] = Gem::Common::ratioWithError(6., 0.4, 2., 0.);
    CHECK(r1 == Approx(3.));
    CHECK(e1 == Approx(0.2));
    // Only the denominator is uncertain: the error is s * p_error / p^2
    auto [r2, e2] = Gem::Common::ratioWithError(6., 0., 2., 0.2);
    CHECK(r2 == Approx(3.));
    CHECK(e2 == Approx(0.3));
    // Exact quantities have an exact ratio
    auto [r3, e3] = Gem::Common::ratioWithError(1.f, 0.f, 4.f, 0.f);
    CHECK(r3 == Approx(0.25f));
    CHECK(e3 == 0.f);
}

TEST_CASE("ratioWithError: division by zero throws", "[common][math][ratioWithError]") {
    REQUIRE_THROWS_AS(Gem::Common::ratioWithError(4., 0.1, 0., 0.1), gem_exception);
}

// --- getRatioError / getRatioErrors --------------------------------------

TEST_CASE(
    "getRatioError: carries the position and takes the ratio of ratioWithError()",
    "[common][math][getRatioError]"
) {
    auto s = std::tuple<double, double, double, double>{2., 0.5, 6., 0.3};
    auto p = std::tuple<double, double, double, double>{2., 0.5, 2., 0.1};
    auto [x, xe, ratio, err] = Gem::Common::getRatioError(s, p);
    auto [expected_ratio, expected_err] = Gem::Common::ratioWithError(6., 0.3, 2., 0.1);
    REQUIRE(x == Approx(2.));
    REQUIRE(xe == Approx(0.));
    REQUIRE(ratio == Approx(expected_ratio));
    REQUIRE(err == Approx(expected_err));
}

TEST_CASE("getRatioError: division by zero throws", "[common][math][getRatioError]") {
    auto s = std::tuple<double, double, double, double>{1., 0., 4., 0.1};
    auto p = std::tuple<double, double, double, double>{1., 0., 0., 0.1};
    REQUIRE_THROWS_AS(Gem::Common::getRatioError(s, p), gem_exception);
}

TEST_CASE("getRatioError: different positions throw", "[common][math][getRatioError]") {
    auto s = std::tuple<double, double, double, double>{1., 0., 4., 0.1};
    auto p = std::tuple<double, double, double, double>{2., 0., 2., 0.1};
    REQUIRE_THROWS_AS(Gem::Common::getRatioError(s, p), gem_exception);
}

TEST_CASE("getRatioErrors: size mismatch throws", "[common][math][getRatioErrors]") {
    using T4 = std::tuple<double, double, double, double>;
    std::vector<T4> const sn{{1., 0., 2., 0.1}};
    std::vector<T4> const pn{{1., 0., 1., 0.1}, {2., 0., 1., 0.1}};
    REQUIRE_THROWS_AS(Gem::Common::getRatioErrors(sn, pn), gem_exception);
}

TEST_CASE("getRatioErrors: applies getRatioError element-wise", "[common][math][getRatioErrors]") {
    using T4 = std::tuple<double, double, double, double>;
    std::vector<T4> const sn{{1., 0., 4., 0.}, {1., 0., 4., 0.}};
    std::vector<T4> const pn{{1., 0., 2., 0.}, {1., 0., 2., 0.}};
    auto result = Gem::Common::getRatioErrors(sn, pn);
    REQUIRE(result.size() == 2);
    for(const auto &r : result) {
        REQUIRE(std::get<2>(r) == Approx(2.));
    }
}

// --- isClose -------------------------------------------------------------

TEST_CASE("isClose: value within margin returns true", "[common][math][isClose]") {
    REQUIRE(Gem::Common::isClose(0.5, 0.5));
    REQUIRE(Gem::Common::isClose(0.50001, 0.5, 0.001));
    REQUIRE(Gem::Common::isClose(-1e-6, 0., 1e-5));
}

TEST_CASE("isClose: value outside margin returns false", "[common][math][isClose]") {
    REQUIRE_FALSE(Gem::Common::isClose(1.0, 0.0));
    REQUIRE_FALSE(Gem::Common::isClose(0.1, 0.0, 0.05));
}

TEST_CASE("isClose: exact zero target", "[common][math][isClose]") {
    REQUIRE(Gem::Common::isClose(0.));
    REQUIRE_FALSE(Gem::Common::isClose(1.0));
}
