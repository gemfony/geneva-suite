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

// The TCP relay of the tests (GTcpRelay.hpp): it forwards, cuts after an exact number of bytes,
// freezes, refuses, closes at once without an upstream, and moves only new connections when its
// upstream changes.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include "GTcpRelay.hpp"

using boost::asio::ip::tcp;
using Gem::Courtier::Tests::GTcpRelay;
using Gem::Courtier::Tests::relayDirection;
using namespace std::chrono_literals;

namespace {

/******************************************************************************/
/** @brief A server that sends back what it receives, on a thread of its own */
class EchoServer {
public:
    EchoServer() {
        acceptor_.open(tcp::v4());
        acceptor_.bind(tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
        acceptor_.listen();
        this->accept_();
        thread_ = std::jthread([this] { ctx_.run(); });
    }
    ~EchoServer() {
        try {
            boost::asio::post(ctx_, [this] {
                boost::system::error_code ignored;
                std::ignore = acceptor_.close(ignored);
                for(const auto &socket : sockets_) {
                    std::ignore = socket->close(ignored);
                }
            });
        }
        catch(...) { // NOLINT(bugprone-empty-catch) -- a test's server; its thread is joined below
        }
        guard_.reset();
        thread_.join();
    }
    EchoServer(const EchoServer &) = delete;
    EchoServer(EchoServer &&) = delete;
    EchoServer &operator=(const EchoServer &) = delete;
    EchoServer &operator=(EchoServer &&) = delete;

    /** @brief @return Its port */
    [[nodiscard]] unsigned short port() const {
        return acceptor_.local_endpoint().port();
    }
    /** @brief @return How many connections it accepted */
    [[nodiscard]] std::size_t nAccepted() const {
        return n_accepted_.load();
    }

private:
    void accept_() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if(ec) {
                return;
            }
            n_accepted_.fetch_add(1);
            auto shared = std::make_shared<tcp::socket>(std::move(socket));
            sockets_.push_back(shared);
            echo_(shared, std::make_shared<std::array<char, 4096>>());
            this->accept_();
        });
    }
    static void echo_(
        const std::shared_ptr<tcp::socket> &socket,
        const std::shared_ptr<std::array<char, 4096>> &buffer
    ) {
        socket->async_read_some(
            boost::asio::buffer(*buffer),
            [socket, buffer](boost::system::error_code ec, std::size_t n) {
                if(ec) {
                    return;
                }
                boost::asio::async_write(
                    *socket,
                    boost::asio::buffer(buffer->data(), n),
                    [socket, buffer](boost::system::error_code wec, std::size_t) {
                        if(not wec) {
                            echo_(socket, buffer);
                        }
                    }
                );
            }
        );
    }

    boost::asio::io_context ctx_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_ =
        boost::asio::make_work_guard(ctx_);
    tcp::acceptor acceptor_{ctx_};
    std::vector<std::shared_ptr<tcp::socket>> sockets_; ///< On its thread
    std::atomic<std::size_t> n_accepted_{0};
    std::jthread thread_;
};

/******************************************************************************/
/** @brief A client connection with operations that wait at most a given time */
class Client {
public:
    /** @brief @param port Where it connects @param ok Set to whether the connect succeeded */
    Client(unsigned short port, bool &ok) {
        boost::system::error_code ec;
        std::ignore =
            socket_.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port), ec);
        ok = not ec;
    }
    /** @brief @param bytes What is sent */
    void send(const std::string &bytes) {
        boost::system::error_code ec;
        boost::asio::write(socket_, boost::asio::buffer(bytes), ec);
    }
    /** @brief Reads up to @p n bytes, at most for @p patience
     *  @return What arrived; error() says why it stopped short */
    std::string read(std::size_t n, std::chrono::milliseconds patience) {
        std::string data(n, '\0');
        std::size_t got = 0;
        error_ = {};
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(data),
            [this, &got](boost::system::error_code ec, std::size_t k) {
                got = k;
                error_ = ec;
            }
        );
        ctx_.restart();
        ctx_.run_for(patience);
        if(not ctx_.stopped()) {
            socket_.cancel();
            ctx_.restart();
            ctx_.run();
        }
        data.resize(got);
        return data;
    }
    /** @brief @return Why the last read stopped (empty if it got everything) */
    [[nodiscard]] boost::system::error_code error() const {
        return error_;
    }

private:
    boost::asio::io_context ctx_;
    tcp::socket socket_{ctx_};
    boost::system::error_code error_;
};

/** @brief @param n How many @return n bytes, not all the same */
std::string pattern(std::size_t n) {
    std::string bytes(n, '\0');
    for(std::size_t i = 0; i < n; ++i) {
        bytes[i] = static_cast<char>('a' + (i % 26));
    }
    return bytes;
}

/** @brief Waits at most 5 s for @p condition @return Whether it held */
template <typename condition_type>
bool eventually(condition_type condition) {
    const auto give_up_at = std::chrono::steady_clock::now() + 5s;
    while(std::chrono::steady_clock::now() < give_up_at) {
        if(condition()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

} /* anonymous namespace */

/******************************************************************************/
TEST_CASE("GTcpRelay forwards a burst both ways", "[courtier][relay]") {
    const EchoServer server;
    GTcpRelay relay(server.port());
    bool ok = false;
    Client client(relay.port(), ok);
    REQUIRE(ok);
    const std::string burst = pattern(200000);
    client.send(burst);
    CHECK(client.read(burst.size(), 5000ms) == burst);
    CHECK(relay.bytes(relayDirection::TO_SERVER) == burst.size());
    CHECK(eventually([&] { return relay.bytes(relayDirection::TO_CLIENT) == burst.size(); }));
    CHECK(relay.nConnections() == 1);
    CHECK(relay.nOpen() == 1);
}

/******************************************************************************/
TEST_CASE("GTcpRelay cuts after exactly n more bytes and resets both ends", "[courtier][relay]") {
    const EchoServer server;
    GTcpRelay relay(server.port());
    bool ok = false;
    Client client(relay.port(), ok);
    REQUIRE(ok);
    const std::string first = pattern(500);
    client.send(first);
    REQUIRE(client.read(first.size(), 5000ms) == first);

    relay.cutAfter(relayDirection::TO_SERVER, 1000); // counted from here
    client.send(pattern(5000));
    // The relay cuts as soon as the 1000 bytes are written: their echo does not come back.
    CHECK(client.read(5000, 5000ms).size() < 5000);
    CHECK(client.error()); // the connection was reset
    CHECK(relay.bytes(relayDirection::TO_SERVER) == 1500);
    CHECK(eventually([&] { return relay.nOpen() == 0; }));
}

/******************************************************************************/
TEST_CASE(
    "GTcpRelay freezes a connection: it stays open and forwards nothing",
    "[courtier][relay]"
) {
    const EchoServer server;
    GTcpRelay relay(server.port());
    bool ok = false;
    Client client(relay.port(), ok);
    REQUIRE(ok);
    REQUIRE(eventually([&] { return relay.nOpen() == 1; }));
    relay.freeze();
    client.send("frozen");
    CHECK(client.read(6, 300ms).empty());
    CHECK(relay.nOpen() == 1);
    CHECK(relay.bytes(relayDirection::TO_SERVER) == 0);

    relay.thaw();
    CHECK(client.read(6, 5000ms) == "frozen");
}

/******************************************************************************/
TEST_CASE("GTcpRelay refuses while it does not accept and accepts again", "[courtier][relay]") {
    const EchoServer server;
    GTcpRelay relay(server.port());
    relay.setAccepting(false);
    bool ok = true;
    const Client refused(relay.port(), ok);
    CHECK_FALSE(ok);

    relay.setAccepting(true);
    Client accepted(relay.port(), ok);
    REQUIRE(ok);
    accepted.send("again");
    CHECK(accepted.read(5, 5000ms) == "again");
}

/******************************************************************************/
TEST_CASE("GTcpRelay without an upstream accepts and closes at once", "[courtier][relay]") {
    GTcpRelay relay(0);
    bool ok = false;
    Client client(relay.port(), ok);
    REQUIRE(ok);
    CHECK(client.read(1, 5000ms).empty());
    CHECK(client.error()); // closed, not timed out
    CHECK(client.error() != boost::asio::error::operation_aborted);
    CHECK(relay.nConnections() == 1);
    CHECK(relay.nOpen() == 0);
}

/******************************************************************************/
TEST_CASE("GTcpRelay moves only new connections to a new upstream", "[courtier][relay]") {
    const EchoServer first_server;
    const EchoServer second_server;
    GTcpRelay relay(first_server.port());
    bool ok = false;
    Client old_client(relay.port(), ok);
    REQUIRE(ok);
    old_client.send("one");
    REQUIRE(old_client.read(3, 5000ms) == "one");

    relay.setUpstream(second_server.port());
    Client new_client(relay.port(), ok);
    REQUIRE(ok);
    new_client.send("two");
    CHECK(new_client.read(3, 5000ms) == "two");
    old_client.send("old");
    CHECK(old_client.read(3, 5000ms) == "old");
    CHECK(first_server.nAccepted() == 1);
    CHECK(second_server.nAccepted() == 1);
}
