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

#include <concepts>
#include <memory>
#include <vector>

#include "common/GCommonInterfaceT.hpp"
#include "common/GSelfTestable.hpp"

using namespace Gem::Common;

namespace {

/******************************************************************************/
// A hierarchy root in the shape Geneva uses: a reflective value type that does NOT
// carry the self-test facet, plus a protected non-virtual test helper its opted-in
// derivatives chain to.

class Root : public GCommonInterfaceT<Root> {
public:
    Root() = default;
    Root(const Root &) = default;
    ~Root() override = default;
    Root &operator=(const Root &) = default;

    [[nodiscard]] int value() const { return v_; }

protected:
    /** @brief The root's own perturbation -- a plain protected helper, not a virtual hook. */
    bool modify_GUnitTests_() {
        ++v_;
        return true;
    }

    void load_(const Root *cp) override { v_ = cp->v_; }

    void compare_(const Root &cp, const expectation &e, const double &limit) const override {
        GToken token("Root", e);
        compare_base_t<GCommonInterfaceT<Root>>(*this, cp, token);
        compare_t(getIdentity(v_, cp.v_, "v_", "cp.v_"), token);
        token.evaluate();
    }

    int v_ = 0;

private:
    [[nodiscard]] Root *clone_() const override { return new Root(*this); }
};

/******************************************************************************/
// A derivative that opts IN and chains to the root helper by qualified call.

class OptedIn
  : public Root
  , public GSelfTestable {
public:
    [[nodiscard]] int specificRuns() const { return no_failure_runs_; }
    [[nodiscard]] int failureRuns() const { return failure_runs_; }

protected:
    bool modify_GUnitTests_() override { return Root::modify_GUnitTests_(); }
    void specificTestsNoFailureExpected_GUnitTests_() override { ++no_failure_runs_; }
    void specificTestsFailuresExpected_GUnitTests_() override { ++failure_runs_; }

private:
    [[nodiscard]] Root *clone_() const override { return new OptedIn(*this); }

    int no_failure_runs_ = 0;
    int failure_runs_ = 0;
};

/******************************************************************************/
// A derivative that opts OUT: it inherits the root only, exactly like a user
// individual compiled into a runtime-loadable module.

class OptedOut : public Root {
private:
    [[nodiscard]] Root *clone_() const override { return new OptedOut(*this); }
};

/******************************************************************************/
// A type that opts in without overriding anything -- it must get the no-op defaults.

class DefaultsOnly : public GSelfTestable {
public:
    using GSelfTestable::modify_GUnitTests;
    using GSelfTestable::specificTestsFailuresExpected_GUnitTests;
    using GSelfTestable::specificTestsNoFailureExpected_GUnitTests;
};

} // namespace

/******************************************************************************/
// The structural guarantee this facet exists for: the universal reflective-value base
// must NOT carry the self-test hooks. It is what makes a loadable module (compiled
// -UGEM_TESTING) layout-compatible with a testing-enabled core by construction rather
// than by discipline -- see the comment in CMakeModules/GenevaIndividualModule.cmake.

TEST_CASE("GSelfTestable is not a base of GCommonInterfaceT", "[common][self-testable]") {
    STATIC_REQUIRE_FALSE(std::derived_from<Root, GSelfTestable>);
    STATIC_REQUIRE_FALSE(std::derived_from<OptedOut, GSelfTestable>);
    STATIC_REQUIRE(std::derived_from<OptedIn, GSelfTestable>);
}

TEST_CASE("GSelfTestable: the default hooks are no-ops", "[common][self-testable]") {
    DefaultsOnly d;
    CHECK_FALSE(d.modify_GUnitTests());
    CHECK_NOTHROW(d.specificTestsNoFailureExpected_GUnitTests());
    CHECK_NOTHROW(d.specificTestsFailuresExpected_GUnitTests());
}

TEST_CASE("GSelfTestable: the public wrappers dispatch to the overrides",
          "[common][self-testable]") {
    OptedIn o;
    CHECK(o.value() == 0);

    CHECK(o.modify_GUnitTests()); // chains to the root helper
    CHECK(o.value() == 1);

    o.specificTestsNoFailureExpected_GUnitTests();
    o.specificTestsFailuresExpected_GUnitTests();
    CHECK(o.specificRuns() == 1);
    CHECK(o.failureRuns() == 1);
}

/******************************************************************************/
// The polymorphic contract: a heterogeneous collection is walked through the CATEGORY
// ROOT, and an element contributes only if it opted into the facet. Skipping the others
// is the intended semantics -- it reproduces what the old no-op default did.

TEST_CASE("GSelfTestable: a heterogeneous walk casts and skips", "[common][self-testable]") {
    std::vector<std::unique_ptr<Root>> population;
    population.push_back(std::make_unique<OptedIn>());
    population.push_back(std::make_unique<OptedOut>());
    population.push_back(std::make_unique<OptedIn>());

    int modified = 0;
    for (auto const &element : population) {
        if (auto *self_testable = dynamic_cast<GSelfTestable *>(element.get());
            self_testable != nullptr && self_testable->modify_GUnitTests()) {
            ++modified;
        }
    }

    CHECK(modified == 2);
    CHECK(population[0]->value() == 1); // opted in  -> perturbed
    CHECK(population[1]->value() == 0); // opted out -> silently skipped
    CHECK(population[2]->value() == 1);
}
