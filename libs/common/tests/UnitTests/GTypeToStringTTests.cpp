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
#include <string>

#include "common/GTypeToStringT.hpp"

using namespace Gem::Common;

TEST_CASE("GTypeToStringT: specialisations return the expected names", "[common][type-to-string]") {
    CHECK(GTypeToStringT<double>::value()       == "double");
    CHECK(GTypeToStringT<float>::value()        == "float");
    CHECK(GTypeToStringT<std::int32_t>::value() == "int32_t");
    CHECK(GTypeToStringT<bool>::value()         == "bool");
    CHECK(GTypeToStringT<std::string>::value()  == "string");
}

TEST_CASE("GTypeToStringT: primary template returns 'unknown' for unknown types", "[common][type-to-string]") {
    struct NotSpecialised {};
    CHECK(GTypeToStringT<NotSpecialised>::value() == "unknown");
    // Integer types other than int32_t fall through to the primary template.
    CHECK(GTypeToStringT<long>::value()           == "unknown");
    CHECK(GTypeToStringT<std::uint8_t>::value()   == "unknown");
    // Pointer / smart-pointer types also fall through.
    CHECK(GTypeToStringT<int *>::value()          == "unknown");
}
