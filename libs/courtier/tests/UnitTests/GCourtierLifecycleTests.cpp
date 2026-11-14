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

/**
 * Tests of the lifetime of the socket servers (asio and websocket): a consumer is destroyed when
 * its last reference is dropped, whether or not stopServer() was called first, and destroying it
 * frees the listening port and ends its clients.
 */

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#include <boost/asio.hpp>

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GAsioConsumerT.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GAsioTransportT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GBaseClientT;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GSubmissionPolicy;
using Gem::Courtier::Consumers::GAsioConsumerClientT;
using Gem::Courtier::Consumers::GWebsocketClientT;
using namespace std::chrono_literals;

namespace {

using AsioConsumer = Gem::Courtier::GAsioConsumerT<GFaultyContainer>;
using WebsocketConsumer = Gem::Courtier::GWebsocketConsumerT<GFaultyContainer>;
using item_ptr = std::unique_ptr<GFaultyContainer>;

constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

/** @brief Builds a started server of the given kind on a port the system chooses. */
template <typename Consumer>
std::shared_ptr<Consumer> startedServer() {
    auto consumer = std::make_shared<Consumer>(/*port=*/0, /*threads=*/2, BIN);
    consumer->startServer();
    return consumer;
}

/** @brief Builds the client that belongs to the given kind of server. */
template <typename Consumer>
std::shared_ptr<GBaseClientT<GFaultyContainer>> matchingClient(unsigned short port) {
    if constexpr(std::is_same_v<Consumer, AsioConsumer>) {
        return std::make_shared<GAsioConsumerClientT<GFaultyContainer>>(
            "127.0.0.1",
            port,
            BIN,
            /*max_reconnects=*/1
        );
    }
    else {
        return std::make_shared<GWebsocketClientT<GFaultyContainer>>(
            "127.0.0.1",
            port,
            BIN,
            /*verbose_control_frames=*/false
        );
    }
}

/** @brief Whether a TCP connection to @p port on this host is refused. */
bool connectIsRefused(unsigned short port) {
    boost::asio::io_context ctx;
    boost::asio::ip::tcp::socket socket(ctx);
    boost::system::error_code ec;
    std::ignore = socket.connect({boost::asio::ip::address_v4::loopback(), port}, ec);
    return static_cast<bool>(ec);
}

} /* anonymous namespace */

TEMPLATE_TEST_CASE(
    "courtier(lifecycle): dropping the last reference destroys the server and frees its port",
    "[courtier][lifecycle][net]",
    AsioConsumer,
    WebsocketConsumer
) {
    auto consumer = startedServer<TestType>();
    const unsigned short port = consumer->getPort();
    const std::weak_ptr<TestType> watch = consumer;

    // No stopServer(): the destructor has to do it. A pending accept that owned the consumer would
    // keep it alive here, with its io threads running and its port open.
    consumer.reset();

    CHECK(watch.expired());
    CHECK(connectIsRefused(port));
}

TEMPLATE_TEST_CASE(
    "courtier(lifecycle): stopServer() may be called more than once, and the server is still "
    "destroyed",
    "[courtier][lifecycle][net]",
    AsioConsumer,
    WebsocketConsumer
) {
    auto consumer = startedServer<TestType>();
    const unsigned short port = consumer->getPort();
    const std::weak_ptr<TestType> watch = consumer;

    consumer->stopServer();
    CHECK(connectIsRefused(port)); // the port is free as soon as stopServer() has returned
    consumer->stopServer();

    consumer.reset();
    CHECK(watch.expired());
}

TEMPLATE_TEST_CASE(
    "courtier(lifecycle): the clients of a destroyed server terminate on their own",
    "[courtier][lifecycle][net]",
    AsioConsumer,
    WebsocketConsumer
) {
    constexpr std::size_t N_CLIENTS = 2;
    constexpr std::size_t N_ITEMS = 16;

    auto consumer = startedServer<TestType>();
    const std::weak_ptr<TestType> watch = consumer;

    std::vector<std::shared_ptr<GBaseClientT<GFaultyContainer>>> clients;
    std::atomic<std::size_t> returned{0};
    std::atomic<bool> any_threw{false};
    std::vector<std::jthread> client_threads;
    for(std::size_t c = 0; c < N_CLIENTS; ++c) {
        clients.push_back(matchingClient<TestType>(consumer->getPort()));
        client_threads.emplace_back([client = clients.back(), &returned, &any_threw] {
            try {
                client->run();
            }
            catch(...) {
                any_threw.store(true);
            }
            returned.fetch_add(1);
        });
    }

    // A batch that the clients evaluate: afterwards they are connected and idle.
    std::vector<item_ptr> items;
    items.reserve(N_ITEMS);
    for(std::size_t i = 0; i < N_ITEMS; ++i) {
        items.push_back(std::make_unique<GFaultyContainer>(i, fault_mode::NONE));
    }
    consumer->processBatch(
        std::span<item_ptr>(items.data(), items.size()),
        GSubmissionPolicy::full_success_or_fatal()
    );

    consumer.reset(); // the server goes away without a word to its clients
    CHECK(watch.expired());

    // The clients notice (a closed connection, or a refused one) and return without being told --
    // and promptly: a timer left armed on a client's io context would hold it back for seconds.
    const auto give_up_at = std::chrono::steady_clock::now() + 5s;
    while(returned.load() < N_CLIENTS && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(returned.load() == N_CLIENTS);
    CHECK_FALSE(any_threw.load());

    for(auto &client : clients) {
        client->flagCloseRequested(); // ends a client that is still running, so the test can finish
    }
}
