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

/**
 * Tests of the separable CMA evolution strategy and its personality traits through their public
 * interface: setting and reading the strategy parameters, and the checks of invalid ones.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "geneva/oa/GSepCmaEvolutionStrategy.hpp"
#include "geneva/oa/GSepCmaEvolutionStrategy_PersonalityTraits.hpp"

namespace oa = Gem::Geneva::OptimizationAlgorithms;

/******************************************************************************/

TEST_CASE("GSepCmaEvolutionStrategy: strategy parameters can be set and read", "[geneva][sepcma]") {
    oa::GSepCmaEvolutionStrategy p_test;

    CHECK_NOTHROW(p_test.setLambda(20));
    CHECK(p_test.getLambda() == 20);

    CHECK_NOTHROW(p_test.setMu(7));
    CHECK(p_test.getMu() == 7);

    CHECK_NOTHROW(p_test.setUseDiagonalCMA(false));
    CHECK(p_test.getUseDiagonalCMA() == false);

    CHECK_NOTHROW(p_test.setParetoMode(true));
    CHECK(p_test.getParetoMode() == true);

    CHECK_NOTHROW(p_test.setInitialSigma(0.5));
    CHECK(std::abs(p_test.getInitialSigma() - 0.5) < 1.e-9);
}

TEST_CASE("GSepCmaEvolutionStrategy: an invalid initial sigma is refused", "[geneva][sepcma]") {
    oa::GSepCmaEvolutionStrategy p_test;
    CHECK_THROWS(p_test.setInitialSigma(-1.));
}

/******************************************************************************/

TEST_CASE("GSepCmaEvolutionStrategy_PersonalityTraits: rank and pareto tag", "[geneva][sepcma]") {
    oa::GSepCmaEvolutionStrategy_PersonalityTraits p_test;

    CHECK_NOTHROW(p_test.setRank(42));
    CHECK(p_test.getRank() == 42);

    p_test.resetParetoTag();
    CHECK(p_test.isOnParetoFront());
    p_test.setIsNotOnParetoFront();
    CHECK(not p_test.isOnParetoFront());
}

/******************************************************************************/
