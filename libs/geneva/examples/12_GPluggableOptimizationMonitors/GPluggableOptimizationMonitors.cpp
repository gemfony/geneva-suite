/**
 * @file GPluggableOptimizationMonitors.cpp
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
#include "geneva/Go2.hpp"
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;

/** The program's own options */
struct ProgramOptions {
    bool printValid = false;                 ///< --printValid
    bool useRawFitness = false;              ///< --useRawFitness
    std::string monitorSpec = "empty";       ///< --monitorSpec
    bool bestOnly = false;                   ///< --bestOnly
    bool observeBoundaries = false;          ///< --observeBoundaries
    std::string logAll = "empty";            ///< --logAll
    std::string monitorNAdaptions = "empty"; ///< --monitorNAdaptions
    std::string logSigma = "empty";          ///< --logSigma
    std::string monitorTimings = "empty";    ///< --monitorTimings
    bool addOneOnly = false;                 ///< --addOneOnly
    bool printInitial = false;               ///< --printInitial
    bool showIterationBoundaries = false;    ///< --showIterationBoundaries
};

/**
 * The optimization, on the process that submits the work.
 *
 * @param go The Go2 of the run
 * @param options The program's own options
 * @return The exit code
 */
// NOLINTNEXTLINE(readability-function-size) -- single example driver: the pluggable monitors the options ask for, and the optimization run
int optimize(Go2 &go, const ProgramOptions &options) {
    const auto
        &[printValid,
          useRawFitness,
          monitorSpec,
          bestOnly,
          observeBoundaries,
          logAll,
          monitorNAdaptions,
          logSigma,
          monitorTimings,
          addOneOnly,
          printInitial,
          showIterationBoundaries] = options;

    //---------------------------------------------------------------------------
    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    std::shared_ptr<gind::GFunctionIndividualFactory> const gfi_ptr(
        new gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json")
    );

    //---------------------------------------------------------------------------
    // Register pluggable optimization monitors, if requested by the user

    // Register a progress plotter with the global optimization algorithm factory
    //! [monitors:register-plotter]
    if(monitorSpec != "empty") {
        std::shared_ptr<GProgressPlotter> const progplot_ptr(new GProgressPlotter());

        progplot_ptr->setProfileSpec(monitorSpec);
        progplot_ptr->setObserveBoundaries(observeBoundaries);
        progplot_ptr->setMonitorValidOnly(
            printValid
        ); // Only record valid parameters, when printValid is set to true
        progplot_ptr->setUseRawEvaluation(
            useRawFitness
        ); // Use untransformed evaluation values for logging
        progplot_ptr->setMonitorBestOnly(
            bestOnly
        ); // Whether only the best solutions should be monitored

        // Request printing of png files (upon processing of the .C file with ROOT)
        progplot_ptr->setAddPrintCommand(true);

        go.registerPluggableOM(progplot_ptr);
    }
    //! [monitors:register-plotter]

    if(logAll != "empty") {
        std::shared_ptr<GAllSolutionFileLogger> const allSolutionLogger_ptr(
            new GAllSolutionFileLogger(logAll)
        );

        allSolutionLogger_ptr->setPrintWithNameAndType(
            true
        ); // Output information about variable names and types
        allSolutionLogger_ptr->setPrintWithCommas(true); // Output commas between values
        allSolutionLogger_ptr->setUseTrueFitness(
            false
        ); // Output "transformed" fitness, not the "true" value
        allSolutionLogger_ptr->setShowValidity(true); // Indicate, whether this is a valid solution

        if(printInitial) {
            allSolutionLogger_ptr->setPrintInitial();
        }

        if(showIterationBoundaries) {
            allSolutionLogger_ptr->setShowIterationBoundaries();
        }

        go.registerPluggableOM(allSolutionLogger_ptr);
    }

    if(monitorNAdaptions != "empty") {
        std::shared_ptr<GNAdpationsLogger> const nAdaptionsLogger_ptr(
            new GNAdpationsLogger(monitorNAdaptions)
        );

        nAdaptionsLogger_ptr->setMonitorBestOnly(false); // Output information for all individuals
        nAdaptionsLogger_ptr->setAddPrintCommand(
            true
        ); // Create a PNG file if Root-file is executed

        go.registerPluggableOM(nAdaptionsLogger_ptr);
    }

    if(logSigma != "empty") {
        std::shared_ptr<GAdaptorPropertyLogger<double>> const sigmaLogger_ptr(
            new GAdaptorPropertyLogger<double>(logSigma, "GDoubleGaussAdaptor", "sigma")
        );

        sigmaLogger_ptr->setMonitorBestOnly(false); // Output information for all individuals
        sigmaLogger_ptr->setAddPrintCommand(true);  // Create a PNG file if Root-file is executed

        go.registerPluggableOM(sigmaLogger_ptr);
    }

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

    // Either add a single individual or take all individuals from the content provider.
    // Adding a single individual is useful for debugging purposes.
    if(addOneOnly) {
        go.push_back(gfi_ptr->get());
    }
    else {
        // Add a content creator so Go2 can generate its own individuals, if necessary
        go.registerContentCreator(gfi_ptr);
    }

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
    auto const p = go.optimize()->getBestGlobalIndividual<gind::GFunctionIndividual>();

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
        gpb.registerCLParameter<bool>(
            "validOnly",
            options.printValid,
            false,
            "Enforces output of valid solutions only",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
        gpb.registerCLParameter<bool>(
            "useRawFitness",
            options.useRawFitness,
            false,
            "Plot untransformed fitness value, even if a transformation takes place for the "
            "purpose of optimization",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
        gpb.registerCLParameter<std::string>(
            "monitorSpec",
            options.monitorSpec,
            std::string("empty"),
            "Allows you to specify variables to be monitored like this: \"d(0, -10, 10)\" (a "
            "double, by its index)"
        );
        gpb.registerCLParameter<bool>(
            "bestOnly",
            options.bestOnly,
            false,
            "Allows you to specify whether only the best solutions should be monitored. This "
            "option only has an effect when monitorSpec is set.",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
        gpb.registerCLParameter<bool>(
            "observeBoundaries",
            options.observeBoundaries,
            false,
            "Only plot inside of specified boundaries (no effect, when monitorSpec hasn't been "
            "set)",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
        gpb.registerCLParameter<std::string>(
            "logAll",
            options.logAll,
            std::string("empty"),
            "Logs all solutions to the file name provided as argument to this switch",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            std::string("./logAll.txt")
        );
        gpb.registerCLParameter<std::string>(
            "monitorAdaptions",
            options.monitorNAdaptions,
            std::string("empty"),
            "Logs the number of adaptions for all individuals over the course of the optimization. "
            "Useful for evolutionary algorithms only.",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            std::string("./nAdaptions.C")
        );
        gpb.registerCLParameter<std::string>(
            "logSigma",
            options.logSigma,
            std::string("empty"),
            "Logs the value of sigma for all or the best adaptors, if GDoubleGaussAdaptors are "
            "being used",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            std::string("./sigmaLog.C")
        );
        gpb.registerCLParameter<std::string>(
            "monitorTimings",
            options.monitorTimings,
            std::string("empty"),
            "Logs the times for all processing steps",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            std::string("timingsLog")
        );
        gpb.registerCLParameter<bool>(
            "addOneIndividualOnly",
            options.addOneOnly,
            false,
            "When set, results in a single individual being added to the collection. This may be "
            "useful for debugging",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
        gpb.registerCLParameter<bool>(
            "printInitial",
            options.printInitial,
            false,
            "[logAll] When set, forces the printout of the initial population prior to the "
            "optimization",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
        gpb.registerCLParameter<bool>(
            "showIterationBoundaries",
            options.showIterationBoundaries,
            false,
            "[logAll] When set, prints a comment inbetween iterations",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
    };

    return runOptimization(
        argc,
        argv,
        {.userOptions = user_options,
         .produce = [&](Go2 &go, const auto &) { return optimize(go, options); }}
    );
}
