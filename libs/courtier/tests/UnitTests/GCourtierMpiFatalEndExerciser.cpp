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

// A process that ends on a configuration error ends the whole MPI job, also when other processes
// wait for it. Run on three ranks under mpirun: every rank initializes MPI through courtier's
// environment (as a Go2 program asks for its role), rank 1 then applies a configuration with an
// entry of the wrong form, and the other ranks wait for rank 1 at a barrier, as a submitter's
// clients wait for its greeting. main() hands the error to Gem::Common::reportFatal(), which
// prints it and calls the end hook courtier's MPI side registered: the job ends with the exit code
// 1 at once. A return without the hook would leave the waiting ranks, and with them the job,
// hanging; an uncaught exception would abort with a core dump instead of the message (the test
// that runs this checks the exit code, the message and a time limit).

// Standard headers go here
#include <iostream>
#include <tuple>

// Boost headers go here
#include <boost/json.hpp>

// MPI headers go here
#include <mpi.h>

// Geneva headers go here
#include "common/GFatalReport.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/mpi/GMPIEnvironmentAccess.hpp"

int main() try {
    // MPI is initialized, and the environment kept, without a holder in main()
    std::ignore = Gem::Courtier::mpiProcessRole();
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if(rank == 1) {
        Gem::Common::GParserBuilder gpb;
        bool scan_randomly = true;
        gpb.registerFileParameter<bool>("scan_randomly", scan_randomly, true);
        gpb.loadFromDocument(boost::json::parse(R"({ "scan_randomly": false })"));
        std::cout << "FAIL: rank 1 applied an entry of the wrong form\n";
        return 0;
    }

    // Rank 1 never arrives
    MPI_Barrier(MPI_COMM_WORLD);
    std::cout << "FAIL: rank " << rank << " passed the barrier without rank 1\n";
    return 0;
}
catch(...) {
    return Gem::Common::reportFatal();
}
