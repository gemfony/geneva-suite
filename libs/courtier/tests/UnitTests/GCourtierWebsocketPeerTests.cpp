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
 * Tests of GWebsocketPeer, one end of a courtier websocket connection, against the rules of
 * GPeerI.hpp: two peers are connected over the loopback interface and, unless a test says
 * otherwise, run on one io context, so every step is deterministic.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "common/concurrency/GCompletionLatchT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/transport/GPeerI.hpp"
#include "courtier/transport/GWebsocketPeer.hpp"

using boost::asio::ip::tcp;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerEnd;
using Gem::Courtier::Consumers::GPeerHandlers;
using Gem::Courtier::Consumers::GWebsocketPeer;
using Gem::Courtier::Consumers::makeMessage;
using namespace std::chrono_literals;

namespace {

/** @brief What one end of the connection has seen. */
struct GSeen {
    std::vector<std::string> messages;
    int nOpen = 0;
    int nClosed = 0;
    GPeerEnd end = GPeerEnd::FAILED;
    GPeerClose how = GPeerClose::NORMAL;
};

/** @brief Two connected sockets on @p ctx: first the accepting end, second the connecting end. */
std::pair<tcp::socket, tcp::socket> connectedPair(boost::asio::io_context &ctx) {
    tcp::acceptor acceptor(ctx, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    tcp::socket connecting(ctx);
    connecting.connect(acceptor.local_endpoint());
    return {acceptor.accept(), std::move(connecting)};
}

/** @brief The server's end (waits for the handshake) on @p socket @param socket The socket
 *  @param options How it is set up @return The peer, not started */
std::shared_ptr<GWebsocketPeer>
serverEnd(tcp::socket socket, GWebsocketPeer::GOptions options = {.name = "server end"}) {
    return std::make_shared<GWebsocketPeer>(std::move(socket), std::move(options));
}

/** @brief The client's end (performs the handshake) on @p socket @param socket The socket
 *  @param options How it is set up @return The peer, not started */
std::shared_ptr<GWebsocketPeer>
clientEnd(tcp::socket socket, GWebsocketPeer::GOptions options = {.name = "client end"}) {
    return std::make_shared<GWebsocketPeer>(
        std::move(socket),
        std::move(options),
        boost::asio::any_io_executor{},
        "localhost"
    );
}

/** @brief Starts @p peer with handlers that record what it is told in @p seen. @p on_open and
 *  @p on_message let a test act from inside the handlers. */
void startRecording(
    const std::shared_ptr<GWebsocketPeer> &peer,
    GSeen &seen,
    std::move_only_function<void()> on_open = {},
    std::move_only_function<void(const std::string &)> on_message = {}
) {
    peer->start(
        GPeerHandlers{
            .onOpen =
                [&seen, on_open = std::move(on_open)]() mutable {
                    ++seen.nOpen;
                    if(on_open) {
                        on_open();
                    }
                },
            .onMessage =
                [&seen, on_message = std::move(on_message)](std::string &&message) mutable {
                    seen.messages.push_back(std::move(message));
                    if(on_message) {
                        on_message(seen.messages.back());
                    }
                },
            .onClosed =
                [&seen](GPeerEnd end, GPeerClose how, std::string_view) {
                    ++seen.nClosed;
                    seen.end = end;
                    seen.how = how;
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
    auto server = serverEnd(std::move(accepted));
    auto client = clientEnd(std::move(connecting));
    client_weak = client;
    startRecording(server, server_seen, {}, [&](const std::string &) {
        if(server_seen.messages.size() == N_BURST) {
            if(auto peer = client_weak.lock()) {
                peer->close(GPeerClose::NORMAL);
            }
        }
    });
    startRecording(client, client_seen, [&] {
        auto peer = client_weak.lock();
        REQUIRE(peer);
        for(std::size_t i = 0; i < N_BURST; ++i) {
            peer->send(makeMessage(
                i == N_BURST / 2 ? std::string(LARGE_MESSAGE_BYTES, 'x') : std::to_string(i)
            ));
        }
    });
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
// Each end is told exactly once that the connection is over, and why: the end that closed sees its
// own close, the other one a regular close by the other end, with what the close said.
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
    auto server = serverEnd(std::move(accepted));
    auto client = clientEnd(std::move(connecting));
    client_weak = client;
    startRecording(server, server_seen);
    startRecording(client, client_seen, [&] {
        auto peer = client_weak.lock();
        REQUIRE(peer);
        peer->send(makeMessage("last words"));
        peer->close(GPeerClose::RUN_ENDED);
        peer->close(GPeerClose::NORMAL); // a second close changes nothing: the first decides
        peer->send(makeMessage("too late")); // after close(): dropped
    });
    client->send(
        makeMessage("too early")
    ); // not open yet: dropped, not written into a stream that is not ready
    server.reset();
    client.reset();

    REQUIRE(runsDry(ctx));

    CHECK(server_seen.nOpen == 1);
    CHECK(client_seen.nOpen == 1);
    REQUIRE(server_seen.messages.size() == 1);
    CHECK(server_seen.messages.front() == "last words");
    CHECK(client_seen.nClosed == 1);
    CHECK(client_seen.end == GPeerEnd::CLOSED_HERE);
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.end == GPeerEnd::CLOSED_THERE);
    CHECK(server_seen.how == GPeerClose::RUN_ENDED);
}

/******************************************************************************/
// A close whose other end vanishes is still this end's close (GPeerI.hpp, rule 6): the connection
// breaks while the close handshake is under way. It is reported once, as CLOSED_HERE, and not as an
// error.
TEST_CASE(
    "GWebsocketPeer reports its own close when the other end vanishes",
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
    auto client = clientEnd(std::move(connecting));
    client_weak = client;
    startRecording(client, client_seen, [&] {
        if(auto peer = client_weak.lock()) {
            peer->close(GPeerClose::NORMAL);
        }
    });
    client.reset();

    REQUIRE(runsDry(ctx));

    CHECK(client_seen.nClosed == 1);
    CHECK(client_seen.end == GPeerEnd::CLOSED_HERE);
    CHECK(client_weak.expired());
}

/******************************************************************************/
// A close whose queued messages cannot be written: the other end shakes hands and then reads
// nothing, so the messages handed over before the close fill the socket's buffers and the close
// frame never goes out. The end comes at the handshake limit (or the idle limit, both 200 ms here)
// and is this end's close.
TEST_CASE(
    "GWebsocketPeer reports a close whose queued messages cannot be written as its own close",
    "[courtier][websocket][peer]"
) {
    constexpr std::size_t N_LARGE = 8; // 24 MiB: far more than the buffers of a loopback socket
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen client_seen;

    boost::beast::websocket::stream<tcp::socket> deaf(std::move(accepted));
    deaf.async_accept([](const boost::system::error_code &ec) { REQUIRE_FALSE(ec); });

    std::weak_ptr<GWebsocketPeer> client_weak;
    auto client = clientEnd(
        std::move(connecting),
        GWebsocketPeer::GOptions{.name = "client end", .pingInterval = 100ms}
    );
    client_weak = client;
    startRecording(client, client_seen, [&] {
        if(auto peer = client_weak.lock()) {
            for(std::size_t i = 0; i < N_LARGE; ++i) {
                peer->send(makeMessage(std::string(LARGE_MESSAGE_BYTES, 'x')));
            }
            peer->close(GPeerClose::NORMAL);
        }
    });
    client.reset();

    REQUIRE(runsDry(ctx));

    CHECK(client_seen.nClosed == 1);
    CHECK(client_seen.end == GPeerEnd::CLOSED_HERE);
    CHECK(client_weak.expired());
}

/******************************************************************************/
// A close sends what was handed over before it (rule 6): a message of many frames that is being
// written when close() is called arrives whole, and so does the one queued behind it; the close
// frame follows them.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; the count comes from the assertion macros
TEST_CASE(
    "GWebsocketPeer sends what was handed over before a close",
    "[courtier][websocket][peer]"
) {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;
    GSeen client_seen;

    std::weak_ptr<GWebsocketPeer> client_weak;
    auto server = serverEnd(std::move(accepted));
    auto client = clientEnd(std::move(connecting));
    client_weak = client;
    startRecording(server, server_seen);
    startRecording(client, client_seen, [&] {
        auto peer = client_weak.lock();
        REQUIRE(peer);
        peer->send(makeMessage(std::string(LARGE_MESSAGE_BYTES, 'x'))); // many frames
        peer->send(makeMessage("waits behind the large one"));
        peer->close(GPeerClose::NORMAL);
    });
    server.reset();
    client.reset();

    REQUIRE(runsDry(ctx));

    REQUIRE(server_seen.messages.size() == 2);
    CHECK(server_seen.messages[0].size() == LARGE_MESSAGE_BYTES);
    CHECK(server_seen.messages[1] == "waits behind the large one");
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.end == GPeerEnd::CLOSED_THERE);
    CHECK(client_seen.nClosed == 1);
    CHECK(client_seen.end == GPeerEnd::CLOSED_HERE);
}

/******************************************************************************/
// What arrives after close() was called (GPeerI.hpp, rule 6). The other end is a plain Beast stream
// on a thread of its own, so that the test decides the order of the frames; it is a client, and
// closes with 1011 (an error). The end under test is a server's end. Whatever read the other end's close -- the peer's own read, or Beast's close
// operation, which does not call the control callback -- the end under test reports what it said.
// (The in-process pair of the tests has no second reader of a close and cannot show the latter.)
//  - The other end's message and close arrive before this end's close has gone out (its queue
//    cannot be written: the other end reads nothing until it closes): the message is handed to
//    the role, and the other end's close is answered by the websocket handshake; the end under
//    test began the close and reports CLOSED_HERE.
//  - This end's close has gone out before the other end sends its message and its own close: Beast
//    discards the message, which the role never sees; the end reports CLOSED_HERE.
//  - The other end answers this end's close in the websocket handshake: CLOSED_HERE.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- three linear scenarios; the count comes from the assertion macros
TEST_CASE(
    "GWebsocketPeer delivers what arrives after close() until its own close has gone out",
    "[courtier][websocket][peer]"
) {
    using namespace std::chrono_literals;
    constexpr std::size_t N_LARGE = 8; // 24 MiB: far more than the buffers of a loopback socket
    const std::string late = "sent before the other end knew of the close";
    enum class Order : std::uint8_t { THEIRS_FIRST, OURS_FIRST, ANSWERED };
    const auto which = GENERATE(Order::THEIRS_FIRST, Order::OURS_FIRST, Order::ANSWERED);

    boost::asio::io_context ctx;
    tcp::acceptor acceptor(ctx, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    GSeen server_seen;
    std::weak_ptr<GWebsocketPeer> server_weak;
    // the end under test is open (and has queued its messages)
    const auto opened = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    // close() has been called on it
    const auto closed_here = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    std::promise<std::uint16_t> their_view; // the code of the close frame the other end read
    boost::asio::io_context own;
    boost::beast::websocket::stream<tcp::socket> ws(own);
    const std::jthread other_end([&, port = acceptor.local_endpoint().port(),
                                  open = opened,
                                  ours_called = closed_here]() {
        try {
            ws.next_layer().connect({boost::asio::ip::address_v4::loopback(), port});
            ws.handshake("localhost", "/");
            // The end under test is closed only now, so that its close frame does not arrive
            // together with the answer to the handshake (Beast would read it with the answer)
            open->wait_for(10s);
            boost::asio::post(ctx, [&server_weak, closed_here] {
                if(auto peer = server_weak.lock()) {
                    peer->close(GPeerClose::NORMAL);
                }
                closed_here->count_down();
            });
            ours_called->wait_for(10s);
            if(which == Order::OURS_FIRST) {
                // Waits until the close frame of the end under test has arrived (it goes out at
                // once: nothing was queued before it); nothing reads it before the message below
                const auto until = std::chrono::steady_clock::now() + 10s;
                while(ws.next_layer().available() == 0 && std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            if(which == Order::ANSWERED) {
                boost::beast::flat_buffer buffer;
                boost::system::error_code ec;
                std::ignore = ws.read(buffer, ec); // reads the close and answers it (Beast)
                their_view.set_value(ws.reason().code);
                return;
            }
            ws.write(boost::asio::buffer(late));
            ws.close(boost::beast::websocket::close_code::internal_error); // reads until the other's
            their_view.set_value(ws.reason().code);
        }
        catch(...) {
            their_view.set_exception(std::current_exception());
        }
    });

    auto server = serverEnd(acceptor.accept());
    server_weak = server;
    startRecording(server, server_seen, [&] {
        if(auto peer = server_weak.lock()) {
            if(which == Order::THEIRS_FIRST) {
                for(std::size_t i = 0; i < N_LARGE; ++i) {
                    peer->send(makeMessage(std::string(LARGE_MESSAGE_BYTES, 'x')));
                }
            }
        }
        opened->count_down();
    });
    server.reset();

    // The context runs until the end under test has ended and the other end is done, which
    // depend on each other; should they not, the other end's socket is shut, so that its thread
    // ends and the test fails instead of hanging.
    auto view = their_view.get_future();
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while(not(ctx.stopped() && view.wait_for(0s) == std::future_status::ready) &&
          std::chrono::steady_clock::now() < deadline) {
        if(ctx.stopped()) {
            std::ignore = view.wait_for(20ms);
        }
        else {
            ctx.run_for(20ms);
        }
    }
    const bool done = ctx.stopped() && view.wait_for(0s) == std::future_status::ready;
    if(not done) {
        boost::system::error_code ignored;
        std::ignore = ws.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
    }
    INFO("order " << static_cast<int>(which));
    REQUIRE(done);
    const std::uint16_t their_code = view.get();

    CHECK(server_seen.nClosed == 1);
    switch(which) {
    case Order::THEIRS_FIRST:
        CHECK(server_seen.messages == std::vector<std::string>{late});
        CHECK(server_seen.end == GPeerEnd::CLOSED_HERE); // this end began the close
        CHECK(server_seen.how == GPeerClose::ERROR); // read by the peer's own read
        CHECK(their_code == boost::beast::websocket::close_code::internal_error); // answered
        break;
    case Order::OURS_FIRST:
        CHECK(server_seen.messages.empty()); // discarded by Beast after our close went out
        CHECK(server_seen.end == GPeerEnd::CLOSED_HERE); // the closes crossed
        CHECK(server_seen.how == GPeerClose::ERROR); // read by Beast's close operation
        break;
    case Order::ANSWERED:
        CHECK(server_seen.messages.empty());
        CHECK(server_seen.end == GPeerEnd::CLOSED_HERE);
        CHECK(server_seen.how == GPeerClose::NORMAL); // the echo of its own close
        break;
    }
}

/******************************************************************************/
// The other end sends D. What the role under test is told depends on when it asks for its own
// close (GPeerI.hpp, rules 1 and 6). Its handler strand, on a context of its own, does not run
// until the closes are over:
//  - DURING_HANDSHAKE: the role closes before its end has opened; the other end sends D and
//    closes. The role is told nothing but the end (rule 1): CLOSED_HERE.
//  - AFTER_THEIR_CLOSE: the end has opened; the other end sends D and closes, and the end, which
//    reads from the open on, answers that close before the role has been told of anything. The
//    role is then told of the open and of D, and the connection was closed there.
//  - LIVE: the end has opened and has read D; the role closes on the live connection before it
//    has been told of the open. The role is told of the open and of D (what was read before its
//    close went out), and the end reports CLOSED_HERE.
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- one linear scenario; the count comes from the assertion macros
TEST_CASE(
    "GWebsocketPeer tells a role what arrived before its close unless it closed before the open",
    "[courtier][websocket][peer]"
) {
    using namespace std::chrono_literals;
    enum class Moment : std::uint8_t {
        DURING_HANDSHAKE,
        AFTER_THEIR_CLOSE,
        LIVE
    };
    const Moment moment =
        GENERATE(Moment::DURING_HANDSHAKE, Moment::AFTER_THEIR_CLOSE, Moment::LIVE);
    INFO(
        (moment == Moment::DURING_HANDSHAKE
             ? "the role closes during the handshake"
             : (moment == Moment::AFTER_THEIR_CLOSE
                    ? "the other end's close is answered before the role is told of the open"
                    : "the role closes a live connection before it is told of the open"))
    );
    boost::asio::io_context ctx;     // the socket's executor
    boost::asio::io_context handler; // the handler strand's context: held until the role closes
    tcp::acceptor acceptor(ctx, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    GSeen server_seen;
    std::atomic<bool> shaken{false};
    std::atomic<bool> ponged{false}; // LIVE: the end has answered a ping sent after D
    const auto go = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    std::promise<std::uint16_t> their_view;
    boost::asio::io_context own;
    boost::beast::websocket::stream<tcp::socket> ws(own);
    const std::jthread other_end([&, port = acceptor.local_endpoint().port(), sent = go]() {
        try {
            ws.next_layer().connect({boost::asio::ip::address_v4::loopback(), port});
            ws.handshake("localhost", "/");
            shaken.store(true);
            sent->wait_for(10s);
            ws.write(boost::asio::buffer(std::string("D")));
            if(moment == Moment::LIVE) {
                // The end answers the ping only once it has read D, which precedes the ping on the
                // wire: the pong proves that D has been read
                ws.control_callback([&ponged](
                                        boost::beast::websocket::frame_type kind,
                                        [[maybe_unused]] boost::beast::string_view payload
                                    ) {
                    if(kind == boost::beast::websocket::frame_type::pong) {
                        ponged.store(true);
                    }
                });
                ws.ping({});
                // Waits for the server's close (the pong arrives on the way); Beast answers the
                // close, and the read ends with closed
                boost::beast::flat_buffer buffer;
                try {
                    ws.read(buffer);
                }
                catch(const boost::system::system_error &e) {
                    if(e.code() != boost::beast::websocket::error::closed) {
                        throw;
                    }
                }
            }
            else {
                ws.close(boost::beast::websocket::close_code::internal_error);
            }
            their_view.set_value(ws.reason().code);
        }
        catch(...) {
            their_view.set_exception(std::current_exception());
        }
    });

    auto server = std::make_shared<GWebsocketPeer>(
        acceptor.accept(),
        GWebsocketPeer::GOptions{.name = "server end"},
        handler.get_executor()
    );
    startRecording(server, server_seen);
    if(moment == Moment::DURING_HANDSHAKE) {
        server->close(GPeerClose::NORMAL); // ordered after start()'s work, before the handshake
    }
    // Runs a context for a moment, also one that had run out of work before
    const auto run = [](boost::asio::io_context &context) {
        if(context.stopped()) {
            context.restart();
        }
        context.run_for(10ms);
    };
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while(not shaken.load() && std::chrono::steady_clock::now() < deadline) {
        run(ctx); // the handshake; the role is not told (its strand does not run)
    }
    REQUIRE(shaken.load());
    go->count_down(); // D, then (LIVE) a ping or (else) the other end's close are on their way
    auto view = their_view.get_future();
    if(moment == Moment::LIVE) {
        while(not ponged.load() && std::chrono::steady_clock::now() < deadline) {
            run(ctx); // the end reads D and answers the ping
        }
        CHECK(ponged.load()); // not REQUIRE: the other end still waits for the close below
        server->close(GPeerClose::NORMAL); // live: the role closes before it was told of the open
    }
    while(view.wait_for(0s) != std::future_status::ready &&
          std::chrono::steady_clock::now() < deadline) {
        run(ctx); // the close handshake, with the role's strand still held
    }
    if(moment == Moment::AFTER_THEIR_CLOSE) {
        server->close(GPeerClose::NORMAL); // the connection has ended: this close changes nothing
    }
    server.reset();
    while((server_seen.nClosed != 1 || view.wait_for(0s) != std::future_status::ready) &&
          std::chrono::steady_clock::now() < deadline) {
        run(ctx);
        run(handler);
    }
    const bool done = view.wait_for(0s) == std::future_status::ready;
    if(not done) {
        boost::system::error_code ignored;
        std::ignore = ws.next_layer().shutdown(tcp::socket::shutdown_both, ignored);
    }
    REQUIRE(done);
    CHECK(server_seen.nClosed == 1);
    if(moment == Moment::DURING_HANDSHAKE) {
        // The role heard nothing of D, only its own end
        CHECK(server_seen.nOpen == 0);
        CHECK(server_seen.messages.empty());
        CHECK(server_seen.end == GPeerEnd::CLOSED_HERE);
    }
    else if(moment == Moment::LIVE) {
        // The role was handed D, and the end is its own close
        CHECK(server_seen.nOpen == 1);
        CHECK(server_seen.messages == std::vector<std::string>{"D"});
        CHECK(server_seen.end == GPeerEnd::CLOSED_HERE);
    }
    else {
        // Answered (the other end's own code came back), and the role was handed D
        CHECK(
            view.get() ==
            static_cast<std::uint16_t>(boost::beast::websocket::close_code::internal_error)
        );
        CHECK(server_seen.nOpen == 1);
        CHECK(server_seen.messages == std::vector<std::string>{"D"});
        CHECK(server_seen.end == GPeerEnd::CLOSED_THERE);
        CHECK(server_seen.how == GPeerClose::ERROR);
    }
}

/******************************************************************************/
// An end that never gets its handshake is told so, without being told that it was open (rule 1).
TEST_CASE("GWebsocketPeer reports a handshake that fails", "[courtier][websocket][peer]") {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;

    auto server = serverEnd(std::move(accepted));
    startRecording(server, server_seen);
    server.reset();

    connecting.close(); // the other end goes away without a handshake

    REQUIRE(runsDry(ctx));

    CHECK(server_seen.nOpen == 0);
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.end == GPeerEnd::FAILED);
}

/******************************************************************************/
// A close requested while the handshake is still under way, from a thread that is not the peer's,
// ends each end exactly once and without a crash, however the two race. Each end does its I/O on
// a strand of a context with two threads.
TEST_CASE(
    "GWebsocketPeer ends once when it is closed before its handshake has completed",
    "[courtier][websocket][peer]"
) {
    constexpr int N_ROUNDS = 200;
    boost::asio::io_context ctx;
    auto guard = boost::asio::make_work_guard(ctx);
    std::vector<std::jthread> threads;
    threads.reserve(2);
    for(int t = 0; t < 2; ++t) {
        threads.emplace_back([&ctx] { ctx.run(); });
    }
    tcp::acceptor acceptor(ctx, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));

    int n_wrong = 0;
    for(int round = 0; round < N_ROUNDS; ++round) {
        tcp::socket connecting(boost::asio::make_strand(ctx));
        connecting.connect(acceptor.local_endpoint());
        tcp::socket accepted = acceptor.accept(boost::asio::make_strand(ctx));

        auto server_closed = std::make_shared<std::atomic<int>>(0);
        auto client_closed = std::make_shared<std::atomic<int>>(0);
        const auto counting = [](const std::shared_ptr<std::atomic<int>> &n) {
            return GPeerHandlers{
                .onOpen = {},
                .onMessage = {},
                .onClosed = [n](GPeerEnd, GPeerClose, std::string_view) { n->fetch_add(1); }
            };
        };
        auto server = serverEnd(std::move(accepted));
        auto client = clientEnd(std::move(connecting));
        server->start(counting(server_closed));
        client->start(counting(client_closed));
        client->close(GPeerClose::NORMAL);
        if(round % 2 == 0) {
            server->close(GPeerClose::RUN_ENDED);
        }
        server.reset();
        client.reset();

        const auto give_up_at = std::chrono::steady_clock::now() + 10s;
        while((server_closed->load() == 0 || client_closed->load() == 0) &&
              std::chrono::steady_clock::now() < give_up_at) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(1ms); // a second report would come now
        if(server_closed->load() != 1 || client_closed->load() != 1) {
            ++n_wrong;
        }
    }
    guard.reset();
    threads.clear();
    CHECK(n_wrong == 0);
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
        auto peer = serverEnd(std::move(accepted));
        peer->start(
            GPeerHandlers{
                .onOpen = [held = std::move(token)] {},
                .onMessage = {},
                .onClosed =
                    [&n_closed_calls](GPeerEnd end, GPeerClose, std::string_view) {
                        CHECK(end == GPeerEnd::CLOSED_HERE);
                        ++n_closed_calls;
                    }
            }
        ); // the handshake is posted, and never run

        REQUIRE_FALSE(token_weak.expired());
        peer->abort();
        CHECK(token_weak.expired());
        peer->abort(); // idempotent
    }
    CHECK(n_closed_calls == 1);
}

/******************************************************************************/
// A role may take longer over a message than the idle limit allows without costing its connection,
// as long as the peer's I/O runs on a thread that does not run the role: the peer goes on reading,
// and answering the other end's pings, while the role works. Here the server's role takes three
// ping intervals over the question; the connection must survive it and end in the regular way.
// Each end does its I/O on a thread of its own, so the client notices if the server falls silent.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; the count comes from the assertion macros
TEST_CASE(
    "GWebsocketPeer keeps its connection while its role works on a message for three ping "
    "intervals",
    "[courtier][websocket][peer]"
) {
    using namespace std::chrono_literals;
    // The handlers' context goes last: the peers that the I/O contexts' handlers still hold (on a
    // run that fails) are destroyed with those, and their strands are on this one.
    boost::asio::io_context role_ctx;  // the handlers of both ends
    boost::asio::io_context server_io; // the I/O of the server's end
    boost::asio::io_context client_io; // the I/O of the client's end
    tcp::acceptor acceptor(server_io, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    tcp::socket connecting(client_io);
    connecting.connect(acceptor.local_endpoint());
    tcp::socket accepted = acceptor.accept();
    GSeen server_seen;
    GSeen client_seen;
    const GWebsocketPeer::GOptions options{.name = "test peer", .pingInterval = 1s};

    std::weak_ptr<GWebsocketPeer> server_weak;
    std::weak_ptr<GWebsocketPeer> client_weak;
    auto server =
        std::make_shared<GWebsocketPeer>(std::move(accepted), options, role_ctx.get_executor());
    auto client = std::make_shared<GWebsocketPeer>(
        std::move(connecting),
        options,
        role_ctx.get_executor(),
        "localhost"
    );
    server_weak = server;
    client_weak = client;
    server->start(
        GPeerHandlers{
            .onOpen = [&server_seen] { ++server_seen.nOpen; },
            .onMessage =
                [&server_seen, &server_weak](std::string &&message) {
                    server_seen.messages.push_back(std::move(message));
                    std::this_thread::sleep_for(3s); // longer than the idle limit of 2 s
                    if(auto peer = server_weak.lock()) {
                        peer->send(makeMessage("answer"));
                    }
                },
            .onClosed =
                [&server_seen](GPeerEnd end, GPeerClose how, std::string_view) {
                    ++server_seen.nClosed;
                    server_seen.end = end;
                    server_seen.how = how;
                }
        }
    );
    client->start(
        GPeerHandlers{
            .onOpen =
                [&client_seen, &client_weak] {
                    ++client_seen.nOpen;
                    if(auto peer = client_weak.lock()) {
                        peer->send(makeMessage("question"));
                    }
                },
            .onMessage =
                [&client_seen, &client_weak](std::string &&message) {
                    client_seen.messages.push_back(std::move(message));
                    if(auto peer = client_weak.lock()) {
                        peer->close(GPeerClose::NORMAL);
                    }
                },
            .onClosed =
                [&client_seen](GPeerEnd end, GPeerClose how, std::string_view) {
                    ++client_seen.nClosed;
                    client_seen.end = end;
                    client_seen.how = how;
                }
        }
    );
    server.reset();
    client.reset();

    {
        const std::jthread server_thread([&server_io] { server_io.run_for(20s); });
        const std::jthread client_thread([&client_io] { client_io.run_for(20s); });
        role_ctx.run_for(20s); // returns when both ends have been told that the connection is over
    }

    REQUIRE(server_seen.messages.size() == 1);
    CHECK(server_seen.messages.front() == "question");
    REQUIRE(client_seen.messages.size() == 1);
    CHECK(client_seen.messages.front() == "answer");
    CHECK(client_seen.nClosed == 1);
    CHECK(client_seen.end == GPeerEnd::CLOSED_HERE); // it did not give up on a silent server
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.end == GPeerEnd::CLOSED_THERE);
}

/******************************************************************************/
// An end opens when its handshake has completed, and from then on it reads -- and so answers the
// other end's pings -- also while its role has not yet been told of the open: the role's executor
// may be busy for any length of time (on a server, every session thread may be encoding a large
// item). Here the server's handlers' context does not run at all for two idle limits after the
// handshake; the client's end must not give up on the server meanwhile. When the context runs
// again, the server's role is told of the open first and then of the message that arrived in the
// meantime (rule 1).
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; the count comes from the assertion macros
TEST_CASE(
    "GWebsocketPeer answers pings from its handshake on while its role's executor is busy",
    "[courtier][websocket][peer]"
) {
    using namespace std::chrono_literals;
    constexpr auto PING = 1s; // the idle limit is 2 s
    constexpr auto HOLD = 4s; // the server's handlers do not run for two idle limits
    // The handlers' context goes last: the peers that the I/O context's handlers still hold (on a
    // run that fails) are destroyed with those, and the server's strand is on this one.
    boost::asio::io_context role_ctx; // the server's handlers: held, then run
    boost::asio::io_context ctx;      // the I/O of both ends, and the client's handlers
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;
    GSeen client_seen;
    bool open_came_first = false;

    auto server = std::make_shared<GWebsocketPeer>(
        std::move(accepted),
        GWebsocketPeer::GOptions{.name = "server end", .pingInterval = PING},
        role_ctx.get_executor()
    );
    auto client = clientEnd(std::move(connecting), {.name = "client end", .pingInterval = PING});
    const std::weak_ptr<GWebsocketPeer> client_weak = client;
    startRecording(server, server_seen, {}, [&server_seen, &open_came_first](const std::string &) {
        open_came_first = server_seen.nOpen == 1;
    });
    startRecording(client, client_seen, [&client_weak] {
        if(auto peer = client_weak.lock()) {
            peer->send(makeMessage("question"));
        }
    });
    server.reset();
    client.reset();

    // Runs a context for a moment, also one that had run out of work before
    const auto run = [](boost::asio::io_context &context) {
        if(context.stopped()) {
            context.restart();
        }
        context.run_for(10ms);
    };
    const auto hold_until = std::chrono::steady_clock::now() + HOLD;
    while(std::chrono::steady_clock::now() < hold_until) {
        run(ctx); // the handshake, the question, and the keep-alive of both ends
    }
    CHECK(client_seen.nOpen == 1);
    CHECK(client_seen.nClosed == 0); // the client did not give up on a server that reads
    CHECK(server_seen.nOpen == 0);   // the server's role has not been told yet

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while(server_seen.messages.empty() && std::chrono::steady_clock::now() < deadline) {
        run(role_ctx);
        run(ctx);
    }
    CHECK(server_seen.nOpen == 1);
    REQUIRE(server_seen.messages.size() == 1);
    CHECK(server_seen.messages.front() == "question");
    CHECK(open_came_first);

    if(auto peer = client_weak.lock()) {
        peer->close(GPeerClose::NORMAL);
    }
    while((server_seen.nClosed == 0 || client_seen.nClosed == 0) &&
          std::chrono::steady_clock::now() < deadline) {
        run(ctx);
        run(role_ctx);
    }
    CHECK(client_seen.nClosed == 1);
    CHECK(client_seen.end == GPeerEnd::CLOSED_HERE);
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.end == GPeerEnd::CLOSED_THERE);
}

/******************************************************************************/
// The other end's close is its close, also when the end of the connection is not completed: here
// the other end sends a close frame and then neither reads nor ends the TCP connection, so the
// close handshake cannot finish and Beast ends it by its time limit. The peer must still report a
// regular close by the other end, not an expired idle limit -- the other end did say that it
// closes. (In a gate run the same case once came about by chance: a client's close arrived just
// as the server's keep-alive timer expired.)
TEST_CASE(
    "GWebsocketPeer reports a close of the other end as such when the connection does not end",
    "[courtier][websocket][peer]"
) {
    using namespace std::chrono_literals;
    boost::asio::io_context ctx;
    tcp::acceptor acceptor(ctx, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));

    const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    const std::jthread other_end([port = acceptor.local_endpoint().port(),
                                  held = release]() {
        boost::asio::io_context own;
        boost::beast::websocket::stream<tcp::socket> ws(own);
        ws.next_layer().connect({boost::asio::ip::address_v4::loopback(), port});
        ws.handshake("localhost", "/");
        // A close frame written by hand (masked, as a client's frames are; code 1000)
        const std::array<unsigned char, 8> close_frame{0x88, 0x82, 0, 0, 0, 0, 0x03, 0xE8};
        boost::asio::write(ws.next_layer(), boost::asio::buffer(close_frame));
        held->wait_for(15s); // holds the connection open, and reads nothing
    });

    GSeen server_seen;
    auto server = serverEnd(
        acceptor.accept(),
        GWebsocketPeer::GOptions{.name = "test peer", .pingInterval = 1s}
    );
    startRecording(server, server_seen);
    server.reset();

    const bool dry = runsDry(ctx);
    release->count_down();
    REQUIRE(dry);
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.end == GPeerEnd::CLOSED_THERE);
}

/******************************************************************************/
// A close asked for while the handshake is under way, and a handshake that then fails: the end could
// not be opened, and says so (rule 1), not that it was closed here.
TEST_CASE(
    "GWebsocketPeer reports a failed handshake as FAILED also after a close",
    "[courtier][websocket][peer]"
) {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;

    auto server = serverEnd(std::move(accepted));
    startRecording(server, server_seen);
    server->close(GPeerClose::RUN_ENDED);
    server.reset();

    connecting.close(); // the other end goes away without a handshake

    REQUIRE(runsDry(ctx));

    CHECK(server_seen.nOpen == 0);
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.end == GPeerEnd::FAILED);
}

/******************************************************************************/
// onOpen is not delivered after close() (and so no message either, rule 1): a close asked for while
// the handshake is under way, and a handshake that then succeeds.
TEST_CASE(
    "GWebsocketPeer does not report onOpen after close()",
    "[courtier][websocket][peer]"
) {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;
    GSeen client_seen;

    auto server = serverEnd(std::move(accepted));
    startRecording(server, server_seen);
    server->close(GPeerClose::RUN_ENDED);
    server.reset();
    ctx.run_for(50ms); // the server's end waits for the handshake

    auto client = clientEnd(std::move(connecting));
    startRecording(client, client_seen);
    client.reset();

    REQUIRE(runsDry(ctx));
    CHECK(server_seen.nOpen == 0);
    CHECK(server_seen.nClosed == 1);
    CHECK(client_seen.nClosed == 1);
    CHECK(client_seen.how == GPeerClose::RUN_ENDED);
}

/******************************************************************************/
// A handshake that runs out of time: the other end keeps its TCP connection open and never sends its
// upgrade request. The end was never opened, so it reports FAILED (rule 1), not SILENT, which is for a
// connection that was open -- with or without a close asked for while the handshake was under way.
// A ping interval of 100 ms makes the handshake limit 200 ms.
TEST_CASE(
    "GWebsocketPeer reports a handshake that runs out of time as FAILED",
    "[courtier][websocket][peer]"
) {
    const bool close_first = GENERATE(false, true);
    INFO((close_first ? "with a close during the handshake" : "without a close"));
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    GSeen server_seen;

    auto server = serverEnd(
        std::move(accepted),
        GWebsocketPeer::GOptions{.name = "server end", .pingInterval = 100ms}
    );
    startRecording(server, server_seen);
    if(close_first) {
        server->close(GPeerClose::RUN_ENDED);
    }
    server.reset();

    REQUIRE(runsDry(ctx)); // the other end is open all the while, and says nothing
    CHECK(server_seen.nOpen == 0);
    CHECK(server_seen.nClosed == 1);
    CHECK(server_seen.end == GPeerEnd::FAILED);
}

/******************************************************************************/
// A client's end starts with its own interval and takes the server's when it is told
// (GPeerI::setKeepAlive(), from the HELLO): Beast's limits change on the live stream and hold from
// its next timer event on. Here the end starts with 2 s (an idle limit of 4 s) and is told 100 ms
// when it opens; the other end completes the handshake and then never reads (it answers no ping).
// The end must report SILENT after Beast's first timer event (2 s) and the new limit, well before
// the 4 s of the old one.
TEST_CASE(
    "GWebsocketPeer takes the interval it is told on the live stream",
    "[courtier][websocket][peer]"
) {
    boost::asio::io_context ctx;
    auto [accepted, connecting] = connectedPair(ctx);
    // The other end: the server's side of the handshake, then nothing until the test ends
    const auto test_over = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    std::jthread other([socket = std::move(accepted), test_over]() mutable {
        boost::beast::websocket::stream<tcp::socket> ws(std::move(socket));
        boost::system::error_code ec;
        ws.accept(ec);
        test_over->wait();
    });

    auto client = clientEnd(std::move(connecting), {.name = "client end", .pingInterval = 2s});
    const std::weak_ptr<GWebsocketPeer> client_weak = client;
    GSeen seen;
    std::chrono::steady_clock::time_point opened_at;
    startRecording(client, seen, [&opened_at, client_weak] {
        opened_at = std::chrono::steady_clock::now();
        if(auto peer = client_weak.lock()) {
            peer->setKeepAlive(100ms); // on the executor, as the client's role calls it
        }
    });
    client.reset();
    CHECK(runsDry(ctx));
    const auto took = std::chrono::steady_clock::now() - opened_at;
    test_over->count_down();

    CHECK(seen.nOpen == 1);
    CHECK(seen.nClosed == 1);
    CHECK(seen.end == GPeerEnd::SILENT);
    CHECK(took < 3s);
}
