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

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Geneva header files go here
#include "common/GCommonEnums.hpp"     // serializationMode
#include "courtier/GCourtierEnums.hpp" // GCLIENTFIRSTCONTACTLIMIT, GMPICONSUMERSESSIONTHREADS

// No MPI header: a program can fill the configuration of the MPI consumer without seeing MPI (the
// consumer itself is in courtier/consumers/GMPIConsumerT.hpp).

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * The configuration of the MPI consumer GMPIConsumerT and of its client GMPIConsumerWorkerNodeT
 * (mnemonic "mpi"). Server and client are built from the same struct; each member says which side
 * reads it. A plain aggregate, to be copied freely; it can be written with designated
 * initializers.
 */
struct MPIConsumerConfig {
    /// The mnemonic of the consumer family this struct configures, as chosen with --consumer
    static constexpr std::string_view MNEMONIC = "mpi";

    /// Both: the serialization of the messages between the submitter and its clients
    Gem::Common::serializationMode serializationMode{Gem::Common::serializationMode::GEM_BINARY};
    /// Server: the number of threads the sessions run on (they encode and decode the messages);
    /// 0: as many as the machine has hardware threads. The poll that receives for every session
    /// runs on a thread of its own besides them.
    std::uint32_t nHandlerThreads{GMPICONSUMERSESSIONTHREADS};
    /// Server: the keep-alive interval in seconds (0: off). Every rank is sent a sign of life that
    /// often; a rank not heard from for more than twice that long is lost, and the job is ended
    /// where MPI would be finalized. The clients take it from the HELLO.
    std::size_t pingInterval = GBEASTCONSUMERPINGINTERVAL;
    /// Client: n, how many work items it holds at once -- the one(s) being evaluated and those
    /// fetched ahead, so that the transfer of the next item overlaps the evaluation of the
    /// current one (1: it asks for the next item when it returns the current one). How many items
    /// it evaluates at the same time is clientParallelEvaluations.
    std::size_t clientPrefetchDepth = 2;
    /// Client: m, how many of the items it holds (clientPrefetchDepth) it evaluates at the same
    /// time; 1 by default; from 1 to clientPrefetchDepth
    std::size_t clientParallelEvaluations = 1;
    /// Client: seconds it waits for the submitter's greeting before it gives up (0: for as long
    /// as it runs)
    std::size_t clientFirstContactLimit =
        static_cast<std::size_t>(GCLIENTFIRSTCONTACTLIMIT.count());
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
