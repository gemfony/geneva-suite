/**
 * @file GOptimizationBenchmarkConfig.hpp
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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

// Boost header files go here

// Geneva header files go here

#include "common/GParserBuilder.hpp"

namespace Gem::Geneva::Benchmarks {

const std::size_t DEFNOPTBENCHTESTS = 10;

/*********************************************************************************/
/**
 * A class that parses configuration options for the GOptimizationBenchmark test
 */
class GOptimizationBenchmarkConfig {
public:
    /*****************************************************************************/
    /**
	 * The primary constructor
	 *
	 * @param configFile The name of a configuration file
	 */
    explicit GOptimizationBenchmarkConfig(std::filesystem::path const &configFile)
      : nTests_(DEFNOPTBENCHTESTS)
      , parDim_(0)
      , resultFile_("result.C") {
        using namespace Gem::Common;

        gpb_.registerFileParameter(
            "n_tests",
            nTests_,
            nTests_,
            VAR_IS_ESSENTIAL,
            "The number of tests to be performed for each dimension"
        );

        // Set up a vector of default values
        std::vector<std::uint32_t> def_pardim;

        def_pardim.push_back(2);
        def_pardim.push_back(4);
        def_pardim.push_back(8);
        def_pardim.push_back(16);
        def_pardim.push_back(32);
        def_pardim.push_back(64);
        def_pardim.push_back(128);
        def_pardim.push_back(256);
        def_pardim.push_back(512);
        // Parked, not forgotten: the sweep is meant to be extendable, and these are the next rungs.
        // Each doubling roughly doubles the run's cost, so they are turned on for a deliberate
        // convergence study, not for the registered benchmark.
        /* def_pardim.push_back(1024);
		def_pardim.push_back(2048);
		def_pardim.push_back(4096); */

        gpb_.registerFileParameter(
            "dimension",
            parDim_,
            def_pardim,
            VAR_IS_ESSENTIAL,
            "Dimensions of the parameter space to be tested"
        );

        gpb_.registerFileParameter(
            "result_file",
            resultFile_,
            resultFile_,
            VAR_IS_ESSENTIAL,
            "The name of a file to which results of the;benchmark should be written"
        );

        // Read in the configuration file
        gpb_.parseConfigFile(configFile);
    }

    /*****************************************************************************/
    /**
	 * Retrieval of the name of the result file
	 *
	 * @return The name of the result file
	 */
    const std::string &getResultFileName() const {
        return resultFile_;
    }

    /*****************************************************************************/
    /**
	 * Retrieval of the vector holding the test dimensions
	 *
	 * @return A vector holding the parameter space dimensions to be tested
	 */
    const std::vector<std::uint32_t> &getParDim() const {
        return parDim_;
    }

    /*****************************************************************************/
    /**
	 * Retrieve the number of tests to be performed for each dimension
	 *
	 * @return The number of tests to be performed for each dimension
	 */
    std::size_t getNTests() const {
        return nTests_;
    }

private:
    /*****************************************************************************/

    GOptimizationBenchmarkConfig() =
        delete; ///< Default constructor: Intentionally private and undefined

    Gem::Common::GParserBuilder gpb_; ///< Handles the actual parsing

    std::size_t nTests_; ///< The number of tests to be performed for each dimension
    std::vector<std::uint32_t> parDim_;
    std::string resultFile_; ///< The name of a file to which results should be written
};

/*********************************************************************************/

} /* namespace Gem::Geneva::Benchmarks */
