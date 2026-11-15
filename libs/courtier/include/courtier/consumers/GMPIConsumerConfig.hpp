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

// Boost header files go here
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>

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
     * @brief Registers this config's command-line options into the given option descriptions.
     *
     * The current member values are used as the option defaults (a default-constructed instance
     * already holds the intended defaults), avoiding duplicating the default values here.
     *
     * @param visible The options_description receiving user-facing options
     * @param hidden The options_description receiving advanced/hidden options
     */
    void addCLOptions_(
        boost::program_options::options_description &visible,
        boost::program_options::options_description &hidden
    ) {
        // The current values of the members are the option defaults: a default-constructed instance
        // holds the intended defaults already, so they are not stated a second time here.
        using boost::program_options::value;

        visible.add_options()(
            "mpi_asyncReq",
            value<bool>(&useAsyncReq)->default_value(useAsyncReq),
            "\t[mpi] Whether the clients should issue a request for the next work item while"
            "the current work item is still being processed"
        );

        visible.add_options()(
            "mpi_nHandlerThreads",
            value<std::uint32_t>(&nHandlerThreads)->default_value(nHandlerThreads),
            "\t[mpi] The number of threads in a thread pool which is used to handle incoming "
            "requests."
            "Defaults to the number of CPU cores in the system, if able to determine by C++ runtime"
        );

        hidden.add_options()(
            "mpi_cleanSessInterval",
            value<std::uint32_t>(&masterCleanSessIntervalMSec)
                ->default_value(masterCleanSessIntervalMSec),
            "\t[mpi] The time interval in ms between each check for the completion "
            "of active sessions on the master node."
        );

        hidden.add_options()(
            "mpi_serializationMode",
            value<Gem::Common::serializationMode>(&serializationMode)
                ->default_value(serializationMode),
            "\t[mpi] Specifies whether serialization shall be done in TEXTMODE (0), XMLMODE (1), "
            "BINARYMODE (2), GEM_BINARYMODE (3) or GEM_JSONMODE (4)"
        );
    }

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
