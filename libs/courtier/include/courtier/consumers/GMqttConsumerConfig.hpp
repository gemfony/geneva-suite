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
#include <string>
#include <string_view>

// Geneva header files go here
#include "common/GCommonEnums.hpp"     // serializationMode
#include "courtier/GCourtierEnums.hpp" // GMQTTDEFAULT*, GBEASTCONSUMERPINGINTERVAL

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The configuration of the MQTT consumer GMqttConsumerT and of its client GMqttClientT
 * (mnemonic "mqtt"): server and clients talk through an MQTT broker (see
 * transport/GMqttPeerTransport.hpp).
 *
 * Server and client are built from the same struct; each member says which side reads it. A plain
 * aggregate, to be copied freely. It includes no network library, so a program can name the
 * consumer by its configuration without seeing the consumer's header.
 */
struct GMqttConsumerConfig {
    /// The mnemonic of the consumer family this struct configures, as chosen with --consumer
    static constexpr std::string_view MNEMONIC = "mqtt";

    /// Both: the name or address of the broker
    std::string ip = GMQTTDEFAULTBROKER;
    /// Both: the broker's port
    unsigned short port = GMQTTDEFAULTPORT;
    /// Both: the prefix of the topics; a server and its clients use the same one, and servers on
    /// one broker use different ones
    std::string topic = GMQTTDEFAULTTOPIC;
    /// Both: the serialization of work items on the wire
    Gem::Common::serializationMode serializationMode = GCONSUMERSERIALIZATIONMODE;
    /// Server: the number of threads the sessions run on (they encode and decode the messages); 0:
    /// as many as the machine has hardware threads. The transport moves the bytes on one thread of
    /// its own.
    std::size_t nThreads = GCONSUMERLISTENERTHREADS;
    /// Both: the MQTT keep-alive in seconds (0: off). The broker publishes the will of an end it
    /// has not heard from within one and a half intervals, which ends the other end. The server
    /// names its value to its clients; a client keeps its own (it is fixed when it connects).
    std::size_t pingInterval = GBEASTCONSUMERPINGINTERVAL;
    /// Client: n, how many work items it holds at once (see GWebsocketConsumerConfig)
    std::size_t clientPrefetchDepth = 1;
    /// Client: m, how many of them it evaluates at the same time; from 1 to clientPrefetchDepth
    std::size_t clientParallelEvaluations = 1;
    /// Client: seconds it tries to reach a server before its first contact with one (0: for as
    /// long as it runs)
    std::size_t clientFirstContactLimit =
        static_cast<std::size_t>(GCLIENTFIRSTCONTACTLIMIT.count());
    /// Client: seconds it tries to reach a server again after it has lost its connection (0: for
    /// as long as it runs)
    std::size_t clientReconnectLimit = static_cast<std::size_t>(GCLIENTRECONNECTLIMIT.count());
};

/******************************************************************************/

} /* namespace Gem::Courtier */
