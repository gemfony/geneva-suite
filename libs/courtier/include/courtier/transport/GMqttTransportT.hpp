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
#include "courtier/consumers/GMqttConsumerConfig.hpp" // the configuration of the family
#include "courtier/transport/GMqttPeerTransport.hpp"  // the connector
#include "courtier/transport/GPeerClientT.hpp"        // the client over any connector

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * The client of the courtier MQTT consumer (GMqttConsumerT): a GPeerClientT over a GMqttConnector.
 * Every connection is a new MQTT client with a new id, which joins the server through the broker.
 * A connection whose broker connection fails, or whose server's will arrives, ends, and the client
 * connects again (see GPeerClientT); one that hears no server within twice its keep-alive (no
 * server took its join, say because none had started yet) ends too, and the client tries again. A
 * client that is told that the run has ended ends.
 *
 * It is configured by a GMqttConsumerConfig, the struct its server is built from too. The client
 * must be held by a std::shared_ptr.
 *
 * @tparam processable_type The work-item type exchanged with the server (must be processable)
 */
template <typename processable_type>
class GMqttClientT final : public GPeerClientT<processable_type> {
public:
    /// The configuration this client (and its server) is built from
    using config_type = Gem::Courtier::GMqttConsumerConfig;

    //-------------------------------------------------------------------------
    /**
     * @brief Initialization from the configuration of the MQTT family.
     *
     * @param config Reads ip, port and topic (the broker and the topic prefix of its server),
     *        serializationMode, pingInterval (the MQTT keep-alive of its connections, in seconds;
     *        0: off), clientPrefetchDepth, clientParallelEvaluations, clientFirstContactLimit and
     *        clientReconnectLimit (see GMqttConsumerConfig). nThreads is ignored.
     */
    explicit GMqttClientT(const config_type &config)
      : GPeerClientT<processable_type>(
            std::make_unique<GMqttConnector>(GMqttOptions{
                .name = "GMqttClientT<> client",
                .host = config.ip,
                .port = config.port,
                .topic = config.topic,
                .keepAlive = std::chrono::seconds(config.pingInterval)
            }),
            GPeerClientOptions{
                .name = "GMqttClientT<processable_type>",
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
    ~GMqttClientT() override = default;

    //-------------------------------------------------------------------------
    // Deleted functions
    GMqttClientT() = delete;
    GMqttClientT(const GMqttClientT<processable_type> &) = delete;
    GMqttClientT(GMqttClientT<processable_type> &&) = delete;
    GMqttClientT<processable_type> &operator=(const GMqttClientT<processable_type> &) = delete;
    GMqttClientT<processable_type> &operator=(GMqttClientT<processable_type> &&) = delete;
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
