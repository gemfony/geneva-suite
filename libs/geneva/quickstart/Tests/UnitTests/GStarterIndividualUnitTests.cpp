/**
 * @file GStarterIndividualUnitTests.cpp
 */

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

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <vector>

// The class to be tested
#include "GStarterIndividual.hpp"

// The standard tests of the installed collection
#include "common/testing/GStandardTests.hpp"

using namespace Gem::Geneva;

/************************************************************************************************/

TEMPLATE_TEST_CASE(
    "checkRoundTrips -- GStarterIndividual",
    "[examples][starter][standard]",
    GStarterIndividual
) {
    STATIC_REQUIRE(Gem::Common::Testing::ModifiableForTest<TestType>);
    Gem::Common::Testing::checkRoundTrips<TestType>();
}

TEMPLATE_TEST_CASE(
    "checkMisuse -- GStarterIndividual",
    "[examples][starter][standard]",
    GStarterIndividual
) {
    Gem::Common::Testing::checkMisuse<TestType>();
}

/************************************************************************************************/

TEST_CASE(
    "GStarterIndividual: the genome has one parameter per start value",
    "[examples][starter]"
) {
    constexpr std::size_t n_entries = 100;
    std::vector<double> const start_values(n_entries, 1.);
    std::vector<double> const lower_boundaries(n_entries, 0.);
    std::vector<double> const upper_boundaries(n_entries, 2.);

    std::shared_ptr<GStarterIndividual> p_test;
    CHECK_NOTHROW(
        p_test = std::make_shared<GStarterIndividual>(
            0, // the first individual
            start_values,
            lower_boundaries,
            upper_boundaries
        )
    );
    REQUIRE(p_test);
    CHECK(p_test->countParameters<double>() == n_entries);
}

TEST_CASE("GStarterIndividual: the target function can be set and read", "[examples][starter]") {
    GStarterIndividual p_test;

    CHECK_NOTHROW(p_test.setTargetFunction(targetFunction::PARABOLA));
    CHECK(targetFunction::PARABOLA == p_test.getTargetFunction());

    CHECK_NOTHROW(p_test.setTargetFunction(targetFunction::NOISYPARABOLA));
    CHECK(targetFunction::NOISYPARABOLA == p_test.getTargetFunction());
}

/************************************************************************************************/
