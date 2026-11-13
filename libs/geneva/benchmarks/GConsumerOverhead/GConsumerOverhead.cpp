/**
 * @file GConsumerOverhead.cpp
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
#include <math.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>

// Boost header files go here

// Geneva header files go here
#include "common/GCommonEnums.hpp"
#include "common/GConfigEmission.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GCourtierHelperFunctions.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/Go2.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;
using namespace Gem::Common;
using namespace Gem::Courtier;

/************************************************************************************************/

// Default settings
const std::uint16_t DEFAULTNPRODUCERTHREADS = 10;
const std::uint16_t DEFAULTNEVALUATIONTHREADS = 4;
const std::size_t DEFAULTNPARENTS =
    5; // Allow to explore the parameter space from many starting points
const std::uint32_t DEFAULTMAXITERATIONS = 2000;
const long DEFAULTMAXMINUTES = 10;
const std::uint32_t DEFAULTREPORTITERATION = 1;
const duplicationScheme DEFAULTRSCHEME = duplicationScheme::VALUEDUPLICATIONSCHEME;
const bool DEFAULTVERBOSE = true;
// The parallelization mode selected on the command line (a plain code, not an enum: the
// per-algorithm execution-mode enum is gone): 0 = serial, 1 = multi-threaded, 2 = networked
constexpr std::uint16_t PM_SERIAL = 0;
constexpr std::uint16_t PM_MULTITHREADED = 1;
constexpr std::uint16_t PM_NETWORKED = 2;
const std::uint16_t DEFAULTPARALLELIZATIONMODE = PM_MULTITHREADED;
const bool DEFAULTUSECOMMONADAPTOR =
    false; // whether to use a common adaptor for all GParameterT objects
const unsigned short DEFAULTPORT = 10000;
const std::string DEFAULTIP = "localhost";
const std::string DEFAULTCONFIGFILE = "./GConsumerOverhead.cfg";
const sortingMode DEFAULTSORTINGSCHEME = sortingMode::MUPLUSNU_SINGLEEVAL;
const std::uint32_t DEFAULTSTARTITERATION = 0;
const std::size_t DEFAULTNBTCONSUMERTHREADS = 2;
const std::uint32_t DEFAULTGBTCNPROCUNITS = 1;
const std::size_t DEFAULTPARDIM = 100;
const double DEFAULTMINVAR = -10.;
const double DEFAULTMAXVAR = 10.;
const std::uint16_t DEFAULTEVALFUNCTION = 0;
const double DEFAULTGDAADPROB = 1.0;

/************************************************************************************************/
/**
 * A function that parses the command line for all required parameters
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent config-registration block: every benchmark CLI option is registered here in sequence
bool parseCommandLine(
    int argc,
    char **argv,
    std::uint16_t &parallelizationMode,
    std::uint16_t &nProducerThreads,
    std::uint16_t &nEvaluationThreads,
    std::size_t &populationSize,
    std::size_t &nParents,
    std::uint32_t &maxIterations,
    long &maxMinutes,
    std::uint32_t &reportIteration,
    duplicationScheme &rScheme,
    sortingMode &smode,
    std::uint32_t &nProcessingUnits,
    double &adProb,
    std::uint32_t &adaptionThreshold,
    double &sigma,
    double &sigmaSigma,
    double &minSigma,
    double &maxSigma,
    std::size_t &parDim,
    double &minVar,
    double &maxVar,
    gind::solverFunction &df
) {
    std::uint16_t evalFunction = 0;

    // Create the parser builder
    Gem::Common::GParserBuilder gpb;

    gpb.registerCLParameter<std::uint16_t>(
        "parallelizationMode,p",
        parallelizationMode,
        DEFAULTPARALLELIZATIONMODE,
        "Whether to run this optimization in serial mode (0), multi-threaded (1) or mt-consumer "
        "(2) mode"
    );

    gpb.registerCLParameter<std::uint16_t>(
        "n_producer_threads",
        nProducerThreads,
        DEFAULTNPRODUCERTHREADS,
        "The amount of random number producer threads"
    );

    gpb.registerCLParameter<std::uint16_t>(
        "n_evaluation_threads",
        nEvaluationThreads,
        DEFAULTNEVALUATIONTHREADS,
        "The amount of threads processing individuals simultaneously"
    );

    gpb.registerCLParameter<std::size_t>(
        "populationSize",
        populationSize,
        DEFAULTPOPULATIONSIZE,
        "The size of the super-population"
    );

    gpb.registerCLParameter<std::size_t>(
        "n_parents",
        nParents,
        DEFAULTNPARENTS,
        "The number of parents in the population"
    );

    gpb.registerCLParameter<std::uint32_t>(
        "maxIterations",
        maxIterations,
        DEFAULTMAXITERATIONS,
        "Maximum number of iterations in the population"
    );

    gpb.registerCLParameter<long>(
        "maxMinutes",
        maxMinutes,
        DEFAULTMAXMINUTES,
        "The maximum number of minutes the optimization of the population should run"
    );

    gpb.registerCLParameter<std::uint32_t>(
        "report_iteration",
        reportIteration,
        DEFAULTREPORTITERATION,
        "The number of iterations after which information should be emitted in the super-population"
    );

    gpb.registerCLParameter<duplicationScheme>(
        "rScheme",
        rScheme,
        DEFAULTRSCHEME,
        "The recombination scheme for the super-population"
    );

    gpb.registerCLParameter<sortingMode>(
        "sortingScheme,o",
        smode,
        DEFAULTSORTINGSCHEME,
        "Determines whether sorting is done in MUCOMMANU_SINGLEEVAL (0), MUPLUSNU_SINGLEEVAL (1) "
        "or MUNU1PRETAIN (2) mode"
    );

    gpb.registerCLParameter<std::uint32_t>(
        "nProcessingUnits",
        nProcessingUnits,
        DEFAULTGBTCNPROCUNITS,
        "Specifies how many processing units are available in networked mode"
    );

    gpb.registerCLParameter<double>(
        "ad_prob",
        adProb,
        DEFAULTGDAADPROB,
        "Specifies the likelihood for adaptions to be actually carried out"
    );

    gpb.registerCLParameter<std::uint32_t>(
        "adaptionThreshold",
        adaptionThreshold,
        DEFAULTADAPTIONTHRESHOLD,
        "Number of calls to adapt() after which adaption parameters should be modified"
    );

    gpb.registerCLParameter<double>(
        "sigma",
        sigma,
        DEFAULTSIGMA,
        "The width of the gaussian used for the adaption of double values"
    );

    gpb.registerCLParameter<double>(
        "sigma_sigma",
        sigmaSigma,
        DEFAULTSIGMASIGMA,
        "The adaption rate of sigma"
    );

    gpb.registerCLParameter<double>(
        "min_sigma",
        minSigma,
        DEFAULTMINSIGMA,
        "The minimum allowed value for sigma"
    );

    gpb.registerCLParameter<double>(
        "max_sigma",
        maxSigma,
        DEFAULTMAXSIGMA,
        "The maximum allowed value for sigma"
    );

    gpb.registerCLParameter<std::size_t>(
        "par_dim",
        parDim,
        DEFAULTPARDIM,
        "The amount of variables in the parabola"
    );

    gpb.registerCLParameter<double>(
        "min_var",
        minVar,
        DEFAULTMINVAR,
        "The lower boundary for all variables"
    );

    gpb.registerCLParameter<double>(
        "max_var",
        maxVar,
        DEFAULTMAXVAR,
        "The upper boundary for all variables"
    );

    gpb.registerCLParameter<std::uint16_t>(
        "evalFunction",
        evalFunction,
        0,
        "The id of the evaluation function"
    );

    // Some post-processing:

    // Check the number of parents in the super-population
    if(2 * nParents > populationSize) {
        glogger << "Error: Invalid number of parents inpopulation" << std::endl
                << "nParents       = " << nParents << std::endl
                << "populationSize = " << populationSize << std::endl
                << GWARNING;

        return false;
    }

    // Assign the demo function
    if(evalFunction > static_cast<std::uint16_t>(gind::MAXDEMOFUNCTION)) {
        std::cout << "Error: Invalid evaluation function: " << evalFunction << std::endl;
        return false;
    }
    df = (gind::solverFunction)evalFunction;

    // Parse the command line and leave if the help flag was given. The parser
    // will emit an appropriate help message by itself
    if(Gem::Common::GCL_HELP_REQUESTED == gpb.parseCommandLine(argc, argv, true /*verbose*/)) {
        return false; // Do not continue
    }

    return true;
}

/************************************************************************************************/
/**
 * The main function.
 */
// NOLINTNEXTLINE(readability-function-size) -- single benchmark main: config setup, consumer registration, workload construction and measurement all belong to one run
int main(int argc, char **argv) {
    // --update-configs: materialize the one config this benchmark owns (the GFunctionIndividual
    // factory's) from code defaults, then exit without running the benchmark.
    if(Gem::Common::configEmissionRequested(argc, argv)) {
        Gem::Common::beginConfigEmission();
        gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json").get();
        Gem::Common::finishConfigEmission();
    }

    std::string const configFile;
    std::uint16_t parallelizationMode{};
    std::string const ip;
    std::uint16_t nProducerThreads = 0;
    std::uint16_t nEvaluationThreads = 0;
    std::size_t populationSize = 0;
    std::size_t nParents = 0;
    std::uint32_t maxIterations = 0;
    long maxMinutes = 0;
    std::uint32_t reportIteration = 0;
    duplicationScheme rScheme;
    std::size_t parDim = 0;
    double minVar = NAN;
    double maxVar = NAN;
    sortingMode smode;
    std::uint32_t nProcessingUnits = 0;
    gind::solverFunction df;
    std::uint32_t adaptionThreshold = 0;
    double sigma = NAN;
    double sigmaSigma = NAN;
    double minSigma = NAN;
    double maxSigma = NAN;
    double adProb = NAN;

    // Parse the command line
    if(!parseCommandLine(
           argc,
           argv,
           parallelizationMode,
           nProducerThreads,
           nEvaluationThreads,
           populationSize,
           nParents,
           maxIterations,
           maxMinutes,
           reportIteration,
           rScheme,
           smode,
           nProcessingUnits,
           adProb,
           adaptionThreshold,
           sigma,
           sigmaSigma,
           minSigma,
           maxSigma,
           parDim,
           minVar,
           maxVar,
           df
       )) {
        exit(0);
    }

    // Random numbers are our most valuable good. Set the number of threads
    Gem::Hap::randomFactory()->setNProducerThreads(nProducerThreads);

    //***************************************************************************
    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    gind::GFunctionIndividualFactory gfi("./config/GFunctionIndividual.json");

    // Create the first set of parent individuals. Initialization of parameters is done randomly.
    std::vector<std::shared_ptr<gen::GGenome>> parentIndividuals;
    for(std::size_t p = 0; p < nParents; p++) {
        std::shared_ptr<gen::GGenome> const functionIndividual_ptr = gfi();

        // Give the individual a genome of `parDim` unbounded doubles in [minVar, maxVar[, sharing one
        // Gauss adaptor (the flat-genome equivalent of a GDoubleCollection). This replaces the
        // factory-produced genome with the benchmark's command-line geometry.
        gen::GGenomeBuilder b;
        b.addDoublePlainGroup(parDim, minVar, maxVar); // structure only; the adaptor lives on the OA config
        dynamic_cast<gen::GGenome &>(*functionIndividual_ptr).setGenome(b.build());

        parentIndividuals.push_back(functionIndividual_ptr);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // We can now start creating populations. Executors are registered for different execution modes

    std::shared_ptr<oa::GEvolutionaryAlgorithm> const pop_ptr(new oa::GEvolutionaryAlgorithm());

    // All three modes are LOCAL here (the "broker" mode used a local thread consumer too); build and
    // register the ONE process-wide consumer. Serial -> stc with one thread, the others -> multithreaded.
    {
        Gem::Geneva::ConsumerSpec spec;
        switch(parallelizationMode) {
        case PM_SERIAL: // Serial (single-threaded) execution
            std::cout << "Using serial execution." << std::endl;
            spec.mnemonic  = "stc";
            spec.n_threads = 1;
            break;

        case PM_MULTITHREADED: // Multi-threaded local execution
            std::cout << "Using plain multi-threaded execution." << std::endl;
            spec.mnemonic  = "stc";
            spec.n_threads = static_cast<unsigned int>(nEvaluationThreads);
            break;

        case PM_NETWORKED: // Historically a local thread consumer behind the broker -- still local.
            std::cout << "Using a local multi-threaded courtier consumer." << std::endl;
            spec.mnemonic  = "stc";
            spec.n_threads = static_cast<unsigned int>(nEvaluationThreads);
            break;
        }
        Gem::Geneva::buildConsumerSetup(spec); // registers the process consumer
    }

    // Add individuals to the population
    for(std::size_t p = 0; p < nParents; p++) {
        pop_ptr->push_back(parentIndividuals[p]->clone());
    }

    // The EA's Gauss adaptor now lives on the OA-owned config (authored here from the shared genome
    // geometry), not baked into the genome layout. Hand it to the population.
    {
        const auto &flat0 = dynamic_cast<const gen::GGenome &>(*parentIndividuals[0]);
        auto cfg = oa::makeAdaptionConfig<oa::GEAAdaptionConfig>(flat0);
        cfg->groupDouble(0).gauss(sigma, sigmaSigma, minSigma, maxSigma, adProb, 0., adaptionThreshold);
        pop_ptr->setAdaptionConfig(cfg);
    }

    // Specify some general population settings
    pop_ptr->setPopulationSizes(populationSize, nParents);
    pop_ptr->setMaxIteration(maxIterations);
    pop_ptr->setMaxTime(std::chrono::minutes(maxMinutes));
    pop_ptr->setReportIteration(reportIteration);
    pop_ptr->setRecombinationMethod(rScheme);
    pop_ptr->setSortingScheme(smode);

    // Do the actual optimization
    pop_ptr->optimize();

    // Terminate
    return 0;
}
