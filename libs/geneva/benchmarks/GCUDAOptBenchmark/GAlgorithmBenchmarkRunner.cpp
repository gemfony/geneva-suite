/**
 * @file GAlgorithmBenchmarkRunner.cpp
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

#include "GAlgorithmBenchmarkRunner.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>


#include "common/GCommonMathHelperFunctionsT.hpp"

namespace Gem::Geneva::Benchmarks {

namespace {

/******************************************************************************/
/**
 * @brief Maps a benchmark-function name (e.g. "PARABOLA", "ackley_canonical") to the corresponding
 * GFunctionIndividual demo function. The names match the solverFunction enum identifiers (and the
 * Gem::Geneva::Benchmarks::FUNC_* ids, 0..14). A plain integer id is also accepted. An unrecognised
 * value is a fatal config error.
 */
gind::solverFunction parseBenchmarkFunction(const std::string &name) {
    static const char *const kNames[] = {
        "PARABOLA", "NOISYPARABOLA", "ROSENBROCK", "ACKLEY", "RASTRIGIN",
        "SCHWEFEL", "SALOMON", "NEGPARABOLA", "ACKLEY_CANONICAL", "GRIEWANK",
        "LEVY", "STYBLINSKI_TANG", "ELLIPSOID", "MICHALEWICZ", "ZAKHAROV"};
    constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));

    std::string up = name;
    std::transform(up.begin(), up.end(), up.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for(int i = 0; i < kCount; ++i) {
        if(up == kNames[i]) {
            return static_cast<gind::solverFunction>(i);
        }
    }
    // Integer fallback: accept "0".."14" as well.
    try {
        std::size_t pos = 0;
        const int v = std::stoi(name, &pos);
        if(pos == name.size() && v >= 0 && v < kCount) {
            return static_cast<gind::solverFunction>(v);
        }
    } catch(...) {
        /* fall through to the throw below */
    }
    throw std::invalid_argument(
        "GAlgorithmBenchmarkRunner: unknown benchmarkFunction '" + name + "'");
}

} // namespace

/******************************************************************************/

GAlgorithmBenchmarkRunner::GAlgorithmBenchmarkRunner(BenchmarkConfig cfg)
    : cfg_(std::move(cfg))
{}

/******************************************************************************/
/**
 * @brief Runs the full benchmark over all algorithm entries.
 *
 * Assumes GenevaInitializer has been created and a consumer has been enrolled
 * with GConsumerRegistryT::setConsumer() by the caller before this is invoked.
 */
std::vector<GAlgorithmBenchmarkResult> GAlgorithmBenchmarkRunner::run() {
    glogger << "GAlgorithmBenchmarkRunner: starting benchmark." << std::endl
            << "  function=" << cfg_.functionName
            << "  dims=" << cfg_.nDimensions
            << "  nRuns=" << cfg_.nRuns
            << std::endl << GLOGGING;

    // Individual configuration (shared across all runs). GFunctionIndividual is a flat individual driven
    // by the generic GIndividualFactory; this benchmark needs a specific genome DIMENSION, so it builds
    // individuals directly through the static hooks rather than through the factory (whose par_dim is fixed
    // by the config file).
    gind::GFunctionIndividual::Config indCfg =
        gind::GFunctionIndividual::readConfig(cfg_.individualConfigFile);
    indCfg.par_dim = cfg_.nDimensions;

    std::vector<GAlgorithmBenchmarkResult> allResults;
    allResults.reserve(cfg_.algorithms.size());

    for (const auto &entry : cfg_.algorithms) {
        std::vector<GBenchmarkRunResult> rawRuns;
        rawRuns.reserve(cfg_.nRuns);

        glogger << "--- Algorithm: " << entry.tag
                << " (" << entry.mnemonic << ") ---" << std::endl
                << GLOGGING;

        for (std::uint32_t r = 0; r < static_cast<std::uint32_t>(cfg_.nRuns); ++r) {
            auto result = runOne(entry, r, indCfg);
            std::cout << "  run " << r
                      << "  fitness=" << result.finalFitness
                      << "  iter=" << result.iterationsConsumed
                      << "  t=" << result.wallTimeSeconds << "s"
                      << "  reason=" << terminationReasonToString(result.terminationReason)
                      << std::endl;
            rawRuns.push_back(std::move(result));
        }

        allResults.push_back(aggregate(entry.tag, cfg_.functionName, cfg_.nDimensions, rawRuns));
    }

    return allResults;
}

/******************************************************************************/
/**
 * @brief Materializes every configuration this benchmark owns without running it.
 */
void GAlgorithmBenchmarkRunner::emitConfigs() {
    // The shared individual configuration (create-if-absent / rewritten in update-in-place mode).
    (void) gind::GFunctionIndividual::readConfig(cfg_.individualConfigFile);

    // Each configured algorithm's configuration, materialized by constructing its factory -- the same
    // call runOne() makes, minus the optimization run.
    for (const auto &entry : cfg_.algorithms) {
        (void) makeAlgorithm(entry);
    }
}

/******************************************************************************/
/**
 * @brief Runs a single optimization and records the result.
 */
GBenchmarkRunResult GAlgorithmBenchmarkRunner::runOne(
    const AlgorithmEntry &entry,
    std::uint32_t runIdx,
    const gind::GFunctionIndividual::Config &indCfg
) {
    // Create algorithm from factory + config file
    auto alg = makeAlgorithm(entry);

    // The algorithm submits its populations through the process consumer (the GPU consumer the
    // caller registered before run()).

    // Attach termination monitor
    auto monitor = std::make_shared<GBenchmarkTerminationMonitor>();
    alg->registerPluggableOM(monitor);

    // Build a fresh individual fully configured the way the factory's get_as<>() would be (base options
    // from the config file), with the benchmark's genome dimension, then set its demo function.
    // benchmarkFunction overrides whatever GFunctionIndividual.json specifies;
    // an unrecognised name is a fatal config error, not a silent fallback.
    auto ind = gind::GFunctionIndividual::buildConfigured(indCfg, cfg_.individualConfigFile);
    ind->setDemoFunction(parseBenchmarkFunction(cfg_.functionName));
    alg->push_back(ind->clone());

    // The genome carries only structure; its Gauss / bi-Gauss adaptor lives on an OA-owned config built
    // from the same parameters. Hand it to the algorithm (a no-op for non-adapting algorithms like swarm /
    // CGD; adopted by EA / SA, which otherwise hard-error at init() for want of an explicit config).
    alg->setAdaptionConfig(gind::GFunctionIndividual::buildAdaptionConfig(*ind, indCfg));

    // Time the optimization
    const auto t0 = std::chrono::steady_clock::now();
    alg->optimize();
    const auto t1 = std::chrono::steady_clock::now();

    GBenchmarkRunResult result;
    result.algorithmTag        = entry.tag;
    result.functionName        = cfg_.functionName;
    result.nDimensions         = cfg_.nDimensions;
    result.runIndex            = runIdx;
    result.finalFitness        = monitor->finalFitness;
    result.iterationsConsumed  = monitor->finalIteration;
    result.wallTimeSeconds     = std::chrono::duration<double>(t1 - t0).count();
    result.terminationReason   = monitor->terminationReason;
    result.targetReached       = monitor->targetReached;
    return result;
}

/******************************************************************************/
/**
 * @brief Creates an optimization algorithm via the appropriate factory.
 *
 * The algorithm is configured entirely from the JSON config file specified
 * in the AlgorithmEntry. The algorithm submits through the process consumer
 * registered via GConsumerRegistryT::setConsumer().
 */
std::shared_ptr<oa::GOptimizationAlgorithmBase>
GAlgorithmBenchmarkRunner::makeAlgorithm(const AlgorithmEntry &entry) {
    if (entry.mnemonic == "ea") {
        return oa::GOptimizationAlgorithmFactoryT<oa::GEvolutionaryAlgorithm>(entry.configFile)
            .get<oa::GOptimizationAlgorithmBase>();
    }
    if (entry.mnemonic == "sa") {
        return oa::GOptimizationAlgorithmFactoryT<oa::GSimulatedAnnealing>(entry.configFile)
            .get<oa::GOptimizationAlgorithmBase>();
    }
    if (entry.mnemonic == "swarm") {
        return oa::GOptimizationAlgorithmFactoryT<oa::GSwarmAlgorithm>(entry.configFile)
            .get<oa::GOptimizationAlgorithmBase>();
    }
    if (entry.mnemonic == "gd" || entry.mnemonic == "cgd") {
        // "gd" was retired as a separate algorithm; it is the steepest-descent mode of the conjugate
        // gradient descent. Produce a CGD and, for the legacy "gd" tag, force steepest descent.
        auto p = oa::GOptimizationAlgorithmFactoryT<oa::GConjugateGradientDescent>(entry.configFile)
                     .get<oa::GOptimizationAlgorithmBase>();
        if (entry.mnemonic == "gd") {
            if (auto cgd = std::dynamic_pointer_cast<oa::GConjugateGradientDescent>(p)) {
                cgd->setGradientMethod(oa::gradientMethod::STEEPEST_DESCENT);
            }
        }
        return p;
    }
    throw std::invalid_argument(
        "GAlgorithmBenchmarkRunner::makeAlgorithm: unknown mnemonic '" + entry.mnemonic + "'");
}

/******************************************************************************/
/**
 * @brief Aggregates per-run results using GStandardDeviation<double>.
 */
GAlgorithmBenchmarkResult GAlgorithmBenchmarkRunner::aggregate(
    const std::string &tag,
    const std::string &funcName,
    std::uint32_t nDims,
    const std::vector<GBenchmarkRunResult> &runs
) {
    GAlgorithmBenchmarkResult agg;
    agg.algorithmTag = tag;
    agg.functionName = funcName;
    agg.nDimensions  = nDims;
    agg.nRuns        = runs.size();
    agg.rawRuns      = runs;

    if (runs.empty()) return agg;

    const auto fitness = runs
        | std::views::transform([](const GBenchmarkRunResult &r) { return r.finalFitness; })
        | std::ranges::to<std::vector<double>>();
    const auto iterations = runs
        | std::views::transform(
              [](const GBenchmarkRunResult &r) { return static_cast<double>(r.iterationsConsumed); })
        | std::ranges::to<std::vector<double>>();
    const auto wallTime = runs
        | std::views::transform([](const GBenchmarkRunResult &r) { return r.wallTimeSeconds; })
        | std::ranges::to<std::vector<double>>();
    const auto successCount =
        std::ranges::count_if(runs, [](const GBenchmarkRunResult &r) { return r.targetReached; });

    auto [mf, sf] = Gem::Common::GStandardDeviation(fitness);
    auto [mi, si] = Gem::Common::GStandardDeviation(iterations);
    auto [mw, sw] = Gem::Common::GStandardDeviation(wallTime);

    agg.meanFinalFitness  = mf;  agg.sigmaFinalFitness = sf;
    agg.meanIterations    = mi;  agg.sigmaIterations   = si;
    agg.meanWallTime      = mw;  agg.sigmaWallTime     = sw;
    agg.successRate       = static_cast<double>(successCount) / static_cast<double>(runs.size());

    return agg;
}

/******************************************************************************/

} /* namespace Gem::Geneva::Benchmarks */
