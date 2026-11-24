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

/**
 * @file
 * @brief Tests of GNeuralNetworkIndividual's output files: the C++ header of the trained network
 * (writeTrainedNetwork()) and the figure of the network's output (writeVisualizationFile()).
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>

#include <unistd.h> // getpid()

#include "GNeuralNetworkTestData.hpp"
#include "common/GExceptions.hpp"
#include "geneva/individuals/GNeuralNetworkIndividual.hpp"

using Gem::Geneva::Individuals::GNeuralNetworkIndividual;
namespace gind = Gem::Geneva::Individuals;

namespace {

/** @brief A network individual over the test's training data, with the default settings. */
GNeuralNetworkIndividual makeIndividual() {
    gind::Testing::provideNeuralNetworkTrainingData();
    return {
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
    };
}

} // namespace

/******************************************************************************/
// Both writers tested the emptiness of their file name twice (`name.empty() || name.empty()`, once
// `name == "" || name.empty()`), a duplicate, not a second condition. Pins the one refusal that is
// meant: no file name, no file.
TEST_CASE(
    "The writers of GNeuralNetworkIndividual refuse an empty file name",
    "[individuals][nn]"
) {
    GNeuralNetworkIndividual ind = makeIndividual();
    CHECK_THROWS_AS(ind.writeVisualizationFile(""), gem_exception);
    CHECK_THROWS_AS(ind.writeTrainedNetwork(""), gem_exception);

    // A file name that can be written is accepted.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("geneva-nn-writers-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    ind.writeTrainedNetwork((dir / "trainedNetwork.hpp").string());
    CHECK(std::filesystem::exists(dir / "trainedNetwork.hpp"));
    std::filesystem::remove_all(dir);
}
