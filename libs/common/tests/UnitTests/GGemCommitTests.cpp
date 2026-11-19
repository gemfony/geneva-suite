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

// The commit the library was built from (common/GGemCommit.hpp) has the form of a git commit id,
// and the identity of the user's program is set once.
// Where the build takes it from is tested on made-up source trees by GemCommitSources
// (CMakeModules/CheckGemCommit.cmake).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <regex>
#include <string>

#include "common/GGemCommit.hpp"

TEST_CASE("gemCommit() is a commit id that may be marked dirty", "[common][commit]") {
    const std::string id{Gem::Common::gemCommit()};
    INFO("gemCommit(): " << id);
    CHECK(std::regex_match(id, std::regex("[0-9a-f]{40}(-dirty)?")));
}

// The identity of the user's program is set once per process: the same text again is accepted,
// another one is refused, and the identity stays the first one.
TEST_CASE("setProgramIdentity() takes one identity per process", "[common][commit]") {
    CHECK(Gem::Common::programIdentity().empty());
    REQUIRE_NOTHROW(Gem::Common::setProgramIdentity("program-one"));
    CHECK(Gem::Common::programIdentity() == "program-one");
    CHECK_NOTHROW(Gem::Common::setProgramIdentity("program-one"));
    CHECK_THROWS_WITH(
        Gem::Common::setProgramIdentity("program-two"),
        Catch::Matchers::ContainsSubstring("program-one") &&
            Catch::Matchers::ContainsSubstring("program-two")
    );
    CHECK(Gem::Common::programIdentity() == "program-one");
}
