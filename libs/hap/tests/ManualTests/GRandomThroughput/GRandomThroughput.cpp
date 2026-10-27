/**
 * @file GRandomThroughput.cpp
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
#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GParserBuilder.hpp"
#include "hap/GRandomDistributionsT.hpp"
#include "hap/GRandomT.hpp"

/******************************************************************************/
/**
 * @brief Times one source: fill+sort a payload nCycles times and report numbers/s.
 *
 * Templated on the random source so the same loop measures QUEUE, LOCAL and
 * STAGED identically (the proxy is the only thing that differs).
 *
 * @tparam S The random source the proxy draws from
 * @return The achieved throughput in double random numbers per second
 */
template <Gem::Hap::randomSource S>
double run_measurement(std::uint32_t packageSize, std::uint32_t nCycles, double lo, double hi) {
    Gem::Hap::GRandomT<S>                  gr;
    std::vector<double>                    payload(packageSize);
    std::uniform_real_distribution<double>  uniform_real(lo, hi);

    std::chrono::system_clock::time_point const startTime = std::chrono::system_clock::now();
    for(std::uint32_t c = 0; c < nCycles; c++) {
        for(auto &p : payload) {
            p = uniform_real(gr);
        }
        std::sort(payload.begin(), payload.end());
    }
    std::chrono::system_clock::time_point const endTime = std::chrono::system_clock::now();
    std::chrono::duration<double>         const duration = endTime - startTime;

    return static_cast<double>(nCycles * packageSize) / duration.count();
}

// NOLINTNEXTLINE(readability-function-size) -- main() of a manual test: CLI parsing, then a switch selecting which randomSource to benchmark via run_measurement()
int main(int argc, char **argv) {
    std::uint16_t nProducerThreads = 4;
    std::uint32_t packageSize = 10000;
    std::uint32_t nCycles = 1000;
    double lowerBoundary = 0.;
    double upperBoundary = 1.;
    std::string source = "queue";

    //----------------------------------------------------------------
    // Create the parser builder
    Gem::Common::GParserBuilder gpb;

    // Specify command line options
    gpb.registerCLParameter<std::uint16_t>(
        "nProducerThreads,n",
        nProducerThreads // the variable to be filled
        ,
        nProducerThreads // the default
        ,
        "The number of threads for the production of random numbers"
    );

    gpb.registerCLParameter<std::uint32_t>(
        "packageSize,p",
        packageSize // the variable to be filled
        ,
        packageSize // the default
        ,
        "The amount of random numbers to be read in one go"
    );

    gpb.registerCLParameter<std::uint32_t>(
        "cycles,c",
        nCycles // the variable to be filled
        ,
        nCycles // the default
        ,
        "The number of times that a vector should be filled with random numbers"
    );

    gpb.registerCLParameter<double>(
        "lowerBoundary,l",
        lowerBoundary // the variable to be filled
        ,
        lowerBoundary // the default
        ,
        "The lower boundary for the production of random numbers"
    );

    gpb.registerCLParameter<double>(
        "upperBoundary,u",
        upperBoundary // the variable to be filled
        ,
        upperBoundary // the default
        ,
        "The upper boundary for the production of random numbers"
    );

    gpb.registerCLParameter<std::string>(
        "source,s",
        source // the variable to be filled
        ,
        source // the default
        ,
        "The random source to benchmark: 'queue', 'local', 'staged' or 'quarantine'"
    );

    // Parse the command line and leave if the help flag was given
    if(Gem::Common::GCL_HELP_REQUESTED == gpb.parseCommandLine(argc, argv, true /*verbose*/)) {
        return 0;
    }

    //----------------------------------------------------------------

    // Configure the random number factory
    Gem::Hap::randomFactory()->setNProducerThreads(nProducerThreads);

    // Run the measurement loop for the requested source (the proxy is the only
    // thing that differs between sources; see run_measurement()).
    double throughput = 0.;
    if(source == "queue") {
        throughput = run_measurement<Gem::Hap::randomSource::QUEUE>(
            packageSize, nCycles, lowerBoundary, upperBoundary);
    }
    else if(source == "local") {
        throughput = run_measurement<Gem::Hap::randomSource::LOCAL>(
            packageSize, nCycles, lowerBoundary, upperBoundary);
    }
    else if(source == "staged") {
        throughput = run_measurement<Gem::Hap::randomSource::STAGED>(
            packageSize, nCycles, lowerBoundary, upperBoundary);
    }
    else if(source == "quarantine") {
        throughput = run_measurement<Gem::Hap::randomSource::QUARANTINE>(
            packageSize, nCycles, lowerBoundary, upperBoundary);
    }
    else {
        std::cerr << "Error: unknown source '" << source
                  << "' (expected 'queue', 'local', 'staged' or 'quarantine')" << '\n';
        return 1;
    }

    // Let the audience know
    double const megabytes = 8. * throughput / (1024 * 1024);
    std::cout << "[" << source << "] achieved a throughput of " << throughput
              << " double random numbers/s (equivalent to " << megabytes << " MB/s)" << '\n';
}
