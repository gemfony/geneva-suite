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

#include <array>
#include <chrono>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/GCommonInterfaceT.hpp"
#include "common/GContainerT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GMemberReflectionT.hpp"
#include "common/GReflectiveInterfaceT.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// GToken: counter / success / message accounting

TEST_CASE("GToken: starts empty and reports zero counters", "[common][expectations][GToken]") {
    GToken const t("MyClass", expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
    CHECK(t.getCallerName() == "MyClass");
    CHECK(t.getExpectation() == expectation::EQUALITY);
    CHECK(t.getLimit() == CE_DEF_SIMILARITY_DIFFERENCE);
    CHECK(GToken("MyClass", expectation::FP_SIMILARITY, 1e-2).getLimit() == 1e-2);
    CHECK_FALSE(t.getExpectationStr().empty());
    CHECK(t.getTestCounter() == 0);
    CHECK(t.getSuccessCounter() == 0);
    CHECK(t.expectationMet()); // zero/zero == met
    CHECK(static_cast<bool>(t));
}

TEST_CASE(
    "GToken: incrTestCounter without matching success ⇒ expectationMet false",
    "[common][expectations][GToken]"
) {
    GToken t("X", expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
    t.incrTestCounter();
    CHECK(t.getTestCounter() == 1);
    CHECK(t.getSuccessCounter() == 0);
    CHECK_FALSE(t.expectationMet());
}

TEST_CASE("GToken: equal increments produce a satisfied token", "[common][expectations][GToken]") {
    GToken t("X", expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
    t.incrTestCounter();
    t.incrSuccessCounter();
    CHECK(t.expectationMet());
}

TEST_CASE("GToken: registerErrorMessage accumulates strings", "[common][expectations][GToken]") {
    GToken t("X", expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
    t.registerErrorMessage("alpha");
    t.registerErrorMessage(g_expectation_violation("beta"));
    auto msgs = t.getErrorMessages();
    CHECK(msgs.contains("alpha"));
    CHECK(msgs.contains("beta"));
}

TEST_CASE(
    "GToken: toString and operator<< both produce non-empty output mentioning the caller",
    "[common][expectations][GToken]"
) {
    // toString() returns a short "Expectation was met" status while operator<<
    // emits the full counter dump; both must mention the caller name.
    GToken t("MyClass", expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
    t.incrTestCounter();
    t.incrSuccessCounter();

    std::ostringstream oss;
    oss << t;
    CHECK_FALSE(oss.str().empty());
    CHECK(oss.str().contains("MyClass"));

    CHECK_FALSE(t.toString().empty());
    CHECK(t.toString().contains("MyClass"));
}

TEST_CASE(
    "GToken: evaluate() throws when expectation is not met",
    "[common][expectations][GToken]"
) {
    GToken t("X", expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
    t.incrTestCounter(); // unmet test
    CHECK_THROWS_AS(t.evaluate(), g_expectation_violation);
}

TEST_CASE(
    "GToken: evaluate() does not throw when expectation is met",
    "[common][expectations][GToken]"
) {
    GToken const t("X", expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
    CHECK_NOTHROW(t.evaluate());
}

// ---------------------------------------------------------------------------
// compare(): non-FP basic type

TEST_CASE("compare<int>: equal values + EQUALITY do not throw", "[common][expectations][compare]") {
    CHECK_NOTHROW(compare(5, 5, "a", "b", expectation::EQUALITY));
}

TEST_CASE(
    "compare<int>: unequal values + EQUALITY throw g_expectation_violation",
    "[common][expectations][compare]"
) {
    CHECK_THROWS_AS(compare(5, 6, "a", "b", expectation::EQUALITY), g_expectation_violation);
}

TEST_CASE(
    "compare<int>: unequal values + INEQUALITY do not throw",
    "[common][expectations][compare]"
) {
    CHECK_NOTHROW(compare(5, 6, "a", "b", expectation::INEQUALITY));
}

TEST_CASE("compare<int>: equal values + INEQUALITY throw", "[common][expectations][compare]") {
    CHECK_THROWS_AS(compare(5, 5, "a", "b", expectation::INEQUALITY), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): floating point — limit honoured

TEST_CASE(
    "compare<double>: within limit + FP_SIMILARITY does not throw",
    "[common][expectations][compare]"
) {
    CHECK_NOTHROW(compare(1.0, 1.0 + 1e-7, "a", "b", expectation::FP_SIMILARITY, 1e-5));
}

TEST_CASE(
    "compare<double>: outside limit + FP_SIMILARITY throws",
    "[common][expectations][compare]"
) {
    CHECK_THROWS_AS(
        compare(1.0, 1.5, "a", "b", expectation::FP_SIMILARITY, 1e-5),
        g_expectation_violation
    );
}

// ---------------------------------------------------------------------------
// compare(): std::chrono::duration and time_point

TEST_CASE("compare<duration>: equal durations + EQUALITY pass", "[common][expectations][compare]") {
    using D = std::chrono::seconds;
    CHECK_NOTHROW(compare(D{5}, D{5}, "a", "b", expectation::EQUALITY));
    CHECK_THROWS_AS(compare(D{5}, D{6}, "a", "b", expectation::EQUALITY), g_expectation_violation);
}

TEST_CASE(
    "compare<time_point>: equal time_points + EQUALITY pass",
    "[common][expectations][compare]"
) {
    using TP = std::chrono::high_resolution_clock::time_point;
    TP const t1 = TP::clock::now();
    TP const t2 = t1;
    CHECK_NOTHROW(compare(t1, t2, "a", "b", expectation::EQUALITY));
}

// ---------------------------------------------------------------------------
// compare(): tribool — an enum, compared as a value.

TEST_CASE("compare<tribool>: equal values + EQUALITY pass", "[common][expectations][compare]") {
    CHECK_NOTHROW(compare(tribool::True, tribool::True, "a", "b", expectation::EQUALITY));
    CHECK_THROWS_AS(
        compare(tribool::True, tribool::False, "a", "b", expectation::EQUALITY),
        g_expectation_violation
    );
}

// ---------------------------------------------------------------------------
// identity / IDENTITY macro

TEST_CASE(
    "identity / getIdentity: stores references and names",
    "[common][expectations][identity]"
) {
    int a = 1;
    int b = 2;
    auto id = getIdentity(a, b, "a", "b");
    CHECK(&id.x == &a);
    CHECK(&id.y == &b);
    CHECK(id.x_name == "a");
    CHECK(id.y_name == "b");
}

TEST_CASE("identity: stream-out names the items", "[common][expectations][identity]") {
    int const a = 1;
    int const b = 2;
    auto id = getIdentity(a, b, "a", "b");
    std::ostringstream oss;
    oss << id;
    CHECK(oss.str().contains("a"));
    CHECK(oss.str().contains("b"));
}

// ---------------------------------------------------------------------------
// compare_t: hands off to compare() and bookkeeps the token

TEST_CASE(
    "compare_t<int>: matching values increment success counter",
    "[common][expectations][compare_t]"
) {
    GToken tok("X", expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
    int const a = 7;
    int const b = 7;
    compare_t(getIdentity(a, b, "a", "b"), tok);
    CHECK(tok.getTestCounter() == 1);
    CHECK(tok.getSuccessCounter() == 1);
    CHECK(tok.expectationMet());
}

TEST_CASE(
    "compare_t<int>: mismatched values are recorded as failures instead of thrown",
    "[common][expectations][compare_t]"
) {
    // compare_t catches g_expectation_violation and records into the token —
    // the call must NOT propagate.
    GToken tok("X", expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
    int const a = 7;
    int const b = 8;
    CHECK_NOTHROW(compare_t(getIdentity(a, b, "a", "b"), tok));
    CHECK(tok.getTestCounter() == 1);
    CHECK(tok.getSuccessCounter() == 0);
    CHECK_FALSE(tok.expectationMet());
    CHECK_FALSE(tok.getErrorMessages().empty());
}

// ---------------------------------------------------------------------------
// compare(): vector<fp> — element-wise comparison with tolerance.

TEST_CASE(
    "compare<vector<double>>: equal vectors + EQUALITY pass",
    "[common][expectations][compare]"
) {
    std::vector<double> const a{1.0, 2.0, 3.0};
    std::vector<double> const b{1.0, 2.0, 3.0};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY));
}

TEST_CASE(
    "compare<vector<double>>: vectors of different sizes + EQUALITY violate",
    "[common][expectations][compare]"
) {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0, 2.0, 3.0};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY), g_expectation_violation);
}

TEST_CASE(
    "compare<vector<double>>: per-element similarity within limit",
    "[common][expectations][compare]"
) {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0 + 1e-7, 2.0 + 1e-7};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::FP_SIMILARITY, 1e-5));
}

TEST_CASE(
    "compare<vector<double>>: per-element similarity outside limit fails",
    "[common][expectations][compare]"
) {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0, 2.5}; // diff > limit
    CHECK_THROWS_AS(
        compare(a, b, "a", "b", expectation::FP_SIMILARITY, 1e-5),
        g_expectation_violation
    );
}

TEST_CASE(
    "compare<vector<double>>: INEQUALITY satisfied when contents differ",
    "[common][expectations][compare]"
) {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0, 9.0};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY));
}

TEST_CASE(
    "compare<vector<double>>: INEQUALITY violated when contents match",
    "[common][expectations][compare]"
) {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0, 2.0};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): a container of containers, compared row by row.

TEST_CASE(
    "compare<vector<vector<double>>>: rows are compared with the similarity limit",
    "[common][expectations][compare]"
) {
    std::vector<std::vector<double>> const a{{1.0, 2.0}, {3.0}};
    std::vector<std::vector<double>> const near{{1.0 + 1e-7, 2.0}, {3.0}};
    std::vector<std::vector<double>> const far{{1.0, 2.0}, {3.5}};
    std::vector<std::vector<double>> const short_rows{{1.0, 2.0}};
    CHECK_NOTHROW(compare(a, near, "a", "near", expectation::FP_SIMILARITY, 1e-5));
    CHECK_THROWS_AS(
        compare(a, far, "a", "far", expectation::FP_SIMILARITY, 1e-5),
        g_expectation_violation
    );
    CHECK_THROWS_AS(
        compare(a, short_rows, "a", "short_rows", expectation::EQUALITY),
        g_expectation_violation
    );
    CHECK_NOTHROW(compare(a, far, "a", "far", expectation::INEQUALITY));
    CHECK_THROWS_AS(compare(a, a, "a", "a", expectation::INEQUALITY), g_expectation_violation);
}

TEST_CASE(
    "compare<vector<vector<bool>>>: a differing row is reported by its index",
    "[common][expectations][compare]"
) {
    std::vector<std::vector<bool>> const a{{true, false}, {false, false}};
    std::vector<std::vector<bool>> const b{{true, false}, {false, true}};
    CHECK_NOTHROW(compare(a, a, "a", "a", expectation::EQUALITY));
    try {
        compare(a, b, "a", "b", expectation::EQUALITY);
        FAIL("the differing row was not found");
    }
    catch(g_expectation_violation const &e) {
        CHECK(std::string(e.what()).find("a[1]") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// compare(): GCommonInterfaceT-derived objects via the gem_type overload.
// A tiny GCommonInterfaceT-derivative is enough.

namespace {

class CmpObj : public GCommonInterfaceT<CmpObj> {
public:
    CmpObj() = default;
    explicit CmpObj(int v)
      : v_(v) {
    }
    [[nodiscard]] int v() const {
        return v_;
    }

protected:
    void load_(CmpObj const *cp) override {
        if(cp)
            v_ = cp->v_;
    }
    void compare_(CmpObj const &cp, expectation const &e, double const &limit) const override {
        GToken token("CmpObj", e, limit);
        compare_base_t<GCommonInterfaceT<CmpObj>>(*this, cp, token);
        compare_t(Gem::Common::getIdentity(v_, cp.v_, "v_", "cp.v_"), token);
        token.evaluate();
    }

private:
    [[nodiscard]] CmpObj *clone_() const override {
        return new CmpObj(*this);
    }

    int v_{0};
};

} // namespace

TEST_CASE(
    "compare<gem_type>: equal Geneva objects + EQUALITY pass",
    "[common][expectations][compare]"
) {
    CmpObj const a(5);
    CmpObj const b(5);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY, 0.));
}

TEST_CASE(
    "compare<gem_type>: unequal Geneva objects + EQUALITY fail",
    "[common][expectations][compare]"
) {
    CmpObj const a(5);
    CmpObj const b(6);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.), g_expectation_violation);
}

TEST_CASE(
    "compare<gem_type>: unequal Geneva objects + INEQUALITY pass",
    "[common][expectations][compare]"
) {
    CmpObj const a(5);
    CmpObj const b(6);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY, 0.));
}

TEST_CASE(
    "compare<gem_type>: equal Geneva objects + INEQUALITY fail",
    "[common][expectations][compare]"
) {
    CmpObj const a(5);
    CmpObj const b(5);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY, 0.), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): a shared_ptr<gem_type>, through its pointee.

TEST_CASE(
    "compare<shared_ptr<gem_type>>: both null + EQUALITY pass",
    "[common][expectations][compare]"
) {
    std::shared_ptr<CmpObj> const a;
    std::shared_ptr<CmpObj> const b;
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY, 0.));
}

TEST_CASE(
    "compare<shared_ptr<gem_type>>: one null one non-null + EQUALITY fail",
    "[common][expectations][compare]"
) {
    std::shared_ptr<CmpObj> const a;
    auto b = std::make_shared<CmpObj>(1);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.), g_expectation_violation);
}

TEST_CASE(
    "compare<shared_ptr<gem_type>>: matching contents + EQUALITY pass",
    "[common][expectations][compare]"
) {
    auto a = std::make_shared<CmpObj>(7);
    auto b = std::make_shared<CmpObj>(7);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY, 0.));
}

TEST_CASE(
    "compare<shared_ptr<gem_type>>: differing contents + EQUALITY fail",
    "[common][expectations][compare]"
) {
    auto a = std::make_shared<CmpObj>(7);
    auto b = std::make_shared<CmpObj>(9);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): a vector<shared_ptr<gem_type>>, element by element through the pointees.

TEST_CASE(
    "compare<vector<shared_ptr<gem_type>>>: identical contents + EQUALITY pass",
    "[common][expectations][compare]"
) {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(2)
    };
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(2)
    };
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY, 0.));
}

TEST_CASE(
    "compare<vector<shared_ptr<gem_type>>>: differing sizes + EQUALITY fail",
    "[common][expectations][compare]"
) {
    std::vector<std::shared_ptr<CmpObj>> const a{std::make_shared<CmpObj>(1)};
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(2)
    };
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.), g_expectation_violation);
}

TEST_CASE(
    "compare<vector<shared_ptr<gem_type>>>: element diff + EQUALITY fail",
    "[common][expectations][compare]"
) {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(2)
    };
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(99)
    };
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.), g_expectation_violation);
}

TEST_CASE(
    "compare<vector<shared_ptr<gem_type>>>: INEQUALITY satisfied when one differs",
    "[common][expectations][compare]"
) {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(2)
    };
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(99)
    };
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY, 0.));
}

TEST_CASE(
    "compare<vector<shared_ptr<gem_type>>>: nullptr vs non-null element + EQUALITY fail",
    "[common][expectations][compare]"
) {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::shared_ptr<CmpObj>(),
        std::make_shared<CmpObj>(2)
    };
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(2)
    };
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): FP_SIMILARITY on a non-FP type (treated as EQUALITY)

TEST_CASE(
    "compare<int>: FP_SIMILARITY on equal ints passes (treated as EQUALITY)",
    "[common][expectations][compare]"
) {
    CHECK_NOTHROW(compare(7, 7, "a", "b", expectation::FP_SIMILARITY));
}

// ---------------------------------------------------------------------------
// compare(): std::chrono::duration INEQUALITY cases

TEST_CASE(
    "compare<duration>: unequal durations + INEQUALITY pass",
    "[common][expectations][compare]"
) {
    using D = std::chrono::seconds;
    CHECK_NOTHROW(compare(D{5}, D{6}, "a", "b", expectation::INEQUALITY));
}

TEST_CASE(
    "compare<duration>: equal durations + INEQUALITY throw",
    "[common][expectations][compare]"
) {
    using D = std::chrono::seconds;
    CHECK_THROWS_AS(
        compare(D{5}, D{5}, "a", "b", expectation::INEQUALITY),
        g_expectation_violation
    );
}

// ---------------------------------------------------------------------------
// compare(): std::chrono::time_point INEQUALITY cases

TEST_CASE(
    "compare<time_point>: different time_points + INEQUALITY pass",
    "[common][expectations][compare]"
) {
    using TP = std::chrono::high_resolution_clock::time_point;
    TP const t1 = TP::clock::now();
    TP const t2 = t1 + std::chrono::nanoseconds{1};
    CHECK_NOTHROW(compare(t1, t2, "a", "b", expectation::INEQUALITY));
}

TEST_CASE(
    "compare<time_point>: equal time_points + INEQUALITY throw",
    "[common][expectations][compare]"
) {
    using TP = std::chrono::high_resolution_clock::time_point;
    TP const t1 = TP::clock::now();
    TP const t2 = t1;
    CHECK_THROWS_AS(compare(t1, t2, "a", "b", expectation::INEQUALITY), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): a container of values (vector<int>)

TEST_CASE(
    "compare<vector<int>>: equal vectors + EQUALITY pass",
    "[common][expectations][compare]"
) {
    std::vector<int> const a{1, 2, 3};
    std::vector<int> const b{1, 2, 3};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY));
}

TEST_CASE(
    "compare<vector<int>>: size mismatch + EQUALITY throws with diagnostic",
    "[common][expectations][compare]"
) {
    std::vector<int> const a{1, 2};
    std::vector<int> const b{1, 2, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY), g_expectation_violation);
}

TEST_CASE(
    "compare<vector<int>>: an element mismatch at the same size throws under EQUALITY",
    "[common][expectations][compare]"
) {
    std::vector<int> const a{1, 2, 3};
    std::vector<int> const b{1, 99, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY), g_expectation_violation);
}

TEST_CASE(
    "compare<vector<int>>: different vectors + INEQUALITY pass",
    "[common][expectations][compare]"
) {
    std::vector<int> const a{1, 2, 3};
    std::vector<int> const b{1, 99, 3};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY));
}

TEST_CASE(
    "compare<vector<int>>: equal vectors + INEQUALITY throws",
    "[common][expectations][compare]"
) {
    std::vector<int> const a{1, 2, 3};
    std::vector<int> const b{1, 2, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): a sorted container (set<int>)

TEST_CASE("compare<set<int>>: equal sets + EQUALITY pass", "[common][expectations][compare]") {
    std::set<int> const a{1, 2, 3};
    std::set<int> const b{1, 2, 3};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY));
}

TEST_CASE(
    "compare<set<int>>: sets of different sizes + EQUALITY throw",
    "[common][expectations][compare]"
) {
    std::set<int> const a{1, 2};
    std::set<int> const b{1, 2, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY), g_expectation_violation);
}

TEST_CASE(
    "compare<set<int>>: different elements at the same size throw under EQUALITY",
    "[common][expectations][compare]"
) {
    std::set<int> const a{1, 2, 3};
    std::set<int> const b{1, 2, 4};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY), g_expectation_violation);
}

TEST_CASE(
    "compare<set<int>>: different sets + INEQUALITY pass",
    "[common][expectations][compare]"
) {
    std::set<int> const a{1, 2, 3};
    std::set<int> const b{1, 2, 4};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY));
}

TEST_CASE("compare<set<int>>: equal sets + INEQUALITY throw", "[common][expectations][compare]") {
    std::set<int> const a{1, 2, 3};
    std::set<int> const b{1, 2, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): shared_ptr<gem_type> INEQUALITY cases

TEST_CASE(
    "compare<shared_ptr<gem_type>>: both null + INEQUALITY throw",
    "[common][expectations][compare]"
) {
    std::shared_ptr<CmpObj> const a;
    std::shared_ptr<CmpObj> const b;
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY, 0.), g_expectation_violation);
}

TEST_CASE(
    "compare<shared_ptr<gem_type>>: one null one non-null + INEQUALITY pass",
    "[common][expectations][compare]"
) {
    std::shared_ptr<CmpObj> const a;
    auto b = std::make_shared<CmpObj>(1);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY, 0.));
}

TEST_CASE(
    "compare<shared_ptr<gem_type>>: differing contents + INEQUALITY pass",
    "[common][expectations][compare]"
) {
    auto a = std::make_shared<CmpObj>(7);
    auto b = std::make_shared<CmpObj>(9);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY, 0.));
}

TEST_CASE(
    "compare<shared_ptr<gem_type>>: matching contents + INEQUALITY throw",
    "[common][expectations][compare]"
) {
    auto a = std::make_shared<CmpObj>(7);
    auto b = std::make_shared<CmpObj>(7);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY, 0.), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): vector<shared_ptr<gem_type>> INEQUALITY — identical containers should throw

TEST_CASE(
    "compare<vector<shared_ptr<gem_type>>>: identical containers + INEQUALITY throw",
    "[common][expectations][compare]"
) {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(2)
    };
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(2)
    };
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY, 0.), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare() over containers of every shape and element kind: each with an expected equality and an
// expected difference, under EQUALITY / FP_SIMILARITY and under INEQUALITY.

namespace {

/** Checks that compare() of a and b meets EQUALITY and fails INEQUALITY. */
template <typename T>
void checkEqual(T const &a, T const &b) {
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY));
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::FP_SIMILARITY));
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY), g_expectation_violation);
}

/** Checks that compare() of a and b fails EQUALITY and FP_SIMILARITY and meets INEQUALITY. */
template <typename T>
void checkDifferent(T const &a, T const &b) {
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY), g_expectation_violation);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::FP_SIMILARITY), g_expectation_violation);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY));
}

/** Returns the message of the violation compare() of a and b throws under the expectation e. */
template <typename T>
std::string violationOf(T const &a, T const &b, expectation e, double limit) {
    try {
        compare(a, b, "a", "b", e, limit);
    }
    catch(g_expectation_violation const &g) {
        return g.what();
    }
    return {};
}

} // namespace

TEST_CASE(
    "compare<deque / list of values>: equal and different",
    "[common][expectations][compare]"
) {
    checkEqual(std::deque<int>{1, 2, 3}, std::deque<int>{1, 2, 3});
    checkDifferent(std::deque<int>{1, 2, 3}, std::deque<int>{1, 2, 4});
    checkDifferent(std::deque<int>{1, 2, 3}, std::deque<int>{1, 2});
    checkEqual(std::list<int>{1, 2, 3}, std::list<int>{1, 2, 3});
    checkDifferent(std::list<int>{1, 2, 3}, std::list<int>{0, 2, 3});
    checkEqual(std::vector<std::string>{"x", "y"}, std::vector<std::string>{"x", "y"});
    checkDifferent(std::vector<std::string>{"x", "y"}, std::vector<std::string>{"x", "z"});
    checkEqual(std::vector<bool>{true, false}, std::vector<bool>{true, false});
    checkDifferent(std::vector<bool>{true, false}, std::vector<bool>{true, true});
}

TEST_CASE(
    "compare<deque / list / set of floating point>: tolerance only under FP_SIMILARITY",
    "[common][expectations][compare]"
) {
    std::deque<double> const d{1.0, 2.0};
    std::deque<double> const d_near{1.0, 2.0 + 1e-7};
    checkEqual(d, d);
    CHECK_NOTHROW(compare(d, d_near, "a", "b", expectation::FP_SIMILARITY, 1e-5));
    CHECK_THROWS_AS(compare(d, d_near, "a", "b", expectation::EQUALITY), g_expectation_violation);
    CHECK_NOTHROW(compare(d, d_near, "a", "b", expectation::INEQUALITY));
    checkDifferent(d, std::deque<double>{1.0, 3.0});

    std::list<float> const l{1.f, 2.f};
    checkEqual(l, l);
    checkDifferent(l, std::list<float>{1.f, 2.5f});
    checkDifferent(l, std::list<float>{1.f});

    std::set<double> const s{1.0, 2.0};
    std::set<double> const s_near{1.0, 2.0 + 1e-7};
    checkEqual(s, std::set<double>{1.0, 2.0});
    CHECK_NOTHROW(compare(s, s_near, "a", "b", expectation::FP_SIMILARITY, 1e-5));
    CHECK_THROWS_AS(compare(s, s_near, "a", "b", expectation::EQUALITY), g_expectation_violation);
    checkDifferent(s, std::set<double>{1.0, 2.5});
}

TEST_CASE(
    "compare<vector<double>>: a deviation beyond the limit names its position",
    "[common][expectations][compare]"
) {
    std::vector<double> const a{1.0, 2.0, 3.0};
    std::vector<double> const b{1.0, 2.0, 3.5};
    CHECK(violationOf(a, b, expectation::FP_SIMILARITY, 1e-5).contains("a[2]"));
    CHECK(violationOf(a, b, expectation::EQUALITY, 1e-5).contains("b[2]"));
    CHECK(violationOf(a, a, expectation::INEQUALITY, 1e-5).contains("a"));
}

TEST_CASE(
    "compare<deque<vector<int>>>: rows are compared one by one",
    "[common][expectations][compare]"
) {
    std::deque<std::vector<int>> const a{{1, 2}, {3}};
    checkEqual(a, std::deque<std::vector<int>>{{1, 2}, {3}});
    checkDifferent(a, std::deque<std::vector<int>>{{1, 2}, {4}});
    checkDifferent(a, std::deque<std::vector<int>>{{1, 2}, {3, 4}});
    checkDifferent(a, std::deque<std::vector<int>>{{1, 2}});
}

TEST_CASE(
    "compare<unique_ptr<gem_type>>: null and content are compared like a shared_ptr",
    "[common][expectations][compare]"
) {
    std::unique_ptr<CmpObj> const null_a;
    std::unique_ptr<CmpObj> const null_b;
    CHECK_NOTHROW(compare(null_a, null_b, "a", "b", expectation::EQUALITY));
    CHECK_THROWS_AS(
        compare(null_a, null_b, "a", "b", expectation::INEQUALITY),
        g_expectation_violation
    );
    auto const one = std::make_unique<CmpObj>(1);
    auto const one_again = std::make_unique<CmpObj>(1);
    auto const two = std::make_unique<CmpObj>(2);
    checkEqual(one, one_again);
    checkDifferent(one, two);
    checkDifferent(one, null_a);
}

TEST_CASE(
    "compare<containers of smart pointers to gem_type>: pointees are compared",
    "[common][expectations][compare]"
) {
    std::vector<std::unique_ptr<CmpObj>> u_a;
    std::vector<std::unique_ptr<CmpObj>> u_b;
    std::vector<std::unique_ptr<CmpObj>> u_c;
    for(int const v : {1, 2}) {
        u_a.push_back(std::make_unique<CmpObj>(v));
        u_b.push_back(std::make_unique<CmpObj>(v));
        u_c.push_back(std::make_unique<CmpObj>(v * 10));
    }
    checkEqual(u_a, u_b);
    checkDifferent(u_a, u_c);
    u_b.push_back(nullptr);
    checkDifferent(u_a, u_b);

    std::deque<std::shared_ptr<CmpObj>> const s_a{std::make_shared<CmpObj>(1), nullptr};
    std::deque<std::shared_ptr<CmpObj>> const s_b{std::make_shared<CmpObj>(1), nullptr};
    std::deque<std::shared_ptr<CmpObj>> const s_c{
        std::make_shared<CmpObj>(1),
        std::make_shared<CmpObj>(1)
    };
    checkEqual(s_a, s_b);
    checkDifferent(s_a, s_c);
    CHECK(violationOf(s_a, s_c, expectation::EQUALITY, 0.).contains("a[1]"));
}

TEST_CASE(
    "compare<vector<gem_type>>: objects are compared through their own compare",
    "[common][expectations][compare]"
) {
    checkEqual(
        std::vector<CmpObj>{CmpObj(1), CmpObj(2)},
        std::vector<CmpObj>{CmpObj(1), CmpObj(2)}
    );
    checkDifferent(
        std::vector<CmpObj>{CmpObj(1), CmpObj(2)},
        std::vector<CmpObj>{CmpObj(1), CmpObj(3)}
    );
}

TEST_CASE(
    "compare<array>: values and floating point numbers element by element",
    "[common][expectations][compare]"
) {
    checkEqual(std::array<int, 3>{1, 2, 3}, std::array<int, 3>{1, 2, 3});
    checkDifferent(std::array<int, 3>{1, 2, 3}, std::array<int, 3>{1, 2, 4});
    std::array<double, 2> const a{1.0, 2.0};
    std::array<double, 2> const a_near{1.0, 2.0 + 1e-7};
    CHECK_NOTHROW(compare(a, a_near, "a", "b", expectation::FP_SIMILARITY, 1e-5));
    CHECK_THROWS_AS(compare(a, a_near, "a", "b", expectation::EQUALITY), g_expectation_violation);
    checkDifferent(a, std::array<double, 2>{1.0, 2.5});
}

TEST_CASE(
    "compare: a floating point deviation reports the limit and a size difference both sizes",
    "[common][expectations][compare]"
) {
    std::vector<double> const a{1.0, 2.0};
    std::string const deviation =
        violationOf(a, std::vector<double>{1.0, 2.5}, expectation::FP_SIMILARITY, 1e-5);
    CHECK(deviation.contains("limit"));
    CHECK(deviation.contains("deviation"));
    std::string const sizes = violationOf(a, std::vector<double>{1.0}, expectation::EQUALITY, 0.);
    CHECK(sizes.contains("a.size() = 2"));
    CHECK(sizes.contains("b.size() = 1"));
}

TEST_CASE("compare<tribool>: compared as a value", "[common][expectations][compare]") {
    checkEqual(tribool::Indeterminate, tribool::Indeterminate);
    checkDifferent(tribool::True, tribool::Indeterminate);
}

// ---------------------------------------------------------------------------
// The limit a caller passes to compare() is applied to every floating point member: of the class
// itself, of its parent slice, inside a container member, and in GContainerT::compare_base().

namespace {

class LimitRoot : public GReflectiveInterfaceT<LimitRoot, GCommonInterfaceT<LimitRoot>> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(make_member("root_d_", self.root_d_));
    }

public:
    static constexpr std::string_view class_name = "LimitRoot";

    double root_d_ = 1.0;
};

class LimitDerived : public GReflectiveInterfaceT<LimitDerived, LimitRoot> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(make_member("d_", self.d_), make_member("v_", self.v_));
    }

public:
    static constexpr std::string_view class_name = "LimitDerived";

    double d_ = 2.0;
    std::vector<double> v_{3.0, 4.0};
};

class LimitContainer : public GPodContainerT<double> {
public:
    explicit LimitContainer(std::vector<double> const &values) {
        for(double const v : values) {
            this->push_back(v);
        }
    }
    ~LimitContainer() override = default;
};

/** Checks that x and y, which deviate by 1e-3 in one place, are similar within 1e-2, not 1e-6. */
template <typename T>
void checkLimitApplied(T const &x, T const &y) {
    CHECK_NOTHROW(x.compare(y, expectation::FP_SIMILARITY, 1e-2));
    CHECK_THROWS_AS(x.compare(y, expectation::FP_SIMILARITY, 1e-6), g_expectation_violation);
    CHECK_NOTHROW(compare(x, y, "x", "y", expectation::FP_SIMILARITY, 1e-2));
    CHECK_THROWS_AS(
        compare(x, y, "x", "y", expectation::FP_SIMILARITY, 1e-6),
        g_expectation_violation
    );
}

} // namespace

TEST_CASE(
    "compare: the limit a caller passes applies to every floating point member",
    "[common][expectations][compare][limit]"
) {
    LimitDerived const x;

    SECTION("a member of the class itself") {
        LimitDerived y;
        y.d_ += 1e-3;
        checkLimitApplied(x, y);
    }

    SECTION("a member of the parent slice") {
        LimitDerived y;
        y.root_d_ += 1e-3;
        checkLimitApplied(x, y);
    }

    SECTION("an element of a container member") {
        LimitDerived y;
        y.v_[1] += 1e-3;
        checkLimitApplied(x, y);
    }
}

TEST_CASE(
    "GContainerT::compare_base: the limit a caller passes applies to the elements",
    "[common][expectations][compare][limit]"
) {
    LimitContainer const x({1.0, 2.0});
    LimitContainer const y({1.0, 2.0 + 1e-3});
    CHECK_NOTHROW(x.compare_base(y, expectation::FP_SIMILARITY, 1e-2));
    CHECK_THROWS_AS(x.compare_base(y, expectation::FP_SIMILARITY, 1e-6), g_expectation_violation);
}

TEST_CASE(
    "compare_t: the comparison takes its limit from the token",
    "[common][expectations][compare_t][limit]"
) {
    double const a = 1.0;
    double const b = 1.0 + 1e-3;
    GToken wide("X", expectation::FP_SIMILARITY, 1e-2);
    compare_t(getIdentity(a, b, "a", "b"), wide);
    CHECK(wide.expectationMet());
    GToken narrow_token("X", expectation::FP_SIMILARITY, 1e-6);
    compare_t(getIdentity(a, b, "a", "b"), narrow_token);
    CHECK_FALSE(narrow_token.expectationMet());
}

// ---------------------------------------------------------------------------
// compare() checks the equality of object states: two NaN are equal, a NaN differs from a number.
// Two equal infinities are similar as well (their difference is NaN).

TEST_CASE(
    "compare<double>: two NaN are equal; a NaN and a number differ",
    "[common][expectations][compare][nan]"
) {
    double const nan = std::numeric_limits<double>::quiet_NaN();
    checkEqual(nan, nan);
    checkDifferent(nan, 1.0);
    checkDifferent(1.0, nan);
    checkEqual(std::vector<double>{1.0, nan}, std::vector<double>{1.0, nan});
    checkDifferent(std::vector<double>{1.0, nan}, std::vector<double>{1.0, 2.0});

    float const nan_f = std::numeric_limits<float>::quiet_NaN();
    checkEqual(nan_f, nan_f);
    checkDifferent(nan_f, 0.f);
}

TEST_CASE(
    "compare<double>: an object holding a NaN equals its copy",
    "[common][expectations][compare][nan]"
) {
    LimitDerived x;
    x.d_ = std::numeric_limits<double>::quiet_NaN();
    LimitDerived const copy(x);
    CHECK_NOTHROW(x.compare(copy, expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE));
    CHECK_THROWS_AS(
        x.compare(copy, expectation::INEQUALITY, CE_DEF_SIMILARITY_DIFFERENCE),
        g_expectation_violation
    );
    LimitDerived const number;
    CHECK_THROWS_AS(
        x.compare(number, expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE),
        g_expectation_violation
    );
}

TEST_CASE(
    "compare<double>: equal infinities are equal and similar; opposite ones differ",
    "[common][expectations][compare][nan]"
) {
    double const inf = std::numeric_limits<double>::infinity();
    checkEqual(inf, inf);
    checkEqual(-inf, -inf);
    checkDifferent(inf, -inf);
    checkDifferent(inf, 1.0);
}

// ---------------------------------------------------------------------------
// An unordered container is compared with its ==: two equal ones with different bucket counts iterate
// in different orders and are still equal.

TEST_CASE(
    "compare<unordered containers>: equal contents in another bucket order are equal",
    "[common][expectations][compare][unordered]"
) {
    std::unordered_set<int> a;
    std::unordered_set<int> b;
    b.reserve(1000);
    for(int i = 0; i < 20; ++i) {
        a.insert(i);
        b.insert(19 - i);
    }
    checkEqual(a, b);
    std::unordered_set<int> c(a);
    c.erase(7);
    c.insert(70);
    checkDifferent(a, c);
    c.erase(70);
    checkDifferent(a, c);

    std::unordered_map<int, std::string> m;
    std::unordered_map<int, std::string> n;
    n.reserve(1000);
    for(int i = 0; i < 20; ++i) {
        m.emplace(i, std::to_string(i));
        n.emplace(19 - i, std::to_string(19 - i));
    }
    checkEqual(m, n);
    n[3] = "three";
    checkDifferent(m, n);

    std::vector<std::unordered_set<int>> const nested_a{a};
    std::vector<std::unordered_set<int>> const nested_b{b};
    checkEqual(nested_a, nested_b);
}

TEST_CASE(
    "compare<map>: pairs that cannot be printed are compared and reported",
    "[common][expectations][compare]"
) {
    std::map<int, std::string> const a{{1, "one"}, {2, "two"}};
    checkEqual(a, std::map<int, std::string>{{1, "one"}, {2, "two"}});
    checkDifferent(a, std::map<int, std::string>{{1, "one"}, {2, "zwei"}});
    checkDifferent(a, std::map<int, std::string>{{1, "one"}});
    std::string const message = violationOf(
        a,
        std::map<int, std::string>{{1, "one"}, {2, "zwei"}},
        expectation::EQUALITY,
        0.
    );
    CHECK(message.contains("a[1]"));
    CHECK(message.contains("cannot be printed"));
}
