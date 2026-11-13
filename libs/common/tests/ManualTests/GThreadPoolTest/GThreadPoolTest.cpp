/**
 * @file GThreadPoolTest.cpp
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

// Standard headers go here
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>

// Boost headers go here

// Geneva headers go here
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "common/concurrency/GThreadPool.hpp"
#include "hap/GRandomDistributionsT.hpp"
#include "hap/GRandomT.hpp"

using namespace Gem::Common;
using namespace Gem::Common::Concurrency;

const std::size_t NRESIZEEVENTS = 0;
const std::size_t NJOBS = 100;
const std::size_t NITERATIONS = 5;
const unsigned int MINTHREADS = 1;
const unsigned int MAXTHREADS = 20;
const unsigned int NINITIALTHREADS = 4;

Gem::Common::Concurrency::GThreadPool gtp{NINITIALTHREADS}; ///< The global threadpool

/************************************************************************/
/**
 * A simple test task that sets a flag when process() has been called
 */
class testTask {
public:
    /********************************************************************/
    /**
	 * The default constructor
	 */
    testTask()
      : counter_value_(0)
      , process_called_(0) { /* nothing */
    }

    /********************************************************************/
    /**
	 * Returns the current counter value (net result of increment/decrement calls)
	 */
    [[nodiscard]] std::int32_t getCounterValue() const {
        return counter_value_;
    }

    /********************************************************************/
    /**
	 * Retrieves the number of process() calls
	 */
    [[nodiscard]] std::uint32_t getProcessCalledValue() const {
        return process_called_;
    }

    /********************************************************************/
    /**
	 * Performs work on this object. This is the function to be executed
	 * inside of the threads
	 */
    void process(bool simulateCrash) {
        if(uniform_bool_(gr_)) {
            this->increment();
        }
        else {
            this->decrement();
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(
                this->uniform_int_(gr_, std::uniform_int_distribution<long>::param_type(10, 20))
            )
        );

        if(true == simulateCrash) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In testTask::process(): Error!" << '\n'
                << "SHF-Exception (Some Horrible Failure)" << '\n'
                << "occurred, as requested ..." << '\n'
            );
        }
    }

private:
    /********************************************************************/
    /**
	 * Increments the local counter
	 */
    void increment() {
        counter_value_++;
        process_called_++;
    }

    /********************************************************************/
    /**
	 * Decrements the local counter
	 */
    void decrement() {
        counter_value_--;
        process_called_++;
    }

    /********************************************************************/
    std::int32_t counter_value_; ///< The internal value to be decremented or incremented
    std::uint32_t
        process_called_; ///< This counter will be incremented whenever process() is called

    Gem::Hap::GRandom gr_; // Instantiates a random number generator
    std::uniform_int_distribution<long> uniform_int_;
    std::bernoulli_distribution uniform_bool_; // probability of 0.5 is the default
};

/************************************************************************/
/**
 * This test tries to ascertain that GThreadPool works as expected. It submits
 * a given number of jobs to the pool, waits for their execution and submits them again
 * a user-definable number of times.
 *
 * TODO: Extract futures and check for errors
 */
// NOLINTNEXTLINE(readability-function-size) -- manual-test main: CLI setup, task submission loop, and result verification form one linear test scenario
int main(int argc, char **argv) {
    Gem::Hap::GRandom gr; // Instantiates a random number generator
    std::uniform_int_distribution<unsigned int> uniform_int_;

    //----------------------------------------------------------------
    // Local variables
    bool simulateThreadCrash = false;
    std::size_t nResizeEvents = NRESIZEEVENTS;
    std::size_t nJobs = NJOBS;             // The number of tasks in each iteration
    std::size_t nIterations = NITERATIONS; // The default number of iterations
    bool showCLOptions = false; // When set to true, will show a summary of command line options

    //----------------------------------------------------------------
    // Create the parser builder -- needed for command line parsing
    Gem::Common::GParserBuilder gpb;

    // Register some command line options
    gpb.registerCLParameter<std::size_t>("nJobs,j", nJobs, NJOBS)
        << "The number of testTask objects on which work is performed";

    gpb.registerCLParameter<std::size_t>("nIterations,i", nIterations, NITERATIONS)
        << "The number of test iteration";

    gpb.registerCLParameter<std::size_t>("nResizeEvents,r", nResizeEvents, NRESIZEEVENTS)
        << "Tests random resizing of the thread pool \"nResizeEvents\" times";

    gpb.registerCLParameter<bool>(
        "simulateThreadCrash,s",
        simulateThreadCrash,
        false // the default value
        ,
        "When set to true, simulates the crash of a single thread",
        true // implicit allowed (i.e. "-s" without argument)
        ,
        true // the implicit value
    );

    gpb.registerCLParameter<bool>(
        "showCLOptions,o",
        showCLOptions,
        false // the default value
        ,
        "When set to true, shows a summary of command line options",
        true // implicit allowed (i.e. "-o" without argument)
        ,
        true // the implicit value
    );

    // Parse the command line and leave if the help flag was given
    if(Gem::Common::GCL_HELP_REQUESTED == gpb.parseCommandLine(argc, argv, showCLOptions)) {
        return 0;
    }

    //----------------------------------------------------------------
    // Start measurements

    // Create a number of test tasks
    std::vector<std::shared_ptr<testTask>> tasks(nJobs);
    for(std::size_t i = 0; i < nJobs; i++) {
        tasks.at(i).reset(new testTask);
    }

    // Submit each task to the pool a number of times
    double const resizeLikelihood = (std::min)(static_cast<double>(nResizeEvents) / static_cast<double>(nIterations), 1.);
    std::bernoulli_distribution weighted_bool(resizeLikelihood);

    for(std::size_t n = 0; n < nIterations; n++) {
        // Submission number n
        for(std::size_t i = 0; i < nJobs; i++) {
            bool stc = false;
            if(i == nJobs - 1 && n == nIterations - 1 && true == simulateThreadCrash) {
                stc = true;
            }

            gtp.async_schedule([&tasks, i, stc]() { (tasks.at(i))->process(stc); });
        }

        if(nResizeEvents > 0 && weighted_bool(gr)) {
            unsigned int const nt = uniform_int_(
                gr,
                std::uniform_int_distribution<unsigned int>::param_type(MINTHREADS, MAXTHREADS)
            );
            gtp.setNThreads(nt);

            glogger << "Resized thread pool to size " << nt << '\n' << GLOGGING;
        }

        // Wait for all tasks to complete and check for errors
        gtp.wait();
    }

    // Check that each task has been called exactly nIterations times
    for(std::size_t i = 0; i < nJobs; i++) {
        if(nIterations != (tasks.at(i))->getProcessCalledValue()) {
            glogger << "In task " << i << ":" << '\n'
                    << "Got wrong number of calls: " << (tasks.at(i))->getProcessCalledValue()
                    << "." << '\n'
                    << GLOGGING;
        }
    }
}
