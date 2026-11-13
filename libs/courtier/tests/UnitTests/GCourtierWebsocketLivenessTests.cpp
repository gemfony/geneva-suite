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
 * Tests of what keeps a websocket connection of the courtier honest: the keep-alive that notices a
 * peer which has fallen silent (on either end), the limit on the handshake, the absence of a limit
 * on the message size, and the answer to a request the server cannot handle. The time limits all
 * derive from the ping interval, which is set to one second here, so each of these tests takes a
 * few seconds.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "courtier/GCommandContainerT.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp"

using boost::asio::ip::tcp;
using Gem::Courtier::fault_mode;
using Gem::Courtier::GCommandContainerT;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GFrameKind;
using Gem::Courtier::GRandomNumberContainer;
using Gem::Courtier::GSubmissionPolicy;
using Gem::Courtier::GWebsocketConsumerT;
using Gem::Courtier::Consumers::GWebsocketClientT;
using namespace std::chrono_literals;
namespace Websocket = boost::beast::websocket;

namespace {

constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;
constexpr std::size_t PING_INTERVAL_S =
    1;                        // a ping after 1 s of silence, the connection closed after 2 s
constexpr auto PATIENCE = 8s; // how long a test waits for something due after about 2 s

/** @brief A started websocket server for work items of type @p Item. */
template <typename Item>
std::shared_ptr<GWebsocketConsumerT<Item>> startedServer(std::size_t n_threads) {
    auto consumer = std::make_shared<GWebsocketConsumerT<Item>>(
        /*port=*/0,
        n_threads,
        BIN,
        PING_INTERVAL_S
    );
    consumer->startServer();
    return consumer;
}

/** @brief A client for @p port with the same keep-alive as the servers of these tests. */
template <typename Item>
std::shared_ptr<GWebsocketClientT<Item>> matchingClient(unsigned short port) {
    return std::make_shared<GWebsocketClientT<Item>>(
        "127.0.0.1",
        port,
        BIN,
        /*verbose_control_frames=*/false,
        /*prefetch_depth=*/1,
        PING_INTERVAL_S
    );
}

/** @brief Polls @p condition until it holds or @p patience has passed.
 *  @return Whether the condition held in time */
template <typename Condition>
bool eventually(Condition condition, std::chrono::milliseconds patience = PATIENCE) {
    const auto give_up_at = std::chrono::steady_clock::now() + patience;
    while(std::chrono::steady_clock::now() < give_up_at) {
        if(condition()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return condition();
}

/** @brief A websocket client played by hand: it connects and completes the handshake, and from then
 *  on does only what a test tells it to. Left alone it is a peer that has fallen silent: it reads
 *  nothing, so it answers no ping. */
struct HandDrivenPeer {
    boost::asio::io_context ctx;
    Websocket::stream<tcp::socket> ws{ctx};

    explicit HandDrivenPeer(unsigned short port) {
        ws.next_layer().connect({boost::asio::ip::address_v4::loopback(), port});
        ws.handshake("127.0.0.1", "/");
        ws.binary(true);
    }
};

} /* anonymous namespace */

TEST_CASE(
    "courtier(websocket): a client that falls silent is dropped, and nothing waits for it",
    "[courtier][websocket][liveness][net]"
) {
    // One io thread only: a session that blocked it while closing would stop the whole server.
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/1);

    HandDrivenPeer silent(consumer->getPort());
    REQUIRE(eventually([&] { return consumer->getNActiveSessions() == 1; }));

    // No pong comes back, so the session must end after two ping intervals.
    CHECK(eventually([&] { return consumer->getNActiveSessions() == 0; }));

    // The server must be able to serve a real client now.
    auto client = matchingClient<GFaultyContainer>(consumer->getPort());
    const std::jthread client_thread([client] { client->run(); });

    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.reserve(8);
    for(std::size_t i = 0; i < 8; ++i) {
        items.push_back(std::make_unique<GFaultyContainer>(i, fault_mode::NONE));
    }
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });
    const bool served = batch.wait_for(PATIENCE) == std::future_status::ready;
    CHECK(served);

    // Let go of the silent connection: a server that still waits for it can then go on, so the
    // test ends instead of hanging.
    boost::system::error_code ec;
    std::ignore = silent.ws.next_layer().close(ec);
    batch.wait();
    client->flagCloseRequested();
}

TEST_CASE(
    "courtier(websocket): a client whose server falls silent terminates on its own",
    "[courtier][websocket][liveness][net]"
) {
    // A "server" that completes the handshake and then does nothing: no reply, no pong.
    boost::asio::io_context server_ctx;
    tcp::acceptor acceptor(server_ctx, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    std::promise<void> release;
    const std::jthread server_thread([&acceptor, held = release.get_future()]() mutable {
        boost::system::error_code ec;
        Websocket::stream<tcp::socket> ws(acceptor.accept(ec));
        if(not ec) {
            ws.accept(ec);
        }
        held.wait(); // hold the connection open, silently, until the test is over
    });

    auto client = matchingClient<GFaultyContainer>(port);
    std::atomic<bool> returned{false};
    const std::jthread client_thread([client, &returned] {
        client->run();
        returned.store(true);
    });

    // Nothing arrives, not even a pong: after two ping intervals the client must give up.
    CHECK(eventually([&] { return returned.load(); }));

    client->flagCloseRequested(); // ends a client that is still waiting, so the test can finish
    release.set_value();
}

TEST_CASE(
    "courtier(websocket): a connection that never starts the handshake is dropped",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);

    boost::asio::io_context ctx;
    tcp::socket socket(ctx);
    socket.connect({boost::asio::ip::address_v4::loopback(), consumer->getPort()});
    REQUIRE(eventually([&] { return consumer->getNActiveSessions() == 1; }));

    // The handshake has the same limit as the idle time. Without one the session waits forever.
    CHECK(eventually([&] { return consumer->getNActiveSessions() == 0; }));
}

TEST_CASE(
    "courtier(websocket): an evaluation much longer than the idle limit keeps its connection",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);
    auto client = matchingClient<GFaultyContainer>(consumer->getPort());
    const std::jthread client_thread([client] { client->run(); });

    // Five seconds of evaluation against an idle limit of two: the client's io thread answers the
    // server's pings while the item is computed elsewhere, so the connection never looks idle.
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(0, fault_mode::SLEEP, /*sleep_ms=*/5000));
    consumer->processBatch(
        std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
        GSubmissionPolicy::full_success_or_fatal()
    );

    CHECK(items.front()->is_processed());
    CHECK(consumer->getNActiveSessions() == 1); // still the session the item went out on
    CHECK(consumer->requeuedCount() == 0);      // and the item was never taken back
    client->flagCloseRequested();
}

TEST_CASE(
    "courtier(websocket): a work item of more than 16 MiB travels in both directions",
    "[courtier][websocket][liveness][net]"
) {
    constexpr std::size_t N_DOUBLES =
        3'000'000; // 24 MB of payload; Beast's own default limit is 16 MiB

    auto consumer = startedServer<GRandomNumberContainer>(/*n_threads=*/2);
    auto client = matchingClient<GRandomNumberContainer>(consumer->getPort());
    const std::jthread client_thread([client] { client->run(); });

    std::vector<std::unique_ptr<GRandomNumberContainer>> items;
    items.push_back(std::make_unique<GRandomNumberContainer>(N_DOUBLES));
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GRandomNumberContainer>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });

    // With a limit on the message size the client fails to read the item and the batch never ends.
    const bool done = batch.wait_for(4 * PATIENCE) == std::future_status::ready;
    CHECK(done);
    if(done) {
        CHECK(items.front()->is_processed());
    }
    else {
        consumer->stopServer(); // releases the waiting batch, so the test ends
        batch.wait();
    }
    client->flagCloseRequested();
}

TEST_CASE(
    "courtier(websocket): a request the server cannot decode is answered with NO_WORK",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);
    HandDrivenPeer peer(consumer->getPort());

    peer.ws.write(boost::asio::buffer(std::string("this is not a frame")));
    boost::beast::flat_buffer buffer;
    boost::system::error_code ec;
    peer.ws.read(buffer, ec);
    REQUIRE_FALSE(ec); // the connection must stay open: closing it would end a real client

    GCommandContainerT<GFaultyContainer> reply{GFrameKind::NONE};
    Gem::Courtier::container_from_string(
        boost::beast::buffers_to_string(buffer.data()),
        reply,
        BIN
    );
    CHECK(reply.kind() == GFrameKind::NO_WORK);
}
