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

// A process that ends on an error nobody handles ends the whole MPI job, also when other processes
// wait for it. Run on three ranks under mpirun: every rank initializes MPI through courtier's
// environment (as a GOptimizer program asks for its role), rank 1 then fails, and the other ranks
// wait for rank 1 at a barrier, as a submitter's clients wait for its greeting. main() installs the
// fatal report (Gem::Common::installFatalReport()) first, as GOptimizer does: the exception reaches
// it without unwinding, it prints the message and ends rank 1 with the exit code 1, and the
// launcher ends the job. The scenario is the first argument:
//  - "config": rank 1 applies a configuration entry of the wrong form; the environment is kept
//    without a holder in main();
//  - "holder": the same, while a local of main() holds the environment -- an unwinding would
//    finalize MPI there and wait for the other ranks;
//  - "thread": a thread of rank 1 throws, which no handler of main() could catch;
//  - "fault": rank 1 calls std::terminate() with no exception in flight, as an internal fault
//    does: the handler prints its line and aborts.
// The test that runs this checks the exit code, the message and a time limit.

// Standard headers go here
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

// Boost headers go here
#include <boost/json.hpp>

// MPI headers go here
#include <mpi.h>

// Geneva headers go here
#include "common/GFatalReport.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/mpi/GMPIEnvironmentAccess.hpp"

namespace {

/** @brief Applies a configuration entry of the wrong form, which throws */
void applyWrongEntry() {
    Gem::Common::GParserBuilder gpb;
    bool scan_randomly = true;
    gpb.registerFileParameter<bool>("scan_randomly", scan_randomly, true);
    gpb.loadFromDocument(boost::json::parse(R"({ "scan_randomly": false })"));
}

} // namespace

int main(int argc, char **argv) {
    Gem::Common::installFatalReport();
    const std::string scenario = argc > 1 ? argv[1] : "config";

    // MPI is initialized; the environment is kept by courtier, and by main() for "holder"
    std::ignore = Gem::Courtier::mpiProcessRole();
    const auto holder = scenario == "holder" ? Gem::Courtier::mpiEnvironment() : nullptr;
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if(rank == 1) {
        if(scenario == "fault") {
            std::terminate();
        }
        if(scenario == "thread") {
            std::thread failing([] { throw std::runtime_error("a thread of rank 1 failed"); });
            failing.join();
        }
        else {
            applyWrongEntry();
        }
        std::cout << "FAIL: rank 1 went on after its error\n";
        return 0;
    }

    // Rank 1 never arrives
    MPI_Barrier(MPI_COMM_WORLD);
    std::cout << "FAIL: rank " << rank << " passed the barrier without rank 1\n";
    return 0;
}
