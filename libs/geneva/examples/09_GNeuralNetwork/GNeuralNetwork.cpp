/**
 * @file GNeuralNetwork.cpp
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

// Standard header files go here
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "geneva/GOptimizer.hpp"

// The individual that should be optimized
#include "geneva/individuals/GNeuralNetworkIndividual.hpp"

using namespace Gem::Geneva;
using namespace Gem::Courtier;
using namespace Gem::Common;
using namespace Gem::Hap;
//! [goptimizer-user-options#1]
using Gem::Common::GParserBuilder; // the program's options join the launcher's parser
//! [goptimizer-user-options#1]

/******************************************************************************/
/**
 * The main function.
 */
// NOLINTNEXTLINE(readability-function-size) -- single example main: the program's options, the data sets of every process and the optimization belong to one driver
int main(int argc, char **argv) {
    //---------------------------------------------------------------------------
    // The program's own options, in the launcher's command line and help text
    // clang-format off
    //! [goptimizer-user-options#2]
    gind::trainingDataType tdt = gind::trainingDataType::TDTNONE;
    std::string trainingDataFile = "./DataSets/hyper_sphere.dat";
    // two input nodes, one output node, two hidden layers with 4 nodes each
    std::string architecture = "2-4-4-1";
    std::size_t nDataSets = 2000;
    std::string resultProgram = "trainedNetwork.hpp";
    std::string visualizationFile = "visualization.C";

    auto user_options = [&](GParserBuilder &gpb) {
        gpb.registerCLParameter<gind::trainingDataType>(
            "trainingDataType", tdt, gind::trainingDataType::TDTNONE,
            "Specify training data to be produced: HYPERCUBE=1, HYPERSPHERE=2, "
            "AXISCENTRIC=3, SINUS=4"
        );
        gpb.registerCLParameter<std::string>(
            "trainingDataFile", trainingDataFile, trainingDataFile,
            "The name of the file to which training data should be written"
        );
        gpb.registerCLParameter<std::string>(
            "architecture", architecture, architecture,
            "The architecture of the network"
        );
        gpb.registerCLParameter<std::size_t>(
            "nDataSets", nDataSets, nDataSets, "The number of data sets to be produced"
        );
        gpb.registerCLParameter<std::string>(
            "resultProgram", resultProgram, resultProgram,
            "The name of the result program"
        );
        gpb.registerCLParameter<std::string>(
            "visualizationFile", visualizationFile, visualizationFile,
            "The name of the visualization file"
        );
    };
    //! [goptimizer-user-options#2]
    // clang-format on

    //---------------------------------------------------------------------------
    // Every process, once the command line is read: the data sets, if we have been asked to
    // produce them (then the program ends), and the global options the individuals read --
    // a client evaluates individuals too
    auto prepare = [&](Gem::Courtier::processRole) -> std::optional<int> {
        if(tdt != gind::trainingDataType::TDTNONE) {
            gind::GNeuralNetworkIndividual::createNetworkData(
                tdt,
                trainingDataFile,
                architecture,
                nDataSets
            );
            return 0;
        }

        // --update-configs: the network-individual configuration is data-independent (adaptor /
        // transfer settings), but the factory can only produce the individual -- and thus write
        // its config -- once a data set exists to size the network genome. Generate a tiny
        // throwaway data set in a temporary location (outside the materialized/installed config
        // directory) so the config can be emitted.
        if(Gem::Common::GParserBuilder::updateInPlace()) {
            auto const tmp_data =
                std::filesystem::temp_directory_path() / "geneva_nn_config_data.dat";
            gind::GNeuralNetworkIndividual::createNetworkData(
                gind::trainingDataType::HYPERSPHERE,
                tmp_data.string(),
                architecture,
                20
            );
            trainingDataFile = tmp_data.string();
        }

        // Store the trainingDataFile in the global options, so the individuals can access it
        neuralNetworkOptions()->set("trainingDataFile", trainingDataFile);
        return std::nullopt;
    };

    //---------------------------------------------------------------------------
    // The submitter only: the optimization
    auto produce = [&](GOptimizer &go, const GOptimizationDescription &) -> int {
        // Create a factory for GNeuralNetworkIndividual objects and perform
        // any necessary initial work.
        auto const gnn_ptr = std::make_shared<gind::GNeuralNetworkIndividualFactory>(
            "./config/GNeuralNetworkIndividual.json"
        );

        // Add a content creator so GOptimizer can generate its own individuals, if necessary
        go.registerContentCreator(gnn_ptr);

        // The network genome carries only structure; the per-weight Gauss adaptor lives on an
        // OA-owned config the factory authors. Register it for the adapting algorithms (EA / SA)
        // so GOptimizer hands it over.
        //
        // Skipped in --update-configs mode: building the sample constructs a neural-network
        // individual, which loads the training data set from disk -- irrelevant to a
        // configuration refresh and unavailable when the configs are materialized (e.g. at build
        // time). GOptimizer still refreshes the individual's config via the content creator
        // registered above.
        if(not Gem::Common::GParserBuilder::updateInPlace()) {
            auto sample = gnn_ptr->get_as<gind::GNeuralNetworkIndividual>();
            auto cfg = gnn_ptr->getAdaptionConfig(*sample);
            go.registerAdaptionConfig("PERSONALITY_EA", cfg);
            go.registerAdaptionConfig("PERSONALITY_SA", cfg);
        }

        // Perform the actual optimization and retrieve the best individual
        auto const p = go.optimize().getBestGlobalIndividual<gind::GNeuralNetworkIndividual>();

        //-----------------------------------------------------------------------
        // Write the trained network as a C++ header, and the figure of its output over the
        // input space as a ROOT macro (for a network with two input nodes)
        p->writeTrainedNetwork(resultProgram);
        p->writeVisualizationFile(visualizationFile);
        return 0;
    };

    //---------------------------------------------------------------------------
    // Geneva's description on courtier's launcher, with the program's options and functions
    //! [goptimizer-user-options#3]
    return runOptimization(
        argc,
        argv,
        {.userOptions = user_options, .prepare = prepare, .produce = produce}
    );
    //! [goptimizer-user-options#3]
}

/******************************************************************************/
