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
#include <optional>
#include <stdexcept>
#include <string>

#include "common/GArchiveNamed.hpp" // archive_named (named-member emitter)
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GMemberReflectionT.hpp"
#include "common/GReflectiveInterfaceT.hpp"
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
        ptrDifferenceCheck(cp, this); // loading from itself throws, as in generated load_()s
        v_ = cp->v_;
    }

    void compare_(const Root &cp, const expectation &e, const double &limit) const override {
        GToken token("Root", e, limit);
        compare_base_t<GCommonInterfaceT<Root>>(*this, cp, token);
        compare_t(getIdentity(v_, cp.v_, "v_", "cp.v_"), token);
        token.evaluate();
    }

private:
    [[nodiscard]] Root *clone_() const override {
        return new Root(
            *this
        ); // NOLINT(cppcoreguidelines-owning-memory): clone_() hands out ownership
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
        GToken token("Derived", e, limit);
        compare_t(
            getIdentity(label_, dynamic_cast<const Derived &>(cp).label_, "label_", "cp.label_"),
            token
        );
        token.evaluate();
    }

private:
    [[nodiscard]] Root *clone_() const override {
        return new Derived(
            *this
        ); // NOLINT(cppcoreguidelines-owning-memory): clone_() hands out ownership
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
    void
    compare_(const Unchangeable &cp, const expectation &e, const double &limit) const override {
        GToken token("Unchangeable", e, limit);
        compare_base_t<GCommonInterfaceT<Unchangeable>>(*this, cp, token);
        token.evaluate();
    }

private:
    [[nodiscard]] Unchangeable *clone_() const override {
        return new Unchangeable(
            *this
        ); // NOLINT(cppcoreguidelines-owning-memory): clone_() hands out ownership
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
// Types with a cache outside their compared members, used by useForTest(): Cached renews it whenever
// its members are replaced; StaleCache forgets to in load_(), as GFactoryT::load() once forgot its
// parse cache.

template <bool RENEWS_CACHE>
class CacheHolder : public GCommonInterfaceT<CacheHolder<RENEWS_CACHE>> {
public:
    /** @brief Changes this object for the standard tests @return true */
    bool modifyForTest() {
        ++v_;
        cache_.reset();
        return true;
    }

    /** @brief Reads the cache, which is filled from v_ on first use @return The cached value */
    int useForTest() {
        if(not cache_) {
            cache_ = 10 * v_;
        }
        return *cache_;
    }

protected:
    void load_(const CacheHolder *cp) override {
        ptrDifferenceCheck(cp, this);
        v_ = cp->v_;
        if constexpr(RENEWS_CACHE) {
            cache_.reset();
        }
    }

    void compare_(const CacheHolder &cp, const expectation &e, const double &limit) const override {
        GToken token("CacheHolder", e, limit);
        compare_base_t<GCommonInterfaceT<CacheHolder>>(*this, cp, token);
        compare_t(getIdentity(v_, cp.v_, "v_", "cp.v_"), token);
        token.evaluate();
    }

private:
    [[nodiscard]] CacheHolder *clone_() const override {
        return new CacheHolder(
            *this
        ); // NOLINT(cppcoreguidelines-owning-memory): clone_() hands out ownership
    }

    friend struct Gem::Weft::access;
    template <class archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] unsigned int version) {
        Gem::Common::archive_named(ar, "v_", v_);
    }

    int v_ = 0;
    std::optional<int> cache_; ///< transient: derived from v_ on first use
};

using Cached = CacheHolder<true>;
using StaleCache = CacheHolder<false>;

/******************************************************************************/
// A reflective type with a floating point member, for the limit of GEqualityPrinter

class WithDouble : public GReflectiveInterfaceT<WithDouble, GCommonInterfaceT<WithDouble>> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(make_member("d_", self.d_));
    }

public:
    static constexpr std::string_view class_name = "WithDouble";

    double d_ = 1.0;
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
GEM_REGISTER_ARCHIVABLE(Cached)       // NOLINT

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
    checkRoundTrips<Root>();
    checkRoundTrips<Derived>();
    checkRoundTrips<Unchangeable>();
    checkMisuse<Root>();
    checkMisuse<Derived>();
    checkMisuse<Unchangeable>();
}

/******************************************************************************/

TEST_CASE(
    "makeTestObject: a default-constructible T gives a populated pointer",
    "[common][standard-tests]"
) {
    auto p = makeTestObject<Plain>();
    REQUIRE(p);
    CHECK(p->v == 99);
}

TEST_CASE(
    "makeTestObject: a gem_exception from the constructor propagates unchanged",
    "[common][standard-tests]"
) {
    CHECK_THROWS_AS(makeTestObject<ThrowsGem>(), gem_exception);
}

TEST_CASE(
    "makeTestObject: another exception is wrapped in a gem_exception that keeps its message",
    "[common][standard-tests]"
) {
    try {
        makeTestObject<ThrowsOther>();
        FAIL("expected gem_exception to be thrown");
    }
    catch(gem_exception const &g) {
        std::string const what = g.what();
        CHECK(what.contains("not a gem exception"));
    }
    catch(...) {
        FAIL("threw a non-gem exception type");
    }
}

/******************************************************************************/

TEST_CASE(
    "GEqualityPrinter: isSimilar() applies the limit it was made with",
    "[common][standard-tests][limit]"
) {
    WithDouble const x;
    WithDouble y;
    y.d_ += 1e-3;
    GEqualityPrinter const wide("wide", 1e-2, not CE_WITH_MESSAGES);
    GEqualityPrinter const narrow_printer("narrow", 1e-6, not CE_WITH_MESSAGES);
    CHECK(wide.isSimilar(x, y));
    CHECK_FALSE(narrow_printer.isSimilar(x, y));
    CHECK_FALSE(wide.isEqual(x, y));
}

/******************************************************************************/

TEST_CASE(
    "GStandardTests: useForTest() is found and a copy is used after each step",
    "[common][standard-tests][use]"
) {
    STATIC_REQUIRE(UsableForTest<Cached>);
    STATIC_REQUIRE_FALSE(UsableForTest<Root>);
    checkRoundTrips<Cached>();

    Root r;
    useForTestIfAvailable(r); // a type without useForTest() is left alone
    CHECK(r.value() == 0);
    Root r_copy(r);
    CHECK(usesAgree(r_copy, r));
}

TEST_CASE(
    "GStandardTests: a cache that a load forgot to renew is found by its use",
    "[common][standard-tests][use]"
) {
    StaleCache original;
    original.modifyForTest();
    StaleCache stale;
    useForTestIfAvailable(stale); // fills the cache from the old value
    stale.load(original);
    CHECK(GEqualityPrinter("stale", CE_DEF_SIMILARITY_DIFFERENCE, not CE_WITH_MESSAGES)
              .isEqual(stale, original));
    CHECK_FALSE(usesAgree(stale, original));
}

/******************************************************************************/
// The public parts of the standard tests, one by one

namespace {

class PodValues : public GPodContainerT<double> {
public:
    explicit PodValues(std::vector<double> const &values) {
        for(double const v : values) {
            this->push_back(v);
        }
    }
    ~PodValues() override = default;
};

} // namespace

TEST_CASE(
    "GEqualityPrinter: a container is compared with a vector of its values",
    "[common][standard-tests]"
) {
    PodValues const values({1.0, 2.0});
    GEqualityPrinter const gep("pod", 1e-2, not CE_WITH_MESSAGES);
    CHECK(gep.isEqual(values, std::vector<double>{1.0, 2.0}));
    CHECK_FALSE(gep.isEqual(values, std::vector<double>{1.0, 2.0 + 1e-3}));
    CHECK_FALSE(gep.isEqual(values, std::vector<double>{1.0}));
    CHECK(gep.isSimilar(values, std::vector<double>{1.0, 2.0 + 1e-3}));
    CHECK_FALSE(gep.isSimilar(values, std::vector<double>{1.0, 2.1}));
}

TEST_CASE(
    "GStandardTests: the steps of the standard tests run one by one",
    "[common][standard-tests]"
) {
    STATIC_REQUIRE(std::same_as<category_root_t<Derived>, Root>);
    STATIC_REQUIRE(std::same_as<category_root_t<Root>, Root>);

    std::shared_ptr<Derived> const changed = makeChangedForTest<Derived>();
    CHECK(changed->value() == 1);
    CHECK(changed->label() == "derived_m");
    std::shared_ptr<Cached> const used = makeUsedTarget<Cached>();
    CHECK(used->useForTest() == 0);

    GEqualityPrinter const gep("steps", CE_DEF_SIMILARITY_DIFFERENCE, CE_WITH_MESSAGES);
    std::shared_ptr<Root> const copy = std::make_shared<Derived>(*changed);
    std::shared_ptr<Root> const original = std::make_shared<Derived>(*changed);
    checkCopyOf(gep, copy, original);

    testCopyConstruction<Derived>(gep);
    testCloneToCategoryRoot<Derived>(gep);
    testCloneToTarget<Derived>(gep);
    testLoadThroughPointer<Derived>(gep);
    testLoadThroughReference<Derived>(gep);
    testCopyAssignment<Root>(gep);
    for(auto const mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        testOwnRoundTrip<Derived>(gep, mode);
        testHelperRoundTrip<Derived>(gep, mode);
        testOwnRoundTrip<Unchangeable>(gep, mode); // skipped, with a notice
    }
    reportSkippedRoundTrip<Unchangeable>("Internal", serializationMode::GEM_JSON);
}
