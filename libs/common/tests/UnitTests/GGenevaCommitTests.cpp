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

// The commit the library was built from (common/GGenevaCommit.hpp) has the form of a git commit id.
// Where the build takes it from is tested on made-up source trees by GenevaCommitSources
// (CMakeModules/CheckGenevaCommit.cmake).

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <string>

#include "common/GGenevaCommit.hpp"

TEST_CASE("genevaCommit() is a commit id that may be marked dirty", "[common][commit]") {
    const std::string id{Gem::Common::genevaCommit()};
    INFO("genevaCommit(): " << id);
    CHECK(std::regex_match(id, std::regex("[0-9a-f]{40}(-dirty)?")));
}
