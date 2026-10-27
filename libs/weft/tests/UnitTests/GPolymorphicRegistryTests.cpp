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

#include <memory>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <vector>

#include "weft/GPolymorphicRegistry.hpp"
#include "weft/GWeftError.hpp"

using Gem::Weft::weft_exception; // the registry now throws Weft's own dependency-free exception type
using Gem::Weft::GPolymorphicRegistry;

// ---------------------------------------------------------------------------
// Toy hierarchies.
//
// The registry is a process-global singleton keyed on the Root type, so a
// TEST_CASE that inspects counts must own a Root nobody else touches. RootT<Tag>
// gives each test its own hierarchy via a locally-defined, unique Tag type.

namespace {

template <typename Tag>
struct RootT {
    using gemfony_common_root_t = RootT; // lets GEM_REGISTER_TYPE route here
    virtual ~RootT() = default;
    [[nodiscard]] virtual int kind() const = 0;
};

template <typename Tag>
struct DerA : RootT<Tag> {
    [[nodiscard]] int kind() const override { return 1; }
};

template <typename Tag>
struct DerB : RootT<Tag> {
    [[nodiscard]] int kind() const override { return 2; }
};

} // namespace

// ---------------------------------------------------------------------------
// Basic round trip: register, create-by-tag, tag-of-object.

TEST_CASE("GPolymorphicRegistry: register / create / tagOf round trip", "[common][polyregistry]") {
    struct Tag {};
    using Root = RootT<Tag>;
    using A = DerA<Tag>;
    using B = DerB<Tag>;

    CHECK(GPolymorphicRegistry<Root>::size() == 0);

    CHECK(GPolymorphicRegistry<Root>::template reg<A>("A"));
    CHECK(GPolymorphicRegistry<Root>::template reg<B>("B"));
    CHECK(GPolymorphicRegistry<Root>::size() == 2);

    CHECK(GPolymorphicRegistry<Root>::contains("A"));
    CHECK(GPolymorphicRegistry<Root>::contains("B"));
    CHECK_FALSE(GPolymorphicRegistry<Root>::contains("C"));
    CHECK(GPolymorphicRegistry<Root>::registered(std::type_index(typeid(A))));

    std::unique_ptr<Root> a = GPolymorphicRegistry<Root>::create("A");
    std::unique_ptr<Root> b = GPolymorphicRegistry<Root>::create("B");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->kind() == 1);
    CHECK(b->kind() == 2);

    CHECK(GPolymorphicRegistry<Root>::tagOf(*a) == "A");
    CHECK(GPolymorphicRegistry<Root>::tagOf(*b) == "B");
}

// ---------------------------------------------------------------------------
// tagOf resolves the DYNAMIC type (this is how a polymorphic pointer is written).

TEST_CASE("GPolymorphicRegistry: tagOf uses the dynamic type through a base reference", "[common][polyregistry]") {
    struct Tag {};
    using Root = RootT<Tag>;
    using A = DerA<Tag>;

    GPolymorphicRegistry<Root>::template reg<A>("A");

    A concrete;
    Root &asBase = concrete; // static type Root, dynamic type A
    CHECK(GPolymorphicRegistry<Root>::tagOf(asBase) == "A");
}

// ---------------------------------------------------------------------------
// Idempotency and collision rules.

TEST_CASE("GPolymorphicRegistry: identical (type,tag) re-registration is idempotent", "[common][polyregistry]") {
    struct Tag {};
    using Root = RootT<Tag>;
    using A = DerA<Tag>;

    CHECK(GPolymorphicRegistry<Root>::template reg<A>("A"));        // first: true
    CHECK_FALSE(GPolymorphicRegistry<Root>::template reg<A>("A"));  // repeat: false, no throw
    CHECK(GPolymorphicRegistry<Root>::size() == 1);
}

TEST_CASE("GPolymorphicRegistry: a tag bound to a different type throws", "[common][polyregistry]") {
    struct Tag {};
    using Root = RootT<Tag>;
    using A = DerA<Tag>;
    using B = DerB<Tag>;

    GPolymorphicRegistry<Root>::template reg<A>("shared");
    CHECK_THROWS_AS(GPolymorphicRegistry<Root>::template reg<B>("shared"), weft_exception);
}

TEST_CASE("GPolymorphicRegistry: a type re-registered under a different tag throws", "[common][polyregistry]") {
    struct Tag {};
    using Root = RootT<Tag>;
    using A = DerA<Tag>;

    GPolymorphicRegistry<Root>::template reg<A>("first");
    CHECK_THROWS_AS(GPolymorphicRegistry<Root>::template reg<A>("second"), weft_exception);
}

// ---------------------------------------------------------------------------
// Failure modes on read.

TEST_CASE("GPolymorphicRegistry: create() on an unknown tag throws", "[common][polyregistry]") {
    struct Tag {};
    using Root = RootT<Tag>;
    CHECK_THROWS_AS(GPolymorphicRegistry<Root>::create("nope"), weft_exception);
}

TEST_CASE("GPolymorphicRegistry: tagOf() on an unregistered dynamic type throws", "[common][polyregistry]") {
    struct Tag {};
    using Root = RootT<Tag>;
    using A = DerA<Tag>;
    A concrete; // never registered
    CHECK_THROWS_AS(GPolymorphicRegistry<Root>::tagOf(concrete), weft_exception);
}

// ---------------------------------------------------------------------------
// tags() is a sorted enumeration (basis for the boot-time completeness check).

TEST_CASE("GPolymorphicRegistry: tags() returns every tag, sorted", "[common][polyregistry]") {
    struct Tag {};
    using Root = RootT<Tag>;
    using A = DerA<Tag>;
    using B = DerB<Tag>;

    GPolymorphicRegistry<Root>::template reg<B>("zeta");
    GPolymorphicRegistry<Root>::template reg<A>("alpha");

    std::vector<std::string> tags = GPolymorphicRegistry<Root>::tags();
    REQUIRE(tags.size() == 2);
    CHECK(tags[0] == "alpha");
    CHECK(tags[1] == "zeta");
}

// ---------------------------------------------------------------------------
// Distinct Roots have independent registries.

TEST_CASE("GPolymorphicRegistry: registries are isolated per hierarchy root", "[common][polyregistry]") {
    struct TagX {};
    struct TagY {};
    using RootX = RootT<TagX>;
    using RootY = RootT<TagY>;

    GPolymorphicRegistry<RootX>::template reg<DerA<TagX>>("A");

    // RootY's registry is untouched by RootX's registration.
    CHECK(GPolymorphicRegistry<RootX>::size() == 1);
    CHECK(GPolymorphicRegistry<RootY>::size() == 0);
    CHECK(GPolymorphicRegistry<RootX>::contains("A"));
    CHECK_FALSE(GPolymorphicRegistry<RootY>::contains("A"));
}

// ---------------------------------------------------------------------------
// Explicit-factory overload: the seam for a private-constructor type, whose
// ctor the registration site cannot reach directly.

namespace {

struct PrivRoot {
    using gemfony_common_root_t = PrivRoot;
    virtual ~PrivRoot() = default;
    [[nodiscard]] virtual int kind() const = 0;
};

class PrivDer : public PrivRoot {
public:
    [[nodiscard]] int kind() const override { return 42; }
    // A public maker that reaches the private ctor from inside the class.
    static std::unique_ptr<PrivRoot> make() { return std::unique_ptr<PrivRoot>(new PrivDer()); }

private:
    PrivDer() = default;
};

} // namespace

TEST_CASE("GPolymorphicRegistry: explicit-factory overload constructs a private-ctor type", "[common][polyregistry]") {
    static_assert(not std::is_default_constructible_v<PrivDer>,
                  "PrivDer's default ctor must be inaccessible for this test to exercise the seam");

    GPolymorphicRegistry<PrivRoot>::reg(
        "PrivDer", std::type_index(typeid(PrivDer)),
        +[]() -> std::unique_ptr<PrivRoot> { return PrivDer::make(); });

    std::unique_ptr<PrivRoot> p = GPolymorphicRegistry<PrivRoot>::create("PrivDer");
    REQUIRE(p);
    CHECK(p->kind() == 42);
    CHECK(GPolymorphicRegistry<PrivRoot>::tagOf(*p) == "PrivDer");
}

// ---------------------------------------------------------------------------
// The GEM_REGISTER_TYPE macro registers at static-init time, routing to the
// hierarchy deduced from the type (gemfony_common_root_t) and keying on the
// stringized type name.

namespace {

struct MacroRoot {
    using gemfony_common_root_t = MacroRoot;
    virtual ~MacroRoot() = default;
    [[nodiscard]] virtual int kind() const = 0;
};

struct MacroA : MacroRoot {
    [[nodiscard]] int kind() const override { return 1; }
};

struct MacroB : MacroRoot {
    [[nodiscard]] int kind() const override { return 2; }
};

} // namespace

GEM_REGISTER_TYPE(MacroA)
GEM_REGISTER_TYPE(MacroB)

TEST_CASE("GPolymorphicRegistry: GEM_REGISTER_TYPE populated the registry at startup", "[common][polyregistry]") {
    CHECK(GPolymorphicRegistry<MacroRoot>::contains("MacroA"));
    CHECK(GPolymorphicRegistry<MacroRoot>::contains("MacroB"));

    std::unique_ptr<MacroRoot> a = GPolymorphicRegistry<MacroRoot>::create("MacroA");
    REQUIRE(a);
    CHECK(a->kind() == 1);
    CHECK(GPolymorphicRegistry<MacroRoot>::tagOf(*a) == "MacroA");
}
