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
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "common/GCommonEnums.hpp"
#include "common/record/GRecordTypes.hpp"
#include "common/testing/GStandardTests.hpp"

using namespace Gem::Common;
using Gem::Common::Testing::checkEnumNames;

// Generic helper: stream the enum out, parse the integral form back in and
// expect bit-for-bit equality. The integral-only round-trip mirrors the
// stream-based string<->enum conversion all callers go through (Gem::Common::fromString<>).
template <class E>
E round_trip_int(E in) {
    std::ostringstream oss;
    oss << std::to_underlying(in); // numeric form, matches operator<< overloads
    std::istringstream iss(oss.str());
    E out{};
    iss >> out;
    return out;
}

TEST_CASE("ENUMBASETYPE: aliases std::uint16_t", "[common][enums]") {
    static_assert(std::is_same_v<ENUMBASETYPE, std::uint16_t>);
}

// ---------------------------------------------------------------------------
// sortOrder / dimensions / logType / serializationMode / expectation:
// numeric round-trip is the contract.

TEST_CASE("sortOrder: numeric round-trip preserves value", "[common][enums]") {
    CHECK(round_trip_int(sortOrder::LOWERISBETTER) == sortOrder::LOWERISBETTER);
    CHECK(round_trip_int(sortOrder::HIGHERISBETTER) == sortOrder::HIGHERISBETTER);
}

TEST_CASE("dimensions: numeric round-trip preserves value", "[common][enums]") {
    for(auto d : {dimensions::Dim1, dimensions::Dim2, dimensions::Dim3, dimensions::Dim4}) {
        CHECK(round_trip_int(d) == d);
    }
}

TEST_CASE("logType: numeric round-trip preserves value", "[common][enums]") {
    for(auto l :
        {logType::ERROR_REPORT,
         logType::TERMINATION,
         logType::WARNING,
         logType::LOGGING,
         logType::FILE,
         logType::STDOUT,
         logType::STDERR}) {
        CHECK(round_trip_int(l) == l);
    }
}

TEST_CASE("serializationMode: numeric round-trip preserves value", "[common][enums]") {
    for(auto m : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        CHECK(round_trip_int(m) == m);
    }
}

TEST_CASE("expectation: numeric round-trip preserves value", "[common][enums]") {
    for(auto e : {expectation::EQUALITY, expectation::FP_SIMILARITY, expectation::INEQUALITY}) {
        CHECK(round_trip_int(e) == e);
    }
}

// ---------------------------------------------------------------------------
// tribool prints names (not numeric form).

TEST_CASE("tribool::operator<<: prints the textual name", "[common][enums]") {
    std::ostringstream oss_true;
    std::ostringstream oss_false;
    std::ostringstream oss_indet;
    oss_true << tribool::True;
    oss_false << tribool::False;
    oss_indet << tribool::Indeterminate;
    CHECK(oss_true.str() == "True");
    CHECK(oss_false.str() == "False");
    CHECK(oss_indet.str() == "Indeterminate");
}

TEST_CASE("tribool::operator<<: out-of-range value is diagnosed (not silent)", "[common][enums]") {
    // Force an unknown enumerator (e.g. via a corrupted-stream load). The stream-out must not
    // produce an empty/silent output, and must not throw (it may be part of an error report).
    auto bad = static_cast<tribool>(99);
    std::ostringstream oss;
    CHECK_NOTHROW(oss << bad);
    CHECK(oss.str() == "<invalid value 99>");
}

TEST_CASE("indeterminate() helper", "[common][enums]") {
    CHECK_FALSE(indeterminate(tribool::True));
    CHECK_FALSE(indeterminate(tribool::False));
    CHECK(indeterminate(tribool::Indeterminate));
}

// ---------------------------------------------------------------------------
// The names of the enums (enum_names_v): every name is pinned, since files, scripts and the
// manual quote them, and each name leads back to its value.

TEST_CASE("enum names: common's enums", "[common][enums][names]") {
    checkEnumNames<serializationMode>(
        {{serializationMode::GEM_BINARY, "GEM_BINARY"}, {serializationMode::GEM_JSON, "GEM_JSON"}}
    );
    checkEnumNames<tribool>(
        {{tribool::False, "False"},
         {tribool::Indeterminate, "Indeterminate"},
         {tribool::True, "True"}}
    );
    checkEnumNames<logType>(
        {{logType::ERROR_REPORT, "ERROR_REPORT"},
         {logType::TERMINATION, "TERMINATION"},
         {logType::WARNING, "WARNING"},
         {logType::LOGGING, "LOGGING"},
         {logType::FILE, "FILE"},
         {logType::STDOUT, "STDOUT"},
         {logType::STDERR, "STDERR"},
         {logType::EXIT, "EXIT"}}
    );
    checkEnumNames<expectation>(
        {{expectation::EQUALITY, "EQUALITY"},
         {expectation::FP_SIMILARITY, "FP_SIMILARITY"},
         {expectation::INEQUALITY, "INEQUALITY"}}
    );
    using Record::columnType;
    checkEnumNames<columnType>(
        {{columnType::INTEGER, "integer"},
         {columnType::UNSIGNED, "unsigned"},
         {columnType::FLOATING, "floating"},
         {columnType::BOOLEAN, "boolean"},
         {columnType::TEXT, "text"},
         {columnType::TIMESTAMP, "timestamp"}}
    );
    CHECK(serializationModeChoices() == "GEM_BINARY (3) or GEM_JSON (4)");
}

TEST_CASE("enum names: a name is read at compile time", "[common][enums][names]") {
    static_assert(enumName(serializationMode::GEM_JSON) == "GEM_JSON");
    static_assert(enumFromName<tribool>("Indeterminate") == tribool::Indeterminate);
    SUCCEED();
}

TEST_CASE(
    "enum names: an unknown name is refused; an unknown value is named as such",
    "[common][enums][names]"
) {
    CHECK_FALSE(enumFromName<serializationMode>("GEM_XML").has_value());
    CHECK_FALSE(enumFromName<serializationMode>("gem_binary").has_value()); // spelled exactly
    CHECK_FALSE(enumFromName<serializationMode>("").has_value());
    // A number that is none of the values prints as itself, without an exception
    // Not noexcept: the text for a value without a name is built, which allocates
    static_assert(not noexcept(enumName(serializationMode::GEM_BINARY)));
    CHECK(enumName(static_cast<serializationMode>(99)) == "<invalid value 99>");
}

namespace {
/** @brief An enum whose name table misses a value */
enum class threeValues : std::uint8_t {
    A,
    B,
    C
};
/** @brief An enum whose name table uses a name twice */
enum class twoValues : std::uint8_t {
    A,
    B
};
} // namespace

namespace Gem::Common {
template <>
inline constexpr auto numeric_enum_values_v<threeValues> =
    numeric_enum_range<threeValues::A, threeValues::C>();
template <>
inline constexpr auto enum_names_v<threeValues> =
    std::to_array<std::pair<threeValues, std::string_view>>(
        {{threeValues::A, "A"}, {threeValues::B, "B"}}
    );
template <>
inline constexpr auto enum_names_v<twoValues> =
    std::to_array<std::pair<twoValues, std::string_view>>(
        {{twoValues::A, "X"}, {twoValues::B, "X"}}
    );
} /* namespace Gem::Common */

TEST_CASE(
    "enum names: an incomplete or ambiguous table is found at compile time",
    "[common][enums][names]"
) {
    // enumName() and enumFromName() assert this; a table like these two does not compile with them
    static_assert(not enumNamesAreComplete<threeValues>());
    static_assert(not enumNamesAreComplete<twoValues>());
    static_assert(enumNamesAreComplete<serializationMode>());
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Every enum that has operator<< / operator>> defined for stream interop should
// round-trip through a string both directions via those operators (the pattern
// the config parser / serialization use through Gem::Common::fromString<T>).

TEST_CASE("enum stream operators: round-trip through string<->enum", "[common][enums][streaming]") {
    auto check = [](auto in) {
        std::ostringstream os;
        os << in;
        std::istringstream is(os.str());
        std::decay_t<decltype(in)> out{};
        is >> out;
        CHECK(out == in);
    };

    check(sortOrder::HIGHERISBETTER);
    check(dimensions::Dim3);
    check(logType::WARNING);
    check(serializationMode::GEM_JSON);
    check(expectation::FP_SIMILARITY);
}
