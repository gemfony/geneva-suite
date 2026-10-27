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
 * Convergence checks for the from-scratch Standard PSO 2011 (SPSO-2011) optimization algorithm on a
 * flat-genome sphere individual. SPSO-2011 is an FP-only swarm algorithm that needs no adaption config
 * (it samples positions from a hypersphere around a center of gravity rather than mutating via adaptors).
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <vector>

#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GStandardPSO2011.hpp"

namespace gen = Gem::Geneva::Genome;
namespace oa = Gem::Geneva::OptimizationAlgorithms;

namespace {

/******************************************************************************/
/**
 * A flat-genome sphere individual: a configurable number of constrained doubles in [-5, 5), started at
 * 3.0 (so the initial fitness is n_dim * 9). Authored entirely through the builder; SPSO needs NO
 * adaption config. fitness = sum of squares (optimum: the origin).
 */
template <std::size_t N_DIM>
class SpherePSO : public gen::GGenomeT<SpherePSO<N_DIM>> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    SpherePSO() {
        gen::GGenomeBuilder b;
        b.addDoubleGroup(N_DIM, -5., 5.).init(3.0); // structure only; SPSO needs no adaptor
        this->setGenome(b.build());
    }
    SpherePSO(const SpherePSO &) = default;

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->template streamline<double>(v);
        double s = 0.;
        for(double const x : v) {
            s += x * x;
        }
        return {s};
    }
};

/** @brief Sphere value of the best individual, also asserting the constraints held. */
template <std::size_t N_DIM>
double bestSphere(const SpherePSO<N_DIM> &best) {
    std::vector<double> v;
    best.template streamline<double>(v);
    double s = 0.;
    for(double const x : v) {
        s += x * x;
        CHECK(x >= -5.0);
        CHECK(x < 5.0);
    }
    return s;
}

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("Standard PSO 2011 optimizes a 5-dim flat sphere", "[spso2011]") {
    auto pop = std::make_shared<oa::GStandardPSO2011>();
    pop->setSwarmSize(40);
    pop->setMaxIteration(300);
    pop->setMaxStallIteration(0); // 0 == disabled: run the full budget so the swarm fully converges
    pop->setReportIteration(100000);
    pop->push_back(SpherePSO<5>().clone());
    pop->optimize();

    auto best = pop->getBestGlobalIndividual<SpherePSO<5>>();
    REQUIRE(best);
    CHECK(bestSphere<5>(*best) < 1.e-2); // far below the f = 5 * 9 = 45 start
}

/******************************************************************************/

TEST_CASE("Standard PSO 2011 optimizes a 10-dim flat sphere", "[spso2011]") {
    auto pop = std::make_shared<oa::GStandardPSO2011>();
    pop->setSwarmSize(40);
    pop->setMaxIteration(600);
    pop->setMaxStallIteration(0); // 0 == disabled: run the full budget so the swarm fully converges
    pop->setReportIteration(100000);
    pop->push_back(SpherePSO<10>().clone());
    pop->optimize();

    auto best = pop->getBestGlobalIndividual<SpherePSO<10>>();
    REQUIRE(best);
    CHECK(bestSphere<10>(*best) < 1.0); // well below the f = 10 * 9 = 90 start
}

/******************************************************************************/

TEST_CASE("Standard PSO 2011 parameter validation", "[spso2011]") {
    auto pso = std::make_shared<oa::GStandardPSO2011>();

    pso->setSwarmSize(25);
    CHECK(pso->getSwarmSize() == 25);
    pso->setNInformants(5);
    CHECK(pso->getNInformants() == 5);

    CHECK_THROWS(pso->setSwarmSize(1));  // need at least 2 particles
    CHECK_THROWS(pso->setNInformants(0)); // need at least 1 informant
}

/******************************************************************************/
