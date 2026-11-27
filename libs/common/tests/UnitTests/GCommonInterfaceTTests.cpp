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
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "common/GArchiveNamed.hpp"     // archive_named (named-member emitter)
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE

#include "common/GCommonEnums.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// Concrete Geneva-style class. Inherits from GCommonInterfaceT<Self> (CRTP)
// and implements every pure virtual the framework requires.

namespace {

class TestObj : public GCommonInterfaceT<TestObj> {
public:
    TestObj() = default;
    explicit TestObj(int v)
      : v_(v) {
    }

    [[nodiscard]] int v() const {
        return v_;
    }
    void v(int v) {
        v_ = v;
    }

    /** Makes the next writes throw after v_ was written (a write that fails midway). */
    static inline bool throw_while_saving = false;

protected:
    void load_(TestObj const *cp) override {
        if(cp)
            v_ = cp->v_;
    }

    void compare_(TestObj const &cp, expectation const &e, double const &limit) const override {
        GToken token("TestObj", e, limit);
        compare_base_t<GCommonInterfaceT<TestObj>>(*this, cp, token);
        compare_t(Gem::Common::getIdentity(v_, cp.v_, "v_", "cp.v_"), token);
        token.evaluate();
    }

private:
    [[nodiscard]] TestObj *clone_() const override {
        return new TestObj(*this);
    }

    friend struct Gem::Weft::access;
    template <class archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] unsigned int version) {
        Gem::Common::archive_named(ar, "v_", v_);
        if constexpr(archive_type::is_saving) {
            if(throw_while_saving) {
                throw std::runtime_error("TestObj: a write that fails midway");
            }
        }
    }

    int v_{0};
};

} // namespace

GEM_REGISTER_ARCHIVABLE(
    TestObj
) // polymorphic-root dispatch for the GEM_BINARY / GEM_JSON toString path

// ---------------------------------------------------------------------------
// gemfony_common_interface_indicator: the interface should be detectable.

TEST_CASE("GCommonInterfaceT: TestObj satisfies gemfony_common_interface", "[common][interface]") {
    STATIC_REQUIRE(gemfony_common_interface<TestObj>);
}

// ---------------------------------------------------------------------------
// clone() / clone<DerivedSelf>()

TEST_CASE(
    "GCommonInterfaceT::clone: returns a deep copy as shared_ptr<Self>",
    "[common][interface][clone]"
) {
    TestObj src(42);
    auto cp = src.clone();
    REQUIRE(cp);
    CHECK(cp.get() != &src);
    CHECK(cp->v() == 42);

    // Mutating the clone must not affect the original.
    cp->v(7);
    CHECK(src.v() == 42);
}

TEST_CASE(
    "GCommonInterfaceT::clone<T>: returns unique_ptr<T> when T derives from Self",
    "[common][interface][clone]"
) {
    // clone<Self>() also works -- the clone is converted with a checked dynamic_cast.
    TestObj const src(11);
    auto cp = src.clone<TestObj>();
    REQUIRE(cp);
    CHECK(cp->v() == 11);
}

// ---------------------------------------------------------------------------
// load(shared_ptr) / load(reference)

TEST_CASE("GCommonInterfaceT::load(shared_ptr): copies remote state", "[common][interface][load]") {
    TestObj a(1);
    auto b = std::make_shared<TestObj>(99);
    a.load(b);
    CHECK(a.v() == 99);
}

TEST_CASE("GCommonInterfaceT::load(reference): copies remote state", "[common][interface][load]") {
    TestObj a(1);
    TestObj const b(77);
    a.load(b);
    CHECK(a.v() == 77);
}

// ---------------------------------------------------------------------------
// compare(): EQUALITY pass / INEQUALITY pass / EQUALITY violation

TEST_CASE(
    "GCommonInterfaceT::compare: EQUALITY on equal objects does not throw",
    "[common][interface][compare]"
) {
    TestObj const a(5);
    TestObj const b(5);
    CHECK_NOTHROW(a.compare(b, expectation::EQUALITY, 0.));
}

TEST_CASE(
    "GCommonInterfaceT::compare: INEQUALITY on different objects does not throw",
    "[common][interface][compare]"
) {
    TestObj const a(5);
    TestObj const b(6);
    CHECK_NOTHROW(a.compare(b, expectation::INEQUALITY, 0.));
}

TEST_CASE(
    "GCommonInterfaceT::compare: EQUALITY on different objects throws",
    "[common][interface][compare]"
) {
    TestObj const a(5);
    TestObj const b(6);
    CHECK_THROWS_AS(a.compare(b, expectation::EQUALITY, 0.), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// name()

TEST_CASE(
    "GCommonInterfaceT::name: defaults to the templated form when not overridden",
    "[common][interface][name]"
) {
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
TEST_CASE(
    "GCommonInterfaceT: toString/fromString round-trip in GEM_BINARY mode",
    "[common][interface][serialize]"
) {
    TestObj const src(456);
    std::string const s = src.toString(serializationMode::GEM_BINARY);
    REQUIRE_FALSE(s.empty());

    TestObj dst(0);
    dst.fromString(s, serializationMode::GEM_BINARY);
    CHECK(dst.v() == 456);
}

TEST_CASE(
    "GCommonInterfaceT: toString/fromString round-trip in GEM_JSON mode",
    "[common][interface][serialize]"
) {
    TestObj const src(-789);
    std::string const s = src.toString(serializationMode::GEM_JSON);
    REQUIRE_FALSE(s.empty());

    TestObj dst(0);
    dst.fromString(s, serializationMode::GEM_JSON);
    CHECK(dst.v() == -789);
}

// The object streams and the two free functions write one format (R138-37): a string of toString() is
// read by sharedPtrFromString<>(), a string of sharedPtrToString() by fromString(), in both modes.
TEST_CASE(
    "GCommonInterfaceT: strings of toString() and sharedPtrToString() are interchangeable",
    "[common][interface][serialize]"
) {
    for(auto const mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        TestObj const src(123);
        auto const restored = sharedPtrFromString<TestObj>(src.toString(mode), mode);
        REQUIRE(restored);
        CHECK(restored->v() == 123);

        TestObj dst(0);
        dst.fromString(sharedPtrToString(std::make_shared<TestObj>(321), mode), mode);
        CHECK(dst.v() == 321);

        CHECK(src.toString(mode) == sharedPtrToString(std::make_shared<TestObj>(123), mode));
    }
}

// ---------------------------------------------------------------------------
// toFile / fromFile round-trip

TEST_CASE(
    "GCommonInterfaceT: toFile / fromFile round-trip in GEM_JSON mode",
    "[common][interface][serialize]"
) {
    auto base = std::filesystem::temp_directory_path() / "gem_interface_tests";
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

// A write that fails midway leaves the file that was there before, and no temporary file. Regression
// (2026-11-20): toFile() wrote into the target itself, so a write that failed -- or a process that was
// killed while it wrote a checkpoint -- left an empty or a truncated file in its place.
TEST_CASE(
    "GCommonInterfaceT::toFile: a write that fails midway leaves the previous file",
    "[common][interface][serialize]"
) {
    auto const base = std::filesystem::temp_directory_path() / "gem_interface_tests_failed_write";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    auto const path = base / "obj.json";

    TestObj const before(1);
    before.toFile(path, serializationMode::GEM_JSON);

    TestObj const after(2);
    TestObj::throw_while_saving = true;
    CHECK_THROWS(after.toFile(path, serializationMode::GEM_JSON));
    TestObj::throw_while_saving = false;

    TestObj dst(0);
    REQUIRE_NOTHROW(dst.fromFile(path, serializationMode::GEM_JSON));
    CHECK(dst.v() == 1);
    // Only the file itself is left in the directory: the temporary file was removed
    CHECK(
        std::distance(
            std::filesystem::directory_iterator(base),
            std::filesystem::directory_iterator{}
        ) == 1
    );
    std::filesystem::remove_all(base);
}

TEST_CASE("GCommonInterfaceT::fromFile: missing file throws", "[common][interface][serialize]") {
    TestObj dst(0);
    CHECK_THROWS_AS(
        dst.fromFile(std::filesystem::path("/no/such/file/gem_xyz"), serializationMode::GEM_JSON),
        gem_exception
    );
}

// ---------------------------------------------------------------------------
// report(): non-empty JSON.

TEST_CASE("GCommonInterfaceT::report: returns non-empty JSON", "[common][interface][report]") {
    TestObj const a(5);
    auto r = a.report();
    CHECK_FALSE(r.empty());
    CHECK(r.contains("v_"));
}
