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

#include <cstdint>
#include <memory>
#include <string>

#include "common/GCommonInterfaceT.hpp"
#include "common/GTypeTraitsT.hpp"

using namespace Gem::Common;

namespace {

// ---------------------------------------------------------------------------
// Helper test types. The concepts of GTypeTraitsT.hpp are compile-time predicates; the static_asserts
// below are the test: if a value regresses, the file no longer compiles.

struct HasNothing {};

struct HasCommonInterface : public gemfony_common_interface_indicator {};

struct DerivedFromCommonInterface : public HasCommonInterface {};

// The indicator is inherited privately by GCommonInterfaceT; the concept still sees it.
struct PrivateIndicator : private gemfony_common_interface_indicator {};

// A class of the collection's own common interface.
class CommonObj : public GCommonInterfaceT<CommonObj> {};

static_assert(not gemfony_common_interface<HasNothing>);
static_assert(gemfony_common_interface<HasCommonInterface>);
static_assert(gemfony_common_interface<DerivedFromCommonInterface>);
static_assert(gemfony_common_interface<PrivateIndicator>);
static_assert(gemfony_common_interface<CommonObj>);
static_assert(not gemfony_common_interface<int>);
static_assert(not gemfony_common_interface<std::string>);
static_assert(not gemfony_common_interface<std::shared_ptr<CommonObj>>);

static_assert(arithmetic<int>);
static_assert(arithmetic<std::uint8_t>);
static_assert(arithmetic<bool>);
static_assert(arithmetic<double>);
static_assert(arithmetic<float>);
static_assert(not arithmetic<std::string>);
static_assert(not arithmetic<int *>);
static_assert(not arithmetic<HasNothing>);

} // namespace

// A runtime TEST_CASE gives Catch a hook, so the executable reports a non-empty test session.
TEST_CASE("GTypeTraitsT runtime checks", "[common][trait]") {
    CHECK(gemfony_common_interface<CommonObj>);
    CHECK_FALSE(gemfony_common_interface<HasNothing>);
    CHECK(arithmetic<double>);
    CHECK_FALSE(arithmetic<std::string>);
}
