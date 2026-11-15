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
#include "courtier/GCourtierEnums.hpp" // the consumer defaults

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The configuration of the asio consumer GAsioConsumerT and of its client
 * GAsioConsumerClientT (mnemonic "asio").
 *
 * Server and client are built from the same struct; each member says which side reads it. A
 * plain aggregate, to be copied freely. It includes no network library, so a program can name the
 * consumer by its configuration without seeing the consumer's header.
 */
struct GAsioConsumerConfig {
    /// The mnemonic of the consumer family this struct configures, as chosen with --consumer
    static constexpr std::string_view MNEMONIC = "asio";

    /// Client: the name or address of the server
    std::string ip = "localhost";
    /// Server: the port to listen on (0: a free port, see getPort()). Client: the server's port
    unsigned short port = 0;
    /// Both: the serialization of work items on the wire
    Gem::Common::serializationMode serializationMode = Gem::Common::serializationMode::GEM_BINARY;
    /// Server: the number of io threads; 0: as many as the machine has hardware threads
    std::size_t nThreads = 0;
    /// Client: connection attempts in a row before it gives up
    std::size_t maxReconnects = 0;
    /// Client: the largest number of work items it holds at once (1: one after the other)
    std::size_t clientPrefetchDepth = 1;
};

/******************************************************************************/

} /* namespace Gem::Courtier */
