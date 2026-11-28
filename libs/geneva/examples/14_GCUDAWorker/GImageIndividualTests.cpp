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

#include <cmath>
#include <set>
#include <vector>

// The class to be tested
#include "GImageIndividual.hpp"

using namespace Gem::Geneva;

/******************************************************************************/
/**
 * @brief The radii of an individual made from a Config the way the factory makes it: the genome
 * structure first, then the per-object hook.
 * @param c The configuration of the individual
 * @return The radius of every triangle
 */
static std::vector<double> radiiOf(GImageIndividual::Config const &c) {
    GImageIndividual ind;
    ind.setGenome(GImageIndividual::buildGenome(c));
    GImageIndividual::applyConfig(ind, c);
    std::vector<double> radii;
    for(auto const &t : ind.getTriangleData()) {
        radii.push_back(static_cast<double>(t.radius));
    }
    return radii;
}

/******************************************************************************/

TEST_CASE("GImageIndividual: start_size sets every radius", "[examples][monalisa]") {
    for(bool const mutate_alpha : {false, true}) {
        GImageIndividual::Config c;
        c.n_triangles = 20;
        c.start_size = 0.1;
        c.mutate_alpha_channel = mutate_alpha;
        for(double const r : radiiOf(c)) {
            CHECK(std::abs(r - 0.1) < 1e-6);
        }
    }
}

TEST_CASE("GImageIndividual: a negative start_size draws the radii", "[examples][monalisa]") {
    CHECK(GImageIndividual::Config{}.start_size == -1.);

    GImageIndividual::Config c;
    c.n_triangles = 20;
    c.start_size = -1.;
    auto const radii = radiiOf(c);
    for(double const r : radii) {
        CHECK(r >= c.min_size);
        CHECK(r < c.max_size);
    }
    CHECK(std::set<double>(radii.begin(), radii.end()).size() > 1);
}
