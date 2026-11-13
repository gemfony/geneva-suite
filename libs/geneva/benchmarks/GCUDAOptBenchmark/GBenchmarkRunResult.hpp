/**
 * @file GBenchmarkRunResult.hpp
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

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Gem::Geneva::Benchmarks {

/******************************************************************************/
/**
 * @brief Reason why an optimization run terminated.
 *
 * Maps directly to the three halt criteria available in
 * GOptimizationAlgorithmBase: maximum iterations, stall counter
 * threshold, and quality threshold. Unknown is used when the reason
 * cannot be determined (e.g., if the algorithm object is no longer
 * accessible after the run).
 */
enum class TerminationReason : std::uint8_t {
    MaxIterations,   ///< setMaxIteration() limit reached
    StallThreshold,  ///< setStallCounterThreshold() limit reached without improvement
    QualityThreshold,///< setQualityThreshold() target fitness reached
    Unknown
};

/******************************************************************************/
/**
 * @brief Converts a TerminationReason to a short ASCII string for CSV output.
 */
inline std::string_view terminationReasonToString(TerminationReason r) noexcept {
    switch (r) {
        case TerminationReason::MaxIterations:    return "max_iter";
        case TerminationReason::StallThreshold:   return "stall";
        case TerminationReason::QualityThreshold: return "target_reached";
        case TerminationReason::Unknown:          return "unknown";
    }
    return "unknown";
}

/******************************************************************************/
/**
 * @brief Result data for a single optimization run of one algorithm.
 *
 * Populated by GAlgorithmBenchmarkRunner after each call to alg->optimize().
 * All fields except algorithmTag and functionName are filled from the
 * optimisation algorithm's post-run state.
 */
struct GBenchmarkRunResult {
    std::string   algorithmTag;        ///< Free label, e.g. "ea_pop100" or "sa_default"
    std::string   functionName;        ///< GFunctionIndividual function name, e.g. "PARABOLA"
    std::uint32_t nDimensions  {0};    ///< Parameter dimension used in this run
    std::uint32_t runIndex     {0};    ///< 0-based index within the N runs for this tag

    double        finalFitness        {0.0}; ///< Best raw fitness at termination
    std::uint32_t iterationsConsumed  {0};   ///< Number of iterations executed
    double        wallTimeSeconds     {0.0}; ///< Wall-clock time from optimize() start to end
    TerminationReason terminationReason {TerminationReason::Unknown};
    bool          targetReached {false};     ///< true iff terminationReason == QualityThreshold
};

/******************************************************************************/
/**
 * @brief Aggregated results for one algorithm tag over N runs.
 *
 * Computed by GAlgorithmBenchmarkRunner::aggregate() from a
 * vector<GBenchmarkRunResult> using GStandardDeviation<double>.
 *
 * "sigma" fields hold the sample standard deviation (not the standard error
 * of the mean), so that the spread across runs is visible even for small N.
 */
struct GAlgorithmBenchmarkResult {
    std::string   algorithmTag;
    std::string   functionName;
    std::uint32_t nDimensions {0};
    std::size_t   nRuns       {0};

    double meanFinalFitness  {0.0};  double sigmaFinalFitness {0.0};
    double meanIterations    {0.0};  double sigmaIterations   {0.0};
    double meanWallTime      {0.0};  double sigmaWallTime     {0.0};

    /// Fraction of runs that reached the quality threshold (0.0 – 1.0).
    double successRate {0.0};

    /// Raw per-run data, retained so the result writer can produce raw CSVs.
    std::vector<GBenchmarkRunResult> rawRuns;
};

/******************************************************************************/

} /* namespace Gem::Geneva::Benchmarks */
