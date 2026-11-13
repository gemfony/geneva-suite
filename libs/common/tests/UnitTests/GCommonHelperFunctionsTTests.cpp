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
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"

using Catch::Approx;

// Minimal class hierarchy used across pointer-related tests.

namespace {

struct TBase {
    virtual ~TBase() = default;
    int base_val = 0;
};

struct TDerived : TBase {
    int derived_val = 42;
};

struct TOther : TBase {
    int other_val = 99;
};

enum class ScopedColor : unsigned {
    Red   = 0,
    Green = 1,
    Blue  = 2
};

// Underlying type std::uint8_t (range 0..255) is used to exercise the
// enum range checking in narrow (300 / -1 do not fit).
enum class SmallEnum : std::uint8_t {
    A = 0,
    B = 1,
    C = 200
};

} // namespace

// --- ptrDifferenceCheck --------------------------------------------------

TEST_CASE(
    "ptrDifferenceCheck raw: different pointers do not throw",
    "[common][helper][ptrDifferenceCheck]"
) {
    int const a = 1;
    int const b = 2;
    REQUIRE_NOTHROW(Gem::Common::ptrDifferenceCheck(&a, &b));
}

TEST_CASE(
    "ptrDifferenceCheck raw: null first pointer is a no-op",
    "[common][helper][ptrDifferenceCheck]"
) {
    int const a = 1;
    REQUIRE_NOTHROW(Gem::Common::ptrDifferenceCheck<int>(nullptr, &a));
}

#ifdef DEBUG
TEST_CASE(
    "ptrDifferenceCheck raw: same pointer throws in DEBUG",
    "[common][helper][ptrDifferenceCheck]"
) {
    int a = 1;
    REQUIRE_THROWS_AS(Gem::Common::ptrDifferenceCheck(&a, &a), geneva_exception);
}

TEST_CASE(
    "ptrDifferenceCheck shared_ptr: aliasing shared_ptrs throw in DEBUG",
    "[common][helper][ptrDifferenceCheck]"
) {
    auto p = std::make_shared<int>(42);
    REQUIRE_THROWS_AS(Gem::Common::ptrDifferenceCheck(p, p), geneva_exception);
}
#endif

TEST_CASE(
    "ptrDifferenceCheck shared_ptr: distinct objects do not throw",
    "[common][helper][ptrDifferenceCheck]"
) {
    auto p1 = std::make_shared<int>(1);
    auto p2 = std::make_shared<int>(2);
    REQUIRE_NOTHROW(Gem::Common::ptrDifferenceCheck(p1, p2));
}

// --- g_ptr_conversion ----------------------------------------------------

TEST_CASE(
    "g_ptr_conversion raw: valid downcast returns non-null",
    "[common][helper][g_ptr_conversion]"
) {
    TDerived d;
    const TBase *base_ptr = &d;
    const TDerived *derived_ptr = Gem::Common::g_ptr_conversion<TBase, TDerived>(base_ptr);
    REQUIRE(derived_ptr != nullptr);
    REQUIRE(derived_ptr == &d);
}

TEST_CASE("g_ptr_conversion raw: null input returns null", "[common][helper][g_ptr_conversion]") {
    const TDerived *p =
        Gem::Common::g_ptr_conversion<TBase, TDerived>(static_cast<const TBase *>(nullptr));
    REQUIRE(p == nullptr);
}

#ifdef DEBUG
TEST_CASE(
    "g_ptr_conversion raw: wrong dynamic type throws in DEBUG",
    "[common][helper][g_ptr_conversion]"
) {
    TOther other;
    const TBase *base_ptr = &other;
    REQUIRE_THROWS_AS((Gem::Common::g_ptr_conversion<TBase, TDerived>(base_ptr)), geneva_exception);
}
#endif

TEST_CASE(
    "g_ptr_conversion shared_ptr: valid downcast returns non-null",
    "[common][helper][g_ptr_conversion]"
) {
    auto sp = std::make_shared<TDerived>();
    auto base_sp = std::static_pointer_cast<TBase>(sp);
    auto derived_sp = Gem::Common::g_ptr_conversion<TBase, TDerived>(base_sp);
    REQUIRE(derived_sp != nullptr);
    REQUIRE(derived_sp.get() == sp.get());
}

TEST_CASE(
    "g_ptr_conversion shared_ptr: null input returns null",
    "[common][helper][g_ptr_conversion]"
) {
    std::shared_ptr<TBase> const null_sp;
    auto result = Gem::Common::g_ptr_conversion<TBase, TDerived>(null_sp);
    REQUIRE(result == nullptr);
}

// --- convertSmartPointer -------------------------------------------------

TEST_CASE("convertSmartPointer: valid downcast succeeds", "[common][helper][convertSmartPointer]") {
    auto sp = std::make_shared<TDerived>();
    auto base_sp = std::static_pointer_cast<TBase>(sp);
    auto derived_sp = Gem::Common::convertSmartPointer<TBase, TDerived>(base_sp);
    REQUIRE(derived_sp != nullptr);
}

#ifdef DEBUG
TEST_CASE(
    "convertSmartPointer: null input throws in DEBUG",
    "[common][helper][convertSmartPointer]"
) {
    std::shared_ptr<TBase> null_sp;
    REQUIRE_THROWS_AS(
        (Gem::Common::convertSmartPointer<TBase, TDerived>(null_sp)),
        geneva_exception
    );
}

TEST_CASE(
    "convertSmartPointer: wrong dynamic type throws in DEBUG",
    "[common][helper][convertSmartPointer]"
) {
    auto sp = std::make_shared<TOther>();
    auto base_sp = std::static_pointer_cast<TBase>(sp);
    REQUIRE_THROWS_AS(
        (Gem::Common::convertSmartPointer<TBase, TDerived>(base_sp)),
        geneva_exception
    );
}
#endif

// --- vecToString ---------------------------------------------------------

TEST_CASE("vecToString: empty vector returns empty string", "[common][helper][vecToString]") {
    REQUIRE(Gem::Common::vecToString(std::vector<int>{}).empty());
}

TEST_CASE("vecToString: integer vector formats correctly", "[common][helper][vecToString]") {
    std::string s = Gem::Common::vecToString(std::vector<int>{1, 2, 3});
    REQUIRE(s == "1 2 3 ");
}

TEST_CASE("vecToString: single-element vector", "[common][helper][vecToString]") {
    REQUIRE(Gem::Common::vecToString(std::vector<int>{42}) == "42 ");
}

// --- splitStringT (single separator) -------------------------------------

TEST_CASE("splitStringT<int>: splits space-separated integers", "[common][helper][splitStringT]") {
    auto result = Gem::Common::splitStringT<int>("1 2 3 4", " ");
    REQUIRE(result.size() == 4);
    REQUIRE(result[0] == 1);
    REQUIRE(result[3] == 4);
}

TEST_CASE(
    "splitStringT<double>: splits comma-separated doubles",
    "[common][helper][splitStringT]"
) {
    auto result = Gem::Common::splitStringT<double>("1.5,2.5,3.5", ",");
    REQUIRE(result.size() == 3);
    REQUIRE(result[0] == Approx(1.5));
    REQUIRE(result[2] == Approx(3.5));
}

TEST_CASE("splitStringT<string>: splits pipe-separated strings", "[common][helper][splitStringT]") {
    auto result = Gem::Common::splitStringT<std::string>("foo|bar|baz", "|");
    REQUIRE(result.size() == 3);
    REQUIRE(result[0] == "foo");
    REQUIRE(result[2] == "baz");
}

// --- splitStringT (two separators) ---------------------------------------

TEST_CASE("splitStringT<int,int>: splits pairs from '0/1 2/3'", "[common][helper][splitStringT2]") {
    auto result = Gem::Common::splitStringT<int, int>("0/1 2/3", " ", "/");
    REQUIRE(result.size() == 2);
    REQUIRE(std::get<0>(result[0]) == 0);
    REQUIRE(std::get<1>(result[0]) == 1);
    REQUIRE(std::get<0>(result[1]) == 2);
    REQUIRE(std::get<1>(result[1]) == 3);
}

TEST_CASE("splitStringT two-sep: identical separators throw", "[common][helper][splitStringT2]") {
    REQUIRE_THROWS_AS((Gem::Common::splitStringT<int, int>("0/1", "/", "/")), geneva_exception);
}

// --- getMapItem ----------------------------------------------------------

TEST_CASE("getMapItem: returns correct value for existing key", "[common][helper][getMapItem]") {
    std::map<std::string, int> m{{"a", 1}, {"b", 2}, {"c", 3}};
    REQUIRE(Gem::Common::getMapItem(m, "b") == 2);
}

TEST_CASE("getMapItem: modifying returned reference changes map", "[common][helper][getMapItem]") {
    std::map<std::string, int> m{{"x", 10}};
    Gem::Common::getMapItem(m, "x") = 99;
    REQUIRE(m.at("x") == 99);
}

TEST_CASE(
    "getMapItem const: returns correct value for existing key",
    "[common][helper][getMapItem]"
) {
    const std::map<std::string, double> m{{"pi", std::numbers::pi}};
    REQUIRE(Gem::Common::getMapItem(m, "pi") == Approx(3.14159));
}

TEST_CASE("getMapItem: missing key throws", "[common][helper][getMapItem]") {
    std::map<std::string, int> m{{"a", 1}};
    REQUIRE_THROWS_AS(Gem::Common::getMapItem(m, "z"), geneva_exception);
}

TEST_CASE("getMapItem: empty map throws", "[common][helper][getMapItem]") {
    std::map<std::string, int> m;
    REQUIRE_THROWS_AS(Gem::Common::getMapItem(m, "a"), geneva_exception);
}

// --- to_string -----------------------------------------------------------

TEST_CASE("to_string: integral types", "[common][helper][to_string]") {
    REQUIRE(Gem::Common::to_string(42) == "42");
    REQUIRE(Gem::Common::to_string(-7) == "-7");
    REQUIRE(Gem::Common::to_string(0) == "0");
    REQUIRE(Gem::Common::to_string(42u) == "42");
}

TEST_CASE("to_string: floating-point types", "[common][helper][to_string]") {
    REQUIRE(Gem::Common::to_string(1.5) == "1.5");
    REQUIRE(Gem::Common::to_string(0.) == "0");
    REQUIRE(Gem::Common::to_string(1.5f) == "1.5");
}

TEST_CASE("to_string: scoped enum yields its underlying integer", "[common][helper][to_string]") {
    REQUIRE(Gem::Common::to_string(ScopedColor::Red) == "0");
    REQUIRE(Gem::Common::to_string(ScopedColor::Green) == "1");
    REQUIRE(Gem::Common::to_string(ScopedColor::Blue) == "2");
}

// --- erase_if ------------------------------------------------------------

TEST_CASE("erase_if: removes matching elements and returns count", "[common][helper][erase_if]") {
    std::vector<int> v{1, 2, 3, 4, 5, 6};
    std::size_t const n = Gem::Common::erase_if(v, [](int x) { return x % 2 == 0; });
    REQUIRE(n == 3);
    REQUIRE(v == std::vector<int>{1, 3, 5});
}

TEST_CASE(
    "erase_if: nothing matches returns 0 and leaves container unchanged",
    "[common][helper][erase_if]"
) {
    std::vector<int> v{1, 3, 5};
    std::size_t const n = Gem::Common::erase_if(v, [](int x) { return x % 2 == 0; });
    REQUIRE(n == 0);
    REQUIRE(v == std::vector<int>{1, 3, 5});
}

TEST_CASE("erase_if: empty container is a no-op", "[common][helper][erase_if]") {
    std::vector<int> v;
    std::size_t const n = Gem::Common::erase_if(v, [](int) { return true; });
    REQUIRE(n == 0);
    REQUIRE(v.empty());
}

TEST_CASE("erase_if: all elements match clears the container", "[common][helper][erase_if]") {
    std::vector<int> v{2, 4, 6};
    std::size_t const n = Gem::Common::erase_if(v, [](int) { return true; });
    REQUIRE(n == 3);
    REQUIRE(v.empty());
}

// --- environmentVariableAs -----------------------------------------------

TEST_CASE(
    "environmentVariableAs: missing variable returns empty optional",
    "[common][helper][environmentVariableAs]"
) {
    auto result = Gem::Common::environmentVariableAs<int>("GENEVA_TEST_NONEXISTENT_VAR_XYZ_12345");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE(
    "environmentVariableAs: existing variable is read and converted",
    "[common][helper][environmentVariableAs]"
) {
    ::setenv("GENEVA_TEST_VAR", "42", 1);
    auto result = Gem::Common::environmentVariableAs<int>("GENEVA_TEST_VAR");
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 42);
    ::unsetenv("GENEVA_TEST_VAR");
}

TEST_CASE(
    "environmentVariableAs: string variable is returned verbatim",
    "[common][helper][environmentVariableAs]"
) {
    ::setenv("GENEVA_TEST_STR", "hello", 1);
    auto result = Gem::Common::environmentVariableAs<std::string>("GENEVA_TEST_STR");
    REQUIRE(result.has_value());
    REQUIRE(result.value() == "hello");
    ::unsetenv("GENEVA_TEST_STR");
}

// --- narrow ---------------------------------------------------------
//
// narrow<To>(From) is a checked numeric/enum cast: it returns the
// converted value, or throws std::overflow_error if a narrowing conversion
// would change the value. Range checks apply when the *target* is an integer
// (or an enum whose underlying type is an integer), or when the source is a
// wider floating-point type and the target is narrower floating-point. Integer
// or enum conversions to a floating-point target are NOT range-checked. Enum
// source/target types are
// handled via their underlying integer type.

TEST_CASE("narrow: lossless widening preserves the value", "[common][helper][narrow]") {
    REQUIRE(Gem::Common::narrow<int>(std::int16_t{1234}) == 1234);
    REQUIRE(Gem::Common::narrow<std::int64_t>(std::int32_t{-7}) == -7);
    REQUIRE(Gem::Common::narrow<unsigned>(std::uint8_t{255}) == 255u);
}

TEST_CASE("narrow: in-range integer narrowing succeeds", "[common][helper][narrow]") {
    REQUIRE(Gem::Common::narrow<std::int8_t>(100) == std::int8_t{100});
    REQUIRE(Gem::Common::narrow<std::uint8_t>(255) == std::uint8_t{255});
    REQUIRE(Gem::Common::narrow<std::int32_t>(std::int64_t{123456}) == 123456);
}

TEST_CASE(
    "narrow: integer overflow throws std::overflow_error",
    "[common][helper][narrow]"
) {
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::int8_t>(128), std::overflow_error);
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::int8_t>(-129), std::overflow_error);
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::uint8_t>(256), std::overflow_error);
}

TEST_CASE(
    "narrow: signed->unsigned underflow throws for narrower targets",
    "[common][helper][narrow]"
) {
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::uint8_t>(std::int32_t{-1}), std::overflow_error);
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::uint16_t>(std::int32_t{-1}), std::overflow_error);
}

TEST_CASE("narrow: float->integer truncates toward zero", "[common][helper][narrow]") {
    REQUIRE(Gem::Common::narrow<int>(3.0) == 3);
    REQUIRE(Gem::Common::narrow<int>(3.9) == 3);
    REQUIRE(Gem::Common::narrow<int>(-3.9) == -3);
}

TEST_CASE(
    "narrow: float->integer overflow throws std::overflow_error",
    "[common][helper][narrow]"
) {
    // The source range is validated before the (otherwise UB) float->int cast.
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::int32_t>(1.0e18), std::overflow_error);
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::int32_t>(-1.0e18), std::overflow_error);
}

TEST_CASE(
    "narrow: exactly-representable int32 maximum from double is accepted",
    "[common][helper][narrow]"
) {
    // int32_max (2^31-1) IS exactly representable as a double, so equality at
    // the boundary is legitimate (the max_is_exact path).
    constexpr double i32max = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    REQUIRE(
        Gem::Common::narrow<std::int32_t>(i32max)
        == std::numeric_limits<std::int32_t>::max()
    );
}

TEST_CASE("narrow: int64/double boundary (2^63) is rejected", "[common][helper][narrow]") {
    // int64_max == 2^63-1 is NOT exactly representable as a double;
    // static_cast<double>(int64_max) rounds up to 2^63, which is out of range
    // for int64_t. narrow must reject it rather than convert.
    const double i64max_as_double = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    REQUIRE_THROWS_AS(
        Gem::Common::narrow<std::int64_t>(i64max_as_double),
        std::overflow_error
    );

    // A value comfortably below 2^63 round-trips and matches the plain cast.
    REQUIRE_NOTHROW(Gem::Common::narrow<std::int64_t>(9.0e18));
    REQUIRE(Gem::Common::narrow<std::int64_t>(9.0e18) == static_cast<std::int64_t>(9.0e18));
}

TEST_CASE("narrow: enum source uses its underlying integer", "[common][helper][narrow]") {
    REQUIRE(Gem::Common::narrow<int>(SmallEnum::C) == 200);
    REQUIRE(Gem::Common::narrow<unsigned>(SmallEnum::B) == 1u);
}

TEST_CASE("narrow: integer->enum within range succeeds", "[common][helper][narrow]") {
    REQUIRE(Gem::Common::narrow<SmallEnum>(1) == SmallEnum::B);
    REQUIRE(Gem::Common::narrow<SmallEnum>(200) == SmallEnum::C);
}

TEST_CASE(
    "narrow: integer->enum out of range is bounds-checked and throws",
    "[common][helper][narrow]"
) {
    // SmallEnum's underlying type is std::uint8_t (max 255); 300 and -1 cannot
    // be represented. This is exactly the case boost::numeric_cast cannot guard
    // (it does not accept enum target types at all).
    REQUIRE_THROWS_AS(Gem::Common::narrow<SmallEnum>(300), std::overflow_error);
    REQUIRE_THROWS_AS(Gem::Common::narrow<SmallEnum>(-1), std::overflow_error);
}

TEST_CASE(
    "narrow: integer->floating target is not range-checked (precision loss allowed)",
    "[common][helper][narrow]"
) {
    // Integer/enum -> floating is always in range; precision may be lost (e.g.
    // int64_max -> double) but that is inherent and never throws.
    REQUIRE_NOTHROW(Gem::Common::narrow<double>(std::numeric_limits<std::int64_t>::max()));
    REQUIRE(Gem::Common::narrow<double>(42) == 42.0);
    REQUIRE(Gem::Common::narrow<double>(std::int32_t{123456}) == 123456.0);
}

TEST_CASE(
    "narrow: floating overflow to a narrower type throws",
    "[common][helper][narrow]"
) {
    // double -> float overflow: the magnitude exceeds FLT_MAX. Validated and
    // rejected before the cast (an out-of-range floating conversion is UB).
    REQUIRE_THROWS_AS(Gem::Common::narrow<float>(1.0e300), std::overflow_error);
    REQUIRE_THROWS_AS(Gem::Common::narrow<float>(-1.0e300), std::overflow_error);

    // In-range narrowing is allowed even though precision is lost.
    REQUIRE_NOTHROW(Gem::Common::narrow<float>(3.14159265358979));
    REQUIRE_NOTHROW(Gem::Common::narrow<float>(1.0e30));

    // inf passes through unchanged (no value is lost).
    REQUIRE_NOTHROW(Gem::Common::narrow<float>(std::numeric_limits<double>::infinity()));
    REQUIRE(std::isinf(Gem::Common::narrow<float>(std::numeric_limits<double>::infinity())));
}
