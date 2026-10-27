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

#include <chrono>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// GToken: counter / success / message accounting

TEST_CASE("GToken: starts empty and reports zero counters", "[common][expectations][GToken]") {
    GToken const t("MyClass", expectation::EQUALITY);
    CHECK(t.getCallerName() == "MyClass");
    CHECK(t.getExpectation() == expectation::EQUALITY);
    CHECK_FALSE(t.getExpectationStr().empty());
    CHECK(t.getTestCounter() == 0);
    CHECK(t.getSuccessCounter() == 0);
    CHECK(t.expectationMet());          // zero/zero == met
    CHECK(static_cast<bool>(t));
}

TEST_CASE("GToken: incrTestCounter without matching success ⇒ expectationMet false",
          "[common][expectations][GToken]") {
    GToken t("X", expectation::EQUALITY);
    t.incrTestCounter();
    CHECK(t.getTestCounter() == 1);
    CHECK(t.getSuccessCounter() == 0);
    CHECK_FALSE(t.expectationMet());
}

TEST_CASE("GToken: equal increments produce a satisfied token",
          "[common][expectations][GToken]") {
    GToken t("X", expectation::EQUALITY);
    t.incrTestCounter();
    t.incrSuccessCounter();
    CHECK(t.expectationMet());
}

TEST_CASE("GToken: registerErrorMessage accumulates strings",
          "[common][expectations][GToken]") {
    GToken t("X", expectation::EQUALITY);
    t.registerErrorMessage("alpha");
    t.registerErrorMessage(g_expectation_violation("beta"));
    auto msgs = t.getErrorMessages();
    CHECK(msgs.contains("alpha"));
    CHECK(msgs.contains("beta"));
}

TEST_CASE("GToken: toString and operator<< both produce non-empty output mentioning the caller",
          "[common][expectations][GToken]") {
    // toString() returns a short "Expectation was met" status while operator<<
    // emits the full counter dump; both must mention the caller name.
    GToken t("MyClass", expectation::EQUALITY);
    t.incrTestCounter();
    t.incrSuccessCounter();

    std::ostringstream oss;
    oss << t;
    CHECK_FALSE(oss.str().empty());
    CHECK(oss.str().contains("MyClass"));

    CHECK_FALSE(t.toString().empty());
    CHECK(t.toString().contains("MyClass"));
}

TEST_CASE("GToken: evaluate() throws when expectation is not met",
          "[common][expectations][GToken]") {
    GToken t("X", expectation::EQUALITY);
    t.incrTestCounter();        // unmet test
    CHECK_THROWS_AS(t.evaluate(), g_expectation_violation);
}

TEST_CASE("GToken: evaluate() does not throw when expectation is met",
          "[common][expectations][GToken]") {
    GToken const t("X", expectation::EQUALITY);
    CHECK_NOTHROW(t.evaluate());
}

// ---------------------------------------------------------------------------
// compare(): non-FP basic type

TEST_CASE("compare<int>: equal values + EQUALITY do not throw",
          "[common][expectations][compare]") {
    CHECK_NOTHROW(compare(5, 5, "a", "b", expectation::EQUALITY));
}

TEST_CASE("compare<int>: unequal values + EQUALITY throw g_expectation_violation",
          "[common][expectations][compare]") {
    CHECK_THROWS_AS(compare(5, 6, "a", "b", expectation::EQUALITY),
                    g_expectation_violation);
}

TEST_CASE("compare<int>: unequal values + INEQUALITY do not throw",
          "[common][expectations][compare]") {
    CHECK_NOTHROW(compare(5, 6, "a", "b", expectation::INEQUALITY));
}

TEST_CASE("compare<int>: equal values + INEQUALITY throw",
          "[common][expectations][compare]") {
    CHECK_THROWS_AS(compare(5, 5, "a", "b", expectation::INEQUALITY),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): floating point — limit honoured

TEST_CASE("compare<double>: within limit + FP_SIMILARITY does not throw",
          "[common][expectations][compare]") {
    CHECK_NOTHROW(compare(1.0, 1.0 + 1e-7, "a", "b",
                          expectation::FP_SIMILARITY, 1e-5));
}

TEST_CASE("compare<double>: outside limit + FP_SIMILARITY throws",
          "[common][expectations][compare]") {
    CHECK_THROWS_AS(
        compare(1.0, 1.5, "a", "b", expectation::FP_SIMILARITY, 1e-5),
        g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): std::chrono::duration and time_point

TEST_CASE("compare<duration>: equal durations + EQUALITY pass",
          "[common][expectations][compare]") {
    using D = std::chrono::seconds;
    CHECK_NOTHROW(compare(D{5}, D{5}, "a", "b", expectation::EQUALITY));
    CHECK_THROWS_AS(compare(D{5}, D{6}, "a", "b", expectation::EQUALITY),
                    g_expectation_violation);
}

TEST_CASE("compare<time_point>: equal time_points + EQUALITY pass",
          "[common][expectations][compare]") {
    using TP = std::chrono::high_resolution_clock::time_point;
    TP const t1 = TP::clock::now();
    TP const t2 = t1;
    CHECK_NOTHROW(compare(t1, t2, "a", "b", expectation::EQUALITY));
}

// ---------------------------------------------------------------------------
// compare(): tribool — special overload, declared in the header.

TEST_CASE("compare<tribool>: equal values + EQUALITY pass",
          "[common][expectations][compare]") {
    CHECK_NOTHROW(compare(tribool::True, tribool::True, "a", "b",
                          expectation::EQUALITY));
    CHECK_THROWS_AS(
        compare(tribool::True, tribool::False, "a", "b", expectation::EQUALITY),
        g_expectation_violation);
}

// ---------------------------------------------------------------------------
// identity / IDENTITY macro

TEST_CASE("identity / getIdentity: stores references and names",
          "[common][expectations][identity]") {
    int a = 1;
    int b = 2;
    auto id = getIdentity(a, b, "a", "b");
    CHECK(&id.x == &a);
    CHECK(&id.y == &b);
    CHECK(id.x_name == "a");
    CHECK(id.y_name == "b");
    CHECK(id.limit == CE_DEF_SIMILARITY_DIFFERENCE);
}

TEST_CASE("identity: stream-out names the items",
          "[common][expectations][identity]") {
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

TEST_CASE("compare_t<int>: matching values increment success counter",
          "[common][expectations][compare_t]") {
    GToken tok("X", expectation::EQUALITY);
    int const a = 7;
    int const b = 7;
    compare_t(getIdentity(a, b, "a", "b"), tok);
    CHECK(tok.getTestCounter() == 1);
    CHECK(tok.getSuccessCounter() == 1);
    CHECK(tok.expectationMet());
}

TEST_CASE("compare_t<int>: mismatched values are recorded as failures, not thrown",
          "[common][expectations][compare_t]") {
    // compare_t catches g_expectation_violation and records into the token —
    // the call must NOT propagate.
    GToken tok("X", expectation::EQUALITY);
    int const a = 7;
    int const b = 8;
    CHECK_NOTHROW(compare_t(getIdentity(a, b, "a", "b"), tok));
    CHECK(tok.getTestCounter() == 1);
    CHECK(tok.getSuccessCounter() == 0);
    CHECK_FALSE(tok.expectationMet());
    CHECK_FALSE(tok.getErrorMessages().empty());
}

// ---------------------------------------------------------------------------
// compare(): vector<fp> overload — element-wise comparison with tolerance.

TEST_CASE("compare<vector<double>>: equal vectors + EQUALITY pass",
          "[common][expectations][compare]") {
    std::vector<double> const a{1.0, 2.0, 3.0};
    std::vector<double> const b{1.0, 2.0, 3.0};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY));
}

TEST_CASE("compare<vector<double>>: vectors of different sizes + EQUALITY violate",
          "[common][expectations][compare]") {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0, 2.0, 3.0};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY),
                    g_expectation_violation);
}

TEST_CASE("compare<vector<double>>: per-element similarity within limit",
          "[common][expectations][compare]") {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0 + 1e-7, 2.0 + 1e-7};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::FP_SIMILARITY, 1e-5));
}

TEST_CASE("compare<vector<double>>: per-element similarity outside limit fails",
          "[common][expectations][compare]") {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0, 2.5};   // diff > limit
    CHECK_THROWS_AS(
        compare(a, b, "a", "b", expectation::FP_SIMILARITY, 1e-5),
        g_expectation_violation);
}

TEST_CASE("compare<vector<double>>: INEQUALITY satisfied when contents differ",
          "[common][expectations][compare]") {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0, 9.0};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY));
}

TEST_CASE("compare<vector<double>>: INEQUALITY violated when contents match",
          "[common][expectations][compare]") {
    std::vector<double> const a{1.0, 2.0};
    std::vector<double> const b{1.0, 2.0};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): GCommonInterfaceT-derived objects via the geneva-type overload.
// A tiny GCommonInterfaceT-derivative is enough.

namespace {

class CmpObj : public GCommonInterfaceT<CmpObj> {
public:
    CmpObj() = default;
    explicit CmpObj(int v) : v_(v) {}
    [[nodiscard]] int v() const { return v_; }

protected:
    void load_(CmpObj const *cp) override { if(cp) v_ = cp->v_; }
    void compare_(CmpObj const &cp, expectation const &e, [[maybe_unused]] double const & limit) const override {
        GToken token("CmpObj", e);
        compare_base_t<GCommonInterfaceT<CmpObj>>(*this, cp, token);
        compare_t(Gem::Common::getIdentity(v_, cp.v_, "v_", "cp.v_"), token);
        token.evaluate();
    }

private:
    [[nodiscard]] CmpObj *clone_() const override { return new CmpObj(*this); }

    int v_{0};
};

} // namespace

TEST_CASE("compare<geneva_type>: equal Geneva objects + EQUALITY pass",
          "[common][expectations][compare]") {
    CmpObj const a(5);
    CmpObj const b(5);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY, 0.));
}

TEST_CASE("compare<geneva_type>: unequal Geneva objects + EQUALITY fail",
          "[common][expectations][compare]") {
    CmpObj const a(5);
    CmpObj const b(6);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.),
                    g_expectation_violation);
}

TEST_CASE("compare<geneva_type>: unequal Geneva objects + INEQUALITY pass",
          "[common][expectations][compare]") {
    CmpObj const a(5);
    CmpObj const b(6);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY, 0.));
}

TEST_CASE("compare<geneva_type>: equal Geneva objects + INEQUALITY fail",
          "[common][expectations][compare]") {
    CmpObj const a(5);
    CmpObj const b(5);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY, 0.),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): shared_ptr<geneva_type> overload.

TEST_CASE("compare<shared_ptr<geneva_type>>: both null + EQUALITY pass",
          "[common][expectations][compare]") {
    std::shared_ptr<CmpObj> const a;
    std::shared_ptr<CmpObj> const b;
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY, 0.));
}

TEST_CASE("compare<shared_ptr<geneva_type>>: one null one non-null + EQUALITY fail",
          "[common][expectations][compare]") {
    std::shared_ptr<CmpObj> const a;
    auto b = std::make_shared<CmpObj>(1);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.),
                    g_expectation_violation);
}

TEST_CASE("compare<shared_ptr<geneva_type>>: matching contents + EQUALITY pass",
          "[common][expectations][compare]") {
    auto a = std::make_shared<CmpObj>(7);
    auto b = std::make_shared<CmpObj>(7);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY, 0.));
}

TEST_CASE("compare<shared_ptr<geneva_type>>: differing contents + EQUALITY fail",
          "[common][expectations][compare]") {
    auto a = std::make_shared<CmpObj>(7);
    auto b = std::make_shared<CmpObj>(9);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): container<shared_ptr<geneva_type>> overload — vector<shared_ptr>.

TEST_CASE("compare<vector<shared_ptr<geneva_type>>>: identical contents + EQUALITY pass",
          "[common][expectations][compare]") {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(2)};
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(2)};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY, 0.));
}

TEST_CASE("compare<vector<shared_ptr<geneva_type>>>: differing sizes + EQUALITY fail",
          "[common][expectations][compare]") {
    std::vector<std::shared_ptr<CmpObj>> const a{std::make_shared<CmpObj>(1)};
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(2)};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.),
                    g_expectation_violation);
}

TEST_CASE("compare<vector<shared_ptr<geneva_type>>>: element diff + EQUALITY fail",
          "[common][expectations][compare]") {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(2)};
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(99)};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.),
                    g_expectation_violation);
}

TEST_CASE("compare<vector<shared_ptr<geneva_type>>>: INEQUALITY satisfied when one differs",
          "[common][expectations][compare]") {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(2)};
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(99)};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY, 0.));
}

TEST_CASE("compare<vector<shared_ptr<geneva_type>>>: nullptr vs non-null element + EQUALITY fail",
          "[common][expectations][compare]") {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::shared_ptr<CmpObj>(),  std::make_shared<CmpObj>(2)};
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(2)};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY, 0.),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): FP_SIMILARITY on a non-FP type (treated as EQUALITY)

TEST_CASE("compare<int>: FP_SIMILARITY on equal ints passes (treated as EQUALITY)",
          "[common][expectations][compare]") {
    CHECK_NOTHROW(compare(7, 7, "a", "b", expectation::FP_SIMILARITY));
}

// ---------------------------------------------------------------------------
// compare(): std::chrono::duration INEQUALITY cases

TEST_CASE("compare<duration>: unequal durations + INEQUALITY pass",
          "[common][expectations][compare]") {
    using D = std::chrono::seconds;
    CHECK_NOTHROW(compare(D{5}, D{6}, "a", "b", expectation::INEQUALITY));
}

TEST_CASE("compare<duration>: equal durations + INEQUALITY throw",
          "[common][expectations][compare]") {
    using D = std::chrono::seconds;
    CHECK_THROWS_AS(compare(D{5}, D{5}, "a", "b", expectation::INEQUALITY),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): std::chrono::time_point INEQUALITY cases

TEST_CASE("compare<time_point>: different time_points + INEQUALITY pass",
          "[common][expectations][compare]") {
    using TP = std::chrono::high_resolution_clock::time_point;
    TP const t1 = TP::clock::now();
    TP const t2 = t1 + std::chrono::nanoseconds{1};
    CHECK_NOTHROW(compare(t1, t2, "a", "b", expectation::INEQUALITY));
}

TEST_CASE("compare<time_point>: equal time_points + INEQUALITY throw",
          "[common][expectations][compare]") {
    using TP = std::chrono::high_resolution_clock::time_point;
    TP const t1 = TP::clock::now();
    TP const t2 = t1;
    CHECK_THROWS_AS(compare(t1, t2, "a", "b", expectation::INEQUALITY),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): non-FP container (vector<int>) — exercises the basic-type container overload

TEST_CASE("compare<vector<int>>: equal vectors + EQUALITY pass",
          "[common][expectations][compare]") {
    std::vector<int> const a{1, 2, 3};
    std::vector<int> const b{1, 2, 3};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY));
}

TEST_CASE("compare<vector<int>>: size mismatch + EQUALITY throws with diagnostic",
          "[common][expectations][compare]") {
    std::vector<int> const a{1, 2};
    std::vector<int> const b{1, 2, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY),
                    g_expectation_violation);
}

TEST_CASE("compare<vector<int>>: same size, element mismatch + EQUALITY throws",
          "[common][expectations][compare]") {
    std::vector<int> const a{1, 2, 3};
    std::vector<int> const b{1, 99, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY),
                    g_expectation_violation);
}

TEST_CASE("compare<vector<int>>: different vectors + INEQUALITY pass",
          "[common][expectations][compare]") {
    std::vector<int> const a{1, 2, 3};
    std::vector<int> const b{1, 99, 3};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY));
}

TEST_CASE("compare<vector<int>>: equal vectors + INEQUALITY throws",
          "[common][expectations][compare]") {
    std::vector<int> const a{1, 2, 3};
    std::vector<int> const b{1, 2, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): set<int> — exercises the set/sorted-container overload

TEST_CASE("compare<set<int>>: equal sets + EQUALITY pass",
          "[common][expectations][compare]") {
    std::set<int> const a{1, 2, 3};
    std::set<int> const b{1, 2, 3};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::EQUALITY));
}

TEST_CASE("compare<set<int>>: sets of different sizes + EQUALITY throw",
          "[common][expectations][compare]") {
    std::set<int> const a{1, 2};
    std::set<int> const b{1, 2, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY),
                    g_expectation_violation);
}

TEST_CASE("compare<set<int>>: same size, different elements + EQUALITY throw",
          "[common][expectations][compare]") {
    std::set<int> const a{1, 2, 3};
    std::set<int> const b{1, 2, 4};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::EQUALITY),
                    g_expectation_violation);
}

TEST_CASE("compare<set<int>>: different sets + INEQUALITY pass",
          "[common][expectations][compare]") {
    std::set<int> const a{1, 2, 3};
    std::set<int> const b{1, 2, 4};
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY));
}

TEST_CASE("compare<set<int>>: equal sets + INEQUALITY throw",
          "[common][expectations][compare]") {
    std::set<int> const a{1, 2, 3};
    std::set<int> const b{1, 2, 3};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): shared_ptr<geneva_type> INEQUALITY cases

TEST_CASE("compare<shared_ptr<geneva_type>>: both null + INEQUALITY throw",
          "[common][expectations][compare]") {
    std::shared_ptr<CmpObj> const a;
    std::shared_ptr<CmpObj> const b;
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY, 0.),
                    g_expectation_violation);
}

TEST_CASE("compare<shared_ptr<geneva_type>>: one null one non-null + INEQUALITY pass",
          "[common][expectations][compare]") {
    std::shared_ptr<CmpObj> const a;
    auto b = std::make_shared<CmpObj>(1);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY, 0.));
}

TEST_CASE("compare<shared_ptr<geneva_type>>: differing contents + INEQUALITY pass",
          "[common][expectations][compare]") {
    auto a = std::make_shared<CmpObj>(7);
    auto b = std::make_shared<CmpObj>(9);
    CHECK_NOTHROW(compare(a, b, "a", "b", expectation::INEQUALITY, 0.));
}

TEST_CASE("compare<shared_ptr<geneva_type>>: matching contents + INEQUALITY throw",
          "[common][expectations][compare]") {
    auto a = std::make_shared<CmpObj>(7);
    auto b = std::make_shared<CmpObj>(7);
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY, 0.),
                    g_expectation_violation);
}

// ---------------------------------------------------------------------------
// compare(): vector<shared_ptr<geneva_type>> INEQUALITY — identical containers should throw

TEST_CASE("compare<vector<shared_ptr<geneva_type>>>: identical containers + INEQUALITY throw",
          "[common][expectations][compare]") {
    std::vector<std::shared_ptr<CmpObj>> const a{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(2)};
    std::vector<std::shared_ptr<CmpObj>> const b{
        std::make_shared<CmpObj>(1), std::make_shared<CmpObj>(2)};
    CHECK_THROWS_AS(compare(a, b, "a", "b", expectation::INEQUALITY, 0.),
                    g_expectation_violation);
}
