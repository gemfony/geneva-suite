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
#include <utility>

#include "dietrich/plotting/GPlotEnums.hpp"

using namespace Gem::Dietrich;

// The dimensions of a plot stream as their numeric value (moved here from common's enum tests
// together with the enum).

TEST_CASE("dimensions: numeric round-trip preserves value", "[dietrich][enums]") {
    for(auto d : {dimensions::Dim1, dimensions::Dim2, dimensions::Dim3, dimensions::Dim4}) {
        std::ostringstream oss;
        oss << std::to_underlying(d);
        std::istringstream iss(oss.str());
        dimensions out{};
        iss >> out;
        CHECK(out == d);
    }
}

TEST_CASE("dimensions: round-trip through its stream operators", "[dietrich][enums][streaming]") {
    std::ostringstream os;
    os << dimensions::Dim3;
    std::istringstream is(os.str());
    dimensions out{};
    is >> out;
    CHECK(out == dimensions::Dim3);
}
