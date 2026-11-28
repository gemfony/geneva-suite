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
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <type_traits>
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

#include "common/GJsonIO.hpp"
#include "common/testing/GStandardTests.hpp"

#include <boost/json/value_to.hpp>

using namespace Gem::Geneva;
using Gem::Common::Testing::GEqualityPrinter;

// ============================================================================
// makeTestObject specializations for classes without a public default constructor. They must be
// visible where the standard tests are instantiated below.
// ============================================================================

namespace Gem::Common::Testing {

/** @brief Individuals::GTestIndividual2 -- public constructor (size, PERFOBJECTTYPE). */
template <>
inline std::shared_ptr<Individuals::GTestIndividual2>
makeTestObject<Individuals::GTestIndividual2>() {
    return std::make_shared<Individuals::GTestIndividual2>(
        std::size_t(100),
        Individuals::PERFOBJECTTYPE::PERFGDOUBLEOBJECT
    );
}

/** @brief Individuals::GLineFitIndividual -- public constructor taking a vector of {x,y} sample points. */
template <>
inline std::shared_ptr<Individuals::GLineFitIndividual>
makeTestObject<Individuals::GLineFitIndividual>() {
    std::vector<std::tuple<double, double>> const
        data_points{{0., 0.}, {1., 1.}, {2., 2.}, {3., 3.}};
    return std::make_shared<Individuals::GLineFitIndividual>(data_points);
}

/** @brief Individuals::GNeuralNetworkIndividual -- its default constructor is private; the parameterized one
 *  is used with the default settings. */
template <>
inline std::shared_ptr<Individuals::GNeuralNetworkIndividual>
makeTestObject<Individuals::GNeuralNetworkIndividual>() {
    return std::make_shared<Individuals::GNeuralNetworkIndividual>(
        Individuals::GNN_DEF_MINVAR,
        Individuals::GNN_DEF_MAXVAR,
        Individuals::GNN_DEF_SIGMA,
        Individuals::GNN_DEF_SIGMASIGMA,
        Individuals::GNN_DEF_MINSIGMA,
        Individuals::GNN_DEF_MAXSIGMA,
        Individuals::GNN_DEF_ADPROB,
        Individuals::GNN_DEF_ADAPTADPROB,
        Individuals::GNN_DEF_MINADPROB,
        Individuals::GNN_DEF_MAXADPROB
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
    OA::GAntColonyOptimization,
    OA::GConjugateGradientDescent,
    OA::GEvolutionaryAlgorithm,
    OA::GGeneralizedSimulatedAnnealing,
    OA::GNelderMead,
    OA::GParameterScan,
    OA::GSepCmaEvolutionStrategy,
    OA::GSimulatedAnnealing,
    OA::GStandardPSO2011,
    OA::GSwarmAlgorithm>;

/** @brief A list of types with every type once, in the order of first appearance. */
template <typename result_type, typename... types>
struct unique_types {
    using type = result_type;
};
template <typename... done_types, typename T, typename... rest_types>
struct unique_types<std::tuple<done_types...>, T, rest_types...> {
    using type = typename unique_types<
        std::conditional_t<
            (std::is_same_v<T, done_types> || ...),
            std::tuple<done_types...>,
            std::tuple<done_types..., T>>,
        rest_types...>::type;
};

/** @brief The personality traits the algorithms of a list name, each once (several algorithms share
 *  GPositionPersonalityTraits). */
template <typename list_type>
struct personality_traits_of;
template <typename... algorithm_types>
struct personality_traits_of<std::tuple<algorithm_types...>> {
    using type =
        typename unique_types<std::tuple<>, typename algorithm_types::personality_traits_type...>::
            type;
};

/** @brief All algorithm types under the standard tests. GMetaEvolutionaryAlgorithm is added by hand: it
 *  carries the EA's personality traits, so its mnemonic would be "ea", and it is not registered in the
 *  factory store -- the store comparison below cannot see it. */
using algorithm_types = decltype(std::tuple_cat(
    std::declval<registered_algorithm_types>(),
    std::declval<std::tuple<OA::GMetaEvolutionaryAlgorithm>>()
));

/** @brief The personality traits of all registered algorithms (MetaEA's are the EA's). */
using trait_types = personality_traits_of<registered_algorithm_types>::type;

/** @brief The individuals under the standard tests. */
using individual_types = std::tuple<
    Individuals::GTestIndividual1,
    Individuals::GTestIndividual2,
    Individuals::GTestIndividual3,
    Individuals::GLineFitIndividual,
    Individuals::GFunctionIndividual,
    Individuals::GDelayIndividual,
    Individuals::GExternalEvaluatorIndividual,
    Individuals::GMetaOptimizerIndividualT<Individuals::GFunctionIndividual>,
    Individuals::GNeuralNetworkIndividual>;

/** @brief The concrete GGenomeConstraint subclasses defined in GFunctionIndividual.hpp. Their own
 *  members (the constants) are not changed by modifyForTest(), which they inherit from their root; the
 *  focused round-trip TEST_CASEs below cover those members. */
using constraint_types = std::tuple<
    Individuals::GDoubleSumConstraint,
    Individuals::GSphereConstraint,
    Individuals::GDoubleSumGapConstraint>;

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
    Individuals::GOptOptMonitorT<Individuals::GFunctionIndividual>>;

/** @brief The mnemonics of the registered algorithms, sorted.
 *  @return One mnemonic per algorithm of registered_algorithm_types */
template <typename... algorithm_types>
std::vector<std::string> mnemonicsOf(std::tuple<algorithm_types...> const * /* list */) {
    std::vector<std::string> mnemonics{std::string(algorithm_types::oa_mnemonic)...};
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

TEMPLATE_LIST_TEST_CASE("checkRoundTrips — trait types", "[geneva][standard]", trait_types) {
    Gem::Common::Testing::checkRoundTrips<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "checkRoundTrips — individual types",
    "[geneva][standard]",
    individual_types
) {
    if constexpr(std::same_as<TestType, Individuals::GNeuralNetworkIndividual>) {
        Individuals::Testing::provideNeuralNetworkTrainingData();
    }
    Gem::Common::Testing::checkRoundTrips<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "checkRoundTrips — constraint types",
    "[geneva][standard]",
    constraint_types
) {
    Gem::Common::Testing::checkRoundTrips<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "checkRoundTrips — algorithm types",
    "[geneva][standard]",
    algorithm_types
) {
    Gem::Common::Testing::checkRoundTrips<TestType>();
}

TEMPLATE_LIST_TEST_CASE("checkRoundTrips — monitor types", "[geneva][standard]", monitor_types) {
    Gem::Common::Testing::checkRoundTrips<TestType>();
}

// ============================================================================
// Standard tests — failures expected
// ============================================================================

TEMPLATE_LIST_TEST_CASE(
    "checkMisuse — trait types",
    "[geneva][standard][failures-expected]",
    trait_types
) {
    Gem::Common::Testing::checkMisuse<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "checkMisuse — individual types",
    "[geneva][standard][failures-expected]",
    individual_types
) {
    if constexpr(std::same_as<TestType, Individuals::GNeuralNetworkIndividual>) {
        Individuals::Testing::provideNeuralNetworkTrainingData();
    }
    Gem::Common::Testing::checkMisuse<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "checkMisuse — constraint types",
    "[geneva][standard][failures-expected]",
    constraint_types
) {
    Gem::Common::Testing::checkMisuse<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "checkMisuse — algorithm types",
    "[geneva][standard][failures-expected]",
    algorithm_types
) {
    Gem::Common::Testing::checkMisuse<TestType>();
}

TEMPLATE_LIST_TEST_CASE(
    "checkMisuse — monitor types",
    "[geneva][standard][failures-expected]",
    monitor_types
) {
    Gem::Common::Testing::checkMisuse<TestType>();
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
        Individuals::GDoubleSumConstraint const original(3.5);
        Individuals::GDoubleSumConstraint restored(2.0);

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
        Individuals::GSphereConstraint const original(3.5);
        Individuals::GSphereConstraint restored(2.0);

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
        Individuals::GDoubleSumGapConstraint const original(3.5, 1.25);
        Individuals::GDoubleSumGapConstraint restored(2.0, 0.25);

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
        Individuals::GExternalEvaluatorIndividual original;
        original.setRunId("custom-run-id-xyz");
        Individuals::GExternalEvaluatorIndividual restored;
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

// Safety net for the OA::GOptimizationAlgorithmBase serialize() refactor: OA::GOptimizationAlgorithmBase is the central
// serialised class for all optimization algorithms (checkpoints + network
// transport). Its split save()/load() was collapsed into a single serialize()
// after std::filesystem::path gained a free serialization. This test sets several
// OA::GOptimizationAlgorithmBase members -- in particular cp_directory_path_ (the path that forced the
// former split) -- to NON-DEFAULT values and verifies they survive a round-trip in
// both modes, guarding against silently dropping a member.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent serialization sweep (BINARY/JSON) for OA::GOptimizationAlgorithmBase's members
TEST_CASE(
    "OA::GOptimizationAlgorithmBase (via GEvolutionaryAlgorithm) round-trips its members incl. the "
    "checkpoint path",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    for(auto mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        OA::GEvolutionaryAlgorithm original;
        // Set several OA::GOptimizationAlgorithmBase members to non-default values via public setters.
        // "." is an always-existing directory, so setCheckpointBaseName creates nothing.
        original.setCheckpointBaseName(".", "custom_checkpoint_base");
        original.setCheckpointInterval(7);
        original.setMaxIteration(4242);
        original.setReportIteration(13);
        original.setMaxStallIteration(99);

        OA::GEvolutionaryAlgorithm restored;
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
            "OA::GOptimizationAlgorithmBase-roundtrip",
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
        Individuals::GTestIndividual1 ind;
        ind.setAssignedIteration(7);
        ind.setMaxMode(maxMode::MAXIMIZE);
        return ind;
    };

    // --- in-memory load() (clone path) ---
    {
        Individuals::GTestIndividual1 const original = make_original();
        Individuals::GTestIndividual1 restored; // defaults

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
        Individuals::GTestIndividual1 const original = make_original();
        Individuals::GTestIndividual1 restored; // defaults

        REQUIRE_NOTHROW(restored.fromString(original.toString(mode), mode));

        CHECK(restored.getAssignedIteration() == 7);
        CHECK(restored.getMaxMode() == maxMode::MAXIMIZE);

        GEqualityPrinter const gep("GGenome-roundtrip", pow(10, -7), Gem::Common::CE_WITH_MESSAGES);
        CHECK(gep.isSimilar(restored, original));
    }
}

// Safety net for the OA::GSwarmAlgorithm serialize()/load_()/compare_() unification
// onto a single localMembers() declaration (the unconditional members) plus a manual
// tail for the conditionally-reconstructed neighborhood/global-best members. This test
// sets several of the about-to-be-tied members to NON-DEFAULT values and verifies they
// survive a wire round-trip AND an in-memory load() (clone path), guarding against the
// default-value masking that hides a dropped member.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent serialization sweep (load() clone path + BINARY/JSON wire round-trip) for OA::GSwarmAlgorithm's tied members
TEST_CASE(
    "OA::GSwarmAlgorithm round-trips its members in BINARY/JSON and via load()",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    auto makeOriginal = []() {
        OA::GSwarmAlgorithm a;
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

    auto checkGetters = [](const OA::GSwarmAlgorithm &restored) {
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
        OA::GSwarmAlgorithm const original = makeOriginal();
        OA::GSwarmAlgorithm restored;
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
        OA::GSwarmAlgorithm const original = makeOriginal();
        OA::GSwarmAlgorithm restored;
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

// Safety net for the OA::GParameterScan serialize()/load_()/compare_() unification
// onto a single localMembers() declaration (the plain members) plus a manual tail
// (the scan-parameter vectors, whose element type lacks the Gemfony common interface,
// and the load-only transient cycle_logic_halt_). This test sets several of the
// about-to-be-tied plain members to NON-DEFAULT values and verifies they survive a
// wire round-trip AND an in-memory load() (clone path).
TEST_CASE(
    "OA::GParameterScan round-trips its members in BINARY/JSON and via load()",
    "[geneva][serialization]"
) {
    using Gem::Common::serializationMode;

    auto makeOriginal = []() {
        OA::GParameterScan a;
        // Non-default values for the localMembers()-tied plain members.
        a.setScanRandomly(false); // default is true
        a.setNSimpleScans(7);     // sets simple_scan_items_ (default 0)
        return a;
    };

    auto checkGetters = [](const OA::GParameterScan &restored) {
        CHECK(restored.getScanRandomly() == false);
        CHECK(restored.getNSimpleScans() == 7);
    };

    // --- in-memory load() (clone path) ---
    {
        OA::GParameterScan const original = makeOriginal();
        OA::GParameterScan restored;
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
        OA::GParameterScan const original = makeOriginal();
        OA::GParameterScan restored;
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
    "OA::GParameterScan round-trips its concrete scan parameters (all four leaf types)",
    "[geneva][serialization][ps]"
) {
    using Gem::Common::serializationMode;

    auto makeOriginal = []() {
        OA::GParameterScan a;
        a.setScanRandomly(
            false
        ); // GRID scan -> each leaf pre-computes a data grid that must survive too
        // One parameter of every supported leaf type: double, float, int32 and bool.
        a.setParameterSpecs("d(0, -5., 5., 3), f(1, -5., 5., 3), i(2, -5, 5, 3), b(3)");
        return a;
    };

    // --- in-memory load() (clone path: the mixin-generated clone_/load_ over each leaf) ---
    {
        OA::GParameterScan const original = makeOriginal();
        OA::GParameterScan restored;
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
        OA::GParameterScan const original = makeOriginal();
        OA::GParameterScan restored;
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
    auto best = std::make_shared<Individuals::GTestIndividual1>();
    best->process(); // registerPersonalBest() wants a processed individual

    OA::GSwarmAlgorithm_PersonalityTraits source;
    source.registerPersonalBest(best);

    OA::GSwarmAlgorithm_PersonalityTraits assigned;
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
class GEAFactoryProbe : public OA::GOptimizationAlgorithmFactoryT<OA::GEvolutionaryAlgorithm> {
public:
    using OA::GOptimizationAlgorithmFactoryT<
        OA::GEvolutionaryAlgorithm>::GOptimizationAlgorithmFactoryT;

    /** @return The registered monitor, not owned */
    [[nodiscard]] OA::GBasePluggableOM const *monitor() const {
        return this->pluggable_om_.get();
    }
    /** @return The registered content creator, not owned */
    [[nodiscard]] Gem::Common::GFactoryT<Genome::GGenome> const *contentCreator() const {
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
        std::make_shared<Genome::GIndividualFactory<Individuals::GFunctionIndividual>>(
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
    using Meta = Individuals::GMetaOptimizerIndividualT<Individuals::GFunctionIndividual>;
    namespace n = OA::ea_tunable;

    auto p = std::make_shared<Meta>();
    const auto manifest = OA::GEvolutionaryAlgorithm::tunableManifest();
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

// ============================================================================
// The meta-optimizer factory's configuration file sets the start value and the search range of
// the knobs under the keys that existing configuration files use (CODING-STYLE 4.4: keys are
// frozen). Without a file the factory writes one with these keys and the manifest's defaults.
// ============================================================================

namespace {
/** @brief The meta genome a factory produced: values and bounds per channel, in manifest order */
struct MetaSearchSpace {
    std::vector<std::int32_t> int_values, int_lower, int_upper;
    std::vector<double> values, lower, upper;
};

/** @brief Produces one meta individual from the configuration file @p config_file */
MetaSearchSpace producedSearchSpace(std::filesystem::path const &config_file) {
    auto sub_factory = std::make_shared<Individuals::GFunctionIndividualFactory>(
        config_file.parent_path() / "GFunctionIndividual.json"
    );
    Individuals::GMetaOptimizerIndividualFactoryT<Individuals::GFunctionIndividual> factory(
        config_file
    );
    factory.registerIndividualFactory(sub_factory);
    auto p =
        factory.get_as<Individuals::GMetaOptimizerIndividualT<Individuals::GFunctionIndividual>>();

    MetaSearchSpace s;
    s.int_values = p->parameterValues<std::int32_t>();
    p->boundaries<std::int32_t>(s.int_lower, s.int_upper);
    s.values = p->parameterValues<double>();
    p->boundaries<double>(s.lower, s.upper);
    return s;
}

/** @brief Whether two vectors of genome values agree (the genome may round a value it stores) */
bool sameValues(std::vector<double> const &a, std::vector<double> const &b) {
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](double x, double y) { return std::abs(x - y) < 1e-9; });
}

/** @brief A fresh, empty directory for one test's configuration files */
std::filesystem::path freshConfigDirectory(std::string const &name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}
} // namespace

TEST_CASE(
    "Meta-optimizer factory: without a configuration file the knobs keep the manifest's defaults",
    "[geneva][meta]"
) {
    const auto dir = freshConfigDirectory("geneva-meta-config-defaults");
    const auto config_file = dir / "GMetaOptimizerIndividual.json";
    const MetaSearchSpace s = producedSearchSpace(config_file);

    MetaSearchSpace expected;
    for(const auto &tp : OA::GEvolutionaryAlgorithm::tunableManifest()) {
        if(tp.is_integer) {
            expected.int_values.push_back(static_cast<std::int32_t>(tp.init));
            expected.int_lower.push_back(static_cast<std::int32_t>(tp.lower));
            expected.int_upper.push_back(static_cast<std::int32_t>(tp.upper));
        }
        else {
            expected.values.push_back(tp.init);
            expected.lower.push_back(tp.lower);
            expected.upper.push_back(tp.upper);
        }
    }
    CHECK(s.int_values == expected.int_values);
    CHECK(s.int_lower == expected.int_lower);
    CHECK(s.int_upper == expected.int_upper);
    CHECK(sameValues(s.values, expected.values));
    CHECK(sameValues(s.lower, expected.lower));
    CHECK(sameValues(s.upper, expected.upper));

    // The file the factory wrote carries today's keys, each with its default
    const auto written = Gem::Common::parseJsonFile(config_file).as_object();
    const std::vector<std::pair<std::string, double>> keys{
        {"init_n_parents", 1.},         {"n_parents_lb", 1.},
        {"n_parents_ub", 6.},           {"init_n_children", 100.},
        {"n_children_lb", 5.},          {"n_children_ub", 250.},
        {"init_amalgamation_lklh", 0.}, {"amalgamation_lklh_lb", 0.},
        {"amalgamation_lklh_ub", 1.},   {"init_min_ad_prob", 0.},
        {"min_ad_prob_lb", 0.},         {"min_ad_prob_ub", 0.1},
        {"init_ad_prob_range", 0.9},    {"ad_prob_range_lb", 0.1},
        {"ad_prob_range_ub", 0.9},      {"init_ad_prob_start_percentage", 1.},
        {"init_adapt_ad_prob", 0.1},    {"adapt_ad_prob_lb", 0.},
        {"adapt_ad_prob_ub", 1.},       {"init_min_sigma", 0.001},
        {"min_sigma_lb", 0.001},        {"min_sigma_ub", 0.09999},
        {"init_sigma_range", 0.2},      {"sigma_range_lb", 0.1},
        {"sigma_range_ub", 0.9},        {"init_sigma_range_percentage", 1.},
        {"init_sigma_sigma", 0.1},      {"sigma_sigma_lb", 0.},
        {"sigma_sigma_ub", 1.}
    };
    for(const auto &[key, default_value] : keys) {
        INFO(key);
        REQUIRE(written.contains(key));
        CHECK(boost::json::value_to<double>(written.at(key).at("default")) == default_value);
    }
    // The two start positions within a band have no range keys: their range is [0, 1]
    for(const char *absent :
        {"ad_prob_start_percentage_lb",
         "ad_prob_start_percentage_ub",
         "sigma_range_percentage_lb",
         "sigma_range_percentage_ub"}) {
        INFO(absent);
        CHECK(not written.contains(absent));
    }
}

TEST_CASE(
    "Meta-optimizer factory: a configuration file with today's keys sets every knob",
    "[geneva][meta]"
) {
    const auto dir = freshConfigDirectory("geneva-meta-config-keys");
    const auto config_file = dir / "GMetaOptimizerIndividual.json";
    {
        std::ofstream out(config_file);
        out << R"({
            "init_n_parents": { "value": 2 },
            "n_parents_lb": { "value": 1 },
            "n_parents_ub": { "value": 4 },
            "init_n_children": { "value": 20 },
            "n_children_lb": { "value": 10 },
            "n_children_ub": { "value": 40 },
            "init_amalgamation_lklh": { "value": 0.2 },
            "amalgamation_lklh_lb": { "value": 0.1 },
            "amalgamation_lklh_ub": { "value": 0.6 },
            "init_min_ad_prob": { "value": 0.01 },
            "min_ad_prob_lb": { "value": 0.005 },
            "min_ad_prob_ub": { "value": 0.05 },
            "init_ad_prob_range": { "value": 0.5 },
            "ad_prob_range_lb": { "value": 0.2 },
            "ad_prob_range_ub": { "value": 0.8 },
            "init_ad_prob_start_percentage": { "value": 0.5 },
            "init_adapt_ad_prob": { "value": 0.2 },
            "adapt_ad_prob_lb": { "value": 0.05 },
            "adapt_ad_prob_ub": { "value": 0.7 },
            "init_min_sigma": { "value": 0.01 },
            "min_sigma_lb": { "value": 0.002 },
            "min_sigma_ub": { "value": 0.05 },
            "init_sigma_range": { "value": 0.3 },
            "sigma_range_lb": { "value": 0.15 },
            "sigma_range_ub": { "value": 0.6 },
            "init_sigma_range_percentage": { "value": 0.25 },
            "init_sigma_sigma": { "value": 0.3 },
            "sigma_sigma_lb": { "value": 0.1 },
            "sigma_sigma_ub": { "value": 0.9 }
        })";
    }
    const MetaSearchSpace s = producedSearchSpace(config_file);

    // n_parents, n_children
    CHECK(s.int_values == std::vector<std::int32_t>{2, 20});
    CHECK(s.int_lower == std::vector<std::int32_t>{1, 10});
    CHECK(s.int_upper == std::vector<std::int32_t>{4, 40});
    // amalgamation, min_ad_prob, ad_prob_range, ad_prob_start_pct, adapt_ad_prob, min_sigma,
    // sigma_range, sigma_range_pct, sigma_sigma
    CHECK(sameValues(s.values, {0.2, 0.01, 0.5, 0.5, 0.2, 0.01, 0.3, 0.25, 0.3}));
    CHECK(sameValues(s.lower, {0.1, 0.005, 0.2, 0., 0.05, 0.002, 0.15, 0., 0.1}));
    CHECK(sameValues(s.upper, {0.6, 0.05, 0.8, 1., 0.7, 0.05, 0.6, 1., 0.9}));
}

// ============================================================================
// The options of a tunable manifest come from its knobs alone: every knob of a test manifest,
// including one added to it, is registered with its default and range and is set from a file.
// ============================================================================

TEST_CASE(
    "Meta-optimizer: every knob of a tunable manifest is an option with its default and range",
    "[geneva][meta]"
) {
    const auto dir = freshConfigDirectory("geneva-meta-tunable-options");
    std::vector<OA::TunableParam> manifest{
        {"count", true, 3., 1., 9., "number of things", "", false},
        {"rate", false, 0.5, 0.1, 0.9, "rate of things", "speed", false},
        {"share", false, 0.25, 0., 1., "share of things", "", true}
    };
    // A knob added to the manifest needs nothing else
    manifest.push_back({"weight", false, 2., 1., 4., "weight of things", "", false});

    // The defaults: the file written from the registered options
    {
        auto knobs = manifest;
        Gem::Common::GParserBuilder gpb;
        Individuals::registerTunableOptions(gpb, manifest, knobs);
        CHECK(gpb.numberOfFileOptions() == 3 + 3 + 1 + 3);
        gpb.writeConfigFile(dir / "defaults.json");
    }
    const auto written = Gem::Common::parseJsonFile(dir / "defaults.json").as_object();
    const std::vector<std::pair<std::string, double>> defaults{
        {"init_count", 3.},
        {"count_lb", 1.},
        {"count_ub", 9.},
        {"init_speed", 0.5},
        {"speed_lb", 0.1},
        {"speed_ub", 0.9},
        {"init_share", 0.25},
        {"init_weight", 2.},
        {"weight_lb", 1.},
        {"weight_ub", 4.}
    };
    for(const auto &[key, default_value] : defaults) {
        INFO(key);
        REQUIRE(written.contains(key));
        CHECK(boost::json::value_to<double>(written.at(key).at("default")) == default_value);
    }
    CHECK(
        written.at("init_count").at("default").is_int64()
    ); // an integer knob is read as an integer
    CHECK(not written.contains("share_lb"));
    CHECK(not written.contains("share_ub"));

    // The values: a file sets every knob; the fixed range stays as it is
    {
        std::ofstream out(dir / "values.json");
        out << R"({
            "init_count": { "value": 4 }, "count_lb": { "value": 2 }, "count_ub": { "value": 7 },
            "init_speed": { "value": 0.3 }, "speed_lb": { "value": 0.2 }, "speed_ub": { "value": 0.4 },
            "init_share": { "value": 0.75 },
            "init_weight": { "value": 3 }, "weight_lb": { "value": 2.5 }, "weight_ub": { "value": 3.5 }
        })";
    }
    std::vector<OA::TunableParam> knobs = manifest;
    {
        Gem::Common::GParserBuilder gpb;
        Individuals::registerTunableOptions(gpb, manifest, knobs);
        gpb.parseConfigFile(dir / "values.json");
    }
    const std::vector<std::array<double, 3>>
        expected{{4., 2., 7.}, {0.3, 0.2, 0.4}, {0.75, 0., 1.}, {3., 2.5, 3.5}};
    REQUIRE(knobs.size() == expected.size());
    for(std::size_t i = 0; i < knobs.size(); ++i) {
        INFO(knobs[i].name);
        CHECK(std::abs(knobs[i].init - expected[i][0]) < 1e-12);
        CHECK(std::abs(knobs[i].lower - expected[i][1]) < 1e-12);
        CHECK(std::abs(knobs[i].upper - expected[i][2]) < 1e-12);
    }
}

// ============================================================================
// The names of geneva's enums (enum_names_v): every name is pinned, as reports and benchmark
// configurations quote them, and each name leads back to its value.
// ============================================================================

TEST_CASE("geneva's enums: every value has its name", "[geneva][names]") {
    using Gem::Common::Testing::checkEnumNames;
    checkEnumNames<evaluationPolicy>(
        {{evaluationPolicy::USESIMPLEEVALUATION, "USESIMPLEEVALUATION"},
         {evaluationPolicy::USEWORSTCASEFORINVALID, "USEWORSTCASEFORINVALID"},
         {evaluationPolicy::USESIGMOID, "USESIGMOID"}}
    );
    using Individuals::metaOptimizationTarget;
    checkEnumNames<metaOptimizationTarget>(
        {{metaOptimizationTarget::BESTFITNESS, "best fitness"},
         {metaOptimizationTarget::MINSOLVERCALLS, "minimum number of solver calls"},
         {metaOptimizationTarget::MC_MINSOLVER_BESTFITNESS,
          "multi-criterion target with best fitness, minimum number of solver calls"}}
    );
    using Individuals::solverFunction;
    checkEnumNames<solverFunction>(
        {{solverFunction::PARABOLA, "PARABOLA"},
         {solverFunction::NOISYPARABOLA, "NOISYPARABOLA"},
         {solverFunction::ROSENBROCK, "ROSENBROCK"},
         {solverFunction::ACKLEY, "ACKLEY"},
         {solverFunction::RASTRIGIN, "RASTRIGIN"},
         {solverFunction::SCHWEFEL, "SCHWEFEL"},
         {solverFunction::SALOMON, "SALOMON"},
         {solverFunction::NEGPARABOLA, "NEGPARABOLA"},
         {solverFunction::ACKLEY_CANONICAL, "ACKLEY_CANONICAL"},
         {solverFunction::GRIEWANK, "GRIEWANK"},
         {solverFunction::LEVY, "LEVY"},
         {solverFunction::STYBLINSKI_TANG, "STYBLINSKI_TANG"},
         {solverFunction::ELLIPSOID, "ELLIPSOID"},
         {solverFunction::MICHALEWICZ, "MICHALEWICZ"},
         {solverFunction::ZAKHAROV, "ZAKHAROV"}}
    );

    // The meta-optimizer's report and an individual's JSON carry the names
    using Meta = Individuals::GMetaOptimizerIndividualT<Individuals::GFunctionIndividual>;
    auto p = std::make_shared<Meta>();
    Meta::addContent(p, OA::GEvolutionaryAlgorithm::tunableManifest());
    CHECK(p->print(false).contains("Optimization target: \"best fitness\""));
    CHECK(p->toJSON().at("transformationPolicy").as_string() == "USESIMPLEEVALUATION");
}
