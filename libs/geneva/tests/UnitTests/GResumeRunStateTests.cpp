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
 * A run resumed from a checkpoint goes on with the run state of its algorithm: the state is part of the
 * checkpoint, and the setup of the resumed run keeps it. A new run builds it afresh (initRunState_()).
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include <boost/json.hpp>

#include "common/GCommonEnums.hpp"
#include "common/testing/GStandardTests.hpp"
#include "geneva/GFaultInjector.hpp"
#include "geneva/individuals/GLineFitIndividual.hpp"
#include "geneva/oa/GAntColonyOptimization.hpp"
#include "geneva/oa/GConjugateGradientDescent.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GGeneralizedSimulatedAnnealing.hpp"
#include "geneva/oa/GNelderMead.hpp"
#include "geneva/oa/GParameterScan.hpp"
#include "geneva/oa/GStandardPSO2011.hpp"
#include "geneva/oa/GSwarmAlgorithm.hpp"

namespace fs = std::filesystem;
namespace gind = Gem::Geneva::Individuals;
namespace oa = Gem::Geneva::OptimizationAlgorithms;

namespace {

/******************************************************************************/
/** @brief Thrown by GSetupProbe when the setup of a run is complete. */
struct setup_done {};

/**
 * @brief An algorithm whose run ends right after its setup (population, personalities, init()). Its
 * first iteration keeps a copy of the algorithm as it stands then.
 *
 * @tparam oa_type The algorithm under test
 */
template <typename oa_type>
class GSetupProbe final : public oa_type {
public:
    std::optional<oa_type> afterSetup; ///< The algorithm as its setup left it

private:
    /** @brief Keeps the algorithm and ends the run. @return Never returns */
    std::tuple<double, double> cycleLogic_() override {
        afterSetup.emplace(static_cast<oa_type const &>(*this));
        throw setup_done{};
    }
};

/**
 * @brief Runs a probe up to the end of its setup.
 * @param probe The probe
 * @param offset The iteration to start with
 * @return The algorithm as its setup left it
 */
template <typename oa_type>
oa_type setupOf(GSetupProbe<oa_type> &probe, std::uint32_t offset) {
    probe.setReportIteration(0);
    REQUIRE_THROWS_AS(probe.optimize(offset), setup_done);
    REQUIRE(probe.afterSetup);
    return *probe.afterSetup;
}

/** @brief The first member named key in a JSON document (depth first), or nullptr */
boost::json::value const *findMember(boost::json::value const &doc, std::string const &key) {
    if(doc.is_object()) {
        for(auto const &[name, value] : doc.as_object()) {
            if(name == key) {
                return &value;
            }
            if(auto const *found = findMember(value, key)) {
                return found;
            }
        }
    }
    else if(doc.is_array()) {
        for(auto const &value : doc.as_array()) {
            if(auto const *found = findMember(value, key)) {
                return found;
            }
        }
    }
    return nullptr;
}

/**
 * @brief A member of an algorithm as the checkpoint codec (JSON) writes it.
 * @param alg The algorithm
 * @param key The member's name in the member list
 * @return The member's JSON text
 */
template <typename oa_type>
std::string member(oa_type const &alg, std::string const &key) {
    auto const doc = boost::json::parse(alg.toString(Gem::Common::serializationMode::GEM_JSON));
    auto const *found = findMember(doc, key);
    REQUIRE(found != nullptr);
    return boost::json::serialize(*found);
}

/** @brief Appends every number of a JSON value (the codec writes a double as a string) */
void collectNumbers(boost::json::value const &v, std::vector<double> &numbers) {
    if(v.is_array()) {
        for(auto const &e : v.as_array()) {
            collectNumbers(e, numbers);
        }
    }
    else if(v.is_object()) {
        for(auto const &[name, e] : v.as_object()) {
            collectNumbers(e, numbers);
        }
    }
    else if(v.is_string()) {
        numbers.push_back(std::stod(std::string(v.as_string())));
    }
    else if(v.is_number()) {
        numbers.push_back(v.to_number<double>());
    }
}

/**
 * @brief The numbers a member of an algorithm holds, in the order the codec writes them.
 * @param alg The algorithm
 * @param key The member's name in the member list
 * @return The numbers
 */
template <typename oa_type>
std::vector<double> numbers(oa_type const &alg, std::string const &key) {
    std::vector<double> result;
    collectNumbers(boost::json::parse(member(alg, key)), result);
    return result;
}

/** @brief Appends the numbers of every member named key inside a JSON value */
void collectNamed(
    boost::json::value const &v,
    std::string const &key,
    std::vector<double> &numbers
) {
    if(v.is_object()) {
        for(auto const &[name, e] : v.as_object()) {
            if(name == key) {
                collectNumbers(e, numbers);
            }
            else {
                collectNamed(e, key, numbers);
            }
        }
    }
    else if(v.is_array()) {
        for(auto const &e : v.as_array()) {
            collectNamed(e, key, numbers);
        }
    }
}

/**
 * @brief The numbers of the members named key inside a member of an algorithm, in the order the codec
 * writes them.
 * @param alg The algorithm
 * @param container The member's name in the member list
 * @param key The name of the inner members
 * @return The numbers
 */
template <typename oa_type>
std::vector<double>
numbersNamed(oa_type const &alg, std::string const &container, std::string const &key) {
    std::vector<double> result;
    collectNamed(boost::json::parse(member(alg, container)), key, result);
    return result;
}

/** @brief The individual of the tests: a line fit, two floating point parameters */
gind::GLineFitIndividual lineFit() {
    return gind::GLineFitIndividual({{0., 0.}, {1., 1.}, {2., 2.}});
}

/**
 * @brief Runs an algorithm a few iterations on the line fit and writes its final checkpoint.
 * @param alg The algorithm, configured by the caller
 * @return The final checkpoint, in a directory of its own
 */
template <typename oa_type>
fs::path runToCheckpoint(oa_type &alg) {
    fs::path const cp_dir = fs::temp_directory_path() /
                            ("geneva-resume-test-" + std::to_string(std::random_device{}()));
    alg.push_back(lineFit().clone());
    alg.setMaxIteration(4);
    alg.setMaxStallIteration(0);
    alg.setReportIteration(0);
    alg.setCheckpointInterval(1000); // the first iteration and the final one
    alg.setCheckpointBaseName(cp_dir.string(), "resume.cp");
    alg.optimize();

    for(auto const &entry : fs::directory_iterator(cp_dir)) {
        if(entry.path().filename().string().find("-final-") != std::string::npos) {
            return entry.path();
        }
    }
    FAIL("no final checkpoint was written");
    return {};
}

/**
 * @brief Resumes a probe from a checkpoint up to the end of its setup.
 * @param cp The checkpoint
 * @param configure Gives the probe the job settings a run needs (an adaption config)
 * @return The algorithm as the setup of the resumed run left it
 */
template <typename oa_type>
oa_type resumedSetupOf(
    fs::path const &cp,
    std::type_identity_t<std::function<void(oa_type &)>> const &configure = [](oa_type &) {}
) {
    GSetupProbe<oa_type> probe;
    probe.loadCheckpoint(cp);
    configure(probe);
    return setupOf(probe, probe.getIteration() + 1);
}

/**
 * @brief Checks that an algorithm, its run state included, survives both codecs unchanged.
 * @param alg The algorithm
 */
template <typename oa_type>
void checkRoundTrips(oa_type const &alg) {
    using Gem::Common::serializationMode;
    for(auto const mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        oa_type restored;
        REQUIRE_NOTHROW(restored.fromString(alg.toString(mode), mode));
        Gem::Common::Testing::GEqualityPrinter const gep(
            "run-state-roundtrip",
            1e-7,
            Gem::Common::CE_WITH_MESSAGES
        );
        CHECK(gep.isSimilar(restored, alg));
    }
}

/**
 * @brief Starts a new run on a probe that holds the state of an earlier run, up to the end of its
 * setup.
 * @param earlier The algorithm of the earlier run
 * @param configure Gives the probe the job settings a run needs (an adaption config)
 * @return The algorithm as the setup of the new run left it
 */
template <typename oa_type>
oa_type newSetupOf(
    oa_type const &earlier,
    std::type_identity_t<std::function<void(oa_type &)>> const &configure = [](oa_type &) {}
) {
    GSetupProbe<oa_type> probe;
    probe.load(earlier);
    configure(probe);
    return setupOf(probe, 0);
}

/** @brief Counts the evaluations of the process; it injects no fault */
class GEvaluationCounter final : public Gem::Geneva::GFaultInjector {
public:
    /** @brief Counts one evaluation @return No fault */
    Fault
    evaluate(Gem::Geneva::Genome::GGenome const & /*item*/, Gem::Hap::GRandomBase & /*gr*/)
        override {
        count_.fetch_add(1);
        return Fault::NONE;
    }

    /** @brief @return The evaluations counted so far */
    [[nodiscard]] std::size_t count() const {
        return count_.load();
    }

private:
    std::atomic<std::size_t> count_{0}; ///< The evaluations counted so far
};

/** @brief Registers an evaluation counter for its lifetime */
class GCountingScope {
public:
    GCountingScope() {
        Gem::Geneva::GFaultInjectorRegistry::set(std::make_unique<GEvaluationCounter>());
    }
    ~GCountingScope() {
        Gem::Geneva::GFaultInjectorRegistry::clear();
    }
    GCountingScope(GCountingScope const &) = delete;
    GCountingScope &operator=(GCountingScope const &) = delete;

    /** @brief @return The evaluations counted so far */
    [[nodiscard]] static std::size_t count() {
        auto const *counter =
            dynamic_cast<GEvaluationCounter const *>(Gem::Geneva::GFaultInjectorRegistry::get());
        REQUIRE(counter != nullptr);
        return counter->count();
    }
};

/**
 * @brief Resumes an algorithm from a checkpoint for one iteration.
 * @param cp The checkpoint
 * @param configure Gives the algorithm the job settings a run needs (an adaption config)
 * @param evaluations Receives the number of evaluations of that iteration
 * @return The algorithm after the iteration
 */
template <typename oa_type>
oa_type oneMoreIteration(
    fs::path const &cp,
    std::type_identity_t<std::function<void(oa_type &)>> const &configure,
    std::size_t &evaluations
) {
    oa_type alg;
    alg.loadCheckpoint(cp);
    configure(alg);
    alg.setCheckpointInterval(0);
    alg.setReportIteration(0);
    alg.setMaxIteration(alg.getIteration() + 2); // one iteration
    GCountingScope const counting;
    alg.optimize(alg.getIteration() + 1);
    evaluations = GCountingScope::count();
    return alg;
}

/**
 * @brief The floating point parameters of every individual of an algorithm.
 * @param alg The algorithm
 * @return One vector per individual
 */
template <typename oa_type>
std::vector<std::vector<double>> positions(oa_type const &alg) {
    std::vector<std::vector<double>> result;
    for(auto const &ind : alg) {
        result.push_back(ind->internalFPValues());
    }
    return result;
}

/// A best fitness before any evaluation
constexpr double UNSET = std::numeric_limits<double>::max();

/**
 * @brief An algorithm that counts how often the setup builds its run state. Like an algorithm a user
 * adds, it does not ask whether its run was resumed.
 *
 * @tparam oa_type The algorithm it extends
 */
template <typename oa_type>
class GRunStateCounter final : public oa_type {
public:
    std::size_t fresh_starts = 0; ///< How often the setup built the run state

protected:
    /** @brief Counts and builds the run state of the algorithm it extends */
    void initRunState_() override {
        ++fresh_starts;
        oa_type::initRunState_();
    }
};

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("A resumed Standard PSO 2011 goes on with its velocities and bests", "[oa][resume]") {
    oa::GStandardPSO2011 run;
    run.setSwarmSize(5);
    auto const cp = runToCheckpoint(run);

    auto const resumed = resumedSetupOf<oa::GStandardPSO2011>(cp);
    for(auto const *key :
        {"velocities_",
         "personal_bests_",
         "personal_best_fitness_",
         "global_best_",
         "global_best_fitness_"}) {
        INFO(key);
        CHECK(member(resumed, key) == member(run, key));
    }
    // The state survives both codecs (the resumed algorithm, whose best-of-iteration record -- compared,
    // but not serialized -- is still empty)
    checkRoundTrips(resumed);

    // A swarm of another size cannot go on with this state
    oa::GStandardPSO2011 other_size;
    other_size.loadCheckpoint(cp);
    fs::remove_all(cp.parent_path());
    other_size.setSwarmSize(6);
    CHECK_THROWS_WITH(
        other_size.optimize(other_size.getIteration() + 1),
        Catch::Matchers::ContainsSubstring("does not fit the population")
    );

    // A new run starts from unevaluated bests
    auto const started = newSetupOf(run);
    CHECK(numbers(started, "personal_best_fitness_") == std::vector<double>(5, UNSET));
    CHECK(numbers(started, "global_best_fitness_") == std::vector<double>{UNSET});
    CHECK(member(started, "velocities_") != member(run, "velocities_"));
}

/******************************************************************************/

TEST_CASE(
    "A resumed generalized simulated annealing goes on with its chain clocks",
    "[oa][resume]"
) {
    oa::GGeneralizedSimulatedAnnealing run;
    run.setNChains(3);
    auto const cp = runToCheckpoint(run);

    auto const resumed = resumedSetupOf<oa::GGeneralizedSimulatedAnnealing>(cp);
    for(auto const *key : {"chain_step_", "chain_best_energy_", "chain_stall_"}) {
        INFO(key);
        CHECK(member(resumed, key) == member(run, key));
    }
    checkRoundTrips(resumed);

    // Another number of chains cannot go on with these clocks
    oa::GGeneralizedSimulatedAnnealing other_size;
    other_size.loadCheckpoint(cp);
    fs::remove_all(cp.parent_path());
    other_size.setNChains(4);
    CHECK_THROWS_WITH(
        other_size.optimize(other_size.getIteration() + 1),
        Catch::Matchers::ContainsSubstring("does not fit the population")
    );

    // A new run starts every clock at the first step
    auto const started = newSetupOf(run);
    CHECK(numbers(started, "chain_step_") == std::vector<double>(3, 1.));
    CHECK(numbers(started, "chain_best_energy_") == std::vector<double>(3, UNSET));
    CHECK(numbers(started, "chain_stall_") == std::vector<double>(3, 0.));
}

/******************************************************************************/

TEST_CASE("A resumed ant colony optimization goes on with its archive", "[oa][resume]") {
    oa::GAntColonyOptimization run;
    run.setArchiveSize(5);
    run.setNAnts(2);
    auto const cp = runToCheckpoint(run);
    auto const saved = numbersNamed(run, "archive_", "fitness");
    REQUIRE(saved.size() == 5);

    auto const resumed = resumedSetupOf<oa::GAntColonyOptimization>(cp);
    CHECK(member(resumed, "archive_") == member(run, "archive_"));
    checkRoundTrips(resumed);

    // The first iteration of the resumed run samples two ants from the archive and merges them, so
    // no rank of the archive gets worse (re-seeding it from the population would lose its best)
    std::size_t evaluations = 0;
    auto const one_more =
        oneMoreIteration<oa::GAntColonyOptimization>(cp, [](auto &) {}, evaluations);
    auto const merged = numbersNamed(one_more, "archive_", "fitness");
    REQUIRE(merged.size() == saved.size());
    for(std::size_t rank = 0; rank < saved.size(); ++rank) {
        INFO("rank " << rank);
        CHECK(merged[rank] <= saved[rank]);
    }

    // Another archive size cannot go on with this archive
    oa::GAntColonyOptimization other_size;
    other_size.loadCheckpoint(cp);
    fs::remove_all(cp.parent_path());
    other_size.setArchiveSize(6);
    CHECK_THROWS_WITH(
        other_size.optimize(other_size.getIteration() + 1),
        Catch::Matchers::ContainsSubstring("does not fit the population")
    );

    // A new run starts with an empty archive, which its first iteration fills from the seeds
    auto const started = newSetupOf(run);
    CHECK(numbersNamed(started, "archive_", "fitness").empty());
}

/******************************************************************************/

TEST_CASE(
    "A resumed evolutionary algorithm goes on with its global step size and evolution path",
    "[oa][resume]"
) {
    auto const adaption_config = lineFit().getAdaptionConfig();
    auto const configure = [&adaption_config](oa::GEvolutionaryAlgorithm &alg) {
        alg.setAdaptionConfig(adaption_config);
    };
    oa::GEvolutionaryAlgorithm run;
    run.setPopulationSizes(8, 2);
    run.setStepControl(oa::stepControl::CSA);
    configure(run);
    auto const cp = runToCheckpoint(run);
    REQUIRE(numbers(run, "p_sigma_") != std::vector<double>{0.}); // the controller has moved

    auto const resumed = resumedSetupOf<oa::GEvolutionaryAlgorithm>(cp, configure);
    fs::remove_all(cp.parent_path());
    CHECK(resumed.getGlobalSigma() == run.getGlobalSigma());
    CHECK(member(resumed, "p_sigma_") == member(run, "p_sigma_"));
    checkRoundTrips(resumed);

    // A new run starts from the seed sigma of the adaption config, as a new algorithm does
    oa::GEvolutionaryAlgorithm brand_new;
    brand_new.setPopulationSizes(8, 2);
    brand_new.setStepControl(oa::stepControl::CSA);
    brand_new.push_back(lineFit().clone());
    auto const started = newSetupOf(run, configure);
    CHECK(numbers(started, "p_sigma_") == std::vector<double>{0.});
    CHECK(started.getGlobalSigma() == newSetupOf(brand_new, configure).getGlobalSigma());
}

/******************************************************************************/

TEST_CASE(
    "The first iteration of a resumed evolutionary algorithm evaluates only the children",
    "[oa][resume]"
) {
    auto const adaption_config = lineFit().getAdaptionConfig();
    auto const configure = [&adaption_config](oa::GEvolutionaryAlgorithm &alg) {
        alg.setAdaptionConfig(adaption_config);
    };
    oa::GEvolutionaryAlgorithm run;
    run.setPopulationSizes(8, 2);
    configure(run);
    auto const cp = runToCheckpoint(run);

    // The parents of the checkpoint were evaluated in the run that wrote it
    std::size_t evaluations = 0;
    oneMoreIteration<oa::GEvolutionaryAlgorithm>(cp, configure, evaluations);
    fs::remove_all(cp.parent_path());
    CHECK(evaluations == 6);
}

/******************************************************************************/

TEST_CASE("A resumed swarm goes on with its neighborhood bests", "[oa][resume]") {
    oa::GSwarmAlgorithm run;
    run.setSwarmSizes(2, 3);
    auto const cp = runToCheckpoint(run);
    oa::GSwarmAlgorithm saved;
    saved.loadCheckpoint(cp);

    std::size_t evaluations = 0;
    auto resumed = oneMoreIteration<oa::GSwarmAlgorithm>(cp, [](auto &) {}, evaluations);
    fs::remove_all(cp.parent_path());

    // No neighborhood best gets worse (starting them anew from this iteration would lose them)
    for(std::size_t n = 0; n < 2; ++n) {
        INFO("neighborhood " << n);
        CHECK(
            resumed.getBestNeighborhoodIndividual<Gem::Geneva::Genome::GGenome>(n)
                ->transformed_fitness(0) <=
            saved.getBestNeighborhoodIndividual<Gem::Geneva::Genome::GGenome>(n)
                ->transformed_fitness(0)
        );
    }

    // The particles move on in the first iteration of the resumed run
    auto const before = positions(saved);
    auto const after = positions(resumed);
    CHECK(std::ranges::count_if(after, [&before](auto const &p) {
              return std::ranges::find(before, p) == before.end();
          }) > 0);
}

/******************************************************************************/

TEST_CASE(
    "A resumed conjugate gradient descent goes on with its difference-quotient children",
    "[oa][resume]"
) {
    oa::GConjugateGradientDescent run;
    run.setNStartingPoints(1);
    auto const cp = runToCheckpoint(run);
    oa::GConjugateGradientDescent saved;
    saved.loadCheckpoint(cp);

    // The first iteration of the resumed run steps from the evaluated children of the checkpoint
    auto const resumed = resumedSetupOf<oa::GConjugateGradientDescent>(cp);
    CHECK(positions(resumed) == positions(saved));

    // Another number of starting points cannot go on with this population
    oa::GConjugateGradientDescent other_size;
    other_size.loadCheckpoint(cp);
    fs::remove_all(cp.parent_path());
    other_size.setNStartingPoints(2);
    CHECK_THROWS_WITH(
        other_size.optimize(other_size.getIteration() + 1),
        Catch::Matchers::ContainsSubstring("does not fit the population")
    );
}

/******************************************************************************/

TEST_CASE("A resumed Nelder-Mead goes on with its simplices", "[oa][resume]") {
    oa::GNelderMead run;
    run.setNSimplices(2);
    auto const cp = runToCheckpoint(run);
    oa::GNelderMead saved;
    saved.loadCheckpoint(cp);

    // The simplices and the pending trials of the checkpoint, which the first iteration of the
    // resumed run decides on
    auto const resumed = resumedSetupOf<oa::GNelderMead>(cp);
    CHECK(positions(resumed) == positions(saved));
    CHECK(member(resumed, "trials_pending_") == "true");
    checkRoundTrips(resumed);

    // Another number of simplices cannot go on with this population
    oa::GNelderMead other_size;
    other_size.loadCheckpoint(cp);
    fs::remove_all(cp.parent_path());
    other_size.setNSimplices(3);
    CHECK_THROWS_WITH(
        other_size.optimize(other_size.getIteration() + 1),
        Catch::Matchers::ContainsSubstring("does not fit the population")
    );

    // A new run builds its simplices afresh and proposes its first trials in its second iteration
    auto const started = newSetupOf(run);
    CHECK(member(started, "trials_pending_") == "false");
    CHECK(positions(started) != positions(saved));
}

/******************************************************************************/

TEST_CASE("A resumed random parameter scan goes on with its count", "[oa][resume]") {
    oa::GParameterScan run;
    run.setNSimpleScans(1000);
    auto const cp = runToCheckpoint(run);
    REQUIRE(run.getNScansPerformed() > 0);

    auto const resumed = resumedSetupOf<oa::GParameterScan>(cp);
    fs::remove_all(cp.parent_path());
    CHECK(resumed.getNScansPerformed() == run.getNScansPerformed());

    // A new run counts from zero
    CHECK(newSetupOf(run).getNScansPerformed() == 0);
}

/******************************************************************************/

TEST_CASE("The setup builds the run state of a new run and not of a resumed one", "[oa][resume]") {
    // A new run builds it (the counting algorithm writes no checkpoint: its type is not registered)
    GRunStateCounter<oa::GStandardPSO2011> fresh;
    fresh.setSwarmSize(5);
    fresh.push_back(lineFit().clone());
    fresh.setMaxIteration(2);
    fresh.setReportIteration(0);
    fresh.setCheckpointInterval(0);
    fresh.optimize();
    CHECK(fresh.fresh_starts == 1);

    // A resumed run goes on with the run state of its checkpoint
    oa::GStandardPSO2011 run;
    run.setSwarmSize(5);
    auto const cp = runToCheckpoint(run);
    GRunStateCounter<oa::GStandardPSO2011> resumed;
    resumed.loadCheckpoint(cp);
    fs::remove_all(cp.parent_path());
    resumed.setCheckpointInterval(0);
    resumed.setReportIteration(0);
    resumed.setMaxIteration(resumed.getIteration() + 2); // one iteration
    resumed.optimize(resumed.getIteration() + 1);
    CHECK(resumed.fresh_starts == 0);

    // The next run of the same algorithm is a new one
    resumed.setMaxIteration(2);
    resumed.optimize();
    CHECK(resumed.fresh_starts == 1);
}
