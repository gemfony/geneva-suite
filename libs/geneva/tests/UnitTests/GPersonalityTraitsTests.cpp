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
 * Tests of the algorithms' personality traits through their public interface: the parent/child state
 * and population position of the parent/child traits, and the swarm traits' flags.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

#include "common/GExceptions.hpp"
#include "geneva/oa/GBaseParChildPersonalityTraits.hpp"
#include "geneva/oa/GSwarmAlgorithm_PersonalityTraits.hpp"

namespace oa = Gem::Geneva::OptimizationAlgorithms;

/******************************************************************************/

TEST_CASE(
    "GBaseParChildPersonalityTraits: an object can be marked as parent or child",
    "[geneva][traits]"
) {
    oa::GBaseParChildPersonalityTraits p_test;

    CHECK_NOTHROW(p_test.setIsParent());
    CHECK(p_test.isParent() == true);

    CHECK_NOTHROW(p_test.setIsChild());
    CHECK(p_test.isParent() == false);
}

TEST_CASE(
    "GBaseParChildPersonalityTraits: the parent counter is incremented and reset",
    "[geneva][traits]"
) {
    oa::GBaseParChildPersonalityTraits p_test;

    CHECK_NOTHROW(p_test.setIsChild());
    CHECK(p_test.isParent() == false);
    CHECK(p_test.getParentCounter() == 0);

    for(std::uint32_t i = 1; i <= 10; i++) {
        CHECK_NOTHROW(p_test.setIsParent());
        CHECK(p_test.getParentCounter() == i);
    }

    CHECK_NOTHROW(p_test.setIsChild());
    CHECK(p_test.isParent() == false);
    CHECK(p_test.getParentCounter() == 0);
}

TEST_CASE(
    "GBaseParChildPersonalityTraits: the population position can be set and read",
    "[geneva][traits]"
) {
    oa::GBaseParChildPersonalityTraits p_test;

    for(std::size_t i = 0; i < 10; i++) {
        CHECK_NOTHROW(p_test.setPopulationPosition(i));
        CHECK(p_test.getPopulationPosition() == i);
    }
}

TEST_CASE(
    "GBaseParChildPersonalityTraits: parent ids can be set read and unset",
    "[geneva][traits]"
) {
    oa::GBaseParChildPersonalityTraits p_test;

    for(std::size_t i = 0; i < 10; i++) {
        CHECK_NOTHROW(p_test.setParentId(i));
        CHECK(p_test.getParentId() == i);
        CHECK(p_test.parentIdSet() == true);
        CHECK_NOTHROW(p_test.unsetParentId());
        CHECK(p_test.parentIdSet() == false);
    }
}

TEST_CASE("GBaseParChildPersonalityTraits: reading an unset parent id throws", "[geneva][traits]") {
    oa::GBaseParChildPersonalityTraits p_test;

    CHECK_NOTHROW(p_test.unsetParentId());
    CHECK_THROWS_AS(p_test.getParentId(), Gem::Common::gem_exception);
}

/******************************************************************************/

TEST_CASE(
    "GSwarmAlgorithm_PersonalityTraits: the no-position-update flag is set and reset",
    "[geneva][traits]"
) {
    oa::GSwarmAlgorithm_PersonalityTraits p_test;

    CHECK_NOTHROW(p_test.setNoPositionUpdate());
    CHECK(p_test.noPositionUpdate() == true);

    // Reading resets the flag
    CHECK(p_test.checkNoPositionUpdateAndReset() == true);
    CHECK(p_test.noPositionUpdate() == false);

    // Read again: "false" stays
    CHECK(p_test.checkNoPositionUpdateAndReset() == false);
    CHECK(p_test.noPositionUpdate() == false);
}

TEST_CASE(
    "GSwarmAlgorithm_PersonalityTraits: the neighborhood can be set and read",
    "[geneva][traits]"
) {
    oa::GSwarmAlgorithm_PersonalityTraits p_test;

    for(std::size_t i = 0; i < 10; i++) {
        CHECK_NOTHROW(p_test.setNeighborhood(i));
        CHECK(p_test.getNeighborhood() == i);
    }
}

/******************************************************************************/
