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
#include <sstream>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "geneva/Go2.hpp"

// The individual that should be optimized
#include "geneva/individuals/GNeuralNetworkIndividual.hpp"

using namespace Gem::Geneva;
using namespace Gem::Courtier;
using namespace Gem::Common;
using namespace Gem::Hap;
//! [go2-user-options#1]
namespace po = boost::program_options;
//! [go2-user-options#1]

/******************************************************************************/
/**
 * The main function.
 */
// NOLINTNEXTLINE(readability-function-size) -- single example main: additional CLI options, training-data setup, Go2 setup and the optimization run all belong to one driver
int main(int argc, char **argv) {
    //---------------------------------------------------------------------------
    // Assemble additional command line options to be passed to Go2
    //! [go2-user-options#2]
    gind::trainingDataType tdt = gind::trainingDataType::TDTNONE;
    std::string trainingDataFile = "./DataSets/hyper_sphere.dat";
    std::string architecture =
        "2-4-4-1"; // two input nodes, one output node, two hidden layers with 4 nodes each
    std::size_t nDataSets = 2000;
    std::string resultProgram = "trainedNetwork.hpp";
    std::string visualizationFile = "visualization.C";

    // Assemble command line options
    boost::program_options::options_description user_options;
    user_options.add_options()(
        "trainingDataType",
        po::value<gind::trainingDataType>(&tdt)->default_value(gind::trainingDataType::TDTNONE),
        "Specify training data to be produced: HYPERCUBE=1, HYPERSPHERE=2, AXISCENTRIC=3, SINUS=4"
    )(
        "trainingDataFile",
        po::value<std::string>(&trainingDataFile)->default_value(trainingDataFile),
        "The name of the file to which training data should be written"
    )(
        "architecture",
        po::value<std::string>(&architecture)->default_value(architecture),
        "The architecture of the network"
    )(
        "nDataSets",
        po::value<std::size_t>(&nDataSets)->default_value(nDataSets),
        "The number of data sets to be produced"
    )(
        "resultProgram",
        po::value<std::string>(&resultProgram)->default_value(resultProgram),
        "The name of the result program"
    )(
        "visualizationFile",
        po::value<std::string>(&visualizationFile)->default_value(visualizationFile),
        "The name of the visualization file"
    );
    //! [go2-user-options#2]

    //---------------------------------------------------------------------------
    // Create the main optimizer-wrapper
    //! [go2-user-options#3]
    Go2 go(argc, argv, "./config/Go2.json", user_options);
    //! [go2-user-options#3]

    //---------------------------------------------------------------------------
    // Produce data sets if we have been asked to do so, then leave
    if(tdt != gind::trainingDataType::TDTNONE) {
        gind::GNeuralNetworkIndividual::createNetworkData(tdt, trainingDataFile, architecture, nDataSets);
        return 0;
    }

    // --update-configs: the network-individual configuration is data-independent (adaptor / transfer
    // settings), but the factory can only produce the individual -- and thus write its config -- once a
    // data set exists to size the network genome. Generate a tiny throwaway data set in a temporary
    // location (outside the materialized/installed config directory) so the config can be emitted.
    if(go.updateConfigsMode()) {
        auto const tmp_data = std::filesystem::temp_directory_path() / "geneva_nn_config_data.dat";
        gind::GNeuralNetworkIndividual::createNetworkData(
            gind::trainingDataType::HYPERSPHERE, tmp_data.string(), architecture, 20);
        trainingDataFile = tmp_data.string();
    }

    // Store the trainingDataFile in the global options, so they can be accessed by the individuals
    neuralNetworkOptions()->set("trainingDataFile", trainingDataFile);

    //---------------------------------------------------------------------------
    // Client mode
    if(go.clientMode()) {
        return go.clientRun();
    } // Execution will end here in client mode

    //---------------------------------------------------------------------------

    // Create a factory for GNeuralNetworkIndividual objects and perform
    // any necessary initial work.
    std::shared_ptr<gind::GNeuralNetworkIndividualFactory> const gnn_ptr(
        new gind::GNeuralNetworkIndividualFactory("./config/GNeuralNetworkIndividual.json")
    );

    // Add a content creator so Go2 can generate its own individuals, if necessary
    go.registerContentCreator(gnn_ptr);

    // The network genome carries only structure; the per-weight Gauss adaptor lives on an OA-owned config
    // the factory authors. Register it for the adapting algorithms (EA / SA) so Go2 hands it over.
    //
    // Skipped in --update-configs mode: building the sample constructs a neural-network individual, which
    // loads the training data set from disk -- irrelevant to a configuration refresh and unavailable when
    // the configs are materialized (e.g. at build time). Go2 still refreshes the individual's config via
    // the content creator registered above.
    if(not go.updateConfigsMode()) {
        auto sample = gnn_ptr->get_as<gind::GNeuralNetworkIndividual>();
        auto cfg = gnn_ptr->getAdaptionConfig(*sample);
        go.registerAdaptionConfig("PERSONALITY_EA", cfg);
        go.registerAdaptionConfig("PERSONALITY_SA", cfg);
    }

    // Perform the actual optimization and retrieve the best individual
    auto const p =
        go.optimize()->getBestGlobalIndividual<gind::GNeuralNetworkIndividual>();

    //---------------------------------------------------------------------------
    // Output the result- and the visualization-program (if available)
    p->writeTrainedNetwork(resultProgram);
    p->writeVisualizationFile(visualizationFile);

    // Terminate Geneva
    return 0;
}

/******************************************************************************/
