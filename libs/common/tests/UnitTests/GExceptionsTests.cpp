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
#include <stdexcept>
#include <string>

#include "common/GExceptions.hpp"

TEST_CASE("geneva_exception: basic construction and what()", "[common][exceptions]") {
    geneva_exception const e("custom message");
    CHECK(std::string{e.what()} == "custom message");

    // Inheritance: geneva_exception is a std::runtime_error.
    static_assert(std::is_base_of_v<std::runtime_error, geneva_exception>);
    static_assert(std::is_base_of_v<std::exception, geneva_exception>);
}

TEST_CASE("geneva_exception: stream-out operator prints what()", "[common][exceptions]") {
    geneva_exception const e("payload-text");
    std::ostringstream oss;
    oss << e;
    CHECK(oss.str() == "payload-text");
}

TEST_CASE("g_expectation_violation: derives from geneva_exception", "[common][exceptions]") {
    static_assert(std::is_base_of_v<geneva_exception, g_expectation_violation>);

    // Can be caught as geneva_exception, as runtime_error, and as exception.
    try {
        throw g_expectation_violation("mismatch");
    } catch(const geneva_exception &g) {
        CHECK(std::string{g.what()} == "mismatch");
    }

    try {
        throw g_expectation_violation("mismatch-2");
    } catch(const std::runtime_error &r) {
        CHECK(std::string{r.what()} == "mismatch-2");
    }

    try {
        throw g_expectation_violation("mismatch-3");
    } catch(const std::exception &e) {
        CHECK(std::string{e.what()} == "mismatch-3");
    }
}

TEST_CASE("raiseException macro: throws geneva_exception with the streamed message",
          "[common][exceptions][raise]") {
    bool caught = false;
    std::string what;
    try {
        raiseException("detailed-error-payload");
    } catch(const geneva_exception &g) {
        caught = true;
        what   = g.what();
    }
    REQUIRE(caught);

    // The macro wraps the streamed payload in a fixed envelope. Verify the
    // payload appears, and the envelope identifiers ("ERROR", "file …", and
    // "line …") are present so future refactors keep the framing intact.
    CHECK(what.contains("detailed-error-payload"));
    CHECK(what.contains("ERROR"));
    CHECK(what.contains("in file "));
    CHECK(what.contains("near line "));
}

TEST_CASE("raiseException macro: payload may chain streamable values",
          "[common][exceptions][raise]") {
    bool caught = false;
    std::string what;
    try {
        raiseException("a=" << 42 << " b=" << 3.14 << " c=" << std::string{"foo"});
    } catch(const geneva_exception &g) {
        caught = true;
        what   = g.what();
    }
    REQUIRE(caught);
    CHECK(what.contains("a=42"));
    CHECK(what.contains("b=3.14"));
    CHECK(what.contains("c=foo"));
}
