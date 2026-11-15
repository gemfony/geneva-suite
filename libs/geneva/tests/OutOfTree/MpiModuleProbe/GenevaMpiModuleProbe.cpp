/**
 * @file GenevaMpiModuleProbe.cpp
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

/**
 * An out-of-tree program against an installed Geneva with the MPI module (see CMakeLists.txt beside it).
 * It exits with 0 if the "mpi" consumer is available, and otherwise prints why the module could not be
 * loaded and exits with 1. It is compiled without any definition of the collection's own build (in
 * particular without GENEVA_BUILD_WITH_MPI_CONSUMER), as a program outside the tree is.
 */

#include <cstdio>

#include "courtier/mpi/GMPIEnvironmentAccess.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/GMPISubClientOptimizer.hpp"

int main(int argc, char **argv) {
    // Linked: the sub-client optimizer from libgemfony-geneva-mpi and the environment's functions
    // from libgemfony-courtier-mpi. Neither is called here -- that would initialize MPI -- but the
    // references make the links real.
    if(argc > 1000) {
        Gem::Geneva::GMPISubClientOptimizer const optimizer(argc, argv, "./config/Go2.json");
        return static_cast<int>(Gem::Courtier::mpiProcessRole());
    }

    // Loaded at run time: the "mpi" consumer, from the module installed next to libgemfony-geneva.
    if(not Gem::Geneva::isKnownConsumer("mpi")) {
        std::fputs(Gem::Geneva::optionalConsumerNotes().c_str(), stderr);
        std::fputs("GenevaMpiModuleProbe: the mpi consumer is not available\n", stderr);
        return 1;
    }
    std::puts("GenevaMpiModuleProbe: the mpi consumer is available");
    return 0;
}
