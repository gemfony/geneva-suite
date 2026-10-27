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

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>


#include "common/GArchiveNamed.hpp"       // archive_named (named-member emitter)
#include "weft/GArchivePolymorphic.hpp"   // GEM_REGISTER_ARCHIVABLE

#include "common/GCommonEnums.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GExceptions.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// Concrete Geneva-style class. Inherits from GCommonInterfaceT<Self> (CRTP)
// and implements every pure virtual the framework requires.

namespace {

class TestObj : public GCommonInterfaceT<TestObj> {
public:
    TestObj() = default;
    explicit TestObj(int v) : v_(v) {}

    [[nodiscard]] int  v() const { return v_; }
    void v(int v) { v_ = v; }

protected:
    void load_(TestObj const *cp) override {
        if(cp) v_ = cp->v_;
    }

    void compare_(TestObj const &cp, expectation const &e, [[maybe_unused]] double const & limit) const override {
        GToken token("TestObj", e);
        compare_base_t<GCommonInterfaceT<TestObj>>(*this, cp, token);
        compare_t(Gem::Common::getIdentity(v_, cp.v_, "v_", "cp.v_"), token);
        token.evaluate();
    }


private:
    [[nodiscard]] TestObj *clone_() const override { return new TestObj(*this); }

    friend struct Gem::Weft::access;
    template <class Archive>
    void serialize(Archive &ar, [[maybe_unused]] unsigned int version) {
        Gem::Common::archive_named(ar, "v_", v_);
    }

    int v_{0};
};

} // namespace

GEM_REGISTER_ARCHIVABLE(TestObj) // polymorphic-root dispatch for the GEM_BINARY / GEM_JSON toString path

// ---------------------------------------------------------------------------
// gemfony_common_interface_indicator: the interface should be detectable.

TEST_CASE("GCommonInterfaceT: TestObj satisfies has_gemfony_common_interface",
          "[common][interface]") {
    // Note: has_clone_member / has_load_member intentionally not asserted here.
    // The base provides overloaded `clone()` (templated + non-templated), so
    // `&T::clone` is ambiguous and the trait reports false — by design.
    static_assert(has_gemfony_common_interface<TestObj>::value);
}

// ---------------------------------------------------------------------------
// clone() / clone<DerivedSelf>()

TEST_CASE("GCommonInterfaceT::clone: returns a deep copy as shared_ptr<Self>",
          "[common][interface][clone]") {
    TestObj src(42);
    auto cp = src.clone();
    REQUIRE(cp);
    CHECK(cp.get() != &src);
    CHECK(cp->v() == 42);

    // Mutating the clone must not affect the original.
    cp->v(7);
    CHECK(src.v() == 42);
}

TEST_CASE("GCommonInterfaceT::clone<T>: returns shared_ptr<T> when T derives from Self",
          "[common][interface][clone]") {
    // clone<Self>() also works — it forwards through convertSmartPointer.
    TestObj const src(11);
    auto cp = src.clone<TestObj>();
    REQUIRE(cp);
    CHECK(cp->v() == 11);
}

// ---------------------------------------------------------------------------
// load(shared_ptr) / load(reference)

TEST_CASE("GCommonInterfaceT::load(shared_ptr): copies remote state",
          "[common][interface][load]") {
    TestObj a(1);
    auto    b = std::make_shared<TestObj>(99);
    a.load(b);
    CHECK(a.v() == 99);
}

TEST_CASE("GCommonInterfaceT::load(reference): copies remote state",
          "[common][interface][load]") {
    TestObj a(1);
    TestObj const b(77);
    a.load(b);
    CHECK(a.v() == 77);
}

// ---------------------------------------------------------------------------
// compare(): EQUALITY pass / INEQUALITY pass / EQUALITY violation

TEST_CASE("GCommonInterfaceT::compare: EQUALITY on equal objects does not throw",
          "[common][interface][compare]") {
    TestObj const a(5);
    TestObj const b(5);
    CHECK_NOTHROW(a.compare(b, expectation::EQUALITY, 0.));
}

TEST_CASE("GCommonInterfaceT::compare: INEQUALITY on different objects does not throw",
          "[common][interface][compare]") {
    TestObj const a(5);
    TestObj const b(6);
    CHECK_NOTHROW(a.compare(b, expectation::INEQUALITY, 0.));
}

TEST_CASE("GCommonInterfaceT::compare: EQUALITY on different objects throws",
          "[common][interface][compare]") {
    TestObj const a(5);
    TestObj const b(6);
    CHECK_THROWS_AS(a.compare(b, expectation::EQUALITY, 0.), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// name()

TEST_CASE("GCommonInterfaceT::name: defaults to the templated form when not overridden",
          "[common][interface][name]") {
    TestObj const a;
    // Default name_() returns "GCommonInterfaceT<g_class_type>" — the base
    // does not see TestObj's type via the virtual dispatch (we do not
    // override name_ in TestObj), but the call must succeed non-empty.
    CHECK_FALSE(a.name().empty());
}

// ---------------------------------------------------------------------------
// toString / fromString round-trip in each archive mode

// The GArchive (Weft) codec modes are the only serialization modes: the toString/fromString path is
// driven through the polymorphic root dispatch (GEM_REGISTER_ARCHIVABLE(TestObj)). GEM_BINARY is the
// networked-wire default, GEM_JSON the checkpoint default.
TEST_CASE("GCommonInterfaceT: toString/fromString round-trip in GEM_BINARY mode",
          "[common][interface][serialize]") {
    TestObj const src(456);
    std::string const s = src.toString(serializationMode::GEM_BINARY);
    REQUIRE_FALSE(s.empty());

    TestObj dst(0);
    dst.fromString(s, serializationMode::GEM_BINARY);
    CHECK(dst.v() == 456);
}

TEST_CASE("GCommonInterfaceT: toString/fromString round-trip in GEM_JSON mode",
          "[common][interface][serialize]") {
    TestObj const src(-789);
    std::string const s = src.toString(serializationMode::GEM_JSON);
    REQUIRE_FALSE(s.empty());

    TestObj dst(0);
    dst.fromString(s, serializationMode::GEM_JSON);
    CHECK(dst.v() == -789);
}

// ---------------------------------------------------------------------------
// toFile / fromFile round-trip

TEST_CASE("GCommonInterfaceT: toFile / fromFile round-trip in GEM_JSON mode",
          "[common][interface][serialize]") {
    auto base = std::filesystem::temp_directory_path() / "geneva_interface_tests";
    std::filesystem::create_directories(base);
    auto path = base / "obj.json";
    std::filesystem::remove(path);

    TestObj const src(314);
    src.toFile(path, serializationMode::GEM_JSON);
    REQUIRE(std::filesystem::exists(path));

    TestObj dst(0);
    dst.fromFile(path, serializationMode::GEM_JSON);
    CHECK(dst.v() == 314);
    std::filesystem::remove(path);
}

TEST_CASE("GCommonInterfaceT::fromFile: missing file throws",
          "[common][interface][serialize]") {
    TestObj dst(0);
    CHECK_THROWS_AS(
        dst.fromFile(std::filesystem::path("/no/such/file/geneva_xyz"), serializationMode::GEM_JSON),
        geneva_exception);
}

// ---------------------------------------------------------------------------
// report(): non-empty JSON.

TEST_CASE("GCommonInterfaceT::report: returns non-empty JSON",
          "[common][interface][report]") {
    TestObj const a(5);
    auto r = a.report();
    CHECK_FALSE(r.empty());
    CHECK(r.contains("v_"));
}

