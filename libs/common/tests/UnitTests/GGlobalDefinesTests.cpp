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

#include "common/GGlobalDefines.hpp"

// common is the base of every library of the collection, so it must not take global names
// from a user. The short names oa, gen, gind and gbm belong to Geneva (its namespace aliases,
// geneva/GNamespaceAliases.hpp); a program built on common alone may use them for its own
// globals. A global variable and a namespace alias of the same name cannot coexist, so this
// file does not compile while common/GGlobalDefines.hpp declares any of the four aliases.
// The lower-case names are the point of the test, hence the suppressed naming check.
// NOLINTBEGIN(readability-identifier-naming)
[[maybe_unused]] static constexpr int oa = 1;
[[maybe_unused]] static constexpr int gen = 2;
[[maybe_unused]] static constexpr int gind = 3;
[[maybe_unused]] static constexpr int gbm = 4;
// NOLINTEND(readability-identifier-naming)

TEST_CASE("common leaves the names of Geneva's namespace aliases to the user", "[common][defines]") {
    CHECK(oa + gen + gind + gbm == 10);
}
