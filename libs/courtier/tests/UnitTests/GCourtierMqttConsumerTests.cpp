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

// The MQTT consumer and its client when a process comes late or goes: a client started well before
// its server, a client that dies while it evaluates, and a server whose ends die. They run against
// the tests' broker (the CTest fixture of tests/mqttTestBroker.sh). What a cut broker connection
// does to the two ends of one connection (the end FAILED, the other SILENT by the will, nothing
// connects again) is shown on the transport alone by GCourtierMqttTransportTests; these cases show
// what the consumer and the client make of it.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "GDemoProcessingContainers.hpp"
#include "GInProcessTestSupport.hpp"
#include "GMqttTestSupport.hpp"
#include "GTcpRelay.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GMqttConsumerT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GMqttConsumerConfig;
using Gem::Courtier::GMqttConsumerT;
using Gem::Courtier::GSubmissionPolicy;
using Gem::Courtier::Consumers::GMqttClientT;
using Gem::Courtier::Tests::GTcpRelay;
using Gem::Courtier::Tests::mqttTestOptions;
using namespace std::chrono_literals;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;

constexpr auto PATIENCE = 20s; // how long a case waits for what is due within a few seconds

/** @brief Waits, at most PATIENCE, for @p condition @return Whether it held in time */
template <typename condition_type>
bool eventually(condition_type condition) {
    return Gem::Courtier::Tests::eventually(condition, PATIENCE);
}

/** @brief @return The configuration of a server and its clients on the tests' broker, under a
 *  topic prefix of its own, with a keep-alive of one second (the handshake limit is two) */
GMqttConsumerConfig testConfig() {
    const auto options = mqttTestOptions(1s);
    return GMqttConsumerConfig{
        .ip = options.host,
        .port = options.port,
        .topic = options.topic,
        .nThreads = 2,
        .pingInterval = 1
    };
}

/** @brief @return A started server of @p config */
std::shared_ptr<GMqttConsumerT<GFaultyContainer>> startedServer(const GMqttConsumerConfig &config) {
    auto consumer = std::make_shared<GMqttConsumerT<GFaultyContainer>>(config);
    consumer->startServer();
    return consumer;
}

/** @brief A client of @p config that runs on a thread of its own and is asked to close when this
 *  goes (one that is not asked connects again, and its thread is never joined) */
class RunningClient {
public:
    explicit RunningClient(const GMqttConsumerConfig &config)
      : client_(std::make_shared<GMqttClientT<GFaultyContainer>>(config))
      , thread_([client = client_] { client->run(); }) { /* nothing */
    }
    ~RunningClient() {
        client_->flagCloseRequested();
    }
    RunningClient(const RunningClient &) = delete;
    RunningClient(RunningClient &&) = delete;
    RunningClient &operator=(const RunningClient &) = delete;
    RunningClient &operator=(RunningClient &&) = delete;

    /** @brief @return The client */
    [[nodiscard]] GMqttClientT<GFaultyContainer> &client() const {
        return *client_;
    }

private:
    std::shared_ptr<GMqttClientT<GFaultyContainer>> client_; ///< The client
    std::jthread thread_;                                    ///< Runs it; joined after the close
};

/** @brief @param n How many @param mode What they do @param sleep_ms How long they sleep
 *  @return Work items */
std::vector<item_ptr>
itemsOf(std::size_t n, fault_mode mode = fault_mode::NONE, unsigned int sleep_ms = 0) {
    std::vector<item_ptr> items;
    for(std::size_t i = 0; i < n; ++i) {
        items.push_back(std::make_unique<GFaultyContainer>(i, mode, sleep_ms));
    }
    return items;
}

/**
 * One batch, handed to a server on a thread of its own, whose wait is bounded: a batch that is not
 * done within PATIENCE (a case that fails) has its server stopped when this goes, which ends it,
 * so that the case fails with its own message instead of waiting for ctest's limit. Its policy
 * cannot end the process: an item that has not come back by then is neither resubmitted nor
 * fatal, its slot is refilled from an unprocessed template (and the case's check of the items
 * fails). In a case that passes no item goes missing, so the policy decides nothing.
 */
class BoundedBatch {
public:
    BoundedBatch(GMqttConsumerT<GFaultyContainer> &consumer, std::vector<item_ptr> &items)
      : consumer_(consumer)
      , done_(std::async(std::launch::async, [&consumer, &items] {
          Gem::Courtier::GSubmissionPolicy policy = GSubmissionPolicy::clone_on_partial_return();
          policy.fatal_on_none_returned = false;
          consumer.processBatch(
              std::span<item_ptr>(items.data(), items.size()),
              policy,
              std::make_unique<GFaultyContainer>(items.size())
          );
      })) { /* nothing */
    }
    /** @brief Stops the server if the batch is not done, then waits for its end */
    ~BoundedBatch() {
        if(done_.wait_for(0s) != std::future_status::ready) {
            consumer_.stopServer();
        }
        done_.wait();
    }
    BoundedBatch(const BoundedBatch &) = delete;
    BoundedBatch(BoundedBatch &&) = delete;
    BoundedBatch &operator=(const BoundedBatch &) = delete;
    BoundedBatch &operator=(BoundedBatch &&) = delete;

    /** @brief Waits for the batch, at most PATIENCE @return Whether it is done */
    [[nodiscard]] bool done() {
        return done_.wait_for(PATIENCE) == std::future_status::ready;
    }

private:
    GMqttConsumerT<GFaultyContainer> &consumer_; ///< The server
    std::future<void> done_;                     ///< The batch's end
};

/**
 * A broker of the case's own, started and ended by the tests' script (tests/mqttTestBroker.sh), on
 * a free port that it keeps when it is started again: the fixture's broker serves the other tests
 * at the same time and is not stopped. Ended when this goes.
 */
class OwnBroker {
public:
    OwnBroker() {
        REQUIRE(script_("start") == 0);
        std::ifstream file(std::string(GCOURTIER_MQTT_OWN_BROKER_DIR) + "/port");
        unsigned int port = 0;
        REQUIRE(static_cast<bool>(file >> port));
        port_ = static_cast<unsigned short>(port);
    }
    ~OwnBroker() {
        std::ignore = script_("stop");
    }
    OwnBroker(const OwnBroker &) = delete;
    OwnBroker(OwnBroker &&) = delete;
    OwnBroker &operator=(const OwnBroker &) = delete;
    OwnBroker &operator=(OwnBroker &&) = delete;

    /** @brief @return Its port */
    [[nodiscard]] unsigned short port() const {
        return port_;
    }
    /** @brief Ends it (its process goes; its clients lose their connections) */
    void stop() {
        REQUIRE(script_("stop") == 0);
    }
    /** @brief Starts it again on its port */
    void startAgain() {
        REQUIRE(script_("start", port_) == 0);
    }

private:
    /** @brief Runs the script (it ends within seconds) @return Its exit status */
    static int script_(const std::string &action, unsigned short port = 0) {
        std::string command = std::string("'") + GCOURTIER_MQTT_BROKER_SCRIPT + "' " + action +
                              " '" + GCOURTIER_MOSQUITTO + "' '" + GCOURTIER_MQTT_OWN_BROKER_DIR +
                              "'";
        if(port != 0) {
            command += " " + std::to_string(port);
        }
        return std::system((command + " > /dev/null 2>&1").c_str());
    }

    unsigned short port_ = 0; ///< Its port
};

/** @brief @param items Work items @return Whether every one has been processed */
bool allProcessed(const std::vector<item_ptr> &items) {
    return std::ranges::all_of(items, [](const item_ptr &item) { return item->is_processed(); });
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * The batch system's case: a client starts five seconds before its server. Its joins reach no
 * listener; each of its connections ends SILENT after the handshake limit (two keep-alive
 * intervals), and it joins again, without a limit of its own (--mqtt_firstContactLimit is 0 by
 * default). Once the server has started, its next join is taken and it gets work.
 */
TEST_CASE(
    "MQTT consumer: a client started before its server gets work once the server starts",
    "[courtier][mqtt]"
) {
    const GMqttConsumerConfig config = testConfig();
    const RunningClient early(config);
    std::this_thread::sleep_for(5s);
    CHECK(early.client().losses() >= 1); // it has joined, heard nobody, and joined again

    const auto consumer = startedServer(config);
    auto items = itemsOf(8);
    BoundedBatch batch(*consumer, items);
    REQUIRE(batch.done());
    CHECK(allProcessed(items));
}

/******************************************************************************/
/**
 * A client dies while it evaluates: its broker connection ends without a DISCONNECT (a relay
 * between it and the broker is cut, and refuses it from then on). The broker publishes the will
 * of its end, the server's end of its connection ends SILENT, and the lease puts the item it held
 * back at once: another client evaluates it, and the batch completes. Without the will nothing
 * would bring the item back (there is no reclaim time by default) and the batch would wait.
 */
TEST_CASE(
    "MQTT consumer: the items of a client that dies go back to the server",
    "[courtier][mqtt]"
) {
    const GMqttConsumerConfig config = testConfig();
    const auto consumer = startedServer(config);
    GTcpRelay relay(config.port);
    GMqttConsumerConfig through_relay = config;
    through_relay.port = relay.port();

    std::vector<item_ptr> items = itemsOf(4, fault_mode::SLEEP, 1000);
    {
        const RunningClient dying(through_relay);
        REQUIRE(eventually([&] { return consumer->connectedClients() == 1U; }));
        BoundedBatch batch(*consumer, items);
        std::this_thread::sleep_for(300ms); // it holds an item and evaluates it
        relay.setAccepting(false);
        relay.cutAll();

        const RunningClient other(config);
        REQUIRE(batch.done());
    }
    CHECK(allProcessed(items));
    CHECK(consumer->requeuedCount() >= 1);
    CHECK(relay.nConnections() == 1); // the dead client did not come back
}

/******************************************************************************/
/**
 * A server's ends die (its broker connections, through a relay that is cut and refuses from then
 * on): the broker publishes the will of the client's server end, the client's end ends SILENT, and
 * the client connects again -- and finds a server started afterwards on the same topic prefix,
 * which it serves.
 */
TEST_CASE(
    "MQTT consumer: a client whose server dies connects again and serves the next one",
    "[courtier][mqtt]"
) {
    const GMqttConsumerConfig config = testConfig();
    GTcpRelay relay(config.port);
    GMqttConsumerConfig through_relay = config;
    through_relay.port = relay.port();
    const auto dying = startedServer(through_relay);

    const RunningClient client(config);
    REQUIRE(eventually([&] { return dying->connectedClients() == 1U; }));
    relay.setAccepting(false);
    relay.cutAll();
    REQUIRE(eventually([&] { return client.client().losses() == 1; }));

    const auto next = startedServer(config);
    auto items = itemsOf(8);
    BoundedBatch batch(*next, items);
    REQUIRE(batch.done());
    CHECK(allProcessed(items));
}

/******************************************************************************/
/**
 * The broker restarts during a run: every MQTT connection ends -- the clients' and the server's
 * ends, and the listener's own client, which receives the joins. The clients connect again once
 * the broker is back (on its port), and the listener must listen again for their joins: the items
 * the lost sessions held go back, and the batch completes.
 */
TEST_CASE("MQTT consumer: a run goes on when its broker restarts", "[courtier][mqtt]") {
    OwnBroker broker;
    GMqttConsumerConfig config = testConfig();
    config.port = broker.port();
    const auto consumer = startedServer(config);
    const RunningClient first(config);
    const RunningClient second(config);
    REQUIRE(eventually([&] { return consumer->connectedClients() == 2U; }));

    auto items = itemsOf(8, fault_mode::SLEEP, 500);
    BoundedBatch batch(*consumer, items);
    std::this_thread::sleep_for(300ms); // both clients hold an item
    broker.stop();
    std::this_thread::sleep_for(1s);
    broker.startAgain();
    REQUIRE(batch.done());
    CHECK(allProcessed(items));
}
