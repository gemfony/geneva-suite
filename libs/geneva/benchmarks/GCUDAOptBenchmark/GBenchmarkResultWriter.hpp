/**
 * @file GBenchmarkResultWriter.hpp
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

#include <string>
#include <vector>

#include "GBenchmarkRunResult.hpp"

namespace Gem::Geneva::Benchmarks {

/******************************************************************************/
/**
 * @brief Writes benchmark results to CSV files.
 *
 * Two file types are produced:
 *  - raw_<tag>_<function>_<date>.csv  — one row per run, written per tag
 *  - summary_<date>.csv               — aggregated mean ± sigma per tag
 *
 * All methods are static; no state is needed.
 */
class GBenchmarkResultWriter {
public:
    /**
     * @brief Writes raw per-run CSV for one algorithm tag.
     *
     * File name: <outputDir>/raw_<tag>_<function>_<date>.csv
     * Columns: run_index, algorithm_tag, function, nDims, final_fitness,
     *          iterations, wall_time_s, termination_reason, target_reached
     */
    static void writeRawCSV(
        const GAlgorithmBenchmarkResult &result,
        const std::string &outputDir
    );

    /**
     * @brief Writes a summary CSV containing aggregated stats for all tags.
     *
     * File name: <outputDir>/summary_<date>.csv
     * Columns: algorithm_tag, function, nDims, nRuns,
     *          mean_fitness, sigma_fitness,
     *          mean_iterations, sigma_iterations,
     *          mean_wall_time_s, sigma_wall_time_s,
     *          success_rate
     */
    static void writeSummaryCSV(
        const std::vector<GAlgorithmBenchmarkResult> &results,
        const std::string &outputDir
    );

    /**
     * @brief Prints a human-readable summary table to stdout.
     */
    static void printSummary(const std::vector<GAlgorithmBenchmarkResult> &results);

private:
    static std::string datestamp();
    static std::string sanitize(const std::string &s);
};

/******************************************************************************/

} /* namespace Gem::Geneva::Benchmarks */
