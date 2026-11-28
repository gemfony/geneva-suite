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

// Geneva header files go here
#include "geneva/GOptimizer.hpp"
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"
#include "geneva/oa/GPostProcessorT.hpp"

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GOptimizationAlgorithmFactoryT.hpp"

using namespace Gem::Geneva;

/** The program's own options */
struct ProgramOptions {
    std::string monitorTimings = "empty"; ///< --monitorTimings
    bool usePostProcessor = false;        ///< --usePostProcessor
};

/**
 * The optimization, on the process that submits the work.
 *
 * @param go The GOptimizer of the run
 * @param options The program's own options
 * @return The exit code
 */
int optimize(GOptimizer &go, const ProgramOptions &options) {
    const auto &[monitorTimings, usePostProcessor] = options;

    //---------------------------------------------------------------------------
    // --update-configs: the post-optimizer's configuration (GPostEvolutionaryAlgorithm.json) is a standard
    // evolutionary-algorithm config read by GEvolutionaryAlgorithmPostOptimizer. It is not part of
    // GOptimizer's owned config set and is built only when --usePostProcessor is given, so a config
    // refresh would miss it. Materialize it here from its factory defaults; GOptimizer refreshes
    // the rest and exits when optimize() runs below.
    if(Gem::Common::GParserBuilder::updateInPlace()) {
        OptimizationAlgorithms::GOptimizationAlgorithmFactoryT<
            OptimizationAlgorithms::GEvolutionaryAlgorithm>(
            "./config/GPostEvolutionaryAlgorithm.json"
        )
            .get<OptimizationAlgorithms::GOptimizationAlgorithmBase>();
    }

    //---------------------------------------------------------------------------
    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    auto const gfi_ptr = std::make_shared<Individuals::GFunctionIndividualFactory>(
        "./config/GFunctionIndividual.json"
    );

    //---------------------------------------------------------------------------
    // Register a post-processor, if this was requested by the user
    // clang-format off
    //! [results-postprocessor]
    if(usePostProcessor) {
        auto const eaPostOptimizer_ptr = std::make_shared<GEvolutionaryAlgorithmPostOptimizer>(
                "./config/GPostEvolutionaryAlgorithm.json"
            );

        // Make sure only evolutionary algorithms may perform postprocessing
        eaPostOptimizer_ptr->allowPostProcessingFor("ea");

        // Make the post-processor known to the factory
        gfi_ptr->registerPostProcessor(eaPostOptimizer_ptr);
    }
    //! [results-postprocessor]
    // clang-format on

    //---------------------------------------------------------------------------
    // Register pluggable optimization monitors, if requested by the user
    // See example 12 for more monitors

    if(monitorTimings != "empty") {
        // The times go into the stream named by the option (a CSV file and a plot in the record
        // directory, --recordDir)
        auto const processingTimesLogger_ptr = std::make_shared<GProcessingTimesLogger>(
            100, // nBins in x-direction
            100  // nBins in y-direction
        );
        processingTimesLogger_ptr->setStreamName(monitorTimings);
        go.registerPluggableOM(processingTimesLogger_ptr);
    }

    //---------------------------------------------------------------------------

    // Add a content creator so GOptimizer can generate its own individuals, if necessary
    go.registerContentCreator(gfi_ptr);

    // The genome carries only structure; the configured adaptor lives on an OA-owned config the factory
    // authors. Register it for the adapting algorithms (EA / SA) so GOptimizer hands it over before
    // they run.
    {
        auto sample = gfi_ptr->get_as<Individuals::GFunctionIndividual>();
        auto cfg = gfi_ptr->getAdaptionConfig(*sample);
        go.registerAdaptionConfig("ea", cfg);
        go.registerAdaptionConfig("sa", cfg);
    }

    // Add a default optimization algorithm to the GOptimizer object. This is optional.
    // Indeed "ea" is the default setting anyway. However, if you do not like it, you
    // can register another default algorithm here, which will then be used, unless
    // you specify other algorithms on the command line. You can also add a smart
    // pointer to an optimization algorithm here instead of its mnemonic.
    go.registerDefaultAlgorithm("ea");

    // Perform the actual optimization
    auto const p = go.optimize().getBestGlobalIndividual<Individuals::GFunctionIndividual>();

    // Here you can do something with the best individual ("p") found.
    // We simply print its content here, by means of an operator<< implemented
    // in the GFunctionIndividual code.
    std::cout << "Best result found:" << '\n' << p << '\n';
    return 0;
}

/******************************************************************************/

/******************************************************************************/
/**
 * The main function
 */
int main(int argc, char **argv) {
    //---------------------------------------------------------------------------
    // The program's own options, in the launcher's command line and help text
    ProgramOptions options;
    auto user_options = [&](Gem::Common::GParserBuilder &gpb) {
        gpb.registerCLParameter<std::string>(
            "monitorTimings",
            options.monitorTimings,
            std::string("empty"),
            "Records the times of all processing steps into the stream of this name (files "
            "<name>.csv and <name>.C in the record directory)",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            std::string("processing_times")
        );
        gpb.registerCLParameter<bool>(
            "usePostProcessor",
            options.usePostProcessor,
            false,
            "Whether or not to post-process individuals (using evolutionary algorithms in this "
            "example)",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
    };

    return runOptimization(
        argc,
        argv,
        {.userOptions = user_options,
         .produce = [&](GOptimizer &go, const auto &) { return optimize(go, options); }}
    );
}
