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

#include <memory>
#include <string>

#include "common/GTypeTraitsT.hpp"

using namespace Gem::Common;

namespace {

// ---------------------------------------------------------------------------
// Helper test types
//
// All three traits in GTypeTraitsT.hpp are pure compile-time predicates, so
// these test types exist mainly to anchor the static_asserts below. The
// `static_assert`s also serve as the actual "test" — if the trait values
// regress, the file fails to compile, which fails the build step.

struct HasNothing {};

struct HasCommonInterface : public gemfony_common_interface_indicator {};

struct DerivedFromCommonInterface : public HasCommonInterface {};

struct HasCompare {
    void compare([[maybe_unused]] int value) const {}
};

struct HasClone {
    void clone() const {}
};

struct HasLoad {
    void load([[maybe_unused]] int value) {}
};

struct HasAll : public gemfony_common_interface_indicator {
    void compare([[maybe_unused]] int value) const {}
    void clone() const {}
    void load([[maybe_unused]] int value) {}
};

// Different return / argument signatures should still satisfy the predicate
// — the requires-clause only takes the address of the member.
struct HasUnusualCompare {
    static std::string compare([[maybe_unused]] double first, [[maybe_unused]] double second) noexcept {
        return {};
    }
};

// ---------------------------------------------------------------------------
// Compile-time checks: if any of these flips, the file no longer compiles.

static_assert(not has_gemfony_common_interface<HasNothing>::value);
static_assert(    has_gemfony_common_interface<HasCommonInterface>::value);
static_assert(    has_gemfony_common_interface<DerivedFromCommonInterface>::value);
static_assert(not has_gemfony_common_interface<int>::value);
static_assert(not has_gemfony_common_interface<std::string>::value);
static_assert(    has_gemfony_common_interface<HasAll>::value);

static_assert(not has_compare_member<HasNothing>::value);
static_assert(    has_compare_member<HasCompare>::value);
static_assert(    has_compare_member<HasAll>::value);
static_assert(    has_compare_member<HasUnusualCompare>::value);

static_assert(not has_clone_member<HasNothing>::value);
static_assert(    has_clone_member<HasClone>::value);
static_assert(    has_clone_member<HasAll>::value);
// std::shared_ptr has no public clone() method.
static_assert(not has_clone_member<std::shared_ptr<int>>::value);

static_assert(not has_load_member<HasNothing>::value);
static_assert(    has_load_member<HasLoad>::value);
static_assert(    has_load_member<HasAll>::value);

} // namespace

// A single runtime TEST_CASE confirms the trait values can also be observed
// at runtime (covers gcov hits for any non-inlinable instantiation path) and
// gives Catch a hook so the executable reports a non-empty test session.
TEST_CASE("GTypeTraitsT runtime checks", "[common][trait]") {
    CHECK(has_gemfony_common_interface<HasCommonInterface>::value);
    CHECK_FALSE(has_gemfony_common_interface<HasNothing>::value);

    CHECK(has_compare_member<HasCompare>::value);
    CHECK_FALSE(has_compare_member<HasNothing>::value);

    CHECK(has_clone_member<HasClone>::value);
    CHECK_FALSE(has_clone_member<HasNothing>::value);

    CHECK(has_load_member<HasLoad>::value);
    CHECK_FALSE(has_load_member<HasNothing>::value);

    // Composite — the canonical Geneva interface type satisfies all four.
    CHECK(has_gemfony_common_interface<HasAll>::value);
    CHECK(has_compare_member<HasAll>::value);
    CHECK(has_clone_member<HasAll>::value);
    CHECK(has_load_member<HasAll>::value);
}
