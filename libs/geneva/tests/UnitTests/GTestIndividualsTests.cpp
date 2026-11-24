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
 * Tests of the test individuals' own functions, through their public interface.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>

#include "geneva/individuals/GTestIndividual3.hpp"

namespace gind = Gem::Geneva::Individuals;

/******************************************************************************/

TEST_CASE(
    "GTestIndividual3: repeated extraction of the plain data gives the same output",
    "[geneva][individuals]"
) {
    constexpr std::size_t ntests = 100;

    auto const p = std::make_shared<gind::GTestIndividual3>();
    std::size_t const n_items = p->countParameters<double>();
    REQUIRE(n_items > 0);

    std::shared_ptr<float[]> const result_old = p->getPlainData();
    for(std::size_t i = 0; i < ntests; i++) {
        std::shared_ptr<float[]> const result_new = p->getPlainData();
        for(std::size_t m = 0; m < n_items; m++) {
            CHECK(result_old[m] == result_new[m]);
        }
    }
}

/******************************************************************************/
