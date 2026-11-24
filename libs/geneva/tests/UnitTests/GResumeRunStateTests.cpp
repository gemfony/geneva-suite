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
 * checkpoint, and the setup of the resumed run (init()) keeps it. A new run builds it afresh.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include <boost/json.hpp>

#include "common/GCommonEnums.hpp"
#include "common/testing/GStandardTests.hpp"
#include "geneva/individuals/GLineFitIndividual.hpp"
#include "geneva/oa/GGeneralizedSimulatedAnnealing.hpp"
#include "geneva/oa/GStandardPSO2011.hpp"

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
 * @return The algorithm as the setup of the resumed run left it
 */
template <typename oa_type>
oa_type resumedSetupOf(fs::path const &cp) {
    GSetupProbe<oa_type> probe;
    probe.loadCheckpoint(cp);
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
 * @return The algorithm as the setup of the new run left it
 */
template <typename oa_type>
oa_type newSetupOf(oa_type const &earlier) {
    GSetupProbe<oa_type> probe;
    probe.load(earlier);
    return setupOf(probe, 0);
}

/// A best fitness before any evaluation
constexpr double UNSET = std::numeric_limits<double>::max();

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
