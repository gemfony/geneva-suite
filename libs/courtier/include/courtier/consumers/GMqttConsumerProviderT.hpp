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
#include "courtier/consumers/GMqttConsumerConfig.hpp"
#include "courtier/consumers/GMqttConsumerT.hpp"
#include "courtier/transport/GMqttTransportT.hpp" // GMqttClientT

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The provider of the MQTT consumer ("mqtt"): server and clients meet on an MQTT broker,
 * under a topic prefix of their own.
 * @tparam processable_type The item type
 */
template <ProcessableItem processable_type>
class GMqttConsumerProviderT final
  : public GConfiguredConsumerProviderT<processable_type, GMqttConsumerConfig> {
    using base_type = GConfiguredConsumerProviderT<processable_type, GMqttConsumerConfig>;
    using config_type = GMqttConsumerConfig;

public:
    /** @brief The consumer's class @return Its name */
    [[nodiscard]] std::string getName() const override {
        return "GMqttConsumerT";
    }
    /** @brief A networked consumer @return true */
    [[nodiscard]] bool needsClient() const override {
        return true;
    }

    /** @brief Builds the server and starts it @param spec Mnemonic and configuration @return It */
    typename base_type::consumer_ptr setup(const ConsumerSpec &spec) override {
        auto consumer =
            std::make_shared<GMqttConsumerT<processable_type>>(configOf<config_type>(spec));
        consumer->startServer();
        return consumer;
    }
    /** @brief Builds a client of the server @param spec Mnemonic and configuration @return It */
    [[nodiscard]] typename base_type::client_ptr
    buildClient(const ConsumerSpec &spec) const override {
        return std::make_shared<Consumers::GMqttClientT<processable_type>>(
            configOf<config_type>(spec)
        );
    }

protected:
    /** @brief The --mqtt_* options @param gpb The launcher's parser */
    void addOptions_(Gem::Common::GParserBuilder &gpb) override {
        const config_type defaults;
        this->option(
            gpb,
            "mqtt_ip",
            &config_type::ip,
            defaults.ip,
            "[mqtt] The name or ip of the MQTT broker"
        );
        this->option(
            gpb,
            "mqtt_port",
            &config_type::port,
            defaults.port,
            "[mqtt] The port of the MQTT broker"
        );
        this->option(
            gpb,
            "mqtt_topic",
            &config_type::topic,
            defaults.topic,
            "[mqtt] The prefix of the topics: a server and its clients use the same "
            "one, servers on one broker different ones"
        );
        this->option(
                gpb,
                "mqtt_serializationMode",
                &config_type::serializationMode,
                defaults.serializationMode,
                "[mqtt] Serialization in " + Gem::Common::serializationModeChoices()
        )
            .setHidden();
        this->option(
                gpb,
                "mqtt_pingInterval",
                &config_type::pingInterval,
                defaults.pingInterval,
                "[mqtt] The MQTT keep-alive in seconds, for server and client (0 == off): "
                "the broker publishes the will of a process it has not heard from within one "
                "and a half intervals, which ends the other end of its connections. A client "
                "keeps its own value: set it alike on both sides. A client that waits for its "
                "server joins again after two intervals and a pause that grows to 30 s: it "
                "finds a server that has started within at most both, about a minute at the "
                "defaults"
        )
            .setHidden();
        this->option(
                gpb,
                "mqtt_prefetchDepth",
                &config_type::clientPrefetchDepth,
                defaults.clientPrefetchDepth,
                "[mqtt] n: how many work items a client holds at once (see "
                "--beast_prefetchDepth)"
        )
            .setHidden();
        this->option(
                gpb,
                "mqtt_parallelEvaluations",
                &config_type::clientParallelEvaluations,
                defaults.clientParallelEvaluations,
                "[mqtt] m: how many of the items a client holds it evaluates at the same "
                "time; from 1 to the prefetch depth"
        )
            .setHidden();
        this->option(
                gpb,
                "mqtt_firstContactLimit",
                &config_type::clientFirstContactLimit,
                defaults.clientFirstContactLimit,
                "[mqtt] Seconds a client tries to reach its server before it gives up "
                "(0 == for as long as it runs)"
        )
            .setHidden();
        this->option(
                gpb,
                "mqtt_reconnectLimit",
                &config_type::clientReconnectLimit,
                defaults.clientReconnectLimit,
                "[mqtt] Seconds a client that has lost its connection tries to reach its "
                "server again (0 == for as long as it runs)"
        )
            .setHidden();
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier */
