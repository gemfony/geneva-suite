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
 * Tests of GWebsocketPeer, the part of a courtier websocket connection that the client and the
 * server's session share: two peers are connected over the loopback interface and run on one io
 * context, so every step is deterministic.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "courtier/transport/GWebsocketPeer.hpp"

using boost::asio::ip::tcp;
using Gem::Courtier::Consumers::GWebsocketPeer;

namespace {

/** @brief What one end of the connection has seen. */
struct GSeen {
    std::vector<std::string> messages;
    int nOpen = 0;
    int nClosed = 0;
    boost::system::error_code cause;
};

/** @brief Two connected sockets on @p ctx: first the accepting end, second the connecting end. */
std::pair<tcp::socket, tcp::socket> connectedPair(boost::asio::io_context &ctx) {
    tcp::acceptor acceptor(ctx, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    tcp::socket connecting(ctx);
    connecting.connect(acceptor.local_endpoint());
    return {acceptor.accept(), std::move(connecting)};
}

/** @brief A peer on @p socket that records what it is told in @p seen. @p on_open and @p on_message
 *  let a test act from inside the handlers. */
std::shared_ptr<GWebsocketPeer> recordingPeer(
    tcp::socket socket,
    GSeen &seen,
    std::move_only_function<void()> on_open = {},
    std::move_only_function<void(const std::string &)> on_message = {}
) {
    return std::make_shared<GWebsocketPeer>(
        std::move(socket),
        GWebsocketPeer::GOptions{.name = "test peer"},
        GWebsocketPeer::GHandlers{
            .onOpen =
                [&seen, on_open = std::move(on_open)]() mutable {
                    ++seen.nOpen;
                    if(on_open) {
                        on_open();
                    }
                },
            .onMessage =
                [&seen, on_message = std::move(on_message)](const std::string &message) mutable {
                    seen.messages.push_back(message);
                    if(on_message) {
                        on_message(seen.messages.back());
                    }
                },
            .onClosed =
                [&seen](const boost::system::error_code &cause) {
                    ++seen.nClosed;
                    seen.cause = cause;
                }
        }
    );
}

/** @brief Runs @p ctx until it has nothing left to do, but not for longer than ten seconds. */
bool runsDry(boost::asio::io_context &ctx) {
    ctx.run_for(std::chrono::seconds(10));
    return ctx.stopped();
}

const std::size_t N_BURST = 200;
const std::size_t LARGE_MESSAGE_BYTES = 3U << 20U;

} /* anonymous namespace */

/******************************************************************************/
// Beast allows one write at a time, so the peer queues. A burst handed over in one go -- a large
// message among it, which takes many frames -- must arrive complete and in the order it was sent.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; the count comes from the assertion macros
TEST_CASE("GWebsocketPeer delivers a burst of messages in order", "[courtier][websocket][peer]") {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;
    GSeen client_seen;

    std::weak_ptr<GWebsocketPeer> client_weak;
    auto server = recordingPeer(std::move(accepted), server_seen, {}, [&](const std::string &) {
        if(server_seen.messages.size() == N_BURST) {
            if(auto peer = client_weak.lock()) {
                peer->close(GWebsocketPeer::close_code::normal);
            }
        }
    });
    auto client = recordingPeer(std::move(connecting), client_seen, [&] {
        auto peer = client_weak.lock();
        REQUIRE(peer);
        for(std::size_t i = 0; i < N_BURST; ++i) {
            peer->send(
                i == N_BURST / 2 ? std::string(LARGE_MESSAGE_BYTES, 'x') : std::to_string(i)
            );
        }
    });
    client_weak = client;

    server->acceptHandshake();
    client->connectHandshake("localhost");
    server.reset(); // from here on the pending operations own the peers
    client.reset();

    REQUIRE(runsDry(ctx));

    REQUIRE(server_seen.messages.size() == N_BURST);
    for(std::size_t i = 0; i < N_BURST; ++i) {
        if(i == N_BURST / 2) {
            CHECK(server_seen.messages[i].size() == LARGE_MESSAGE_BYTES);
        }
        else {
            CHECK(server_seen.messages[i] == std::to_string(i));
        }
    }
    CHECK(client_weak.expired()); // nothing holds a peer once its connection is over
}

/******************************************************************************/
// Each end is told exactly once that the connection is over, and why: the end that closed sees no
// error, the other one a regular close.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; the count comes from the assertion macros
TEST_CASE(
    "GWebsocketPeer reports the end of a connection once to each end",
    "[courtier][websocket][peer]"
) {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;
    GSeen client_seen;

    std::weak_ptr<GWebsocketPeer> client_weak;
    auto server = recordingPeer(std::move(accepted), server_seen);
    auto client = recordingPeer(std::move(connecting), client_seen, [&] {
        auto peer = client_weak.lock();
        REQUIRE(peer);
        peer->send("last words");
        peer->close(GWebsocketPeer::close_code::normal); // a message of one frame is not cut
        peer->close(GWebsocketPeer::close_code::normal); // a second close changes nothing
    });
    client_weak = client;

    server->acceptHandshake();
    client->connectHandshake("localhost");
    client->send("too early"); // not open yet: dropped, not written into a stream that is not ready
    server.reset();
    client.reset();

    REQUIRE(runsDry(ctx));

    CHECK(server_seen.nOpen == 1);
    CHECK(client_seen.nOpen == 1);
    REQUIRE(server_seen.messages.size() == 1);
    CHECK(server_seen.messages.front() == "last words");
    CHECK(client_seen.nClosed == 1);
    CHECK_FALSE(client_seen.cause);
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.cause == boost::beast::websocket::error::closed);
}

/******************************************************************************/
// A close that the other end never answers is still this end's close: the connection breaks while
// the close handshake is under way, and the break is not reported as an error.
TEST_CASE(
    "GWebsocketPeer reports its own close as such when the other end vanishes",
    "[courtier][websocket][peer]"
) {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen client_seen;

    // The other end shakes hands and then neither reads nor answers.
    boost::beast::websocket::stream<tcp::socket> silent(std::move(accepted));
    boost::asio::steady_timer vanish(ctx);
    silent.async_accept([&](const boost::system::error_code &ec) {
        REQUIRE_FALSE(ec);
        vanish.expires_after(std::chrono::milliseconds(200));
        vanish.async_wait([&](const boost::system::error_code &) {
            boost::system::error_code ignored;
            std::ignore = silent.next_layer().close(ignored);
        });
    });

    std::weak_ptr<GWebsocketPeer> client_weak;
    auto client = recordingPeer(std::move(connecting), client_seen, [&] {
        if(auto peer = client_weak.lock()) {
            peer->close(GWebsocketPeer::close_code::normal);
        }
    });
    client_weak = client;
    client->connectHandshake("localhost");
    client.reset();

    REQUIRE(runsDry(ctx));

    CHECK(client_seen.nClosed == 1);
    CHECK_FALSE(client_seen.cause);
    CHECK(client_weak.expired());
}

/******************************************************************************/
// A close ends the exchange of messages at once: it does not wait for the write queue. What waits
// behind the message being written is never sent, and that message itself arrives whole or not at
// all (Beast puts the close frame between two of its frames).
TEST_CASE(
    "GWebsocketPeer does not wait for its write queue at a close",
    "[courtier][websocket][peer]"
) {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;
    GSeen client_seen;

    std::weak_ptr<GWebsocketPeer> client_weak;
    auto server = recordingPeer(std::move(accepted), server_seen);
    auto client = recordingPeer(std::move(connecting), client_seen, [&] {
        auto peer = client_weak.lock();
        REQUIRE(peer);
        peer->send(std::string(LARGE_MESSAGE_BYTES, 'x')); // many frames
        peer->send("waits behind the large one");
        peer->close(GWebsocketPeer::close_code::normal);
    });
    client_weak = client;

    server->acceptHandshake();
    client->connectHandshake("localhost");
    server.reset();
    client.reset();

    REQUIRE(runsDry(ctx));

    for(const auto &message : server_seen.messages) {
        CHECK(message.size() == LARGE_MESSAGE_BYTES); // never a part, never the queued one
    }
    CHECK(server_seen.messages.size() <= 1);
    CHECK(server_seen.nClosed == 1);
    CHECK(client_seen.nClosed == 1);
    CHECK_FALSE(client_seen.cause);
}

/******************************************************************************/
// And nothing that arrives after close() was called is handed on: the role has said it is done. A
// client would otherwise start an evaluation whose result it can no longer return.
TEST_CASE(
    "GWebsocketPeer delivers nothing after close() was called",
    "[courtier][websocket][peer]"
) {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;
    GSeen client_seen;

    std::weak_ptr<GWebsocketPeer> server_weak;
    std::weak_ptr<GWebsocketPeer> client_weak;
    auto server = recordingPeer(std::move(accepted), server_seen, [&] {
        if(auto peer = server_weak.lock()) {
            peer->send("crosses the close");
        }
    });
    auto client = recordingPeer(std::move(connecting), client_seen, [&] {
        if(auto peer = client_weak.lock()) {
            peer->close(GWebsocketPeer::close_code::normal);
        }
    });
    server_weak = server;
    client_weak = client;

    server->acceptHandshake();
    client->connectHandshake("localhost");
    server.reset();
    client.reset();

    REQUIRE(runsDry(ctx));

    CHECK(client_seen.messages.empty());
    CHECK(client_seen.nClosed == 1);
    CHECK(server_seen.nClosed == 1);
}

/******************************************************************************/
// An end that never gets its handshake is told so.
TEST_CASE("GWebsocketPeer reports a handshake that fails", "[courtier][websocket][peer]") {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;

    auto server = recordingPeer(std::move(accepted), server_seen);
    server->acceptHandshake();
    server.reset();

    connecting.close(); // the other end goes away without a handshake

    REQUIRE(runsDry(ctx));

    CHECK(server_seen.nOpen == 0);
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.cause);
}

/******************************************************************************/
// abort() ends a connection whose io context no longer runs: the handlers are dropped at once, and
// with them the peer's hold on whatever they captured.
TEST_CASE(
    "GWebsocketPeer::abort() drops the handlers without a running io context",
    "[courtier][websocket][peer]"
) {
    auto token = std::make_shared<int>(0);
    std::weak_ptr<int> const token_weak = token;
    int n_closed_calls = 0;
    {
        boost::asio::io_context ctx;
        auto [accepted, connecting] = connectedPair(ctx);
        auto peer = std::make_shared<GWebsocketPeer>(
            std::move(accepted),
            GWebsocketPeer::GOptions{.name = "test peer"},
            GWebsocketPeer::GHandlers{
                .onOpen = [held = std::move(token)] {},
                .onMessage = {},
                .onClosed =
                    [&n_closed_calls](const boost::system::error_code &cause) {
                        CHECK_FALSE(cause);
                        ++n_closed_calls;
                    }
            }
        );
        peer->acceptHandshake(); // pending, and never run

        REQUIRE_FALSE(token_weak.expired());
        peer->abort();
        CHECK(token_weak.expired());
        peer->abort(); // idempotent
    }
    CHECK(n_closed_calls == 1);
}
