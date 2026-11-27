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

#include <exception>
#include <sstream>
#include <stdexcept>
#include <string>

#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"

TEST_CASE("gem_exception: basic construction and what()", "[common][exceptions]") {
    Gem::Common::gem_exception const e("custom message");
    CHECK(std::string{e.what()} == "custom message");

    // Inheritance: gem_exception is a std::runtime_error.
    static_assert(std::is_base_of_v<std::runtime_error, Gem::Common::gem_exception>);
    static_assert(std::is_base_of_v<std::exception, Gem::Common::gem_exception>);
}

TEST_CASE("gem_exception: stream-out operator prints what()", "[common][exceptions]") {
    Gem::Common::gem_exception const e("payload-text");
    std::ostringstream oss;
    oss << e;
    CHECK(oss.str() == "payload-text");
}

TEST_CASE("g_expectation_violation: derives from gem_exception", "[common][exceptions]") {
    static_assert(
        std::is_base_of_v<Gem::Common::gem_exception, Gem::Common::g_expectation_violation>
    );

    // Can be caught as gem_exception, as runtime_error, and as exception.
    try {
        throw Gem::Common::g_expectation_violation("mismatch");
    }
    catch(const Gem::Common::gem_exception &g) {
        CHECK(std::string{g.what()} == "mismatch");
    }

    try {
        throw Gem::Common::g_expectation_violation("mismatch-2");
    }
    catch(const std::runtime_error &r) {
        CHECK(std::string{r.what()} == "mismatch-2");
    }

    try {
        throw Gem::Common::g_expectation_violation("mismatch-3");
    }
    catch(const std::exception &e) {
        CHECK(std::string{e.what()} == "mismatch-3");
    }
}

TEST_CASE(
    "throw gem_exception(g_error_streamer() << ...): the message names the place of the throw",
    "[common][exceptions][raise]"
) {
    bool caught = false;
    std::string what;
    try {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "a=" << 42 << " b=" << 3.14 << " c=" << std::string{"foo"}
        );
    }
    catch(const Gem::Common::gem_exception &g) {
        caught = true;
        what = g.what();
    }
    REQUIRE(caught);
    CHECK(what.contains("a=42"));
    CHECK(what.contains("b=3.14"));
    CHECK(what.contains("c=foo"));
    CHECK(what.contains("GExceptionsTests.cpp at line "));
}

TEST_CASE(
    "describeException(): the what() of a std::exception or that it is none",
    "[common][exceptions][describe]"
) {
    using Gem::Common::describeException;
    CHECK(describeException(std::make_exception_ptr(std::runtime_error("no file"))) == "no file");
    CHECK(
        describeException(std::make_exception_ptr(Gem::Common::gem_exception("bad entry"))) ==
        "bad entry"
    );
    CHECK(
        describeException(std::make_exception_ptr(42)) ==
        "an exception that is not a std::exception"
    );
    CHECK(describeException(nullptr) == "no exception");

    // The exception in flight, as a catch(...) clause hands it over
    try {
        throw std::logic_error("in flight");
    }
    catch(...) {
        CHECK(describeException(std::current_exception()) == "in flight");
    }
}
