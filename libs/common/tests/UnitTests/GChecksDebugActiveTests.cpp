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

// GEM_DEBUG_CHECK() in a translation unit compiled without NDEBUG, whatever the build type: the
// check is active. Everything common/GChecks.hpp needs is included first, so that only the
// definition of the macro sees the NDEBUG set here.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <source_location>
#include <string>

#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GGlobalDefines.hpp"

#undef NDEBUG
#include "common/GChecks.hpp"

TEST_CASE(
    "GEM_DEBUG_CHECK without NDEBUG: a condition that holds throws nothing",
    "[common][checks]"
) {
    int evaluated = 0;
    CHECK_NOTHROW(GEM_DEBUG_CHECK((++evaluated, true), "never shown"));
    CHECK(evaluated == 1);
}

TEST_CASE(
    "GEM_DEBUG_CHECK without NDEBUG: a failed condition throws with the place of the check",
    "[common][checks]"
) {
    std::uint_least32_t line = 0;
    std::string message;
    try {
        line = std::source_location::current().line() + 1; // the line of the check below
        GEM_DEBUG_CHECK(2 < 1, "two is not smaller than ", 1);
    }
    catch(Gem::Common::gem_exception const &e) {
        message = e.what();
    }
    CHECK(message.contains("GChecksDebugActiveTests.cpp"));
    CHECK(message.contains("at line " + std::to_string(line) + " "));
    CHECK(message.contains("two is not smaller than 1\n"));
}
