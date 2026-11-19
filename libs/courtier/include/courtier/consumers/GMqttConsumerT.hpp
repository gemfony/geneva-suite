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
#include "courtier/consumers/GMqttConsumerConfig.hpp"
#include "courtier/consumers/GPeerConsumerT.hpp"
#include "courtier/transport/GMqttPeerTransport.hpp" // the listener
#include "courtier/transport/GMqttTransportT.hpp"    // the client, for users of this header

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The courtier MQTT consumer: the server's shell (GPeerConsumerT) over a GMqttListener. Server and
 * clients talk through an MQTT broker; every client's connection is a pair of GMqttPeer ends, each
 * with an MQTT client of its own (see transport/GMqttPeerTransport.hpp). Its client is
 * GMqttClientT; both are configured by a GMqttConsumerConfig.
 *
 * startServer() connects to the broker and refuses, naming it, if the broker cannot be reached.
 * Threads: one thread moves the bytes of every connection; the sessions run on nThreads threads of
 * the shell's (0: one per hardware thread).
 *
 * The end of a run (stopServer()): every client is told that the run has ended and the server waits
 * for the connections to end, at most the stop grace (GMQTTCONSUMERSTOPGRACE by default).
 *
 * Lost clients: the broker publishes the will of a client it has lost -- at once for a process that
 * died, after one and a half keep-alive intervals for one that hangs -- and the session ends; what
 * the client held is put back at once.
 *
 * @tparam processable_type The work-item type handed to clients and reconciled back into the batch
 */
template <typename processable_type>
class GMqttConsumerT final : public GPeerConsumerT<processable_type> {
public:
    /// The configuration this consumer (and its client) is built from
    using config_type = GMqttConsumerConfig;

    /***************************************************************************/
    /** @brief Constructs the MQTT consumer from the configuration of its family.
     *
     *  @param config Reads ip, port and topic (the broker and the topic prefix), nThreads (the
     *         threads of the sessions; 0 selects the hardware concurrency), serializationMode and
     *         pingInterval (the MQTT keep-alive of the server's ends, in seconds; 0: off). The
     *         client members are ignored.
     *  @param stop_grace How long stopServer() waits for the connections to end */
    explicit GMqttConsumerT(
        const config_type &config,
        std::chrono::milliseconds stop_grace = GMQTTCONSUMERSTOPGRACE
    )
      : GPeerConsumerT<processable_type>(
            CONSUMER_NAME,
            std::make_unique<Consumers::GMqttListener>(
                CONSUMER_NAME,
                Consumers::GMqttOptions{
                    .name = "GMqttConsumerT<> session",
                    .host = config.ip,
                    .port = config.port,
                    .topic = config.topic,
                    .keepAlive = std::chrono::seconds(config.pingInterval)
                },
                stop_grace
            ),
            config.nThreads,
            config.serializationMode
        ) { /* nothing */
    }

    /** @brief Stops the server while this object is still complete. */
    ~GMqttConsumerT() override {
        this->stopServerNoThrow_();
    }

    GMqttConsumerT(const GMqttConsumerT &) = delete;
    GMqttConsumerT(GMqttConsumerT &&) = delete;
    GMqttConsumerT &operator=(const GMqttConsumerT &) = delete;
    GMqttConsumerT &operator=(GMqttConsumerT &&) = delete;

private:
    /// The name of this consumer, for the log and the thread budget
    static constexpr const char *CONSUMER_NAME = "Gem::Courtier::GMqttConsumerT";
};

/******************************************************************************/

} /* namespace Gem::Courtier */
