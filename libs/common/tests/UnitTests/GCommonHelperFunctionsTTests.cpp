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
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <locale>
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
    Red = 0,
    Green = 1,
    Blue = 2
};

// Underlying type std::uint8_t (range 0..255) is used to exercise the
// enum range checking in narrow (300 / -1 do not fit).
enum class SmallEnum : std::uint8_t {
    A = 0,
    B = 1,
    C = 200
};

/** @brief Makes @p name the global C++ locale for its lifetime and restores the previous one. */
class GGlobalLocale {
public:
    explicit GGlobalLocale(const char *name)
      : previous_(std::locale()) {
        try {
            std::locale::global(std::locale(name));
            active_ = true;
        }
        catch(const std::runtime_error &) {
            active_ = false; // the locale is not installed
        }
    }
    ~GGlobalLocale() {
        std::locale::global(previous_);
    }
    GGlobalLocale(const GGlobalLocale &) = delete;
    GGlobalLocale &operator=(const GGlobalLocale &) = delete;
    GGlobalLocale(GGlobalLocale &&) = delete;
    GGlobalLocale &operator=(GGlobalLocale &&) = delete;
    /** @return Whether the locale is in force and writes a decimal comma */
    [[nodiscard]] bool hasDecimalComma() const {
        return active_ && std::use_facet<std::numpunct<char>>(std::locale()).decimal_point() == ',';
    }

private:
    std::locale previous_;
    bool active_ = false;
};

constexpr const char *COMMALOCALE = "de_DE.UTF-8";

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

TEST_CASE("ptrDifferenceCheck raw: same pointer throws", "[common][helper][ptrDifferenceCheck]") {
    int a = 1;
    REQUIRE_THROWS_AS(Gem::Common::ptrDifferenceCheck(&a, &a), gem_exception);
}

TEST_CASE(
    "ptrDifferenceCheck shared_ptr: aliasing shared_ptrs throw",
    "[common][helper][ptrDifferenceCheck]"
) {
    auto p = std::make_shared<int>(42);
    REQUIRE_THROWS_AS(Gem::Common::ptrDifferenceCheck(p, p), gem_exception);
}

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

#ifndef NDEBUG // the dynamic type is checked by GEM_DEBUG_CHECK
TEST_CASE(
    "g_ptr_conversion raw: wrong dynamic type throws without NDEBUG",
    "[common][helper][g_ptr_conversion]"
) {
    TOther other;
    const TBase *base_ptr = &other;
    REQUIRE_THROWS_AS((Gem::Common::g_ptr_conversion<TBase, TDerived>(base_ptr)), gem_exception);
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

#ifndef NDEBUG // the dynamic type is checked by GEM_DEBUG_CHECK
TEST_CASE(
    "g_ptr_conversion shared_ptr: wrong dynamic type throws without NDEBUG",
    "[common][helper][g_ptr_conversion]"
) {
    auto sp = std::make_shared<TOther>();
    auto base_sp = std::static_pointer_cast<TBase>(sp);
    REQUIRE_THROWS_AS((Gem::Common::g_ptr_conversion<TBase, TDerived>(base_sp)), gem_exception);
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

TEST_CASE(
    "splitStringT for pairs of int: splits pairs from '0/1 2/3'",
    "[common][helper][splitStringT2]"
) {
    auto result = Gem::Common::splitStringT<int, int>("0/1 2/3", " ", "/");
    REQUIRE(result.size() == 2);
    REQUIRE(std::get<0>(result[0]) == 0);
    REQUIRE(std::get<1>(result[0]) == 1);
    REQUIRE(std::get<0>(result[1]) == 2);
    REQUIRE(std::get<1>(result[1]) == 3);
}

TEST_CASE("splitStringT two-sep: identical separators throw", "[common][helper][splitStringT2]") {
    REQUIRE_THROWS_AS((Gem::Common::splitStringT<int, int>("0/1", "/", "/")), gem_exception);
}

// A fragment without its second sub-field was indexed past the end in a release build (the check
// was compiled only in Debug builds); it now throws in every build.
TEST_CASE(
    "splitStringT two-sep: a fragment without two sub-fields throws",
    "[common][helper][splitStringT2]"
) {
    REQUIRE_THROWS_AS((Gem::Common::splitStringT<int, int>("0/1 2", " ", "/")), gem_exception);
    REQUIRE_THROWS_AS((Gem::Common::splitStringT<int, int>("0/1/2", " ", "/")), gem_exception);
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
    REQUIRE_THROWS_AS(Gem::Common::getMapItem(m, "z"), gem_exception);
}

TEST_CASE("getMapItem: empty map throws", "[common][helper][getMapItem]") {
    std::map<std::string, int> m;
    REQUIRE_THROWS_AS(Gem::Common::getMapItem(m, "a"), gem_exception);
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

// to_string() of a floating-point value wrote max_digits10 digits: 0.1 became "0.10000000000000001"
// in help texts and configuration values. It writes the shortest text that reads back as the same
// value, and parseWhole() reads every such text back, the special values included (the stream
// wrote "inf" and "nan" but could not read them).
TEST_CASE(
    "to_string: the shortest text that reads back as the same value",
    "[common][helper][to_string]"
) {
    CHECK(Gem::Common::to_string(0.1) == "0.1");
    CHECK(Gem::Common::to_string(0.1F) == "0.1");
    CHECK(Gem::Common::to_string(1.0e20) == "1e+20");
    for(const double v :
        {0.1,
         -1.0 / 3.0,
         1.0e-300,
         std::numeric_limits<double>::max(),
         std::numeric_limits<double>::denorm_min(),
         std::numeric_limits<double>::infinity(),
         -std::numeric_limits<double>::infinity()}) {
        CAPTURE(v);
        CHECK(Gem::Common::from_string<double>(Gem::Common::to_string(v)) == v);
    }
    CHECK(std::isnan(
        Gem::Common::from_string<double>(
            Gem::Common::to_string(std::numeric_limits<double>::quiet_NaN())
        )
    ));
}

// A program may make a decimal-comma locale its global C++ locale. The text forms of numbers that
// go to files (CSV rows, configuration values) must not follow it (a defect: the string streams
// took the global locale and wrote "1,5" or "1.234.567,5").
TEST_CASE(
    "to_string / from_string: numbers do not follow the global locale",
    "[common][helper][to_string]"
) {
    const GGlobalLocale comma(COMMALOCALE);
    if(not comma.hasDecimalComma()) {
        SKIP("the decimal-comma locale " << COMMALOCALE << " is not installed");
    }
    CHECK(Gem::Common::to_string(1.5) == "1.5");
    CHECK(Gem::Common::to_string(1234567.5) == "1234567.5");
    CHECK(Gem::Common::to_string(std::chrono::duration<double>(2.5)) == "2.5s");
    CHECK(Gem::Common::from_string<double>("1234.5") == 1234.5);
    CHECK(Gem::Common::from_string<int>("1234567") == 1234567);
}

TEST_CASE("to_string: scoped enum yields its underlying integer", "[common][helper][to_string]") {
    REQUIRE(Gem::Common::to_string(ScopedColor::Red) == "0");
    REQUIRE(Gem::Common::to_string(ScopedColor::Green) == "1");
    REQUIRE(Gem::Common::to_string(ScopedColor::Blue) == "2");
}

// --- parseWhole / from_string --------------------------------------------

// parseWhole() is the one strict reader of the collection: a text is one value of the type, or
// nothing.
TEST_CASE("parseWhole: a text is one value of the type or nothing", "[common][helper][parseWhole]") {
    using Gem::Common::parseWhole;
    CHECK(parseWhole<int>(" 12 ") == 12);
    CHECK(parseWhole<int>("+12") == 12);
    CHECK(parseWhole<int>("-12") == -12);
    CHECK_FALSE(parseWhole<int>("12abc"));
    CHECK_FALSE(parseWhole<int>("1 2"));
    CHECK_FALSE(parseWhole<int>("+-12"));
    CHECK_FALSE(parseWhole<int>(""));
    CHECK_FALSE(parseWhole<int>("1.5"));
    CHECK_FALSE(parseWhole<int>("99999999999"));
    CHECK_FALSE(parseWhole<unsigned>("-1"));
    CHECK_FALSE(parseWhole<std::size_t>("-0"));
    CHECK(parseWhole<std::uint8_t>("255") == std::uint8_t{255});
    CHECK_FALSE(parseWhole<std::uint8_t>("300"));
    CHECK(parseWhole<std::int8_t>("-128") == std::int8_t{-128});
    CHECK(parseWhole<double>("1.5e3") == 1500.);
    CHECK(parseWhole<double>("-0.25") == -0.25);
    CHECK_FALSE(parseWhole<double>("1e400"));
    CHECK_FALSE(parseWhole<double>("1,5"));
    CHECK(std::isnan(parseWhole<double>("nan").value()));
    CHECK(std::isinf(parseWhole<double>("-inf").value()));
    CHECK(parseWhole<float>("0.5") == 0.5F);
    CHECK(parseWhole<bool>("true") == true);
    CHECK(parseWhole<bool>("0") == false);
    CHECK_FALSE(parseWhole<bool>("yes"));
    CHECK(parseWhole<char>("x") == 'x');
    CHECK_FALSE(parseWhole<char>("xy"));
    CHECK(parseWhole<std::string>(" a b ") == " a b ");
}

// from_string() replaced boost::lexical_cast, which threw on text that is no value of the type; it
// returned whatever the stream had read instead (a defect: "abc" read as 0, "12abc" as 12, "1.5" as
// the integer 1, "-1" as an unsigned 4294967295, "a b" as the string "a", "200" as the
// std::uint8_t 50, the character '2').
TEST_CASE(
    "from_string: a text that is not one value of the type throws",
    "[common][helper][from_string]"
) {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_AS(Gem::Common::from_string<int>("abc"), gem_exception);
    CHECK_THROWS_AS(Gem::Common::from_string<int>("12abc"), gem_exception);
    CHECK_THROWS_AS(Gem::Common::from_string<int>("1.5"), gem_exception);
    CHECK_THROWS_AS(Gem::Common::from_string<unsigned>("-1"), gem_exception);
    CHECK_THROWS_AS(Gem::Common::from_string<double>(""), gem_exception);
    CHECK_THROWS_AS(Gem::Common::from_string<std::uint8_t>("300"), gem_exception);
    CHECK_THROWS_WITH(
        Gem::Common::from_string<int>("12abc"),
        ContainsSubstring("\"12abc\"") && ContainsSubstring("int")
    );

    CHECK(Gem::Common::from_string<int>(" 12 ") == 12);
    CHECK(Gem::Common::from_string<int>("+12") == 12);
    CHECK(Gem::Common::from_string<std::uint8_t>("200") == std::uint8_t{200});
    CHECK(Gem::Common::from_string<std::string>("a b") == "a b");
    CHECK(Gem::Common::from_string<bool>("true"));
    CHECK_FALSE(Gem::Common::from_string<bool>("0"));
}

TEST_CASE(
    "splitStringT: a fragment that is not a value of the type throws",
    "[common][helper][splitStringT]"
) {
    CHECK_THROWS_AS(Gem::Common::splitStringT<std::size_t>("2-x-1", "-"), gem_exception);
    CHECK_THROWS_AS(Gem::Common::splitStringT<std::size_t>("2-4.5-1", "-"), gem_exception);
    CHECK_THROWS_AS((Gem::Common::splitStringT<int, int>("0/1 2/y", " ", "/")), gem_exception);
}

// --- environmentVariableAs -----------------------------------------------

TEST_CASE(
    "environmentVariableAs: missing variable returns empty optional",
    "[common][helper][environmentVariableAs]"
) {
    auto result = Gem::Common::environmentVariableAs<int>("GEM_TEST_NONEXISTENT_VAR_XYZ_12345");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE(
    "environmentVariableAs: existing variable is read and converted",
    "[common][helper][environmentVariableAs]"
) {
    ::setenv("GEM_TEST_VAR", "42", 1);
    auto result = Gem::Common::environmentVariableAs<int>("GEM_TEST_VAR");
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 42);
    ::unsetenv("GEM_TEST_VAR");
}

TEST_CASE(
    "environmentVariableAs: string variable is returned verbatim",
    "[common][helper][environmentVariableAs]"
) {
    ::setenv("GEM_TEST_STR", "hello", 1);
    auto result = Gem::Common::environmentVariableAs<std::string>("GEM_TEST_STR");
    REQUIRE(result.has_value());
    REQUIRE(result.value() == "hello");
    ::unsetenv("GEM_TEST_STR");
}

TEST_CASE(
    "environmentVariableAs: a value that is not one of the type throws and names the variable",
    "[common][helper][environmentVariableAs]"
) {
    ::setenv("GEM_TEST_BAD_INT", "12abc", 1);
    CHECK_THROWS_WITH(
        Gem::Common::environmentVariableAs<int>("GEM_TEST_BAD_INT"),
        Catch::Matchers::ContainsSubstring("GEM_TEST_BAD_INT") &&
            Catch::Matchers::ContainsSubstring("\"12abc\"")
    );
    ::unsetenv("GEM_TEST_BAD_INT");
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

TEST_CASE("narrow: integer overflow throws std::overflow_error", "[common][helper][narrow]") {
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

// A round trip between a signed and an unsigned type of the same width, or from a signed type to a
// wider unsigned one, gives back the original value; the change of sign has to be checked by itself
// (a defect: narrow<std::size_t>(-1) gave 18446744073709551615).
TEST_CASE("narrow: a change of sign throws at equal or larger width", "[common][helper][narrow]") {
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::uint32_t>(std::int32_t{-1}), std::overflow_error);
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::size_t>(-1), std::overflow_error);
    REQUIRE_THROWS_AS(
        Gem::Common::narrow<std::int32_t>(std::uint32_t{4294967295U}),
        std::overflow_error
    );
    REQUIRE_THROWS_AS(
        Gem::Common::narrow<std::int64_t>(std::numeric_limits<std::uint64_t>::max()),
        std::overflow_error
    );
    REQUIRE(Gem::Common::narrow<std::uint32_t>(std::int32_t{5}) == 5U);
    REQUIRE(Gem::Common::narrow<std::int32_t>(std::uint32_t{7}) == 7);
    REQUIRE(Gem::Common::narrow<std::size_t>(std::int64_t{0}) == 0U);
}

// Every comparison with NaN is false, so NaN passed both range checks and reached the float->integer
// cast, which is undefined for it (a defect).
TEST_CASE("narrow: NaN to an integer throws", "[common][helper][narrow]") {
    REQUIRE_THROWS_AS(
        Gem::Common::narrow<int>(std::numeric_limits<double>::quiet_NaN()),
        std::overflow_error
    );
    REQUIRE_THROWS_AS(
        Gem::Common::narrow<std::uint64_t>(std::numeric_limits<float>::quiet_NaN()),
        std::overflow_error
    );
}

// The message names the value and both types, so a reader knows what did not fit where.
TEST_CASE("narrow: the message names the value and the two types", "[common][helper][narrow]") {
    using Catch::Matchers::ContainsSubstring;
    REQUIRE_THROWS_WITH(
        Gem::Common::narrow<std::uint8_t>(300),
        ContainsSubstring("300") && ContainsSubstring("int") && ContainsSubstring("unsigned char")
    );
    REQUIRE_THROWS_WITH(
        Gem::Common::narrow<std::int32_t>(1.0e18),
        ContainsSubstring("1e+18") && ContainsSubstring("double")
    );
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
    REQUIRE(Gem::Common::narrow<std::int32_t>(i32max) == std::numeric_limits<std::int32_t>::max());
}

TEST_CASE("narrow: int64/double boundary (2^63) is rejected", "[common][helper][narrow]") {
    // int64_max == 2^63-1 is NOT exactly representable as a double;
    // static_cast<double>(int64_max) rounds up to 2^63, which is out of range
    // for int64_t. narrow must reject it rather than convert.
    const double i64max_as_double = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    REQUIRE_THROWS_AS(Gem::Common::narrow<std::int64_t>(i64max_as_double), std::overflow_error);

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

TEST_CASE("narrow: floating overflow to a narrower type throws", "[common][helper][narrow]") {
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
