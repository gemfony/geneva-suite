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

// The MQTT transport's own cases, against the tests' broker (the CTest fixture of
// tests/mqttTestBroker.sh): the handshake through listener and connector, the order of what a
// connection carries and its close, a broker that cannot be reached, a broker connection that is
// cut (the end fails, the other end hears the will, and nothing connects again), the largest
// message, and topic prefixes that keep servers apart. The rules of GPeerI.hpp are checked on this
// transport by GCourtierPeerRulesTests.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include "GInProcessTestSupport.hpp"
#include "GMqttTestSupport.hpp"
#include "GPeerTestRole.hpp"
#include "GTcpRelay.hpp"
#include "common/GExceptions.hpp"
#include "courtier/transport/GMqttPeerTransport.hpp"
#include "courtier/transport/GPeerI.hpp"

using Gem::Courtier::Consumers::GMqttConnector;
using Gem::Courtier::Consumers::GMqttListener;
using Gem::Courtier::Consumers::GMqttOptions;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerEnd;
using Gem::Courtier::Consumers::GPeerHandlers;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Tests::GTcpRelay;
using Gem::Courtier::Tests::JoinableIo;
using Gem::Courtier::Tests::makeRole;
using Gem::Courtier::Tests::mqttConnect;
using Gem::Courtier::Tests::MqttServer;
using Gem::Courtier::Tests::mqttTestOptions;
using Gem::Courtier::Tests::Role;
using namespace std::chrono_literals;

namespace {

/** @brief Waits, at most ten seconds, for @p condition @return Whether it held in time */
template <typename condition_type>
bool eventually(condition_type condition) {
    return Gem::Courtier::Tests::eventually(condition, 10s);
}

/** @brief The end of a connection as its role was told, with the words that came with it */
struct Ended {
    GPeerEnd end = GPeerEnd::CLOSED_HERE;
    GPeerClose how = GPeerClose::NORMAL;
    std::string detail;
};

/** @brief Handlers that record the end, with the words that came with it
 *  @return The handlers and the future of the end */
std::pair<GPeerHandlers, std::future<Ended>> endWatch() {
    auto promise = std::make_shared<std::promise<Ended>>();
    auto future = promise->get_future();
    return {
        GPeerHandlers{
            .onOpen = [] {},
            .onMessage = [](std::string &&) {},
            .onClosed =
                [promise](GPeerEnd end, GPeerClose how, std::string_view detail) {
                    promise->set_value(
                        Ended{.end = end, .how = how, .detail = std::string(detail)}
                    );
                }
        },
        std::move(future)
    };
}

/** @brief @return The resident memory of this process, in KiB (Linux: VmRSS of /proc/self/status) */
std::size_t residentKiB() {
    std::ifstream status("/proc/self/status");
    std::string key;
    std::size_t value = 0;
    while(status >> key) {
        if(key == "VmRSS:") {
            status >> value;
            return value;
        }
    }
    return 0;
}

/** @brief A port that refuses connections for as long as this object lives: it is bound and does
 *  not listen */
class ClosedPort {
public:
    ClosedPort()
      : socket_(ctx_) {
        socket_.open(boost::asio::ip::tcp::v4());
        socket_.bind(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    }
    /** @brief @return The port */
    [[nodiscard]] unsigned short port() const {
        return socket_.local_endpoint().port();
    }

private:
    boost::asio::io_context ctx_;
    boost::asio::ip::tcp::socket socket_;
};

} /* anonymous namespace */

/******************************************************************************/
/**
 * A client joins: the listener hands out the server's end once it has subscribed, so that what the
 * client sends as soon as its end opens arrives -- all of it, in order, and the close after it with
 * what it says. Both ends can carry messages of up to the broker's packet size less the transport's
 * own bytes.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE(
    "MQTT transport: a client joins and its connection carries messages in order",
    "[courtier][mqtt]"
) {
    constexpr int N_MESSAGES = 200;
    const GMqttOptions options = mqttTestOptions();
    MqttServer server(options);
    JoinableIo client_io(1);
    GMqttConnector connector(options);

    auto [client_end, error] =
        mqttConnect(connector, boost::asio::make_strand(client_io.executor()));
    REQUIRE(client_end);
    REQUIRE(error.empty());
    REQUIRE(eventually([&] { return server.nPeers() == 1; }));
    auto server_end = server.peer(0);
    CHECK(client_end->maxMessageBytes() > 1'000'000);
    CHECK(client_end->maxMessageBytes() < 2'000'000);
    CHECK(server_end->maxMessageBytes() > 1'000'000);

    auto client_role = makeRole();
    auto server_role = makeRole();
    server_role->whenOpen = [](Role &role) { role.send("hello"); };
    client_role->whenOpen = [](Role &role) {
        for(int i = 0; i < N_MESSAGES; ++i) {
            role.send(std::to_string(i));
        }
        role.close(GPeerClose::RUN_ENDED);
    };
    server_role->start(server_end);
    client_role->start(client_end);

    REQUIRE(eventually([&] { return client_role->nClosed() == 1 && server_role->nClosed() == 1; }));
    std::vector<std::string> expected{"open"};
    for(int i = 0; i < N_MESSAGES; ++i) {
        expected.push_back("msg:" + std::to_string(i));
    }
    expected.emplace_back("closed:CLOSED_THERE:RUN_ENDED");
    CHECK(server_role->events() == expected);
    CHECK(client_role->events().back() == "closed:CLOSED_HERE:NORMAL");
    CHECK(server.nPeers() == 1);
}

/******************************************************************************/
/**
 * What a connection costs in memory: Boost.MQTT5 keeps a read buffer of the size of a client's own
 * packet limit and fills it when the client connects. With MQTT's maximum as that limit every
 * MQTT client took 256 MiB; with the transport's limit (2,000,000 bytes) four connections -- eight
 * MQTT clients in this process -- take a few MiB.
 */
TEST_CASE("MQTT transport: a connection costs its ends a few megabytes", "[courtier][mqtt]") {
    constexpr std::size_t N_CONNECTIONS = 4;
    const GMqttOptions options = mqttTestOptions();
    MqttServer server(options);
    JoinableIo client_io(1);
    GMqttConnector connector(options);

    const std::size_t before = residentKiB();
    std::vector<std::shared_ptr<Role>> roles;
    for(std::size_t i = 0; i < N_CONNECTIONS; ++i) {
        auto [client_end, error] =
            mqttConnect(connector, boost::asio::make_strand(client_io.executor()));
        REQUIRE(client_end);
        REQUIRE(eventually([&] { return server.nPeers() == i + 1; }));
        roles.push_back(makeRole());
        roles.back()->start(client_end);
        roles.push_back(makeRole());
        roles.back()->start(server.peer(i));
    }
    const std::size_t after = residentKiB();
    CHECK(after < before + 64 * 1024);

    for(const auto &role : roles) {
        role->close(GPeerClose::NORMAL);
    }
    CHECK(eventually([&] {
        return std::ranges::all_of(roles, [](const auto &role) { return role->nClosed() == 1; });
    }));
}

/******************************************************************************/
/** No broker: the listener refuses to start and names where it looked; the connector reports no
 *  end, with the broker in the reason. */
TEST_CASE(
    "MQTT transport: without a broker the listener refuses to start and the connector fails",
    "[courtier][mqtt]"
) {
    const ClosedPort closed;
    GMqttOptions options = mqttTestOptions();
    options.port = closed.port();
    const std::string where = "127.0.0.1:" + std::to_string(closed.port());

    JoinableIo handlers(1);
    GMqttListener listener("MQTT test listener", options);
    std::string refusal;
    try {
        listener.start(
            handlers.executor(),
            [](const std::shared_ptr<GPeerI> &, Gem::Courtier::GWirePeerId) {}
        );
    }
    catch(const Gem::Common::gem_exception &e) {
        refusal = e.what();
    }
    CHECK(refusal.find(where) != std::string::npos);

    JoinableIo client_io(1);
    GMqttConnector connector(options);
    auto [client_end, error] =
        mqttConnect(connector, boost::asio::make_strand(client_io.executor()));
    CHECK_FALSE(client_end);
    CHECK(error.find(where) != std::string::npos);
}

/******************************************************************************/
/**
 * The broker connection of a client's end is cut (a relay stands between it and the broker): the
 * end fails, the server's end hears its will and ends SILENT, and the client's MQTT client does
 * not connect again -- Boost.MQTT5 would, about a second after the cut. Nothing reaches either
 * role after its end.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE(
    "MQTT transport: an end whose broker connection is cut ends and does not connect again",
    "[courtier][mqtt]"
) {
    const GMqttOptions options = mqttTestOptions();
    MqttServer server(options);
    GTcpRelay relay(options.port);
    GMqttOptions through_relay = options;
    through_relay.port = relay.port();
    JoinableIo client_io(1);
    GMqttConnector connector(through_relay);

    auto [client_end, error] =
        mqttConnect(connector, boost::asio::make_strand(client_io.executor()));
    REQUIRE(client_end);
    REQUIRE(eventually([&] { return server.nPeers() == 1; }));
    auto server_role = makeRole();
    auto client_role = makeRole();
    server_role->whenOpen = [](Role &role) { role.send("hello"); };
    server_role->start(server.peer(0));
    client_role->start(client_end);
    REQUIRE(eventually([&] { return client_role->events().size() == 2; }));
    CHECK(relay.nConnections() == 1);

    relay.cutAll();
    REQUIRE(eventually([&] { return client_role->nClosed() == 1 && server_role->nClosed() == 1; }));
    CHECK(
        client_role->events() ==
        std::vector<std::string>{"open", "msg:hello", "closed:FAILED:NORMAL"}
    );
    CHECK(server_role->events() == std::vector<std::string>{"open", "closed:SILENT:NORMAL"});

    // Boost.MQTT5 reconnects 0.5 to 1.5 s after a loss when it is let
    std::this_thread::sleep_for(3s);
    CHECK(relay.nConnections() == 1);
    CHECK(client_role->events().size() == 3);
    CHECK(server_role->events().size() == 2);
}

/******************************************************************************/
/**
 * The largest message: one of maxMessageBytes() arrives whole (the receiving end has raised its
 * own packet limit); one byte more is not sent, and the end fails with both sizes in what it says,
 * while the other end hears its will.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE(
    "MQTT transport: a message longer than a packet the broker accepts ends the end",
    "[courtier][mqtt]"
) {
    const GMqttOptions options = mqttTestOptions();
    MqttServer server(options);
    JoinableIo client_io(1);
    GMqttConnector connector(options);
    auto [client_end, error] =
        mqttConnect(connector, boost::asio::make_strand(client_io.executor()));
    REQUIRE(client_end);
    REQUIRE(eventually([&] { return server.nPeers() == 1; }));
    const std::size_t limit = client_end->maxMessageBytes();

    auto server_role = makeRole();
    server_role->start(server.peer(0));
    auto [client_handlers, client_ended] = endWatch();
    client_end->start(std::move(client_handlers));

    // The longest message first; one byte more once it has arrived (a failing end cuts what it has
    // not written yet)
    boost::asio::post(client_end->executor(), [peer = client_end, limit] {
        peer->send(Gem::Courtier::Consumers::makeMessage(std::string(limit, 'x')));
    });
    REQUIRE(eventually([&] { return server_role->nMessages() == 1; }));
    boost::asio::post(client_end->executor(), [peer = client_end, limit] {
        peer->send(Gem::Courtier::Consumers::makeMessage(std::string(limit + 1, 'y')));
    });
    REQUIRE(client_ended.wait_for(10s) == std::future_status::ready);
    const Ended client = client_ended.get();
    CHECK(Gem::Common::enumName(client.end) == "FAILED");
    CHECK(client.detail.find(std::to_string(limit + 1) + " bytes") != std::string::npos);
    CHECK(client.detail.find(std::to_string(limit) + " bytes") != std::string::npos);

    REQUIRE(eventually([&] { return server_role->nClosed() == 1; }));
    CHECK(
        server_role->events() ==
        std::vector<std::string>{"open", "msg:" + std::string(limit, 'x'), "closed:SILENT:NORMAL"}
    );
}

/******************************************************************************/
/**
 * Two prefixes on one broker do not see each other: a client of another prefix is not taken on by
 * the listener, and its end, which hears no server, ends SILENT within the handshake limit (twice
 * the keep-alive); a client of the listener's prefix gets through.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE("MQTT transport: a client of another topic prefix is not taken on", "[courtier][mqtt]") {
    const GMqttOptions options = mqttTestOptions(1s);
    MqttServer server(options);
    GMqttOptions other = mqttTestOptions(1s);
    JoinableIo client_io(1);

    GMqttConnector stranger(other);
    auto [stranger_end, stranger_error] =
        mqttConnect(stranger, boost::asio::make_strand(client_io.executor()));
    REQUIRE(stranger_end);
    auto stranger_role = makeRole();
    stranger_role->start(stranger_end);
    REQUIRE(eventually([&] { return stranger_role->nClosed() == 1; }));
    CHECK(stranger_role->events() == std::vector<std::string>{"open", "closed:SILENT:NORMAL"});
    CHECK(server.nPeers() == 0);

    GMqttConnector member(options);
    auto [member_end, member_error] =
        mqttConnect(member, boost::asio::make_strand(client_io.executor()));
    REQUIRE(member_end);
    REQUIRE(eventually([&] { return server.nPeers() == 1; }));
    auto member_role = makeRole();
    member_role->start(member_end);
    member_role->close(GPeerClose::NORMAL);
    CHECK(eventually([&] { return member_role->nClosed() == 1; }));
}
