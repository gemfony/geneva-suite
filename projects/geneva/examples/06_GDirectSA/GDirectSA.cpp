/**
 * @file GDirectSA.cpp
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
 * NOTE: This file shows how to access some of the optimization algorithms
 * directly (SA in this case), without going through the Go2 class. Usually, Go2
 * is the recommended way and will relieve you from many burdensome tasks you
 * otherwise have to perform. Thus, if you are new to Geneva, we recommend that
 * you start with example 01 first rather than following what is shown in this file.
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
#include "geneva/oa/GSimulatedAnnealing.hpp"
#include "courtier/GBaseClientT.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/GenevaInitializer.hpp"

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;
using namespace Gem::Courtier;
using namespace Gem::Hap;
using namespace Gem::Common;

using namespace Gem::Common;
namespace po = boost::program_options;

/******************************************************************************/
// Default settings
// The parallelization mode selected on the command line (a plain code, not an enum: the
// per-algorithm execution-mode enum is gone): 0 = serial, 1 = multi-threaded, 2 = networked
constexpr std::uint16_t PM_SERIAL = 0;
constexpr std::uint16_t PM_MULTITHREADED = 1;
constexpr std::uint16_t PM_NETWORKED = 2;
const std::uint16_t DEFAULTPARALLELIZATIONMODEAP = PM_MULTITHREADED;
const unsigned short DEFAULTPORT = 10000;
const std::string DEFAULTIP = "localhost";
const std::uint16_t DEFAULTNPRODUCERTHREADS = 10;
const Gem::Common::serializationMode DEFAULTSERMODE = Gem::Common::serializationMode::GEM_BINARY;
const bool DEFAULTADDLOCALCONSUMER = false;
const std::uint16_t DEFAULTNEVALUATIONTHREADS = 4;
const std::size_t DEFAULTPOPULATIONSIZE06 = 100;
const std::size_t DEFAULTNPARENTS =
    5; // Allow to explore the parameter space from many starting points
const std::uint32_t DEFAULTMAXITERATIONS = 200;
const std::uint32_t DEFAULTREPORTITERATION = 1;
const long DEFAULTMAXMINUTES = 10;
const duplicationScheme DEFAULTRSCHEME = duplicationScheme::VALUEDUPLICATIONSCHEME;
const std::size_t DEFAULTMAXRECONNECTS = 10;

/******************************************************************************/
/**
 * Parses the command line
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent config-registration block: every GDirectSA CLI option is registered here in sequence
bool parseCommandLine(
    int argc,
    char **argv,
    std::uint16_t &parallelizationMode,
    bool &serverMode,
    std::string &ip,
    unsigned short &port,
    Gem::Common::serializationMode &serMode,
    bool &addLocalConsumer,
    std::uint16_t &nProducerThreads,
    std::uint16_t &nEvaluationThreads,
    std::size_t &populationSize,
    std::size_t &nParents,
    std::uint32_t &maxIterations,
    long &maxMinutes,
    std::uint32_t &reportIteration,
    duplicationScheme &rScheme,
    std::size_t &maxReconnects
) {
    // Create the parser builder
    Gem::Common::GParserBuilder gpb;

    gpb.registerCLParameter<std::uint16_t>(
        "parallelizationMode,p",
        parallelizationMode,
        DEFAULTPARALLELIZATIONMODEAP,
        "Whether to run the optimization in serial (0), multi-threaded (1) or networked (2) mode"
    );

    gpb.registerCLParameter<bool>(
        "serverMode,s",
        serverMode,
        false // Use client mode, if no server option is specified
        ,
        "Whether to run networked execution in server or client mode. The option only has an "
        "effect if \"--parallelizationMode=2\". You can either say \"--server=true\" or just "
        "\"--server\".",
        GCL_IMPLICIT_ALLOWED // Permit implicit values, so that we can say --server instead of --server=true
        ,
        true // Use server mode, of only -s or --server was specified
    );

    gpb.registerCLParameter<std::string>(std::string("ip"), ip, DEFAULTIP, "The ip of the server");

    gpb.registerCLParameter<unsigned short>("port", port, DEFAULTPORT, "The port on the server");

    gpb.registerCLParameter<Gem::Common::serializationMode>(
        "serializationMode",
        serMode,
        DEFAULTSERMODE,
        "Specifies whether serialization shall be done in TEXTMODE (0), XMLMODE (1) or BINARYMODE "
        "(2)"
    );

    gpb.registerCLParameter<bool>(
        "addLocalConsumer",
        addLocalConsumer,
        DEFAULTADDLOCALCONSUMER // Use client mode, if no server option is specified
        ,
        "Whether or not a local consumer should be added to networked execution. You can use this "
        "option with or without arguments.",
        GCL_IMPLICIT_ALLOWED // Permit implicit values, so that we can say --server instead of --server=true
        ,
        true // Use a local consumer if the option --addLocalConsumer was given without arguments
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
        "The amount of threads processing individuals simultaneously in multi-threaded mode"
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

    gpb.registerCLParameter<std::uint32_t>(
        "report_iteration",
        reportIteration,
        DEFAULTREPORTITERATION,
        "The number of iterations after which information should be emitted in the population"
    );

    gpb.registerCLParameter<long>(
        "maxMinutes",
        maxMinutes,
        DEFAULTMAXMINUTES,
        "The maximum number of minutes the optimization of the population should run"
    );

    gpb.registerCLParameter<duplicationScheme>(
        "rScheme",
        rScheme,
        DEFAULTRSCHEME,
        "The recombination scheme of the evolutionary algorithm"
    );

    gpb.registerCLParameter<std::size_t>(
        "maxReconnects",
        maxReconnects,
        DEFAULTMAXRECONNECTS,
        "The number of times a client will try to reconnect when it couldn't reach the server"
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
// NOLINTNEXTLINE(readability-function-size) -- single example main: CLI parsing, Go2 setup and the optimization run all belong to one driver
int main(int argc, char **argv) {
    std::uint16_t parallelizationMode{};
    bool serverMode = false;
    std::string ip;
    unsigned short port = 0;
    std::uint16_t nProducerThreads = 0;
    std::uint16_t nEvaluationThreads = 0;
    std::size_t populationSize = 0;
    std::size_t nParents = 0;
    std::uint32_t maxIterations = 0;
    long maxMinutes = 0;
    std::uint32_t reportIteration = 0;
    duplicationScheme rScheme;
    Gem::Common::serializationMode serMode;
    bool addLocalConsumer = false;
    std::size_t maxReconnects = 0;

    /****************************************************************************/
    // Initialization of Geneva
    GenevaInitializer const gi;

    /****************************************************************************/
    // --update-configs: materialize the configuration files this binary owns, then exit. This
    // direct-OA example drives the algorithm programmatically and never reads an OA config; the only
    // config it owns is the GFunctionIndividual factory's, so constructing that factory and drawing
    // one individual (with GParserBuilder in update-in-place mode) creates/rewrites it canonically.
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
           parallelizationMode,
           serverMode,
           ip,
           port,
           serMode,
           addLocalConsumer,
           nProducerThreads,
           nEvaluationThreads,
           populationSize,
           nParents,
           maxIterations,
           maxMinutes,
           reportIteration,
           rScheme,
           maxReconnects
       )) {
        exit(1);
    }

    /****************************************************************************/
    // Random numbers are our most valuable good. Set the number of threads
    randomFactory()->setNProducerThreads(nProducerThreads);

    /****************************************************************************/
    // If this is a client in networked mode, we can just start the listener and
    // return when it has finished
    if(PM_NETWORKED == parallelizationMode && !serverMode) {
        // Build the networked client through the courtier setup layer. The single mnemonic below
        // drives both this client and the server below -- change it (e.g. to "beast") in both places to
        // switch transport, with no other code change.
        ConsumerSpec spec;
        spec.mnemonic           = "asio";
        spec.ip                 = ip;
        spec.port               = port;
        spec.serialization_mode = serMode;
        spec.max_reconnects     = maxReconnects;

        auto client = buildConsumerClient(spec);

        // Start the actual processing loop
        client->run();

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

    //! [sa-direct-setup#1]
    // Create an empty population
    std::shared_ptr<oa::GSimulatedAnnealing> const pop_ptr(new oa::GSimulatedAnnealing());

    // General settings
    pop_ptr->setPopulationSizes(populationSize, nParents);
    pop_ptr->setMaxIteration(maxIterations);
    pop_ptr->setMaxTime(std::chrono::minutes(maxMinutes));
    pop_ptr->setReportIteration(reportIteration);
    pop_ptr->setRecombinationMethod(rScheme);

    // Add individuals to the population.
    for(auto ind : parentIndividuals) {
        pop_ptr->push_back(ind->clone());
    }

    // The genome carries only structure; the configured Gauss / bi-Gauss adaptor lives on an OA-owned
    // config the factory authors. Hand it to the simulated-annealing population directly.
    pop_ptr->setAdaptionConfig(gfi.getAdaptionConfig(*parentIndividuals[0]));
    //! [sa-direct-setup#1]

    // Build and register the ONE process-wide consumer, depending on the parallelisation mode.
    // The algorithm submits through it automatically.
    {
        Gem::Geneva::ConsumerSpec spec;
        switch(parallelizationMode) {
        //----------------------------------------------------------------------------
        case PM_SERIAL: // Serial (single-threaded) execution
            spec.mnemonic  = "stc";
            spec.n_threads = 1;
            break;

            //----------------------------------------------------------------------------
        case PM_MULTITHREADED: // Multi-threaded local execution
            spec.mnemonic  = "stc";
            spec.n_threads = static_cast<unsigned int>(nEvaluationThreads);
            break;

            //----------------------------------------------------------------------------
        case PM_NETWORKED: // Networked execution (or a purely local consumer for testing)
            if(addLocalConsumer) {
                // "Broker mode" with only a local multi-threaded consumer (testing / benchmarking).
                spec.mnemonic  = "stc";
                spec.n_threads = static_cast<unsigned int>(nEvaluationThreads);
            }
            else {
                // Build a courtier ASIO server via the shared factory; the clients started above (built by
                // buildConsumerClient for the same mnemonic) connect to it.
                spec.mnemonic           = "asio";
                spec.port               = port;
                spec.serialization_mode = serMode;
            }
            break;

            //----------------------------------------------------------------------------
        }
        Gem::Geneva::buildConsumerSetup(spec); // registers the process consumer
    }

    //! [sa-direct-setup#2]
    /****************************************************************************/
    // Perform the actual optimization
    pop_ptr->optimize();

    // Retrieve the best individual found
    auto p = pop_ptr->getBestGlobalIndividual<gind::GFunctionIndividual>();
    //! [sa-direct-setup#2]

    // Here you can do something with the best individual ("p") found.
    // We simply print its content here, by means of an operator<< implemented
    // in the GFunctionIndividual code.
    std::cout << "Best result found:" << '\n' << p << '\n';

    /****************************************************************************/
    // Terminate
    return (0);
}
