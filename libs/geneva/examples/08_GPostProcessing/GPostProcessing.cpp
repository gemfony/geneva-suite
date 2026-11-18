/**
 * @file GPostProcessing.cpp
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
#include <iostream>

// Boost header files go here
#include <boost/program_options.hpp>

// Geneva header files go here
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"
#include "geneva/oa/GPostProcessorT.hpp"
#include "geneva/Go2.hpp"

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GOptimizationAlgorithmFactoryT.hpp"

using namespace Gem::Geneva;
namespace po = boost::program_options;

/******************************************************************************/
/**
 * The main function
 */
// NOLINTNEXTLINE(readability-function-size) -- single example main: additional CLI options, Go2 setup, the optimization run and the post-processing step all belong to one driver
int main(int argc, char **argv) {
    //---------------------------------------------------------------------------
    // We want to add additional command line options

    std::string monitorTimings = "empty";
    bool usePostProcessor = false;

    // Assemble command line options
    boost::program_options::options_description user_options;
    user_options.add_options()(
		"monitorTimings"
		, po::value<std::string>(&monitorTimings)->implicit_value(std::string("timingsLog"))->default_value("empty")
		, "Logs the times for all processing steps"
	)(
		"usePostProcessor"
		, po::value<bool>(&usePostProcessor)->implicit_value(true)->default_value(false)
		, "Whether or not to post-process individuals (using evolutionary algorithms in this example)"
	);

    Go2 go(argc, argv, "./config/Go2.json", user_options);

    //---------------------------------------------------------------------------
    // Client mode
    if(go.clientMode()) {
        return go.clientRun();
    } // Execution will end here in client mode

    //---------------------------------------------------------------------------
    // --update-configs: the post-optimizer's configuration (GPostEvolutionaryAlgorithm.json) is a standard
    // evolutionary-algorithm config read by GEvolutionaryAlgorithmPostOptimizer. It is not part of Go2's
    // owned config set and is built only when --usePostProcessor is given, so a config refresh would miss
    // it. Materialize it here from its factory defaults; Go2 refreshes the rest and exits when optimize()
    // runs below.
    if(go.updateConfigsMode()) {
        OptimizationAlgorithms::GOptimizationAlgorithmFactoryT<
            OptimizationAlgorithms::GEvolutionaryAlgorithm>(
            "./config/GPostEvolutionaryAlgorithm.json"
        ).get<OptimizationAlgorithms::GOptimizationAlgorithmBase>();
    }

    //---------------------------------------------------------------------------
    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    std::shared_ptr<gind::GFunctionIndividualFactory> const gfi_ptr(
        new gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json")
    );

    //---------------------------------------------------------------------------
    // Register a post-processor, if this was requested by the user
    //! [results-postprocessor]
    if(usePostProcessor) {
        std::shared_ptr<GEvolutionaryAlgorithmPostOptimizer> const eaPostOptimizer_ptr(
            new GEvolutionaryAlgorithmPostOptimizer(
                "./config/GPostEvolutionaryAlgorithm.json"
            )
        );

        // Make sure only evolutionary algorithms may perform postprocessing
        eaPostOptimizer_ptr->allowPostProcessingFor("ea");

        // Make the post-processor known to the factory
        gfi_ptr->registerPostProcessor(eaPostOptimizer_ptr);
    }
    //! [results-postprocessor]

    //---------------------------------------------------------------------------
    // Register pluggable optimization monitors, if requested by the user
    // See example 12 for more monitors

    if(monitorTimings != "empty") {
        std::shared_ptr<GProcessingTimesLogger> const processingTimesLogger_ptr(
            new GProcessingTimesLogger(
                "hist_" + monitorTimings + ".C",
                "hist2D_" + monitorTimings + ".C",
                monitorTimings + ".txt",
                100 // nBins in x-direction
                ,
                100 // nBins in y-direction
            )
        );
        go.registerPluggableOM(processingTimesLogger_ptr);
    }

    //---------------------------------------------------------------------------

    // Add a content creator so Go2 can generate its own individuals, if necessary
    go.registerContentCreator(gfi_ptr);

    // The genome carries only structure; the configured adaptor lives on an OA-owned config the factory
    // authors. Register it for the adapting algorithms (EA / SA) so Go2 hands it over before they run.
    {
        auto sample = gfi_ptr->get_as<gind::GFunctionIndividual>();
        auto cfg = gfi_ptr->getAdaptionConfig(*sample);
        go.registerAdaptionConfig("PERSONALITY_EA", cfg);
        go.registerAdaptionConfig("PERSONALITY_SA", cfg);
    }

    // Add a default optimization algorithm to the Go2 object. This is optional.
    // Indeed "ea" is the default setting anyway. However, if you do not like it, you
    // can register another default algorithm here, which will then be used, unless
    // you specify other algorithms on the command line. You can also add a smart
    // pointer to an optimization algorithm here instead of its mnemonic.
    go.registerDefaultAlgorithm("ea");

    // Perform the actual optimization
    auto const p =
        go.optimize()->getBestGlobalIndividual<gind::GFunctionIndividual>();

    // Here you can do something with the best individual ("p") found.
    // We simply print its content here, by means of an operator<< implemented
    // in the GFunctionIndividual code.
    std::cout << "Best result found:" << '\n' << p << '\n';
}
