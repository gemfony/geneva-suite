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

#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "common/GCommonEnums.hpp"

using namespace Gem::Common;

// Generic helper: stream the enum out, parse the integral form back in and
// expect bit-for-bit equality. The integral-only round-trip mirrors the
// stream-based string<->enum conversion all callers go through (Gem::Common::fromString<>).
template <class E>
E round_trip_int(E in) {
    std::ostringstream oss;
    oss << std::to_underlying(in);   // numeric form, matches operator<< overloads
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
    CHECK(round_trip_int(sortOrder::LOWERISBETTER)  == sortOrder::LOWERISBETTER);
    CHECK(round_trip_int(sortOrder::HIGHERISBETTER) == sortOrder::HIGHERISBETTER);
}

TEST_CASE("dimensions: numeric round-trip preserves value", "[common][enums]") {
    for(auto d : {dimensions::Dim1, dimensions::Dim2, dimensions::Dim3, dimensions::Dim4}) {
        CHECK(round_trip_int(d) == d);
    }
}

TEST_CASE("logType: numeric round-trip preserves value", "[common][enums]") {
    for(auto l : {logType::EXCEPTION, logType::TERMINATION, logType::WARNING,
                  logType::LOGGING,   logType::FILE,        logType::STDOUT,
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
    oss_true  << tribool::True;
    oss_false << tribool::False;
    oss_indet << tribool::Indeterminate;
    CHECK(oss_true.str()  == "True");
    CHECK(oss_false.str() == "False");
    CHECK(oss_indet.str() == "Indeterminate");
}

TEST_CASE("tribool::operator<<: out-of-range value is diagnosed (not silent)",
          "[common][enums]") {
    // Force an unknown enumerator (e.g. via a corrupted-stream load). The
    // stream-out must produce a recognisable diagnostic prefix rather than
    // an empty/silent output.
    auto bad = static_cast<tribool>(99);
    std::ostringstream oss;
    oss << bad;
    CHECK(oss.str().contains("tribool::?"));
    CHECK(oss.str().contains("99"));
}

TEST_CASE("indeterminate() helper", "[common][enums]") {
    CHECK_FALSE(indeterminate(tribool::True));
    CHECK_FALSE(indeterminate(tribool::False));
    CHECK(indeterminate(tribool::Indeterminate));
}

// ---------------------------------------------------------------------------
// serModeToString: dedicated debug helper, plus the "unknown" fallback.

TEST_CASE("serModeToString: maps each mode to its name", "[common][enums]") {
    CHECK(serModeToString(serializationMode::GEM_BINARY) == "GEM_BINARY");
    CHECK(serModeToString(serializationMode::GEM_JSON)   == "GEM_JSON");
}

TEST_CASE("serModeToString: unknown value falls through to 'unknown'", "[common][enums]") {
    CHECK(serModeToString(static_cast<serializationMode>(99)) == "unknown");
}

// ---------------------------------------------------------------------------
// Every enum that has operator<< / operator>> defined for stream interop should
// round-trip through a string both directions via those operators (the pattern
// the config parser / serialization use through Gem::Common::fromString<T>).

TEST_CASE("enum stream operators: round-trip through string<->enum",
          "[common][enums][streaming]") {
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
