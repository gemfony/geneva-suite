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

// GEM_DEBUG_CHECK() in a translation unit compiled with NDEBUG, whatever the build type: the check
// is absent, its condition and its message are not evaluated. Everything common/GChecks.hpp needs
// is included first, so that only the definition of the macro sees the NDEBUG set here.

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <source_location>
#include <string>

#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GGlobalDefines.hpp"

#ifndef NDEBUG
#define NDEBUG
#endif
#include "common/GChecks.hpp"

TEST_CASE(
    "GEM_DEBUG_CHECK with NDEBUG: a failed condition throws nothing and is not evaluated",
    "[common][checks]"
) {
    int evaluated = 0;
    int streamed = 0;
    auto const message_part = [&] {
        ++streamed;
        return "never shown";
    };
    CHECK_NOTHROW(GEM_DEBUG_CHECK((++evaluated, false), message_part()));
    CHECK(evaluated == 0);
    CHECK(streamed == 0);
}
