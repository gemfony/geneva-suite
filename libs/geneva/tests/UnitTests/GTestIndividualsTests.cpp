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
#include <vector>

#include "GTestIndividual1.hpp"
#include "GTestIndividual3.hpp"
#include "common/GExceptions.hpp"
#include "geneva/genome/GAuxiliaryStore.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GPositionPersonalityTraits.hpp"
#include "geneva/oa/GSwarmAlgorithm_PersonalityTraits.hpp"
#include "hap/GRandomLeasePool.hpp"

namespace gen = Gem::Geneva::Genome;
namespace gind = Gem::Geneva::Individuals;
namespace oa = Gem::Geneva::OptimizationAlgorithms;

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
// GTestIndividual1: adaption, processing status and personality through the individual's public
// interface (formerly its own self-test functions).

namespace {

/** @brief Adapts an individual's values with the kernels alone: it is not marked for processing. */
void runKernelsOnly(gind::GTestIndividual1 &ind) {
    auto cfg = ind.getAdaptionConfig();
    gen::GAuxiliaryStore scratch;
    cfg->installInto(scratch);
    auto gr_lease = Gem::Hap::randomLeasePool().acquire();
    oa::runAdaptionKernels(ind, scratch, *cfg, *gr_lease);
}

/** @brief Makes sure an individual is processed. */
void processIfDue(gind::GTestIndividual1 &ind) {
    if(ind.is_due_for_processing()) {
        CHECK_NOTHROW(ind.process());
        CHECK(ind.is_processed());
    }
}

} // namespace

TEST_CASE("GTestIndividual1: every adaption changes the object", "[geneva][individuals]") {
    std::size_t const n_tests = 1000;

    gind::GTestIndividual1 const original;
    auto const p_test = original.clone<gind::GTestIndividual1>();
    auto const p_test_old = original.clone<gind::GTestIndividual1>();

    // One adapter held across the loop, so the self-adapting sigma persists between iterations
    oa::StandaloneAdapter adapter(*p_test, p_test->getAdaptionConfig());
    for(std::size_t i = 0; i < n_tests; i++) {
        CHECK_NOTHROW(adapter.adapt(*p_test));
        CHECK(*p_test != *p_test_old);
        CHECK_NOTHROW(p_test_old->load(p_test));
    }
}

TEST_CASE(
    "GTestIndividual1: adapted values change the fitness once the individual is processed",
    "[geneva][individuals]"
) {
    std::size_t const n_tests = 1000;

    gind::GTestIndividual1 p_test;
    processIfDue(p_test);

    double old_fitness = 0.;
    for(std::size_t i = 0; i < n_tests; i++) {
        // Change the values without marking the individual for processing
        CHECK_NOTHROW(runKernelsOnly(p_test));
        INFO("Processing status = " << p_test.getProcessingStatusAsStr() << ", i = " << i);
        CHECK((p_test.is_processed() || p_test.is_unprocessed()));

        CHECK_NOTHROW(p_test.mark_as_due_for_processing());
        CHECK(p_test.is_due_for_processing());
        CHECK(p_test.getProcessingStatus() == Gem::Courtier::processingStatus::DO_PROCESS);

        CHECK_NOTHROW(p_test.process());
        CHECK(p_test.is_processed());
        CHECK(p_test.getProcessingStatus() == Gem::Courtier::processingStatus::PROCESSED);
        double current_fitness = 0.;
        CHECK_NOTHROW(current_fitness = p_test.transformed_fitness(0));

        if(i > 0) {
            INFO("current_fitness = " << current_fitness << ", old_fitness = " << old_fitness);
            CHECK(current_fitness != old_fitness);
        }
        old_fitness = current_fitness;
    }
}

TEST_CASE("GTestIndividual1: adapting a clone leaves the original alone", "[geneva][individuals]") {
    auto const p_test1 = std::make_shared<gind::GTestIndividual1>();
    processIfDue(*p_test1);

    auto const p_test2 = p_test1->clone<gind::GTestIndividual1>();
    CHECK(*p_test2 == *p_test1);

    std::size_t n_adaptions = 0;
    CHECK_NOTHROW(
        n_adaptions = oa::StandaloneAdapter(*p_test2, p_test2->getAdaptionConfig()).adapt(*p_test2)
    );
    CHECK(n_adaptions > 0);
    CHECK(p_test2->is_due_for_processing());
    CHECK(not p_test1->is_due_for_processing());
    CHECK(*p_test1 != *p_test2);
}

TEST_CASE("GTestIndividual1: process() clears the processing request", "[geneva][individuals]") {
    // The individual is algorithm-agnostic data; it needs no personality to be processed
    gind::GTestIndividual1 p_test;

    CHECK_NOTHROW(p_test.mark_as_due_for_processing());
    CHECK(Gem::Courtier::processingStatus::DO_PROCESS == p_test.getProcessingStatus());
    CHECK(p_test.is_due_for_processing());

    CHECK_NOTHROW(p_test.process());
    CHECK(p_test.is_processed());
}

TEST_CASE(
    "GTestIndividual1: adapting and processing one of two equal individuals changes its fitness "
    "and values",
    "[geneva][individuals]"
) {
    gind::GTestIndividual1 const original;
    auto const p_test1 = original.clone<gind::GTestIndividual1>();
    auto const p_test2 = original.clone<gind::GTestIndividual1>();
    CHECK(*p_test1 == *p_test2);

    for(auto *p : {p_test1.get(), p_test2.get()}) {
        CHECK_NOTHROW(p->mark_as_due_for_processing());
        CHECK(p->is_due_for_processing());
        CHECK_NOTHROW(p->process());
        CHECK(p->is_processed());
    }
    double fitness1_old = 0.;
    CHECK_NOTHROW(fitness1_old = p_test1->transformed_fitness(0));

    std::vector<double> values_old;
    CHECK_NOTHROW(values_old = p_test1->parameterValues<double>());

    CHECK_NOTHROW(runKernelsOnly(*p_test1));
    CHECK_NOTHROW(p_test1->mark_as_due_for_processing());

    double fitness1_new = 0.;
    CHECK_NOTHROW(p_test1->process());
    CHECK_NOTHROW(fitness1_new = p_test1->transformed_fitness(0));
    CHECK(fitness1_new != fitness1_old);
    CHECK(*p_test1 != *p_test2);

    std::vector<double> values_new;
    CHECK_NOTHROW(values_new = p_test1->parameterValues<double>());
    CHECK(values_old != values_new);
}

TEST_CASE("GTestIndividual1: the personality can be set read and reset", "[geneva][individuals]") {
    // The personality is algorithm scratch carried on the individual itself (in its GAuxiliaryStore)
    gind::GTestIndividual1 ind;

    CHECK_NOTHROW(ind.resetPersonality());
    CHECK(ind.getPersonality() == "PERSONALITY_NONE");

    CHECK_NOTHROW(
        ind.setPersonality(std::make_shared<oa::GEvolutionaryAlgorithm_PersonalityTraits>())
    );
    CHECK(ind.getPersonality() == "GEvolutionaryAlgorithm_PersonalityTraits");
    CHECK(ind.getPersonalityTraits<oa::GEvolutionaryAlgorithm_PersonalityTraits>());
    CHECK(ind.getPersonalityTraits());

    CHECK_NOTHROW(ind.setPersonality(std::make_shared<oa::GPositionPersonalityTraits>()));
    CHECK(ind.getPersonality() == "GPositionPersonalityTraits");
    CHECK(ind.getPersonalityTraits<oa::GPositionPersonalityTraits>());
    CHECK(ind.getPersonalityTraits());

    CHECK_NOTHROW(ind.setPersonality(std::make_shared<oa::GSwarmAlgorithm_PersonalityTraits>()));
    CHECK(ind.getPersonality() == "GSwarmAlgorithm_PersonalityTraits");
    CHECK(ind.getPersonalityTraits<oa::GSwarmAlgorithm_PersonalityTraits>());
    CHECK(ind.getPersonalityTraits());

    CHECK_NOTHROW(ind.resetPersonality());
    CHECK(ind.getPersonality() == "PERSONALITY_NONE");
}

TEST_CASE(
    "GTestIndividual1: reading the fitness of an individual due for processing throws",
    "[geneva][individuals]"
) {
    gind::GTestIndividual1 p_test;
    CHECK_NOTHROW(p_test.mark_as_due_for_processing());
    CHECK_THROWS_AS(p_test.transformed_fitness(0), gem_exception);
}

TEST_CASE(
    "GTestIndividual1: reading personality traits that are not installed throws",
    "[geneva][individuals]"
) {
    gind::GTestIndividual1 ind;

    CHECK_NOTHROW(ind.resetPersonality());
    CHECK_THROWS_AS(
        ind.getPersonalityTraits<oa::GEvolutionaryAlgorithm_PersonalityTraits>(),
        gem_exception
    );
    CHECK_THROWS_AS(ind.getPersonalityTraits(), gem_exception);

#ifndef NDEBUG // a personality of another type is found by GEM_DEBUG_CHECK (g_ptr_conversion)
    CHECK_NOTHROW(ind.setPersonality(std::make_shared<oa::GSwarmAlgorithm_PersonalityTraits>()));
    CHECK_THROWS_AS(
        ind.getPersonalityTraits<oa::GEvolutionaryAlgorithm_PersonalityTraits>(),
        gem_exception
    );
#endif
}

/******************************************************************************/
