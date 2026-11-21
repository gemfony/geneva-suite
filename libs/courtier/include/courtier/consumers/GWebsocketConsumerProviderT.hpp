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
#include <memory>
#include <string>

// Geneva header files go here
#include "common/GCommonEnums.hpp" // serializationModeChoices()
#include "courtier/GConsumerProviderT.hpp"
#include "courtier/GCourtierEnums.hpp" // GCONSUMERDEFAULTPORT
#include "courtier/consumers/GWebsocketConsumerConfig.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp" // GWebsocketClientT

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The provider of the websocket consumer ("beast"): a server and its clients, which
 * connect to it.
 * @tparam processable_type The item type
 */
template <ProcessableItem processable_type>
class GWebsocketConsumerProviderT final
  : public GConfiguredConsumerProviderT<processable_type, GWebsocketConsumerConfig> {
    using base_type = GConfiguredConsumerProviderT<processable_type, GWebsocketConsumerConfig>;
    using config_type = GWebsocketConsumerConfig;

public:
    /** @brief The consumer's class @return Its name */
    [[nodiscard]] std::string getName() const override {
        return "GWebsocketConsumerT";
    }
    /** @brief A networked consumer @return true */
    [[nodiscard]] bool needsClient() const override {
        return true;
    }

    /** @brief Builds the server and starts it @param spec Mnemonic and configuration @return It */
    typename base_type::consumer_ptr setup(const ConsumerSpec &spec) override {
        auto consumer =
            std::make_shared<GWebsocketConsumerT<processable_type>>(configOf<config_type>(spec));
        consumer->startServer();
        return consumer;
    }
    /** @brief Builds a client of the server @param spec Mnemonic and configuration @return It */
    [[nodiscard]] typename base_type::client_ptr
    buildClient(const ConsumerSpec &spec) const override {
        return std::make_shared<Consumers::GWebsocketClientT<processable_type>>(
            configOf<config_type>(spec)
        );
    }

protected:
    /** @brief The --beast_* options @param gpb The launcher's parser */
    void addOptions_(Gem::Common::GParserBuilder &gpb) override {
        // The defaults are the configuration's, the port excepted: on the command line server and
        // client are two processes, which meet on GCONSUMERDEFAULTPORT.
        const config_type defaults;
        this->option(
            gpb,
            "beast_ip",
            &config_type::ip,
            defaults.ip,
            "[beast] The name or ip of the server"
        );
        this->option(
            gpb,
            "beast_port",
            &config_type::port,
            GCONSUMERDEFAULTPORT,
            "[beast] The port of the server"
        );
        this->option(
                gpb,
                "beast_serializationMode",
                &config_type::serializationMode,
                defaults.serializationMode,
                "[beast] Serialization in " + Gem::Common::serializationModeChoices()
        )
            .setHidden();
        this->option(
                gpb,
                "beast_nListenerThreads",
                &config_type::nThreads,
                defaults.nThreads,
                "[beast] The number of io threads of the server, and of the threads its "
                "sessions run on (0 == as many as the machine has hardware threads)"
        )
            .setHidden();
        this->option(
                gpb,
                "beast_pingInterval",
                &config_type::pingInterval,
                defaults.pingInterval,
                "[beast] Keep-alive, for server and client: seconds of silence after which "
                "a ping is sent; the connection is closed after twice that time without any "
                "inbound data (0 == off). The server names its value to its clients; on a "
                "client the value holds until its server has named its own in the HELLO"
        )
            .setHidden();
        this->option(
                gpb,
                "beast_verboseControlFrames",
                &config_type::verboseControlFrames,
                defaults.verboseControlFrames,
                "[beast] Announce ping/pong/close frames"
        )
            .setHidden();
        this->option(
                gpb,
                "beast_prefetchDepth",
                &config_type::clientPrefetchDepth,
                defaults.clientPrefetchDepth,
                "[beast] n: how many work items a client holds at once -- the one(s) being "
                "evaluated and those fetched ahead, so that the transfer of the next item "
                "overlaps the evaluation of the current one (how many it evaluates at once: "
                "--beast_parallelEvaluations)"
        )
            .setHidden();
        this->option(
                gpb,
                "beast_parallelEvaluations",
                &config_type::clientParallelEvaluations,
                defaults.clientParallelEvaluations,
                "[beast] m: how many of the items a client holds (--beast_prefetchDepth) it "
                "evaluates at the same time; from 1 to the prefetch depth"
        )
            .setHidden();
        this->option(
                gpb,
                "beast_firstContactLimit",
                &config_type::clientFirstContactLimit,
                defaults.clientFirstContactLimit,
                "[beast] Seconds a client tries to reach its server before it gives up "
                "(0 == for as long as it runs)"
        )
            .setHidden();
        this->option(
                gpb,
                "beast_reconnectLimit",
                &config_type::clientReconnectLimit,
                defaults.clientReconnectLimit,
                "[beast] Seconds a client that has lost its connection tries to reach its "
                "server again (0 == for as long as it runs)"
        )
            .setHidden();
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier */
