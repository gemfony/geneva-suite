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

// Standard headers go here
#include <chrono>
#include <memory>

// Geneva headers go here
#include "courtier/consumers/GWebsocketConsumerConfig.hpp" // the configuration of the family
#include "courtier/transport/GPeerClientT.hpp"             // the client over any connector
#include "courtier/transport/GWebsocketPeerTransport.hpp"  // the connector

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * The client of the courtier websocket consumer (GWebsocketConsumerT): a GPeerClientT over a
 * GWebsocketConnector. The connection is kept open while items are evaluated; the keep-alive goes
 * on meanwhile, so an evaluation may take any length of time, and a server that has fallen silent
 * is noticed by the idle limit: the connection ends and the client connects again (see
 * GPeerClientT). A client that is told that the run has ended ends; one that cannot read a message
 * tells the server and stays.
 *
 * It is configured by a GWebsocketConsumerConfig, the struct its server is built from too. The
 * client must be held by a std::shared_ptr.
 *
 * @tparam processable_type The work-item type exchanged with the server (must be processable)
 */
template <typename processable_type>
class GWebsocketClientT final : public GPeerClientT<processable_type> {
public:
    /// The configuration this client (and its server) is built from
    using config_type = Gem::Courtier::GWebsocketConsumerConfig;

    //-------------------------------------------------------------------------
    /**
     * @brief Initialization from the configuration of the websocket family.
     *
     * @param config Reads ip (the name or address of the server), port (the server's port),
     *        serializationMode (the wire format), verboseControlFrames (log every control frame
     *        received), clientPrefetchDepth (n: how many work items the client holds at once; 0
     *        is treated as 1), clientParallelEvaluations (m: how many of them it evaluates at the
     *        same time; between 1 and n, else the client is refused), pingInterval (seconds of silence after which a ping is sent;
     *        after twice that time without any inbound data the connection ends; 0 switches this
     *        off), clientFirstContactLimit (seconds the client tries to reach the server before
     *        it gives up; 0: no limit) and clientReconnectLimit (seconds it tries to reach it again
     *        after a lost connection; 0: no limit). nThreads is ignored.
     */
    explicit GWebsocketClientT(const config_type &config)
      : GPeerClientT<processable_type>(
            std::make_unique<GWebsocketConnector>(
                config.ip,
                config.port,
                GWebsocketPeer::GOptions{
                    .name = "GWebsocketClientT<> client",
                    .mode = config.serializationMode,
                    .verboseControlFrames = config.verboseControlFrames,
                    .pingInterval = std::chrono::seconds(config.pingInterval)
                }
            ),
            GPeerClientOptions{
                .name = "GWebsocketClientT<processable_type>",
                .mode = config.serializationMode,
                .prefetchDepth = config.clientPrefetchDepth,
                .parallelEvaluations = config.clientParallelEvaluations,
                .firstContactLimit = std::chrono::seconds(config.clientFirstContactLimit),
                .reconnectLimit = std::chrono::seconds(config.clientReconnectLimit)
            }
        ) { /* nothing */
    }

    //-------------------------------------------------------------------------
    /** @brief The destructor */
    ~GWebsocketClientT() override = default;

    //-------------------------------------------------------------------------
    // Deleted functions
    GWebsocketClientT() = delete;
    GWebsocketClientT(const GWebsocketClientT<processable_type> &) = delete;
    GWebsocketClientT(GWebsocketClientT<processable_type> &&) = delete;
    GWebsocketClientT<processable_type> &
    operator=(const GWebsocketClientT<processable_type> &) = delete;
    GWebsocketClientT<processable_type> &operator=(GWebsocketClientT<processable_type> &&) = delete;
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
