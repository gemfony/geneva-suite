/**
 * @file GMPIConsumer.cpp
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

/*
 * *****************************************************************************
 * This example demonstrates how to use the MPI consumer with the geneva library
 * to solve an example optimization problem.
 *
 * The GMPIConsumerT combines the qualities of a consumer and its corresponding
 * client in one type. If running in server mode (internally that means it is the rank 0
 * node), it is the process consumer the optimization algorithms submit to.
 * If running in client mode (MPI rank 1-n) then the node just needs to run.
 * The consumer itself figures out its position in the computation cluster and will
 * connect to the master node (rank 0).
 * The master node will then wait for worker nodes to request work items. If a request
 * arrives it will hand a new work item to the worker node.
 * Once the worker node has processed the work item it will send it back to the master node
 * and simultaneously request a new work item. At this point the master node will return the
 * processed work item to the submitter and provide the worker with a new raw work item.
 *
 *
 * This example's basic structure is
 * taken from the example `06_DirectEA`. The example `06_DirectEA` concerning
 * consumers only provides the options of local serial execution, local multicore
 * execution and networked execution using the GAsioConsumer. As the GMPIConsumerT
 * was developed at a later point in time and has another dependency (MPI library),
 * we decided to create this example separate example of how to use the GMPIConsumerT.
 * 
 * The example is best started with a runner program like `mpirun` like so:
 * `mpirun -np 4 ./GMPIConsumer
 * 
 * *****************************************************************************
 */

// Standard header files go here
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>

// Boost header files go here

// Geneva header files go here
#include "common/GConfigEmission.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/GenevaInitializer.hpp"

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;
using namespace Gem::Courtier;
using namespace Gem::Hap;
using namespace Gem::Common;
namespace po = boost::program_options;

/******************************************************************************/
// Default settings
const std::uint16_t DEFAULTNPRODUCERTHREADS = 10;
const std::size_t DEFAULTPOPULATIONSIZE06 = 100;
const std::size_t DEFAULTNPARENTS =
    5; // Allow to explore the parameter space from many starting points
const std::uint32_t DEFAULTMAXITERATIONS = 200;
const long DEFAULTMAXMINUTES = 10;
const std::uint32_t DEFAULTREPORTITERATION = 1;
const duplicationScheme DEFAULTRSCHEME = duplicationScheme::VALUEDUPLICATIONSCHEME;
const sortingMode DEFAULTEAAPPSORTINGMODE = sortingMode::MUCOMMANU_SINGLEEVAL;
const bool DEFAULTLOGTOFILE = false;

/******************************************************************************/
/**
 * Parses the command line
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent CLI-option registration sweep for example 15; splitting would scatter the option list
bool parseCommandLine(
    int argc,
    char **argv,
    std::uint16_t &nProducerThreads,
    std::size_t &populationSize,
    std::size_t &nParents,
    std::uint32_t &maxIterations,
    long &maxMinutes,
    std::uint32_t &reportIteration,
    duplicationScheme &rScheme,
    sortingMode &smode,
    bool &logToFile
) {
    // Create the parser builder
    Gem::Common::GParserBuilder gpb;

    gpb.registerCLParameter<std::uint16_t>(
        "n_producer_threads",
        nProducerThreads,
        DEFAULTNPRODUCERTHREADS,
        "The amount of random number producer threads"
    );

    gpb.registerCLParameter<std::size_t>(
        "populationSize",
        populationSize,
        DEFAULTPOPULATIONSIZE06,
        "The desired size of the population"
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
        "Maximum number of iterations in the optimization"
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
        "The number of iterations after which information should be emitted in the population"
    );

    gpb.registerCLParameter<duplicationScheme>(
        "rScheme",
        rScheme,
        DEFAULTRSCHEME,
        "The recombination scheme of the evolutionary algorithm"
    );

    gpb.registerCLParameter<sortingMode>(
        "smode",
        smode,
        DEFAULTEAAPPSORTINGMODE,
        "Determines whether sorting is done in MUPLUSNU_SINGLEEVAL (0), MUCOMMANU_SINGLEEVAL (1) "
        "or MUNU1PRETAIN (2) mode"
    );

    gpb.registerCLParameter<bool>(
        "logToFile",
        logToFile,
        DEFAULTLOGTOFILE,
        "Boolean flag to indicate whether to write log messages to a file rather than print them "
        "to console"
    );

    // Parse the command line and leave if the help flag was given. The parser
    // will emit an appropriate help message by itself
    if(Gem::Common::GCL_HELP_REQUESTED == gpb.parseCommandLine(argc, argv, true /*verbose*/)) {
        return false; // Do not continue
    }

    return true;
}

/******************************************************************************/
/**
 * The main function.
 */
// NOLINTNEXTLINE(readability-function-size) -- example 15's main(): a single linear setup script (config parsing, MPI-consumer setup, worker/server branch, population + adaption-config wiring, run); splitting would scatter tightly sequential one-shot setup steps
int main(int argc, char **argv) {
    std::uint16_t nProducerThreads = 0;
    std::size_t populationSize = 0;
    std::size_t nParents = 0;
    std::uint32_t maxIterations = 0;
    long maxMinutes = 0;
    std::uint32_t reportIteration = 0;
    duplicationScheme rScheme;
    sortingMode smode;
    bool logToFile = false;

    /****************************************************************************/
    // Initialization of Geneva
    GenevaInitializer const gi;

    /****************************************************************************/
    // --update-configs: materialize the one config this example owns (the GFunctionIndividual factory's)
    // from code defaults, then exit. This runs BEFORE the MPI consumer is built, so no MPI environment
    // (mpirun) is required to refresh the configuration.
    if(Gem::Common::configEmissionRequested(argc, argv)) {
        Gem::Common::beginConfigEmission();
        gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json").get();
        Gem::Common::finishConfigEmission();
    }

    /****************************************************************************/
    // Retrieve all necessary configuration data from the command line

    if(!parseCommandLine(
           argc,
           argv,
           nProducerThreads,
           populationSize,
           nParents,
           maxIterations,
           maxMinutes,
           reportIteration,
           rScheme,
           smode,
           logToFile
       )) {
        exit(1);
    }

    if(logToFile) {
        // writes log messages to a file rather than to std out
        glogger.addLogTarget(std::make_shared<GFileLogger>("GMPIConsumer.cpp.log"));
    }

    /****************************************************************************/
    // Random numbers are our most valuable good. Set the number of threads
    randomFactory()->setNProducerThreads(nProducerThreads);

    // Instantiate the MPI consumer through the shared courtier factory. It is built on every rank and
    // branches by rank: a worker yields a run_worker loop, the master the process consumer to submit
    // through.
    auto mpiSetup = Gem::Geneva::buildConsumerSetup(Gem::Geneva::ConsumerSpec{.mnemonic = "mpi"});

    /****************************************************************************/
    // A worker rank serves work items until the master broadcasts the stop signal, then exits.
    if(mpiSetup.run_worker) {
        mpiSetup.run_worker();
        return 0;
    }

    /****************************************************************************/
    // We can now start creating populations. We refer to them through the base class

    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    gind::GFunctionIndividualFactory gfi("./config/GFunctionIndividual.json");

    // Create the first set of parent individuals. Initialization of parameters is done randomly.
    std::vector<std::shared_ptr<gind::GFunctionIndividual>> parentIndividuals;
    for(std::size_t p = 0; p < nParents; p++) {
        parentIndividuals.push_back(gfi.get_as<gind::GFunctionIndividual>());
    }

    /****************************************************************************/

    // Create an empty population
    auto pop_ptr = std::make_shared<oa::GEvolutionaryAlgorithm>();

    // General settings
    pop_ptr->setPopulationSizes(populationSize, nParents);
    pop_ptr->setMaxIteration(maxIterations);
    pop_ptr->setMaxTime(std::chrono::minutes(maxMinutes));
    pop_ptr->setReportIteration(reportIteration);
    pop_ptr->setRecombinationMethod(rScheme);
    pop_ptr->setSortingScheme(smode);

    // Add individuals to the population.
    for(const auto &i : parentIndividuals) {
        pop_ptr->push_back(i->clone());
    }

    // The genome carries only structure; the configured Gauss / bi-Gauss adaptor lives on an OA-owned
    // config the factory authors. Hand it to the EA directly.
    pop_ptr->setAdaptionConfig(gfi.getAdaptionConfig(*parentIndividuals[0]));

    // buildConsumerSetup already registered the MPI master consumer as the process consumer; the
    // algorithm submits through it automatically.

    /****************************************************************************/
    // Perform the actual optimization
    pop_ptr->optimize();

    // Retrieve the best individual found
    auto p = pop_ptr->getBestGlobalIndividual<gind::GFunctionIndividual>();

    // Here you can do something with the best individual ("p") found.
    // We simply print its content here, by means of an operator<< implemented
    // in the GFunctionIndividual code.
    std::cout << "Best result found:" << '\n' << p << '\n';

    return 0;
}
