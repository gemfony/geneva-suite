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

// The two halves of the websocket transport that make connections: the listener (the server's
// side, GListenerI) and the connector (the client's side, GConnectorI).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <boost/asio.hpp>

#include "courtier/transport/GPeerI.hpp"
#include "courtier/transport/GWebsocketPeerTransport.hpp"

using boost::asio::ip::tcp;
using Gem::Courtier::GWirePeerId;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerEnd;
using Gem::Courtier::Consumers::GPeerHandlers;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Consumers::GWebsocketConnector;
using Gem::Courtier::Consumers::GWebsocketListener;
using Gem::Courtier::Consumers::GWebsocketPeer;
using namespace std::chrono_literals;

namespace {

/** @brief An io context with one thread; stops and joins it when it goes */
class OneThread {
public:
    OneThread()
      : thread_([this] { ctx_.run(); }) { /* nothing */
    }
    ~OneThread() {
        guard_.reset();
        ctx_.stop();
        thread_.join();
    }
    OneThread(const OneThread &) = delete;
    OneThread(OneThread &&) = delete;
    OneThread &operator=(const OneThread &) = delete;
    OneThread &operator=(OneThread &&) = delete;

    /** @brief @return The executor of the context */
    boost::asio::any_io_executor executor() {
        return ctx_.get_executor();
    }

private:
    boost::asio::io_context ctx_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_ =
        boost::asio::make_work_guard(ctx_);
    std::thread thread_;
};

/** @brief Polls @p condition for at most five seconds @return Whether it held */
template <typename Condition>
bool eventually(Condition condition) {
    const auto give_up_at = std::chrono::steady_clock::now() + 5s;
    while(not condition() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

/** @brief @return A port that nothing listens on (bound once, then let go) */
unsigned short freePort() {
    boost::asio::io_context ctx;
    const tcp::acceptor probe(ctx, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    return probe.local_endpoint().port();
}

/** @brief @param ping The ping interval @return The options of a peer with it */
GWebsocketPeer::GOptions withPing(std::chrono::milliseconds ping) {
    return GWebsocketPeer::GOptions{.name = "transport test", .pingInterval = ping};
}

} /* anonymous namespace */

/******************************************************************************/
/** A port that cannot be bound makes start() throw, with the port in the message; the listener
 *  is then as if it had not been started. */
TEST_CASE(
    "GWebsocketListener: a port that is taken makes start throw and names the port",
    "[courtier][websocket][transport]"
) {
    boost::asio::io_context ctx;
    tcp::acceptor taken(ctx, tcp::endpoint(tcp::v4(), 0));
    taken.listen();
    const unsigned short port = taken.local_endpoint().port();

    OneThread sessions;
    GWebsocketListener listener("listener test", port, withPing(1s), 1);
    std::string message;
    try {
        listener.start(sessions.executor(), [](const std::shared_ptr<GPeerI> &, GWirePeerId) {});
    }
    catch(const std::exception &e) {
        message = e.what();
    }
    CHECK(message.contains(std::to_string(port)));
}

/******************************************************************************/
/** Port 0 lets the system choose; the listener tells which port it got, and a connector reaches it:
 *  the listener hands the server's end to on_peer, on the shell's executor, and both ends open. */
TEST_CASE(
    "GWebsocketListener: port 0 yields a port that a connector reaches",
    "[courtier][websocket][transport]"
) {
    OneThread sessions;
    OneThread client;
    GWebsocketListener listener("listener test", 0, withPing(1s), 1);
    std::atomic<int> n_open{0};
    std::atomic<int> n_closed{0};
    std::atomic<GWirePeerId> seen_id{0};
    const auto counting = [&] {
        return GPeerHandlers{
            .onOpen = [&] { n_open.fetch_add(1); },
            .onMessage = {},
            .onClosed = [&](GPeerEnd, GPeerClose, std::string_view) { n_closed.fetch_add(1); }
        };
    };
    std::shared_ptr<GPeerI> server_end;
    std::mutex mutex;
    listener.start(sessions.executor(), [&](std::shared_ptr<GPeerI> peer, GWirePeerId id) {
        seen_id.store(id);
        peer->start(counting());
        const std::scoped_lock lock(mutex);
        server_end = std::move(peer);
    });
    REQUIRE(listener.getPort() != 0);

    GWebsocketConnector connector("localhost", listener.getPort(), withPing(1s));
    boost::asio::post(client.executor(), [&] {
        connector.connect(
            client.executor(),
            [&](const std::shared_ptr<GPeerI> &peer, std::string_view) {
                if(peer) {
                    peer->start(counting()); // from here on its pending work owns it
                }
            }
        );
    });
    REQUIRE(eventually([&] { return n_open.load() == 2; }));
    CHECK(seen_id.load() == 1);
    {
        const std::scoped_lock lock(mutex);
        server_end->close(GPeerClose::NORMAL);
    }
    REQUIRE(eventually([&] { return n_closed.load() == 2; }));
    listener.stop();
    listener.endWork();
}

/******************************************************************************/
/** A connect to a port nothing listens on reports an error and no peer. */
TEST_CASE(
    "GWebsocketConnector: a closed port reports an error and no peer",
    "[courtier][websocket][transport]"
) {
    OneThread client;
    GWebsocketConnector connector("localhost", freePort(), withPing(1s));
    std::atomic<bool> done{false};
    std::atomic<bool> got_peer{true};
    std::string error;
    boost::asio::post(client.executor(), [&] {
        connector.connect(
            client.executor(),
            [&](const std::shared_ptr<GPeerI> &peer, std::string_view why) {
                got_peer.store(peer != nullptr);
                error = std::string(why);
                done.store(true);
            }
        );
    });
    REQUIRE(eventually([&] { return done.load(); }));
    CHECK_FALSE(got_peer.load());
    CHECK_FALSE(error.empty());
}

/******************************************************************************/
/** An attempt that is given up does not report. */
TEST_CASE(
    "GWebsocketConnector: an attempt given up with cancel does not report",
    "[courtier][websocket][transport]"
) {
    OneThread client;
    GWebsocketConnector connector("localhost", freePort(), withPing(1s));
    std::atomic<bool> called{false};
    std::atomic<bool> posted{false};
    boost::asio::post(client.executor(), [&] {
        connector.connect(
            client.executor(),
            [&](const std::shared_ptr<GPeerI> &, std::string_view) { called.store(true); }
        );
        connector.cancel();
        posted.store(true);
    });
    REQUIRE(eventually([&] { return posted.load(); }));
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(called.load());
}

/******************************************************************************/
/** Without the keep-alive a lost client is not noticed by the transport: the consumer then needs
 *  the time lease. */
TEST_CASE(
    "GWebsocketListener: detectsLoss follows the keep-alive",
    "[courtier][websocket][transport]"
) {
    const GWebsocketListener with_ping("listener test", 0, withPing(1s), 1);
    const GWebsocketListener without_ping("listener test", 0, withPing(0ms), 1);
    CHECK(with_ping.detectsLoss());
    CHECK_FALSE(without_ping.detectsLoss());
}
