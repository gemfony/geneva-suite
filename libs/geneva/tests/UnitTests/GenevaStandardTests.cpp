/**
 * @file GenevaStandardTests.cpp
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

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

// All classes that will be tested in this file
#include "GNeuralNetworkTestData.hpp"
#include "GTestIndividual1.hpp"
#include "GTestIndividual2.hpp"
#include "GTestIndividual3.hpp"
#include "geneva/individuals/GDelayIndividual.hpp"
#include "geneva/individuals/GExternalEvaluatorIndividual.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/individuals/GLineFitIndividual.hpp"
#include "geneva/individuals/GMetaOptimizerIndividualT.hpp"
#include "geneva/individuals/GNeuralNetworkIndividual.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (probe types)
// Optimization algorithms (each names its personality traits) and the store they register in
#include "geneva/oa/GAntColonyOptimization.hpp"
#include "geneva/oa/GConjugateGradientDescent.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GFactoryStore.hpp"
#include "geneva/oa/GGeneralizedSimulatedAnnealing.hpp"
#include "geneva/oa/GMetaEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GNelderMead.hpp"
#include "geneva/oa/GParameterScan.hpp"
#include "geneva/oa/GSepCmaEvolutionStrategy.hpp"
#include "geneva/oa/GSimulatedAnnealing.hpp"
#include "geneva/oa/GStandardPSO2011.hpp"
#include "geneva/oa/GSwarmAlgorithm.hpp"
// Constraints
#include "geneva/genome/GGenomeMultiConstraint.hpp"
// Pluggable optimization monitors
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"

#include "common/testing/GStandardTests.hpp"

using namespace Gem::Geneva;
using Gem::Common::Testing::GEqualityPrinter;

// ============================================================================
// TFactory_GUnitTests specializations for classes without a public default constructor. They must be
// visible where the standard tests are instantiated below.
// ============================================================================

namespace Gem::Common::Testing {

/** @brief gind::GTestIndividual2 -- public constructor (size, PERFOBJECTTYPE). */
template <>
inline std::shared_ptr<gind::GTestIndividual2> TFactory_GUnitTests<gind::GTestIndividual2>() {
    return std::make_shared<gind::GTestIndividual2>(
        std::size_t(100),
        gind::PERFOBJECTTYPE::PERFGDOUBLEOBJECT
    );
}

/** @brief gind::GLineFitIndividual -- public constructor taking a vector of {x,y} sample points. */
template <>
inline std::shared_ptr<gind::GLineFitIndividual> TFactory_GUnitTests<gind::GLineFitIndividual>() {
    std::vector<std::tuple<double, double>> const
        data_points{{0., 0.}, {1., 1.}, {2., 2.}, {3., 3.}};
    return std::make_shared<gind::GLineFitIndividual>(data_points);
}

/** @brief gind::GNeuralNetworkIndividual -- its default constructor is private; the parameterized one
 *  is used with the default settings. */
template <>
inline std::shared_ptr<gind::GNeuralNetworkIndividual>
TFactory_GUnitTests<gind::GNeuralNetworkIndividual>() {
    return std::make_shared<gind::GNeuralNetworkIndividual>(
        gind::GNN_DEF_MINVAR,
        gind::GNN_DEF_MAXVAR,
        gind::GNN_DEF_SIGMA,
        gind::GNN_DEF_SIGMASIGMA,
        gind::GNN_DEF_MINSIGMA,
        gind::GNN_DEF_MAXSIGMA,
        gind::GNN_DEF_ADPROB,
        gind::GNN_DEF_ADAPTADPROB,
        gind::GNN_DEF_MINADPROB,
        gind::GNN_DEF_MAXADPROB
    );
}

} // namespace Gem::Common::Testing

// ============================================================================
// The type lists. The algorithms are listed ONCE; their personality traits are derived from that list
// at compile time, and a test below compares the list with what the library registers, so a built-in
// algorithm that is left out turns the suite red.
// ============================================================================

namespace {

/** @brief Every built-in algorithm that registers a factory in oaFactoryStore() (GBuiltinAlgorithms.cpp). */
using registered_algorithm_types = std::tuple<
    oa::GAntColonyOptimization,
    oa::GConjugateGradientDescent,
    oa::GEvolutionaryAlgorithm,
    oa::GGeneralizedSimulatedAnnealing,
    oa::GNelderMead,
    oa::GParameterScan,
    oa::GSepCmaEvolutionStrategy,
    oa::GSimulatedAnnealing,
    oa::GStandardPSO2011,
    oa::GSwarmAlgorithm>;

/** @brief The personality traits each algorithm of a list names, as a list. */
template <typename list_type>
struct personality_traits_of;
template <typename... algorithm_types>
struct personality_traits_of<std::tuple<algorithm_types...>> {
    using type = std::tuple<typename algorithm_types::personality_traits_type...>;
};

/** @brief All algorithm types under the standard tests. GMetaEvolutionaryAlgorithm is added by hand: it
 *  carries the EA's personality traits, so its mnemonic would be "ea", and it is not registered in the
 *  factory store -- the store comparison below cannot see it. */
using algorithm_types = decltype(std::tuple_cat(
    std::declval<registered_algorithm_types>(),
    std::declval<std::tuple<oa::GMetaEvolutionaryAlgorithm>>()
));

/** @brief The personality traits of all registered algorithms (MetaEA's are the EA's). */
using trait_types = personality_traits_of<registered_algorithm_types>::type;

/** @brief The individuals under the standard tests. */
using individual_types = std::tuple<
    gind::GTestIndividual1,
    gind::GTestIndividual2,
    gind::GTestIndividual3,
    gind::GLineFitIndividual,
    gind::GFunctionIndividual,
    gind::GDelayIndividual,
    gind::GExternalEvaluatorIndividual,
    gind::GMetaOptimizerIndividualT<gind::GFunctionIndividual>,
    gind::GNeuralNetworkIndividual>;

/** @brief The concrete GGenomeConstraint subclasses defined in GFunctionIndividual.hpp. Their own
 *  members (the constants) are not changed by modifyForTest(), which they inherit from their root; the
 *  focused round-trip TEST_CASEs below cover those members. */
using constraint_types =
    std::tuple<gind::GDoubleSumConstraint, gind::GSphereConstraint, gind::GDoubleSumGapConstraint>;

/** @brief The pluggable optimization monitors under the standard tests. */
using monitor_types = std::tuple<
    GStandardMonitor,
    GFitnessMonitor,
    GCollectiveMonitor,
    GProgressPlotter,
    GAllSolutionFileLogger,
    GNAdpationsLogger,
    GAdaptorPropertyLogger<double>,
    GProcessingTimesLogger,
    gind::GOptOptMonitorT<gind::GFunctionIndividual>>;

/** @brief The mnemonics of the registered algorithms, sorted.
 *  @return One personality nickname per algorithm of registered_algorithm_types */
template <typename... algorithm_types>
std::vector<std::string> mnemonicsOf(std::tuple<algorithm_types...> const * /* list */) {
    std::vector<std::string> mnemonics{
        std::string(algorithm_types::personality_traits_type::nickname)...
    };
    std::ranges::sort(mnemonics);
    return mnemonics;
}

/** @brief Every type of a list can be changed for the standard tests, so their round-trips run. */
template <typename list_type>
struct all_modifiable_for_test;
template <typename... types>
struct all_modifiable_for_test<std::tuple<types...>> {
    static constexpr bool value = (Gem::Common::Testing::ModifiableForTest<types> && ...);
};

static_assert(all_modifiable_for_test<trait_types>::value);
static_assert(all_modifiable_for_test<individual_types>::value);
static_assert(all_modifiable_for_test<constraint_types>::value);
static_assert(all_modifiable_for_test<algorithm_types>::value);
static_assert(all_modifiable_for_test<monitor_types>::value);

} // anonymous namespace

TEST_CASE(
    "Every algorithm in the factory store is in the standard tests' algorithm list",
    "[geneva][standard]"
) {
    std::vector<std::string> const listed =
        mnemonicsOf(static_cast<registered_algorithm_types const *>(nullptr));
    std::vector<std::string> stored;
    oaFactoryStore()->getKeyVector(stored);
    std::ranges::sort(stored);

    std::vector<std::string> missing_from_list;
    std::ranges::set_difference(stored, listed, std::back_inserter(missing_from_list));
    for(auto const &mnemonic : missing_from_list) {
        FAIL_CHECK(
            "The library registers algorithm \""
            << mnemonic << "\", but registered_algorithm_types does not list it"
        );
    }

    std::vector<std::string> missing_from_store;
    std::ranges::set_difference(listed, stored, std::back_inserter(missing_from_store));
    for(auto const &mnemonic : missing_from_store) {
        FAIL_CHECK(
            "registered_algorithm_types lists algorithm \""
            << mnemonic << "\", but the library does not register it"
        );
    }

    CHECK(listed == stored);
}

// ============================================================================
// Standard tests — no failure expected
// ============================================================================

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_no_failure_expected — trait types",
    "[geneva][standard]",
    trait_types
) {
    Gem::Common::Testing::StandardTests_no_failure_expected<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_no_failure_expected — individual types",
    "[geneva][standard]",
    individual_types
) {
    if constexpr(std::same_as<TestType, gind::GNeuralNetworkIndividual>) {
        gind::Testing::provideNeuralNetworkTrainingData();
    }
    Gem::Common::Testing::StandardTests_no_failure_expected<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_no_failure_expected — constraint types",
    "[geneva][standard]",
    constraint_types
) {
    Gem::Common::Testing::StandardTests_no_failure_expected<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_no_failure_expected — algorithm types",
    "[geneva][standard]",
    algorithm_types
) {
    Gem::Common::Testing::StandardTests_no_failure_expected<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_no_failure_expected — monitor types",
    "[geneva][standard]",
    monitor_types
) {
    Gem::Common::Testing::StandardTests_no_failure_expected<TestType>();
}

// ============================================================================
// Standard tests — failures expected
// ============================================================================

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_failures_expected — trait types",
    "[geneva][standard][failures-expected]",
    trait_types
) {
    Gem::Common::Testing::StandardTests_failures_expected<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_failures_expected — individual types",
    "[geneva][standard][failures-expected]",
    individual_types
) {
    if constexpr(std::same_as<TestType, gind::GNeuralNetworkIndividual>) {
        gind::Testing::provideNeuralNetworkTrainingData();
    }
    Gem::Common::Testing::StandardTests_failures_expected<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_failures_expected — constraint types",
    "[geneva][standard][failures-expected]",
    constraint_types
) {
    Gem::Common::Testing::StandardTests_failures_expected<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_failures_expected — algorithm types",
    "[geneva][standard][failures-expected]",
    algorithm_types
) {
    Gem::Common::Testing::StandardTests_failures_expected<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "StandardTests_failures_expected — monitor types",
    "[geneva][standard][failures-expected]",
    monitor_types
) {
    Gem::Common::Testing::StandardTests_failures_expected<TestType>();
}

// ============================================================================
// Targeted (de)serialization round-trips for the concrete GGenomeConstraint
// subclasses in GFunctionIndividual.hpp. The standard tests change them only through their
// root's modifyForTest(), which does not reach their own constants; these cases set the constants
// to non-default values, so the serialize path of the constants is validated here.
// ============================================================================
TEST_CASE("GDoubleSumConstraint round-trips in BINARY/JSON", "[geneva][serialization]") {
    using Gem::Common::serializationMode;

    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        // Non-default value so the round-trip actually exercises the member
        // (the default is 1.0; a serialize that dropped it would still pass at 1.0).
        gind::GDoubleSumConstraint const original(3.5);
        gind::GDoubleSumConstraint restored(2.0);

        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));

        GEqualityPrinter const gep(
            "GDoubleSumConstraint-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }
}

TEST_CASE("GSphereConstraint round-trips in BINARY/JSON", "[geneva][serialization]") {
    using Gem::Common::serializationMode;

    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        // Non-default value (default 1.0): catches the previously-latent bug where
        // GSphereConstraint::serialize() did not store diameter_ at all.
        gind::GSphereConstraint const original(3.5);
        gind::GSphereConstraint restored(2.0);

        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));

        GEqualityPrinter const gep(
            "GSphereConstraint-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }
}

TEST_CASE("GDoubleSumGapConstraint round-trips in BINARY/JSON", "[geneva][serialization]") {
    using Gem::Common::serializationMode;

    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        // Non-default values (defaults 1.0 / 0.5) so both members are exercised.
        gind::GDoubleSumGapConstraint const original(3.5, 1.25);
        gind::GDoubleSumGapConstraint restored(2.0, 0.25);

        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));

        GEqualityPrinter const gep(
            "GDoubleSumGapConstraint-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }
}

// Regression: GExternalEvaluatorIndividual::serialize() previously omitted run_id_,
// so it was silently lost on (de)serialization. A non-default run_id_ must survive a
// round-trip in both modes.
TEST_CASE(
    "GExternalEvaluatorIndividual round-trips run_id_ in BINARY/JSON",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        gind::GExternalEvaluatorIndividual original;
        original.setRunId("custom-run-id-xyz");
        gind::GExternalEvaluatorIndividual restored;
        restored.setRunId("other-run-id");

        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));

        GEqualityPrinter const gep(
            "GExternalEvaluatorIndividual-runid-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }
}

// Safety net for the oa::GOptimizationAlgorithmBase serialize() refactor: oa::GOptimizationAlgorithmBase is the central
// serialised class for all optimization algorithms (checkpoints + network
// transport). Its split save()/load() was collapsed into a single serialize()
// after std::filesystem::path gained a free serialization. This test sets several
// oa::GOptimizationAlgorithmBase members -- in particular cp_directory_path_ (the path that forced the
// former split) -- to NON-DEFAULT values and verifies they survive a round-trip in
// both modes, guarding against silently dropping a member.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent serialization sweep (BINARY/JSON) for oa::GOptimizationAlgorithmBase's members
TEST_CASE(
    "oa::GOptimizationAlgorithmBase (via GEvolutionaryAlgorithm) round-trips its members incl. the "
    "checkpoint path",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        oa::GEvolutionaryAlgorithm original;
        // Set several oa::GOptimizationAlgorithmBase members to non-default values via public setters.
        // "." is an always-existing directory, so setCheckpointBaseName creates nothing.
        original.setCheckpointBaseName(".", "custom_checkpoint_base");
        original.setCheckpointInterval(7);
        original.setMaxIteration(4242);
        original.setReportIteration(13);
        original.setMaxStallIteration(99);

        oa::GEvolutionaryAlgorithm restored;
        // Deliberately different starting values, so the round-trip has to overwrite them.
        restored.setMaxIteration(1);
        restored.setReportIteration(1);

        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));

        // Explicit getter checks: these would catch a dropped member that the
        // structural compare might otherwise tolerate.
        CHECK(restored.getCheckpointDirectory() == original.getCheckpointDirectory());
        CHECK(restored.getCheckpointBaseName() == original.getCheckpointBaseName());
        CHECK(restored.getCheckpointInterval() == 7);
        CHECK(restored.getMaxIteration() == 4242);
        CHECK(restored.getReportIteration() == 13);
        CHECK(restored.getMaxStallIteration() == 99);

        GEqualityPrinter const gep(
            "oa::GOptimizationAlgorithmBase-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }
}

// Safety net for the GGenome serialize()/load_()/compare_() unification
// onto a single localMembers() declaration. GGenome is the central serialised
// base for all individuals. This test sets several of its localMembers()-listed members
// to non-default values (the adaption limits and the stall / best-known-fitness book-keeping)
// and checks they survive both an in-memory load() (clone path) AND a wire round-trip; if any
// member were dropped from serialize()/load_()/compare_(), one of those would fail. The shared
// optimization direction (carried by the policy) is checked alongside. Exercised on the concrete
// GTestIndividual1 (a GGenome subclass).
TEST_CASE(
    "GGenome (via GTestIndividual1) round-trips its serialised members",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    auto make_original = []() {
        gind::GTestIndividual1 ind;
        ind.setAssignedIteration(7);
        ind.setMaxMode(maxMode::MAXIMIZE);
        return ind;
    };

    // --- in-memory load() (clone path) ---
    {
        gind::GTestIndividual1 const original = make_original();
        gind::GTestIndividual1 restored; // defaults

        REQUIRE_NOTHROW(restored.load(original));

        CHECK(restored.getAssignedIteration() == 7);
        CHECK(restored.getMaxMode() == maxMode::MAXIMIZE);

        GEqualityPrinter const gep(
            "GGenome-load-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }

    // --- wire round-trip in both modes ---
    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        gind::GTestIndividual1 const original = make_original();
        gind::GTestIndividual1 restored; // defaults

        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));

        CHECK(restored.getAssignedIteration() == 7);
        CHECK(restored.getMaxMode() == maxMode::MAXIMIZE);

        GEqualityPrinter const gep("GGenome-roundtrip", pow(10, -7), Gem::Common::CE_WITH_MESSAGES);
        CHECK(gep.isSimilar(restored, original));
    }
}

// Safety net for the oa::GSwarmAlgorithm serialize()/load_()/compare_() unification
// onto a single localMembers() declaration (the unconditional members) plus a manual
// tail for the conditionally-reconstructed neighborhood/global-best members. This test
// sets several of the about-to-be-tied members to NON-DEFAULT values and verifies they
// survive a wire round-trip AND an in-memory load() (clone path), guarding against the
// default-value masking that hides a dropped member.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent serialization sweep (load() clone path + BINARY/JSON wire round-trip) for oa::GSwarmAlgorithm's tied members
TEST_CASE(
    "oa::GSwarmAlgorithm round-trips its members in BINARY/JSON and via load()",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    auto makeOriginal = []() {
        oa::GSwarmAlgorithm a;
        // Non-default values for several of the localMembers()-tied members.
        a.setCPersonal(1.75);
        a.setCNeighborhood(2.25);
        a.setCGlobal(3.5);
        a.setCVelocity(0.875);
        a.setVelocityRangePercentage(0.42);
        a.setUpdateRule(updateRule::SWARM_UPDATERULE_LINEAR);
        a.setRepulsionThreshold(17);
        a.setNeighborhoodsRandomFillUp(false);
        return a;
    };

    auto checkGetters = [](const oa::GSwarmAlgorithm &restored) {
        CHECK(restored.getCPersonal() == 1.75);
        CHECK(restored.getCNeighborhood() == 2.25);
        CHECK(restored.getCGlobal() == 3.5);
        CHECK(restored.getCVelocity() == 0.875);
        CHECK(restored.getVelocityRangePercentage() == 0.42);
        CHECK(restored.getUpdateRule() == updateRule::SWARM_UPDATERULE_LINEAR);
        CHECK(restored.getRepulsionThreshold() == 17);
        CHECK(restored.neighborhoodsFilledUpRandomly() == false);
    };

    // --- in-memory load() (clone path) ---
    {
        oa::GSwarmAlgorithm const original = makeOriginal();
        oa::GSwarmAlgorithm restored;
        restored.setCPersonal(0.1);
        restored.setNeighborhoodsRandomFillUp(true);

        REQUIRE_NOTHROW(restored.load(original));
        checkGetters(restored);

        GEqualityPrinter const gep(
            "GSwarmAlgorithm-load-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }

    // --- wire round-trip in both modes ---
    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        oa::GSwarmAlgorithm const original = makeOriginal();
        oa::GSwarmAlgorithm restored;
        restored.setCPersonal(0.1);
        restored.setNeighborhoodsRandomFillUp(true);

        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));
        checkGetters(restored);

        GEqualityPrinter const gep(
            "GSwarmAlgorithm-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }
}

// Safety net for the oa::GParameterScan serialize()/load_()/compare_() unification
// onto a single localMembers() declaration (the plain members) plus a manual tail
// (the scan-parameter vectors, whose element type lacks the Gemfony common interface,
// and the load-only transient cycle_logic_halt_). This test sets several of the
// about-to-be-tied plain members to NON-DEFAULT values and verifies they survive a
// wire round-trip AND an in-memory load() (clone path).
TEST_CASE(
    "oa::GParameterScan round-trips its members in BINARY/JSON and via load()",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    auto makeOriginal = []() {
        oa::GParameterScan a;
        // Non-default values for the localMembers()-tied plain members.
        a.setScanRandomly(false); // default is true
        a.setNSimpleScans(7);     // sets simple_scan_items_ (default 0)
        return a;
    };

    auto checkGetters = [](const oa::GParameterScan &restored) {
        CHECK(restored.getScanRandomly() == false);
        CHECK(restored.getNSimpleScans() == 7);
    };

    // --- in-memory load() (clone path) ---
    {
        oa::GParameterScan const original = makeOriginal();
        oa::GParameterScan restored;
        restored.setScanRandomly(true);

        REQUIRE_NOTHROW(restored.load(original));
        checkGetters(restored);

        GEqualityPrinter const gep(
            "GParameterScan-load-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }

    // --- wire round-trip in both modes ---
    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        oa::GParameterScan const original = makeOriginal();
        oa::GParameterScan restored;
        restored.setScanRandomly(true);

        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));
        checkGetters(restored);

        GEqualityPrinter const gep(
            "GParameterScan-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }
}

// Regression for the GScanParT fold: the four concrete scan-parameter leaves (GBScanPar, GInt32ScanPar,
// GDScanPar, GFScanPar) lost their hand-written serialize() and now inherit the GReflectiveInterfaceT-generated
// one (base_object<GBaseScanParT<T>> plus an empty localMembers()), with clone_() generated by the same
// mixin. A POPULATED GParameterScan carries one object of every leaf type through its
// d_cnt_/f_cnt_/int32_cnt_/b_cnt_ vectors, so this pins that all four survive a wire round-trip (BINARY,
// JSON) AND an in-memory load() (the generated clone_/load_ path), with their pre-computed grids intact.
// The preceding test populates none of these vectors, so without this one the fold would be untested.
TEST_CASE(
    "oa::GParameterScan round-trips its concrete scan parameters (all four leaf types)",
    "[geneva][serialization][ps]"
) {
    using Gem::Common::serializationMode;

    auto makeOriginal = []() {
        oa::GParameterScan a;
        a.setScanRandomly(
            false
        ); // GRID scan -> each leaf pre-computes a data grid that must survive too
        // One parameter of every supported leaf type: double, float, int32 and bool.
        a.setParameterSpecs("d(0, -5., 5., 3), f(1, -5., 5., 3), i(2, -5, 5, 3), b(3)");
        return a;
    };

    // --- in-memory load() (clone path: the mixin-generated clone_/load_ over each leaf) ---
    {
        oa::GParameterScan const original = makeOriginal();
        oa::GParameterScan restored;
        REQUIRE_NOTHROW(restored.load(original));
        GEqualityPrinter const gep(
            "GParameterScan-scanpar-load",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }

    // --- wire round-trip in both modes (polymorphic serialize of every leaf) ---
    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        oa::GParameterScan const original = makeOriginal();
        oa::GParameterScan restored;
        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));
        GEqualityPrinter const gep(
            "GParameterScan-scanpar-roundtrip",
            pow(10, -7),
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, original));
    }
}

// A minimal concrete GCommonInterfaceT root that carries one member of each divergent-participation kind
// -- plain, load-only and transient -- used to pin the member-descriptor policy matrix directly. The real
// classes exercise these indirectly, but this isolates the axis that distinguishes make_transient_member
// from make_load_only_member: a transient member IS part of comparable identity yet is NOT serialized,
// whereas a load-only member is neither compared nor serialized. No other single test pins that on its own;
// it guards both the policy refactor of the reflection layer and the new transient flavour that the
// GOptimizationAlgorithmBase fold introduced (its best_iteration_individuals_pq_ is exactly this kind).
namespace {
class GPolicyProbe
  : public Gem::Common::
        GReflectiveInterfaceT<GPolicyProbe, Gem::Common::GCommonInterfaceT<GPolicyProbe>> {
    friend struct Gem::Weft::access;
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            Gem::Common::make_member("plain_", self.plain_),
            Gem::Common::make_load_only_member("loadonly_", self.loadonly_),
            Gem::Common::make_transient_member("transient_", self.transient_)
        );
    }

public:
    static constexpr std::string_view class_name = "GPolicyProbe";
    GPolicyProbe() = default;

    int plain_ = 0;     ///< serialized + loaded + compared
    int loadonly_ = 0;  ///< loaded only (skipped by both serialize and compare)
    int transient_ = 0; ///< loaded + compared, but NOT serialized

protected:
};
} // anonymous namespace

GEM_REGISTER_ARCHIVABLE(GPolicyProbe) // NOLINT

TEST_CASE(
    "member descriptor policies: transient is compared yet not serialized (vs load-only)",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    GPolicyProbe a;
    a.plain_ = 1;
    a.loadonly_ = 2;
    a.transient_ = 3;

    // --- load(): every kind is copied in memory ---
    {
        GPolicyProbe b;
        b.load(a);
        CHECK(b.plain_ == 1);
        CHECK(b.loadonly_ == 2);  // load-only IS loaded
        CHECK(b.transient_ == 3); // transient IS loaded
    }

    // --- compare: plain and transient are part of identity; load-only is not ---
    GEqualityPrinter const gep("policy-probe", 0., Gem::Common::CE_WITH_MESSAGES);
    {
        GPolicyProbe c = a;
        c.loadonly_ = 999;        // differs only in the load-only member ...
        CHECK(gep.isEqual(c, a)); // ... yet still equal: load-only is skipped by compare
    }
    {
        GPolicyProbe c = a;
        c.transient_ = 999;           // differs only in the transient member ...
        CHECK(not gep.isEqual(c, a)); // ... NOT equal: a transient member IS compared
    }
    {
        GPolicyProbe c = a;
        c.plain_ = 999;
        CHECK(not gep.isEqual(c, a)); // a plain member is compared
    }

    // --- serialize: only the plain member travels; load-only and transient are skipped ---
    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        GPolicyProbe restored; // members default to 0
        restored.fromString(a.toString(mode), mode);
        CHECK(restored.plain_ == 1);     // serialized -> survives the wire
        CHECK(restored.loadonly_ == 0);  // skipped -> stays at its default
        CHECK(restored.transient_ == 0); // skipped -> stays at its default
    }
}

// A guard for make_base_object_member: a minimal stateful, serializable, copy-assignable base
// (GProbeBase) carried through a derived that folds onto the mixin. It pins the three axes of a base-object
// descriptor -- serialize as base_object, load via the base's operator= (base-slice copy), and cmp_skip --
// which is the tool the GGenome fold uses for its GProcessable base.
namespace {
struct GProbeBase {
    int base_val_ = 0;
    /** @brief Serialization of the base slice (public so base_object can reach it). */
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named(ar, "base_val_", base_val_);
    }
};

class GBaseObjectProbe
  : public GProbeBase
  , public Gem::Common::
        GReflectiveInterfaceT<GBaseObjectProbe, Gem::Common::GCommonInterfaceT<GBaseObjectProbe>> {
    friend struct Gem::Weft::access;
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            Gem::Common::make_base_object_member<GProbeBase>("GProbeBase", self),
            Gem::Common::make_member("d_", self.d_)
        );
    }

    // Disambiguating serialize(): both GProbeBase and the mixin declare serialize().
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::serialize_members(ar, this->localMembers_());
    }

public:
    static constexpr std::string_view class_name = "GBaseObjectProbe";
    GBaseObjectProbe() = default;

    int d_ = 0; ///< a plain derived member (serialized + loaded + compared)

protected:
};
} // anonymous namespace

GEM_REGISTER_ARCHIVABLE(GBaseObjectProbe) // NOLINT

TEST_CASE(
    "member descriptor policies: make_base_object_member carries a stateful base",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    GBaseObjectProbe a;
    a.base_val_ = 7; // in the GProbeBase slice
    a.d_ = 3;        // in the derived

    // --- load(): both the base slice and the derived member are copied ---
    {
        GBaseObjectProbe b;
        b.load(a);
        CHECK(b.base_val_ == 7); // base slice loaded via load_base_slice (Base::operator=)
        CHECK(b.d_ == 3);
    }

    // --- compare: the base is NOT part of comparable identity (cmp_skip); the derived member IS ---
    GEqualityPrinter const gep("base-object-probe", 0., Gem::Common::CE_WITH_MESSAGES);
    {
        GBaseObjectProbe c = a;
        c.base_val_ = 999;        // differs only in the base slice ...
        CHECK(gep.isEqual(c, a)); // ... still equal: the base is cmp_skip
    }
    {
        GBaseObjectProbe c = a;
        c.d_ = 999;
        CHECK(not gep.isEqual(c, a)); // the derived member is compared
    }

    // --- serialize: the base slice travels via base_object ---
    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        GBaseObjectProbe restored;
        restored.fromString(a.toString(mode), mode);
        CHECK(restored.base_val_ == 7); // base slice survived via base_object
        CHECK(restored.d_ == 3);
    }
}

// ============================================================================
// Copy assignment is as deep as copy construction. Both classes below wrote their copy constructor by
// hand (a deep clone of a pointer member) and left the copy assignment implicit, so an assigned object
// shared that member with its source.
// ============================================================================

TEST_CASE(
    "GSwarmAlgorithm_PersonalityTraits: an assigned copy does not share the personal best",
    "[geneva][copy]"
) {
    auto best = std::make_shared<gind::GTestIndividual1>();
    best->process(); // registerPersonalBest() wants a processed individual

    oa::GSwarmAlgorithm_PersonalityTraits source;
    source.registerPersonalBest(best);

    oa::GSwarmAlgorithm_PersonalityTraits assigned;
    assigned = source;

    REQUIRE(assigned.getPersonalBest());
    // registerPersonalBest() loads into the stored individual in place, so a shared one would carry a
    // later registration on the copy back into the source.
    CHECK(assigned.getPersonalBest().get() != source.getPersonalBest().get());
}

TEST_CASE(
    "GCollectiveMonitor: an assigned copy does not share the registered monitors",
    "[geneva][copy]"
) {
    auto member = std::make_shared<GFitnessMonitor>();

    GCollectiveMonitor source;
    source.registerPluggableOM(member); // registration shares the monitor with the caller

    GCollectiveMonitor const constructed(source); // deep, the reference
    GCollectiveMonitor assigned;
    assigned = source;

    // A change to the source's monitor must not reach the assigned copy.
    REQUIRE(member->modifyForTest());
    GEqualityPrinter const gep("collective-assignment", 0., Gem::Common::CE_WITH_MESSAGES);
    CHECK(gep.isEqual(assigned, constructed));
}

namespace {
/** @brief An EA factory that shows the test the two objects it deep-copies */
class GEAFactoryProbe : public oa::GOptimizationAlgorithmFactoryT<oa::GEvolutionaryAlgorithm> {
public:
    using oa::GOptimizationAlgorithmFactoryT<
        oa::GEvolutionaryAlgorithm>::GOptimizationAlgorithmFactoryT;

    /** @return The registered monitor, not owned */
    [[nodiscard]] oa::GBasePluggableOM const *monitor() const {
        return this->pluggable_om_.get();
    }
    /** @return The registered content creator, not owned */
    [[nodiscard]] Gem::Common::GFactoryT<gen::GGenome> const *contentCreator() const {
        return this->content_creator_ptr_.get();
    }
};
} // anonymous namespace

TEST_CASE(
    "GOAFactoryT: an assigned copy does not share the monitor or the content creator",
    "[geneva][copy]"
) {
    GEAFactoryProbe source(std::filesystem::path("./config/source.json"));
    source.registerPluggableOM(std::make_shared<GFitnessMonitor>());
    source.registerContentCreator(
        std::make_shared<gen::GIndividualFactory<gind::GFunctionIndividual>>(
            "./config/creator.json"
        )
    );

    GEAFactoryProbe assigned(std::filesystem::path("./config/assigned.json"));
    assigned = source;

    REQUIRE(assigned.monitor());
    CHECK(assigned.monitor() != source.monitor());
    REQUIRE(assigned.contentCreator());
    CHECK(assigned.contentCreator() != source.contentCreator());
    CHECK(assigned.getConfigFilePath() == source.getConfigFilePath());
}

// ============================================================================
// Meta-optimizer: the search genome is built from the EA tunable manifest and
// read back by name (no MOT_* index math). This pins that the manifest -> genome
// -> readTuned() chain reproduces the manifest's values under their names, and
// that the derived getters (ad_prob etc.) compute correctly.
// ============================================================================

TEST_CASE(
    "Meta-optimizer genome is built from the tunable manifest and read by name",
    "[geneva][meta]"
) {
    using Meta = gind::GMetaOptimizerIndividualT<gind::GFunctionIndividual>;
    namespace n = oa::ea_tunable;

    auto p = std::make_shared<Meta>();
    const auto manifest = oa::GEvolutionaryAlgorithm::tunableManifest();
    Meta::addContent(p, manifest);

    // The defaults come straight from the manifest, addressed by name.
    CHECK(p->getNParents() == 1);
    CHECK(p->getNChildren() == 100);
    CHECK(std::abs(p->getMinSigma() - 0.001) < 1e-9);
    CHECK(std::abs(p->getSigmaRange() - 0.2) < 1e-9);
    CHECK(std::abs(p->getSigmaSigma() - 0.1) < 1e-9);
    // getAdProb = min_ad_prob + ad_prob_start_pct * ad_prob_range = 0 + 1 * 0.9.
    CHECK(std::abs(p->getAdProb() - 0.9) < 1e-9);

    // A reordered / re-valued manifest still reads back correctly by name: build with custom values and
    // confirm the named getters follow the values, not any fixed position.
    auto custom = manifest;
    auto setKnob = [&custom](const char *name, double init, double lo, double hi) {
        for(auto &tp : custom) {
            if(tp.name == name) {
                tp.init = init;
                tp.lower = lo;
                tp.upper = hi;
            }
        }
    };
    setKnob(n::n_parents, 3, 1, 6);
    setKnob(n::n_children, 42, 5, 250);
    setKnob(n::min_sigma, 0.01, 0.001, 0.1);
    setKnob(n::sigma_sigma, 0.5, 0., 1.);

    auto q = std::make_shared<Meta>();
    Meta::addContent(q, custom);
    CHECK(q->getNParents() == 3);
    CHECK(q->getNChildren() == 42);
    CHECK(std::abs(q->getMinSigma() - 0.01) < 1e-9);
    CHECK(std::abs(q->getSigmaSigma() - 0.5) < 1e-9);
}
