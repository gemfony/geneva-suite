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

// Tests of checkThat() (common/GChecks.hpp): the always-on check. The debug-only form
// GEM_DEBUG_CHECK() is tested in GChecksDebugActiveTests.cpp and GChecksDebugAbsentTests.cpp,
// which fix the mode of NDEBUG for themselves; this file tests it in the mode of the build.

#include <catch2/catch_test_macros.hpp>

#include <ostream>
#include <source_location>
#include <string>
#include <type_traits>

#include "common/GChecks.hpp"

namespace {

/** Counts how often it is streamed, so a test can see whether a message was built */
struct GStreamCounter {
    int *count;
};

std::ostream &operator<<(std::ostream &os, GStreamCounter const &counter) {
    ++(*counter.count);
    return os << "counted";
}

/** Returns the message of the gem_exception that f throws, or an empty string */
template <typename function_type>
std::string messageOf(function_type f) {
    try {
        f();
    }
    catch(gem_exception const &e) {
        return e.what();
    }
    return {};
}

} // namespace

TEST_CASE(
    "checkThat: a condition that holds throws nothing and builds no message",
    "[common][checks]"
) {
    int count = 0;
    CHECK_NOTHROW(Gem::Common::checkThat(true, "never shown ", GStreamCounter{&count}));
    CHECK(count == 0);
}

TEST_CASE("checkThat: a failed condition throws gem_exception", "[common][checks]") {
    CHECK_THROWS_AS(Gem::Common::checkThat(1 > 2, "one is not larger than two"), gem_exception);
}

TEST_CASE(
    "checkThat: the message names the place of the check and holds the parts in order",
    "[common][checks]"
) {
    int count = 0;
    std::uint_least32_t line = 0;
    std::string const message = messageOf([&] {
        line = std::source_location::current().line() + 1; // the line of the check below
        Gem::Common::checkThat(false, "value ", 42, " is ", GStreamCounter{&count});
    });

    CHECK(count == 1);
    CHECK(message.starts_with("Recorded on "));
    CHECK(message.contains("GChecksTests.cpp"));
    CHECK(message.contains("at line " + std::to_string(line) + " "));
    CHECK(message.contains("value 42 is counted\n"));
    CHECK(message.ends_with('\n'));
}

TEST_CASE("checkThat: only a bool is accepted as the condition", "[common][checks]") {
    STATIC_CHECK(std::is_convertible_v<bool, Gem::Common::GCheckCondition>);
    STATIC_CHECK_FALSE(std::is_convertible_v<int, Gem::Common::GCheckCondition>);
    STATIC_CHECK_FALSE(std::is_convertible_v<int *, Gem::Common::GCheckCondition>);
    STATIC_CHECK_FALSE(std::is_convertible_v<char const *, Gem::Common::GCheckCondition>);
}

TEST_CASE("GEM_DEBUG_CHECK: follows the NDEBUG of the build", "[common][checks]") {
    int evaluated = 0;
    auto const failing = [&] {
        ++evaluated;
        return false;
    };
#ifdef NDEBUG
    CHECK_NOTHROW(GEM_DEBUG_CHECK(failing(), "absent with NDEBUG"));
    CHECK(evaluated == 0);
#else
    CHECK_THROWS_AS(GEM_DEBUG_CHECK(failing(), "active without NDEBUG"), gem_exception);
    CHECK(evaluated == 1);
#endif
}
