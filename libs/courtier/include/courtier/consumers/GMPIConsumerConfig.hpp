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
#include <cstdint>
#include <string_view>
#include <thread>

// Geneva header files go here
#include "common/GCommonEnums.hpp"     // serializationMode
#include "courtier/GCourtierEnums.hpp" // GMPICONSUMERMASTERCLEANSESSINTERVALMSEC

// No MPI header: a program can fill the configuration of the MPI consumer without seeing MPI (the
// consumer itself is in courtier/consumers/GMPIConsumerT.hpp).

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * The configuration of the MPI consumer GMPIConsumerT and of its client (mnemonic "mpi"), used by
 * the master node and the worker nodes alike. A plain aggregate, shared by the consumer and its
 * client and to be copied freely; it can be written with designated initializers.
 */
struct MPIConsumerConfig {
    /// The mnemonic of the consumer family this struct configures, as chosen with --consumer
    static constexpr std::string_view MNEMONIC = "mpi";

    /// Whether the clients ask for the next work item while the current one is still processed
    bool useAsyncReq{true};
    /// The serialization of the messages between the master node and the worker nodes
    Gem::Common::serializationMode serializationMode{Gem::Common::serializationMode::GEM_BINARY};
    /// The number of threads of the pool that handles incoming requests. Starts as the
    /// recommendation for this machine (nHandlerThreadsRecommendation()).
    std::uint32_t nHandlerThreads{nHandlerThreadsRecommendation()};
    /// The time in ms between two checks whether sending a work item to a worker node completed
    std::uint32_t masterCleanSessIntervalMSec{GMPICONSUMERMASTERCLEANSESSINTERVALMSEC};

    /**
     * @brief Recommends a number of handler threads from the available hardware concurrency.
     * @return The number of hardware threads, or 8 if the C++ runtime cannot determine it
     */
    [[nodiscard]] static std::uint32_t nHandlerThreadsRecommendation() {
        // how many hardware threads are available (0 if unknown)
        const unsigned int hw_threads{std::thread::hardware_concurrency()};

        // if hint has returned 0, default to 8
        return hw_threads != 0 ? hw_threads : 8;
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
