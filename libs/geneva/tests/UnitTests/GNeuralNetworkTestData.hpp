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

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <mutex>
#include <string>

#include <unistd.h> // getpid()

#include "geneva/individuals/GNeuralNetworkIndividual.hpp"

namespace Gem::Geneva::Individuals::Testing {

/******************************************************************************/
/**
 * GNeuralNetworkIndividual reads its network geometry and training data through a process-wide
 * singleton, which loads the file named in the options store once. Writes a small hyper-cube data
 * set (two inputs, a hidden layer of three nodes, one output; inputs in [-0.5, 0.5]) to a temporary
 * file, points the option the singleton reads at it and loads the singleton, once per process. Every
 * test of the individual calls this before it builds one: the tests share a process (a shard of the
 * suite), and whichever comes first loads the data the others then see.
 */
inline void provideNeuralNetworkTrainingData() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto const file = std::filesystem::temp_directory_path() /
                          ("geneva_unit_tests_nn_" + std::to_string(::getpid()) + ".dat");
        GNeuralNetworkIndividual::createHyperCubeNetworkData({2, 3, 1}, 20, 0.5)
            ->saveToDisk(file.string());
        Gem::Common::neuralNetworkOptions()->set("trainingDataFile", file.string());
        REQUIRE(nnTrainingDataStore()->size() == 3); // loads the file into the singleton
        std::filesystem::remove(file);
    });
}

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals::Testing */
