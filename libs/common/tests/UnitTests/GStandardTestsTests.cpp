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

/**
 * Tests of the installed standard-test harness (common/testing/GStandardTests.hpp): its object factory,
 * how it finds modifyForTest(), and the standard tests themselves on small reflective types with and
 * without such a function.
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#include "common/GArchiveNamed.hpp" // archive_named (named-member emitter)
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/testing/GStandardTests.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE

using namespace Gem::Common;
using namespace Gem::Common::Testing;

namespace {

/******************************************************************************/
// A category root that can be changed for the tests, and a derivative that adds a member: it calls the
// root's function and changes its own.

class Root : public GCommonInterfaceT<Root> {
public:
    Root() = default;
    Root(const Root &) = default;
    ~Root() override = default;
    Root &operator=(const Root &) = default;

    [[nodiscard]] int value() const {
        return v_;
    }

    /** @brief Changes this object for the standard tests @return true */
    bool modifyForTest() {
        ++v_;
        return true;
    }

protected:
    void load_(const Root *cp) override {
        ptrDifferenceCheck(
            cp,
            this
        ); // loading from itself throws in DEBUG mode, as in generated load_()s
        v_ = cp->v_;
    }

    void compare_(
        const Root &cp,
        const expectation &e,
        [[maybe_unused]] const double &limit
    ) const override {
        GToken token("Root", e);
        compare_base_t<GCommonInterfaceT<Root>>(*this, cp, token);
        compare_t(getIdentity(v_, cp.v_, "v_", "cp.v_"), token);
        token.evaluate();
    }

private:
    [[nodiscard]] Root *clone_() const override {
        return new Root(*this); // NOLINT(cppcoreguidelines-owning-memory): clone_() hands out ownership
    }

    friend struct Gem::Weft::access;
    template <class archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] unsigned int version) {
        Gem::Common::archive_named(ar, "v_", v_);
    }

    int v_ = 0;
};

class Derived : public Root {
public:
    [[nodiscard]] std::string label() const {
        return label_;
    }

    /** @brief Changes the root's member and this class's own @return true */
    // NOLINTNEXTLINE(bugprone-derived-method-shadowing-base-method): it calls the base's
    bool modifyForTest() {
        Root::modifyForTest();
        label_ += "_m";
        return true;
    }

protected:
    void load_(const Root *cp) override {
        Root::load_(cp);
        label_ = dynamic_cast<const Derived &>(*cp).label_;
    }

    void compare_(const Root &cp, const expectation &e, const double &limit) const override {
        Root::compare_(cp, e, limit);
        GToken token("Derived", e);
        compare_t(
            getIdentity(label_, dynamic_cast<const Derived &>(cp).label_, "label_", "cp.label_"),
            token
        );
        token.evaluate();
    }

private:
    [[nodiscard]] Root *clone_() const override {
        return new Derived(*this); // NOLINT(cppcoreguidelines-owning-memory): clone_() hands out ownership
    }

    friend struct Gem::Weft::access;
    template <class archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] unsigned int version) {
        Gem::Common::archive_named_base<Root>(ar, "Root", *this);
        Gem::Common::archive_named(ar, "label_", label_);
    }

    std::string label_ = "derived";
};

/******************************************************************************/
// A reflective type without modifyForTest(), and one that hides it: a protected function is not part
// of the public interface the harness looks for.

class Unchangeable : public GCommonInterfaceT<Unchangeable> {
protected:
    void load_(const Unchangeable *cp) override {
        ptrDifferenceCheck(cp, this);
    }
    void compare_(
        const Unchangeable &cp,
        const expectation &e,
        [[maybe_unused]] const double &limit
    ) const override {
        GToken token("Unchangeable", e);
        compare_base_t<GCommonInterfaceT<Unchangeable>>(*this, cp, token);
        token.evaluate();
    }

private:
    [[nodiscard]] Unchangeable *clone_() const override {
        return new Unchangeable(*this); // NOLINT(cppcoreguidelines-owning-memory): clone_() hands out ownership
    }

    friend struct Gem::Weft::access;
    template <class archive_type>
    void serialize([[maybe_unused]] archive_type &ar, [[maybe_unused]] unsigned int version) {
    }
};

class HiddenModify {
protected:
    bool modifyForTest() {
        return true;
    }
};

/******************************************************************************/
// Types for the factory

struct Plain {
    int v{99};
};

struct ThrowsGem {
    ThrowsGem() {
        throw gem_exception("intentional from ThrowsGem");
    }
};

struct ThrowsOther {
    ThrowsOther() {
        throw std::logic_error("not a gem exception");
    }
};

} // namespace

GEM_REGISTER_ARCHIVABLE(Root)         // NOLINT
GEM_REGISTER_ARCHIVABLE(Derived)      // NOLINT
GEM_REGISTER_ARCHIVABLE(Unchangeable) // NOLINT

/******************************************************************************/

TEST_CASE(
    "GStandardTests: modifyForTest() is found when it is public",
    "[common][standard-tests]"
) {
    STATIC_REQUIRE(ModifiableForTest<Root>);
    STATIC_REQUIRE(ModifiableForTest<Derived>);
    STATIC_REQUIRE_FALSE(ModifiableForTest<Unchangeable>);
    STATIC_REQUIRE_FALSE(ModifiableForTest<HiddenModify>);
}

TEST_CASE(
    "GStandardTests: modifyForTestIfAvailable calls the type's own function",
    "[common][standard-tests]"
) {
    Derived d;
    CHECK(modifyForTestIfAvailable(d));
    CHECK(d.value() == 1);
    CHECK(d.label() == "derived_m");

    Unchangeable u;
    CHECK_FALSE(modifyForTestIfAvailable(u));
}

TEST_CASE(
    "GStandardTests: the standard tests pass for types with and without modifyForTest()",
    "[common][standard-tests]"
) {
    StandardTests_no_failure_expected<Root>();
    StandardTests_no_failure_expected<Derived>();
    StandardTests_no_failure_expected<Unchangeable>();
    StandardTests_failures_expected<Root>();
    StandardTests_failures_expected<Derived>();
    StandardTests_failures_expected<Unchangeable>();
}

/******************************************************************************/

TEST_CASE(
    "TFactory_GUnitTests: a default-constructible T gives a populated pointer",
    "[common][standard-tests]"
) {
    auto p = TFactory_GUnitTests<Plain>();
    REQUIRE(p);
    CHECK(p->v == 99);
}

TEST_CASE(
    "TFactory_GUnitTests: a gem_exception from the constructor propagates unchanged",
    "[common][standard-tests]"
) {
    CHECK_THROWS_AS(TFactory_GUnitTests<ThrowsGem>(), gem_exception);
}

TEST_CASE(
    "TFactory_GUnitTests: another exception is wrapped in a gem_exception",
    "[common][standard-tests]"
) {
    try {
        TFactory_GUnitTests<ThrowsOther>();
        FAIL("expected gem_exception to be thrown");
    }
    catch(gem_exception const &g) {
        std::string const what = g.what();
        CHECK(what.contains("Caught unknown exception"));
    }
    catch(...) {
        FAIL("threw a non-gem exception type");
    }
}

/******************************************************************************/
