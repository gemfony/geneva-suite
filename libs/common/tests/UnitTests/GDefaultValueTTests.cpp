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

#include "common/GDefaultValueT.hpp"

using namespace Gem::Common;

TEST_CASE("GDefaultValueT: numeric primary template returns T(0)", "[common][default-value]") {
    CHECK(GDefaultValueT<int>::value()           == 0);
    CHECK(GDefaultValueT<long>::value()          == 0L);
    CHECK(GDefaultValueT<std::int32_t>::value()  == std::int32_t{0});
    CHECK(GDefaultValueT<std::uint64_t>::value() == std::uint64_t{0});
    CHECK(GDefaultValueT<double>::value()        == 0.0);
    CHECK(GDefaultValueT<float>::value()         == 0.0f);
}

TEST_CASE("GDefaultValueT<bool> specialisation returns true", "[common][default-value]") {
    // Documented surprise: bool's default is `true`, NOT `false`. The single
    // call site that relied on this behaviour is GBooleanObject's default
    // construction. Pin it down so a future refactor cannot silently flip it.
    CHECK(GDefaultValueT<bool>::value() == true);
}

TEST_CASE("GDefaultValueT<std::string> specialisation returns empty", "[common][default-value]") {
    CHECK(GDefaultValueT<std::string>::value().empty());
    CHECK(GDefaultValueT<std::string>::value().empty());
}
