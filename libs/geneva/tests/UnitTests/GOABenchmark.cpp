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
 * A heavy, TIME-BUDGETED comparison of all optimization algorithms on high-dimensional synthetic
 * problems. Every algorithm is given the SAME wall-clock budget (the timed halt criterion,
 * setMaxTime()) with the iteration / stall caps disabled, so the table answers "how far does each
 * algorithm get in T seconds" -- which folds in each algorithm's real per-iteration overhead, not just
 * its sample efficiency. Large populations / swarms / archives / chains are used throughout.
 *
 * This is an opt-in benchmark, NOT part of the normal suite: it is tagged "[.oabench]" (the leading dot
 * hides it from a default run). Invoke explicitly:
 *     ./geneva/tests/UnitTests/GenevaStandardTests "[oabench]"
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GAntColonyOptimization.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GGeneralizedSimulatedAnnealing.hpp"
#include "geneva/oa/GSepCmaEvolutionStrategy.hpp"
#include "geneva/oa/GSimulatedAnnealing.hpp"
#include "geneva/oa/GStandardPSO2011.hpp"
#include "geneva/oa/GSwarmAlgorithm.hpp"

namespace gen = Gem::Geneva::Genome;
namespace oa = Gem::Geneva::OptimizationAlgorithms;
using Gem::Geneva::activityMode;

namespace {

/******************************************************************************/
// The three benchmark objective functions (all minimised, global optimum 0).
enum class BenchFn : int { Sphere = 0, Rastrigin = 1, Rosenbrock = 2 };

/** @brief The pure objective value for a streamlined parameter vector. Accumulated in long double so that
 *  a strongly-converged run (e.g. sep-CMA on the sphere, |x_i| ~ 1e-162) does not underflow the
 *  sum-of-squares to a misleading exact 0 in double. */
double objective(BenchFn fn, const std::vector<double> &x) {
    long double s = 0.L;
    switch(fn) {
    case BenchFn::Sphere:
        for(double const v : x) {
            s += static_cast<long double>(v) * static_cast<long double>(v);
        }
        return static_cast<double>(s);
    case BenchFn::Rastrigin:
        s = 10.L * static_cast<long double>(x.size());
        for(double const v : x) {
            const long double lv = v;
            s += lv * lv - 10.L * std::cos(2.L * std::numbers::pi_v<long double> * lv);
        }
        return static_cast<double>(s);
    case BenchFn::Rosenbrock:
        for(std::size_t i = 0; i + 1 < x.size(); ++i) {
            const long double a = static_cast<long double>(x[i + 1]) - static_cast<long double>(x[i]) * static_cast<long double>(x[i]);
            const long double b = 1.L - static_cast<long double>(x[i]);
            s += 100.L * a * a + b * b;
        }
        return static_cast<double>(s);
    }
    return static_cast<double>(s);
}

const char *fnName(BenchFn fn) {
    switch(fn) {
    case BenchFn::Sphere: return "Sphere";
    case BenchFn::Rastrigin: return "Rastrigin";
    case BenchFn::Rosenbrock: return "Rosenbrock";
    }
    return "?";
}

/******************************************************************************/
/**
 * A flat-genome benchmark individual: N constrained doubles, scored by the selected objective. The
 * constraint box contains the optimum in its interior (so the constrained fold never distorts the
 * search): [-5.12, 5.12] for sphere/Rastrigin (optimum at 0), [-5, 10] for Rosenbrock (optimum at 1).
 * The start value is offset from the optimum so the run has somewhere to go.
 */
template <std::size_t N, BenchFn FN>
class BenchIndividual : public gen::GGenomeT<BenchIndividual<N, FN>> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    BenchIndividual() {
        gen::GGenomeBuilder b;
        if constexpr(FN == BenchFn::Rosenbrock) {
            b.addDoubleGroup(N, -5., 10.).init(-1.0);
        } else {
            b.addDoubleGroup(N, -5.12, 5.12).init(4.0);
        }
        this->setGenome(b.build());
    }
    BenchIndividual(const BenchIndividual &) = default;

    /** @brief A Gauss adaption config (only the EA and classic SA consume one; the others own their update). */
    [[nodiscard]] std::shared_ptr<oa::GAdaptionConfigBase> buildAdaptionConfig() const {
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
        cfg->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
        return cfg;
    }

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->template streamline<double>(v);
        return {objective(FN, v)};
    }
};

/******************************************************************************/
struct Row {
    std::string algo;
    double best;
    std::size_t iterations;
    double seconds;
};

/** @brief Whether algorithm @p tag should run, per the GBENCH_ALGO env var (unset/empty/"all" -> all).
 *  Running one algorithm per process isolates each run. */
bool want(const char *tag) {
    const char *sel = std::getenv("GBENCH_ALGO");
    if(sel == nullptr) {
        return true;
    }
    const std::string s(sel);
    return s.empty() || s == "all" || s == tag;
}

/** @brief Sets the timed halt + disables the iteration/stall caps, times optimize(), returns wall-seconds. */
template <typename OA>
double timedRun(std::shared_ptr<OA> &pop, double budget_s, std::size_t &iterations_out) {
    pop->setMaxIteration(1000000000); // effectively unlimited; the timed halt governs
    pop->setMaxStallIteration(0);     // disabled
    pop->setReportIteration(1000000000);
    pop->setMaxTime(std::chrono::duration<double>(budget_s));
    const auto t0 = std::chrono::steady_clock::now();
    pop->optimize();
    const auto t1 = std::chrono::steady_clock::now();
    iterations_out = pop->getIteration();
    return std::chrono::duration<double>(t1 - t0).count();
}

/** @brief Runs all algorithms on one (function, dimension) and prints a table sorted by best fitness. */
template <std::size_t N, BenchFn FN>
// NOLINTNEXTLINE(readability-function-size) -- one gate-driven dispatcher running the full algorithm matrix (each `if(want(tag))` block is an independent, self-contained algorithm run); splitting per algorithm would scatter the shared Row/record()/table-printing machinery
void runMatrix(double budget_s) {
    using Ind = BenchIndividual<N, FN>;
    Ind const proto;
    std::vector<Row> rows;

    auto record = [&](const std::string &name, auto pop) {
        std::size_t iters = 0;
        const double secs = timedRun(pop, budget_s, iters);
        auto best = pop->template getBestGlobalIndividual<Ind>();
        std::vector<double> v;
        best->template streamline<double>(v);
        rows.push_back({name, objective(FN, v), iters, secs});
    };

    // Each algorithm is gated by the GBENCH_ALGO env var (unset/"all" -> run all). Running one algorithm
    // per process (GBENCH_ALGO=ea, sepcma, ...) isolates each run -- which the benchmark driver script
    // does, both for fair per-algorithm wall-clock and to side-step a pre-existing heap-corruption that
    // surfaces only when several of the EXISTING OAs run sequentially in one process.

    // --- Existing algorithms ---
    if(want("ea")) {
        auto pop = std::make_shared<oa::GEvolutionaryAlgorithm>();
        pop->setPopulationSizes(256, 64); // (total, nParents)
        pop->push_back(proto.clone());
        pop->setAdaptionConfig(proto.buildAdaptionConfig());
        record("EA", pop);
    }
    if(want("sepcma")) {
        auto pop = std::make_shared<oa::GSepCmaEvolutionStrategy>(); // owns its distribution; auto lambda
        pop->push_back(proto.clone());
        record("sep-CMA-ES", pop);
    }
    if(want("swarm")) {
        auto pop = std::make_shared<oa::GSwarmAlgorithm>();
        pop->setSwarmSizes(8, 32); // 8 neighborhoods x 32 members
        pop->push_back(proto.clone());
        record("swarm (classic)", pop);
    }
    if(want("sa")) {
        auto pop = std::make_shared<oa::GSimulatedAnnealing>();
        pop->setPopulationSizes(256, 64); // (total, nParents)
        pop->push_back(proto.clone());
        pop->setAdaptionConfig(proto.buildAdaptionConfig());
        record("SA (classic)", pop);
    }
    // --- New clean-room algorithms ---
    if(want("spso")) {
        auto pop = std::make_shared<oa::GStandardPSO2011>();
        pop->setSwarmSize(128);
        pop->push_back(proto.clone());
        record("SPSO-2011", pop);
    }
    if(want("acor")) {
        auto pop = std::make_shared<oa::GAntColonyOptimization>();
        pop->setArchiveSize(100);
        pop->setNAnts(20);
        pop->push_back(proto.clone());
        record("ACOR", pop);
    }
    if(want("gsa")) {
        auto pop = std::make_shared<oa::GGeneralizedSimulatedAnnealing>();
        pop->setNChains(64);
        // Stretch the Tsallis cooling so the chains keep making useful moves across a long,
        // high-dimensional, time-bounded run instead of freezing within the first ~100 steps.
        pop->setCoolingTimescale(300.);
        pop->push_back(proto.clone());
        record("Generalized-SA", pop);
    }

    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.best < b.best; });

    std::cout << "\n================ " << fnName(FN) << "  (n=" << N << ", budget=" << budget_s
              << "s/algo) ================\n";
    std::cout << std::left << std::setw(20) << "algorithm" << std::right << std::setw(16) << "best f"
              << std::setw(12) << "iters" << std::setw(10) << "secs" << "\n";
    std::cout << std::string(58, '-') << "\n";
    for(const Row &r : rows) {
        std::cout << std::left << std::setw(20) << r.algo << std::right << std::setw(16)
                  << std::scientific << std::setprecision(3) << r.best << std::setw(12) << r.iterations
                  << std::setw(10) << std::fixed << std::setprecision(2) << r.seconds << "\n";
    }
    // Machine-parseable rows (aggregated across one-algorithm-per-process invocations by the driver):
    for(const Row &r : rows) {
        std::cout << "RESULT," << r.algo << "," << fnName(FN) << "," << N << "," << std::scientific
                  << std::setprecision(6) << r.best << "," << r.iterations << "," << std::fixed
                  << std::setprecision(2) << r.seconds << "\n";
    }
    std::cout.flush();
}

} // namespace

/******************************************************************************/

TEST_CASE("OA benchmark: high-dimensional synthetic functions, equal time budget", "[.oabench]") {
    const char *budget_env = std::getenv("GBENCH_BUDGET");
    const double BUDGET = (budget_env != nullptr) ? std::atof(budget_env) : 5.0; // seconds per algorithm per problem

    std::cout << "\n#### Geneva OA benchmark -- equal wall-clock budget per algorithm (timed halt) ####\n";

    runMatrix<100, BenchFn::Sphere>(BUDGET);
    runMatrix<100, BenchFn::Rastrigin>(BUDGET);
    runMatrix<100, BenchFn::Rosenbrock>(BUDGET);

    runMatrix<200, BenchFn::Sphere>(BUDGET);
    runMatrix<200, BenchFn::Rastrigin>(BUDGET);
    runMatrix<200, BenchFn::Rosenbrock>(BUDGET);

    SUCCEED("benchmark completed");
}
