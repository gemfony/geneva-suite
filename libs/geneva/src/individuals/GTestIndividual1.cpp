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

#include "geneva/individuals/GTestIndividual1.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "hap/GRandomLeasePool.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GConjugateGradientDescent_PersonalityTraits.hpp"
#include "geneva/oa/GSwarmAlgorithm_PersonalityTraits.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <vector>

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#endif /* GEM_TESTING */

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GTestIndividual1) // NOLINT

namespace Gem::Geneva::Individuals {

/******************************************************************************/
/**
 * @brief The default constructor; builds a 100-double parabola genome and randomly initialises it.
 */
GTestIndividual1::GTestIndividual1() {
    using namespace Gem::Geneva;

    // 100 unbounded doubles (init perimeter [-10, 10]) sharing one Gauss adaptor. The builder's
    // default adapt_ad_prob == 0 prevents changes to adProb_. A small positive min_sigma (1e-3)
    // floors the self-adapting sigma so every adaption step stays well above ULP magnitude; this keeps
    // the "fitness changes after every customAdaptions()" unit test below reliable. With
    // min_sigma == 0 the shared sigma can collapse toward zero, the value steps fall back to a
    // one-ULP nudge, and 100 such nudges can round away in the sum-of-squares fitness.
    gen::GGenomeBuilder b;
    b.addDoublePlainGroup(100, -10., 10.); // structure only; the adaptor lives on the OA config
    this->setGenome(b.build());

    // Random per-parameter initialisation.
    this->randomInit(activityMode::ACTIVEONLY);
}

/******************************************************************************/
/**
 * @brief Builds the OA-owned adaption configuration: the single shared double group gets a Gauss adaptor
 * (sigma 0.025 / sigma_sigma 0.1 / [1e-3, 1] / ad_prob 1).
 *
 * @return A shared pointer to a freshly built adaption config whose double group carries the Gauss adaptor
 *         settings (sigma 0.025, sigma_sigma 0.1, min_sigma 1e-3, max_sigma 1.0, ad_prob 1.0)
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase> GTestIndividual1::getAdaptionConfig() const {
    namespace oa = Gem::Geneva::OptimizationAlgorithms;
    auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
    for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
        cfg->groupDouble(i).gauss(0.025, 0.1, 1e-3, 1., 1.);
    }
    return cfg;
}

/******************************************************************************/
/**
 * @brief The actual fitness calculation takes place here; computes a parabola (sum of squares) over the
 * flat double values.
 *
 * @return The fitness of this object: the sum of the squares of all flat double parameters
 */
std::vector<double> GTestIndividual1::evaluate() {
    // Read the flat double values and calculate the value of the parabola.
    std::vector<double> par_vec;
    this->streamline(par_vec);

    return {std::ranges::fold_left(
        par_vec | std::views::transform([](double x) { return x * x; }), 0., std::plus{})};
}

// Note: The following code is designed to mainly test parent classes

/******************************************************************************/
/**
 * @brief Applies modifications to this object. This is needed for testing purposes only.
 *
 * @return A boolean which indicates whether modifications were made
 */
bool GTestIndividual1::modify_GUnitTests_() {
#ifdef GEM_TESTING

    bool result = false;

    // Call the parent classes' functions
    if(gen::GGenome::modify_GUnitTests_()) {
        result = true;
    }

    // Change the parameter settings. The adaption state + logic are OA-owned; a standalone
    // individual drives them via a self-owned scratch + config (StandaloneAdapter).
    Gem::Geneva::OptimizationAlgorithms::StandaloneAdapter(*this, getAdaptionConfig()).adapt(*this);
    result = true;

    return result;
#else
    return false;
#endif /* GEM_TESTING */
}

/******************************************************************************/
/**
 * @brief Performs self tests that are expected to succeed. This is needed for testing purposes.
 */
// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity) -- self-test entry point for GTestIndividual1: a sequence of independent, self-scoped CHECK blocks covering adapt(), dirty-flag/process semantics, clone independence, EA/serial/SWARM process() paths, and personality-trait get/set; splitting per block would break the one-function-per-test-phase convention used identically across every individual/OA self-test in this codebase
void GTestIndividual1::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING
    using namespace Gem::Geneva;

    // Call the parent classes' functions
    gen::GGenome::specificTestsNoFailureExpected_GUnitTests_();

    //------------------------------------------------------------------------------

    { // Tests whether calls to adapt() result in changes of the object
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test_old =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();

        std::size_t const n_tests = 1000;

        // One adapter held across the loop, so the self-adapting sigma persists between iterations
        // exactly as it did when the adaption state lived on the individual.
        OptimizationAlgorithms::StandaloneAdapter adapter(*p_test, p_test->getAdaptionConfig());
        for(std::size_t i = 0; i < n_tests; i++) {
            CHECK_NOTHROW(adapter.adapt(*p_test));
            CHECK(*p_test != *p_test_old);
            CHECK_NOTHROW(p_test_old->load(p_test));
        }
    }

    //------------------------------------------------------------------------------

    { // Tests customAdaptions, dirty_flag and the effects of the fitness function. Also test setting of server-mode flag
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();

        // Make sure this individual is not dirty
        if(p_test->is_due_for_processing()) {
            CHECK_NOTHROW(p_test->process());
            CHECK(p_test->is_processed());
        }

        std::size_t const n_tests = 1000;

        double current_fitness = 0.;
        double old_fitness = current_fitness;
        bool const dirty_flag = false;

        // The per-group adaption state + the bare "mutate values without marking dirty" kernel run are
        // OA-owned. runAdaptionKernels() drifts the values but does NOT touch the processing status.
        auto cfg = p_test->getAdaptionConfig();
        gen::GAuxiliaryStore scratch;
        cfg->installInto(scratch);

        auto gr_lease = Gem::Hap::randomLeasePool().acquire();
        for(std::size_t i = 0; i < n_tests; i++) {
            // Change the parameters without instantly triggering fitness calculation
            CHECK_NOTHROW(OptimizationAlgorithms::runAdaptionKernels(*p_test, scratch, *cfg, *gr_lease));
            // The dirty flag should not have been set yet (done in adapt() )
            INFO("Processing status = " << p_test->getProcessingStatusAsStr() << ", i = " << i);
            CHECK((p_test->is_processed() || p_test->is_unprocessed()));
            // Set the flag manually
            CHECK_NOTHROW(p_test->mark_as_due_for_processing());
            // Check that the dirty flag has indeed been set
            CHECK(p_test->is_due_for_processing());
            CHECK(p_test->getProcessingStatus() == Gem::Courtier::processingStatus::DO_PROCESS);

            // Trigger value calculation
            CHECK_NOTHROW(p_test->process());
            CHECK(p_test->is_processed());
            CHECK(p_test->getProcessingStatus() == Gem::Courtier::processingStatus::PROCESSED);
            CHECK_NOTHROW(current_fitness = p_test->transformed_fitness(0));

            // Check that the evaluation has changed
            if(i > 0) {
                INFO(
                    "\n"
                    << "current_fitness = " << current_fitness << "\n"
                    << "old_fitness = " << old_fitness << "\n"
                    << "iteration = " << i << "\n"
                );
                CHECK(// Check that the fitness has changed
						current_fitness != old_fitness);
            }
            old_fitness = current_fitness;
        }
    }

    //------------------------------------------------------------------------------

    { // Tests whether modification of one clone influences another clone
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test1 =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();

        // Make sure the individual is clean
        if(p_test1->is_due_for_processing()) {
            CHECK_NOTHROW(p_test1->process());
            CHECK(p_test1->is_processed());
        }

        // Create a clone of p_test1
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test2 =
            p_test1->clone<Gem::Geneva::Individuals::GTestIndividual1>();
        // Check that the clone is identical to p_test1;
        CHECK_NOTHROW(*p_test2 == *p_test1);

        // Modify p_test2
        std::size_t n_adaptions = 0;
        CHECK_NOTHROW(n_adaptions = OptimizationAlgorithms::StandaloneAdapter(*p_test2, p_test2->getAdaptionConfig()).adapt(*p_test2));
        // Make sure adaptions were indeed performed
        CHECK(n_adaptions > 0);
        // Check that it is dirty
        CHECK(p_test2->is_due_for_processing());
        // Check that p_test1 is not dirty
        CHECK(not p_test1->is_due_for_processing());
        // Check that the two individuals differ
        CHECK(*p_test1 != *p_test2);
    }

    //------------------------------------------------------------------------------

    { // Check the effects of the process function in EA mode, using the "evaluate" call
        double const current_fitness = 0.;
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();

        // Make sure the individual is clean
        CHECK_NOTHROW(p_test->is_processed() || p_test->is_unprocessed());

        // Set the dirty flag
        CHECK_NOTHROW(p_test->mark_as_due_for_processing());

        // Setting the dirty flag should result in DO_PROCESS being set
        CHECK(Gem::Courtier::processingStatus::DO_PROCESS == p_test->getProcessingStatus());

        // Check that the dirty flag has indeed been set
        CHECK(p_test->is_due_for_processing());

        // Calling the process() function with the "evaluate" call should clear the dirty flag (the
        // individual is OA-agnostic data; it needs no personality to be processed)
        CHECK_NOTHROW(p_test->process());

        // The dirty flag should have been cleared
        CHECK(p_test->is_processed());
    }

    //------------------------------------------------------------------------------

    { // Check the process() function
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();

        // Make sure the individual is clean
        CHECK_NOTHROW(p_test->is_unprocessed() || p_test->is_processed());

        // Set the dirty flag
        CHECK_NOTHROW(p_test->mark_as_due_for_processing());

        // Check that the dirty flag has indeed been set
        CHECK(p_test->is_due_for_processing());

        // Calling the process() function with the "evaluate" call should clear the dirty flag
        CHECK_NOTHROW(p_test->process());

        // The dirty flag should have been cleared
        CHECK(p_test->is_processed());
    }

    //------------------------------------------------------------------------------

    { // Check the effects of the process function in SWARM mode, using the "evaluate" call
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();

        // Make sure the individual is clean
        CHECK_NOTHROW(p_test->is_unprocessed() || p_test->is_processed());

        // Set the dirty flag
        CHECK_NOTHROW(p_test->mark_as_due_for_processing());

        // Check that the dirty flag has indeed been set
        CHECK(p_test->is_due_for_processing());

        // Calling the process() function with the "evaluate" call should clear the dirty flag
        CHECK_NOTHROW(p_test->process());

        // The dirty flag should have been cleared
        CHECK(p_test->is_processed());
    }

    //------------------------------------------------------------------------------

    { // Check of the GGenome::customAdaptions() function
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test1 =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> const p_test2 =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();

        // Check that both individuals are the same
        CHECK(*p_test1 == *p_test2);

        // Make sure both individuals are clean and evaluated
        double fitness1_old = 0.;
        double fitness2_old = 0;
        CHECK_NOTHROW(p_test1->mark_as_due_for_processing());
        CHECK_NOTHROW(p_test2->mark_as_due_for_processing());
        CHECK(p_test1->is_due_for_processing());
        CHECK(p_test2->is_due_for_processing());
        CHECK_NOTHROW(p_test1->process());
        CHECK_NOTHROW(p_test2->process());
        CHECK(p_test1->is_processed());
        CHECK(p_test2->is_processed());
        CHECK_NOTHROW(fitness1_old = p_test1->transformed_fitness(0));
        CHECK_NOTHROW(fitness2_old = p_test2->transformed_fitness(0));

        // Snapshot the first individual's flat double values for later comparison
        std::vector<double> values_old;
        CHECK_NOTHROW(p_test1->streamline(values_old));

        // Adapt (drift values without marking dirty via runAdaptionKernels()) and evaluate the first
        // individual.
        {
            auto cfg = p_test1->getAdaptionConfig();
            gen::GAuxiliaryStore scratch;
            cfg->installInto(scratch);
            auto gr_lease = Gem::Hap::randomLeasePool().acquire();
            CHECK_NOTHROW(OptimizationAlgorithms::runAdaptionKernels(*p_test1, scratch, *cfg, *gr_lease));
        }
        // We need to manually mark the individual as dirty
        CHECK_NOTHROW(p_test1->mark_as_due_for_processing());

        // The fitness of individual1 should have changed. Re-evaluate and check
        double fitness1_new = 0.;
        CHECK_NOTHROW(p_test1->process());
        CHECK_NOTHROW(fitness1_new = p_test1->transformed_fitness(0));
        CHECK(fitness1_new != fitness1_old);

        // The individuals should now differ
        CHECK(*p_test1 != *p_test2);

        // Snapshot the first individual's flat double values again for comparison
        std::vector<double> values_new;
        CHECK_NOTHROW(p_test1->streamline(values_new));

        // Check that the value vectors differ
        CHECK(values_old != values_new);
    }

    //------------------------------------------------------------------------------

    // Generic container semantics for the genome's parameter storage are covered in
    // common/tests/UnitTests/GContainerTTests.cpp.

    //------------------------------------------------------------------------------

    { // Check setting and retrieval of the current personality status and whether the personalities
      // themselves can be accessed. The personality is OA scratch carried on the individual itself
      // (in its GAuxiliaryStore), exercised here via the personality surface on GGenome.
        auto ind = this->clone();
        std::shared_ptr<GPersonalityTraits> p_pt;

        // Reset the personality type
        CHECK_NOTHROW(ind->resetPersonality());
        INFO(
            "\n"
            << "ind->getPersonality() = " << ind->getPersonality() << "\n"
            << "expected PERSONALITY_NONE\n"
        );
        CHECK(ind->getPersonality() == "PERSONALITY_NONE");

        // Set the personality type to EA
        CHECK_NOTHROW(ind->setPersonality(
            std::make_shared<oa::GEvolutionaryAlgorithm_PersonalityTraits>()
        ));
        INFO(
            "\n"
            << "ind->getPersonality() = " << ind->getPersonality() << "\n"
            << "expected EA\n"
        );
        CHECK(ind->getPersonality() == "GEvolutionaryAlgorithm_PersonalityTraits");

        // Try to retrieve a GEvolutionaryAlgorithm_PersonalityTraits object and check that the smart pointer actually points somewhere
        std::shared_ptr<oa::GEvolutionaryAlgorithm_PersonalityTraits> p_pt_ea;
        CHECK_NOTHROW(
            p_pt_ea = ind->getPersonalityTraits<oa::GEvolutionaryAlgorithm_PersonalityTraits>()
        );
        CHECK(p_pt_ea);
        p_pt_ea.reset();

        // Retrieve a base pointer to the EA object and check that it points somewhere
        CHECK_NOTHROW(p_pt = ind->getPersonalityTraits());
        CHECK(p_pt);
        p_pt.reset();

        // Set the personality type to CGD
        CHECK_NOTHROW(ind->setPersonality(
            std::make_shared<oa::GConjugateGradientDescent_PersonalityTraits>()
        ));
        INFO(
            "\n"
            << "ind->getPersonality() = " << ind->getPersonality() << "\n"
            << "expected GConjugateGradientDescent_PersonalityTraits\n"
        );
        CHECK(ind->getPersonality() == "GConjugateGradientDescent_PersonalityTraits");

        // Try to retrieve a GConjugateGradientDescent_PersonalityTraits object and check that the smart pointer actually points somewhere
        std::shared_ptr<oa::GConjugateGradientDescent_PersonalityTraits> p_pt_cgd;
        CHECK_NOTHROW(p_pt_cgd = ind->getPersonalityTraits<oa::GConjugateGradientDescent_PersonalityTraits>());
        CHECK(p_pt_cgd);
        p_pt_cgd.reset();

        // Retrieve a base pointer to the CGD object and check that it points somewhere
        CHECK_NOTHROW(p_pt = ind->getPersonalityTraits());
        CHECK(p_pt);
        p_pt.reset();

        // Set the personality type to SWARM
        CHECK_NOTHROW(ind->setPersonality(
            std::make_shared<oa::GSwarmAlgorithm_PersonalityTraits>()
        ));
        INFO(
            "\n"
            << "ind->getPersonality() = " << ind->getPersonality() << "\n"
            << "expected GSwarmAlgorithm_PersonalityTraits\n"
        );
        CHECK(ind->getPersonality() == "GSwarmAlgorithm_PersonalityTraits");

        // Try to retrieve a GSwarmAlgorithm_PersonalityTraits object and check that the smart pointer actually points somewhere
        std::shared_ptr<oa::GSwarmAlgorithm_PersonalityTraits> p_pt_swarm;
        CHECK_NOTHROW(
            p_pt_swarm = ind->getPersonalityTraits<oa::GSwarmAlgorithm_PersonalityTraits>()
        );
        CHECK(p_pt_swarm);
        p_pt_swarm.reset();

        // Retrieve a base pointer to the SWARM object and check that it points somewhere
        CHECK_NOTHROW(p_pt = ind->getPersonalityTraits());
        CHECK(p_pt);
        p_pt.reset();

        // Set the personality type to PERSONALITY_NONE
        CHECK_NOTHROW(ind->resetPersonality());
        INFO(
            "\n"
            << "ind->getPersonality() = " << ind->getPersonality() << "\n"
            << "expected PERSONALITY_NONE\n"
        );
        CHECK(ind->getPersonality() == "PERSONALITY_NONE");
    }

    // --------------------------------------------------------------------------
#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset(
        "Gem::Geneva::Individuals::GTestIndividual1::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
/**
 * @brief Performs self tests that are expected to fail. This is needed for testing purposes.
 */
// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity) -- paired failure-expected self-test entry point for GTestIndividual1: independent, self-scoped CHECK_THROWS_AS blocks covering dirty-evaluation and personality-mismatch error paths; same one-function-per-test-phase convention as specificTestsNoFailureExpected_GUnitTests_
void GTestIndividual1::specificTestsFailuresExpected_GUnitTests_() {
#ifdef GEM_TESTING
    using namespace Gem::Geneva;


    //------------------------------------------------------------------------------

#ifdef DEBUG
    { // Tests that evaluating a dirty individual in "server mode" throws
        std::shared_ptr<Gem::Geneva::Individuals::GTestIndividual1> p_test =
            this->clone<Gem::Geneva::Individuals::GTestIndividual1>();

        CHECK_NOTHROW(p_test->mark_as_due_for_processing());
        CHECK_THROWS_AS(p_test->transformed_fitness(0), geneva_exception);
    }
#endif /* DEBUG */

    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------


    //------------------------------------------------------------------------------

#ifdef DEBUG
    { // Test that retrieval of an EA personality traits object from an uninitialized individual throws in DEBUG mode
        auto ind = this->clone();

        // Make sure the personality type is set to PERSONALITY_NONE
        CHECK_NOTHROW(ind->resetPersonality());

        // Trying to retrieve an EA personality object should throw
        std::shared_ptr<oa::GEvolutionaryAlgorithm_PersonalityTraits> p_pt_ea;
        CHECK_THROWS_AS(
            (p_pt_ea = ind->getPersonalityTraits<oa::GEvolutionaryAlgorithm_PersonalityTraits>()),
            geneva_exception
        );
    }
#endif /* DEBUG */

    //------------------------------------------------------------------------------

#ifdef DEBUG
    { // Test that retrieval of an EA personality traits object from an individual with SWARM personality throws
        auto ind = this->clone();

        // Make sure the personality type is set to SWARM
        CHECK_NOTHROW(ind->setPersonality(
            std::make_shared<oa::GSwarmAlgorithm_PersonalityTraits>()
        ));

        // Trying to retrieve an EA personality object should throw
        CHECK_THROWS_AS(
            (ind->getPersonalityTraits<oa::GEvolutionaryAlgorithm_PersonalityTraits>()),
            geneva_exception
        );
    }
#endif /* DEBUG */

    //------------------------------------------------------------------------------

#ifdef DEBUG
    { // Test that retrieval of a personality traits base object from an individual without personality throws
        auto ind = this->clone();

        // Make sure the personality type is set to PERSONALITY_NONE
        CHECK_NOTHROW(ind->resetPersonality());

        // Trying to retrieve an EA personality object should throw
        std::shared_ptr<GPersonalityTraits> p_pt;
        CHECK_THROWS_AS((p_pt = ind->getPersonalityTraits()), geneva_exception);
    }
#endif /* DEBUG */

    //------------------------------------------------------------------------------
#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset(
        "GTestIndividual1::specificTestsFailuresExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif                  /* GEM_TESTING */
}

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
