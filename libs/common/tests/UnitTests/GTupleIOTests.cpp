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

#include <sstream>
#include <string>
#include <tuple>

#include "common/GTupleIO.hpp"

using namespace Gem::Common;

TEST_CASE("GTupleIO::g_to_string: single-element tuple is rendered as \"(v)\"",
          "[common][tuple-io]") {
    CHECK(g_to_string(std::make_tuple(42))             == "(42)");
    CHECK(g_to_string(std::make_tuple(std::string{"x"})) == "(x)");
}

TEST_CASE("GTupleIO::g_to_string: multi-element tuple is rendered as \"(v0, v1, ...)\"",
          "[common][tuple-io]") {
    CHECK(g_to_string(std::make_tuple(1, 2))           == "(1, 2)");
    CHECK(g_to_string(std::make_tuple(1, 2, 3))        == "(1, 2, 3)");
    CHECK(g_to_string(std::make_tuple(1, 2.5, "k"))    == "(1, 2.5, k)");
}

TEST_CASE("GTupleIO::g_to_string: empty tuple renders as \"()\"",
          "[common][tuple-io]") {
    CHECK(g_to_string(std::make_tuple()) == "()");
}

TEST_CASE("GTupleIO::g_to_string: non-tuple overload forwards via operator<<",
          "[common][tuple-io]") {
    CHECK(g_to_string(7)                  == "7");
    CHECK(g_to_string(std::string{"abc"}) == "abc");
}

TEST_CASE("GTupleIO::operator<<(ostream&, tuple): streams the same string as g_to_string",
          "[common][tuple-io]") {
    std::ostringstream oss;
    auto t = std::make_tuple(10, 20, 30);
    oss << t;
    CHECK(oss.str() == "(10, 20, 30)");
}

TEST_CASE("GTupleIO::operator<< plays well with surrounding text",
          "[common][tuple-io]") {
    std::ostringstream oss;
    oss << "prefix=" << std::make_tuple(1, 2) << " suffix";
    CHECK(oss.str() == "prefix=(1, 2) suffix");
}
