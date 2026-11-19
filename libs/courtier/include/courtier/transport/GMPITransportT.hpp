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

// No guard on GEM_BUILD_WITH_MPI_CONSUMER: see courtier/mpi/GMPIEnvironment.hpp.

// Standard headers go here
#include <chrono>
#include <memory>

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/consumers/GMPIConsumerConfig.hpp" // the configuration of the family
#include "courtier/mpi/GMPICalls.hpp"                // the MPI calls of courtier's communicator
#include "courtier/mpi/GMPIEnvironment.hpp"          // the communicator and the role of a process
#include "courtier/transport/GMPIPeerTransport.hpp"  // the connector
#include "courtier/transport/GPeerClientT.hpp"       // the client over any connector

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * The client of the MPI consumer (GMPIConsumerT): a GPeerClientT over a GMPIConnector, built on a
 * process whose role in the GMPIEnvironment is CLIENT (Go2 builds it through
 * buildConsumerClient()). It talks to the submitter's rank (RANK_MASTER_NODE) on courtier's
 * communicator.
 *
 * It opens when the submitter's consumer has started (its greeting), and waits for that as long as
 * clientFirstContactLimit says (0: as long as it runs; a master that never starts MPI leaves it
 * waiting until the job ends). It then asks for work; with nothing to do its request waits at the
 * submitter, and the next item reaches it as soon as there is one. With the default depth of 2 it
 * holds the next item while it evaluates the current one.
 *
 * It ends when the submitter announces the end of the run, and when one of its own halt
 * conditions holds (a time or an item limit, a close request): it sends the results of the
 * evaluations that run and closes; the submitter goes on with the other clients and hands out
 * again what this one held. An MPI client has one connection for the whole run: a client whose
 * connection ends otherwise does not connect again (see GPeerClientT).
 *
 * The client must be held by a std::shared_ptr. It holds the environment, and with it MPI
 * initialized, for as long as it exists.
 *
 * @tparam processable_type The work-item type exchanged with the submitter (must be processable)
 */
template <typename processable_type>
class GMPIConsumerWorkerNodeT final : public GPeerClientT<processable_type> {
public:
    /// The configuration this client (and its server) is built from
    using config_type = MPIConsumerConfig;

    //-------------------------------------------------------------------------
    /**
     * @brief Builds the client on a process whose role is CLIENT.
     *
     * @param environment The process's MPI environment; must not be null
     * @param config Reads serializationMode, clientPrefetchDepth (n), clientParallelEvaluations
     *        (m; between 1 and n, else the client is refused) and clientFirstContactLimit.
     *        nHandlerThreads is the server's.
     * @throws gem_exception if @p environment is null or the role of this process is not
     *         CLIENT
     */
    GMPIConsumerWorkerNodeT(std::shared_ptr<GMPIEnvironment> environment, const config_type &config)
      : GPeerClientT<processable_type>(
            std::make_unique<GMPIConnector>(
                std::make_shared<GMPICalls>(requireClient_(std::move(environment))),
                GMPIPeerOptions{.name = "GMPIConsumerWorkerNodeT<> client"}
            ),
            GPeerClientOptions{
                .name = "GMPIConsumerWorkerNodeT<processable_type>",
                .mode = config.serializationMode,
                .prefetchDepth = config.clientPrefetchDepth,
                .parallelEvaluations = config.clientParallelEvaluations,
                .firstContactLimit = std::chrono::seconds(config.clientFirstContactLimit)
            }
        ) { /* nothing */
    }

    //-------------------------------------------------------------------------
    /** @brief The destructor */
    ~GMPIConsumerWorkerNodeT() override = default;

    //-------------------------------------------------------------------------
    // Deleted functions
    GMPIConsumerWorkerNodeT() = delete;
    GMPIConsumerWorkerNodeT(const GMPIConsumerWorkerNodeT<processable_type> &) = delete;
    GMPIConsumerWorkerNodeT(GMPIConsumerWorkerNodeT<processable_type> &&) = delete;
    GMPIConsumerWorkerNodeT<processable_type> &
    operator=(const GMPIConsumerWorkerNodeT<processable_type> &) = delete;
    GMPIConsumerWorkerNodeT<processable_type> &
    operator=(GMPIConsumerWorkerNodeT<processable_type> &&) = delete;

private:
    //-------------------------------------------------------------------------
    /**
     * @brief Returns @p environment if this process is a client of it; throws otherwise.
     * @param environment The process's MPI environment
     * @return The environment
     */
    static std::shared_ptr<GMPIEnvironment>
    requireClient_(std::shared_ptr<GMPIEnvironment> environment) {
        if(not environment) {
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIConsumerWorkerNodeT<>::GMPIConsumerWorkerNodeT(): Error!" << '\n'
                << "The MPI environment must not be empty" << '\n'
            );
        }
        if(environment->role() != Gem::Courtier::processRole::CLIENT) {
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIConsumerWorkerNodeT<>::GMPIConsumerWorkerNodeT(): Error!" << '\n'
                << "The MPI client is built on a process whose role is CLIENT only; this process "
                << "has the role " << static_cast<int>(environment->role()) << '\n'
            );
        }
        return environment;
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
