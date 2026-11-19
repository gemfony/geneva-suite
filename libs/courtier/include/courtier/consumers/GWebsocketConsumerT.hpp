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

#include "common/GGlobalDefines.hpp"

// Standard headers
#include <chrono>
#include <memory>

// Geneva headers
#include "courtier/GCourtierEnums.hpp"
#include "courtier/consumers/GPeerConsumerT.hpp"
#include "courtier/consumers/GWebsocketConsumerConfig.hpp"
#include "courtier/transport/GWebsocketPeerTransport.hpp" // the listener
#include "courtier/transport/GWebsocketTransportT.hpp"    // the client, for users of this header

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The courtier websocket consumer: the server's shell (GPeerConsumerT) over a GWebsocketListener.
 * Every connection is a GWebsocketPeer with keep-alive; its GPeerSessionT hands out work from the
 * current batch and collects results. Its client is GWebsocketClientT; both are configured by a
 * GWebsocketConsumerConfig.
 *
 * Threads. nThreads I/O threads (the listener's) move the bytes and run nothing else; the sessions
 * run on as many threads of the shell's (0: one of each per hardware thread). A session may take
 * as long as it likes over a message without its connection, or any other, missing a ping.
 *
 * The end of a run (stopServer()): every client is told that the run has ended (close code
 * GCONSUMERCLOSECODERUNENDED) and the server waits for the connections to end, at most the stop
 * grace (GWEBSOCKETCONSUMERSTOPGRACE by default); see GPeerConsumerT.
 *
 * Lost clients: with the keep-alive on (a ping interval above 0) a lost client ends its connection
 * by the idle limit and what it held is put back at once; without it a client that vanishes
 * without a TCP reset is not noticed. There is no time limit by default: a client that hangs
 * inside an evaluation while its process answers is not noticed unless the user sets
 * reclaim_lease_ms (GNetworkedTimeoutConfig).
 *
 * @tparam processable_type The work-item type handed to clients and reconciled back into the batch
 */
template <typename processable_type>
class GWebsocketConsumerT final : public GPeerConsumerT<processable_type> {
public:
    /// The configuration this consumer (and its client) is built from
    using config_type = GWebsocketConsumerConfig;

    /***************************************************************************/
    /** @brief Constructs the websocket consumer from the configuration of its family.
     *
     *  @param config Reads port (0 lets the OS pick; the chosen port is read back with getPort()
     *         after startServer()), nThreads (the I/O threads, and as many threads for the
     *         sessions; 0 selects the hardware concurrency), serializationMode (the wire format),
     *         pingInterval (seconds of silence after which a ping is sent; after twice that time
     *         without any inbound data the client is presumed gone; 0 switches this off) and
     *         verboseControlFrames. The client members (ip, clientPrefetchDepth, ...) are ignored.
     *  @param stop_grace How long stopServer() waits for the connections to end */
    explicit GWebsocketConsumerT(
        const config_type &config,
        std::chrono::milliseconds stop_grace = GWEBSOCKETCONSUMERSTOPGRACE
    )
      : GPeerConsumerT<processable_type>(
            CONSUMER_NAME,
            std::make_unique<Consumers::GWebsocketListener>(
                CONSUMER_NAME,
                config.port,
                Consumers::GWebsocketPeer::GOptions{
                    .name = "GWebsocketConsumerT<> session",
                    .mode = config.serializationMode,
                    .verboseControlFrames = config.verboseControlFrames,
                    .pingInterval = std::chrono::seconds(config.pingInterval)
                },
                config.nThreads,
                stop_grace
            ),
            config.nThreads,
            config.serializationMode
        ) { /* nothing */
    }

    /** @brief Stops the server while this object is still complete. */
    ~GWebsocketConsumerT() override {
        this->stopServerNoThrow_();
    }

    GWebsocketConsumerT(const GWebsocketConsumerT &) = delete;
    GWebsocketConsumerT(GWebsocketConsumerT &&) = delete;
    GWebsocketConsumerT &operator=(const GWebsocketConsumerT &) = delete;
    GWebsocketConsumerT &operator=(GWebsocketConsumerT &&) = delete;

    /***************************************************************************/
    /** @brief The port the server listens on (useful when 0 was passed to pick an ephemeral port).
     *  @return The port asked for before startServer(), the bound one afterwards */
    [[nodiscard]] unsigned short getPort() const noexcept {
        return static_cast<const Consumers::GWebsocketListener &>(this->listener()).getPort();
    }

private:
    /// The name of this consumer, for the log and the thread budget
    static constexpr const char *CONSUMER_NAME = "Gem::Courtier::GWebsocketConsumerT";
};

/******************************************************************************/

} /* namespace Gem::Courtier */
