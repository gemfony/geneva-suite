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
#include "courtier/GCourtierEnums.hpp" // GBEASTCONSUMERPINGINTERVAL

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The configuration of the websocket consumer GWebsocketConsumerT and of its client
 * GWebsocketClientT (mnemonic "beast").
 *
 * Server and client are built from the same struct; each member says which side reads it. A
 * plain aggregate, to be copied freely. It includes no network library, so a program can name the
 * consumer by its configuration without seeing the consumer's header.
 */
struct GWebsocketConsumerConfig {
    /// The mnemonic of the consumer family this struct configures, as chosen with --consumer
    static constexpr std::string_view MNEMONIC = "beast";

    /// Client: the name or address of the server
    std::string ip = GCONSUMERDEFAULTSERVER;
    /// Server: the port to listen on (0: a free port, see getPort()). Client: the server's port.
    /// The command line's default is GCONSUMERDEFAULTPORT instead: there server and client are
    /// two processes and have to agree on a port without being told
    unsigned short port = 0;
    /// Both: the serialization of work items on the wire
    Gem::Common::serializationMode serializationMode = GCONSUMERSERIALIZATIONMODE;
    /// Server: the number of io threads, and of the threads the sessions run on (they encode and
    /// decode the messages); 0: as many as the machine has hardware threads
    std::size_t nThreads = GCONSUMERLISTENERTHREADS;
    /// Both: seconds of silence after which a ping is sent; the connection is closed after
    /// twice that time without inbound data (0: off)
    std::size_t pingInterval = GBEASTCONSUMERPINGINTERVAL;
    /// Both: whether ping, pong and close frames are announced in the log
    bool verboseControlFrames = false;
    /// Client: n, how many work items it holds at once -- the one(s) being evaluated and those
    /// fetched ahead, so that the transfer of the next item overlaps the evaluation of the
    /// current one (1: it asks for the next item when it returns the current one). How many it
    /// evaluates at the same time is clientParallelEvaluations.
    std::size_t clientPrefetchDepth = 1;
    /// Client: m, how many of the items it holds (clientPrefetchDepth) it evaluates at the same
    /// time; 1 by default, whatever the depth; from 1 to clientPrefetchDepth
    std::size_t clientParallelEvaluations = 1;
    /// Client: seconds it tries to reach the server before its first contact with it (0: for as
    /// long as it runs)
    std::size_t clientFirstContactLimit =
        static_cast<std::size_t>(GCLIENTFIRSTCONTACTLIMIT.count());
    /// Client: seconds it tries to reach the server again after it has lost its connection (0: for
    /// as long as it runs)
    std::size_t clientReconnectLimit = static_cast<std::size_t>(GCLIENTRECONNECTLIMIT.count());
    /// Client: where a finished result that can no longer be delivered is saved (its evaluated
    /// item, one file each; see GPeerClientT)
    std::string clientUndeliveredDir = ".";
};

/******************************************************************************/

} /* namespace Gem::Courtier */
