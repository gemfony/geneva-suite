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

// The end of a run on the websocket transport: the server tells every client that the run has
// ended, and waits for them, bounded (GPeerConsumerT::stopServer(), GPeerI.hpp rule 8). Clients that
// evaluate, clients that wait for work, clients that confirm at once, and a connection that never
// reads.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

#include "GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GNetworkedTimeoutConfig.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp"

using boost::asio::ip::tcp;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GWebsocketConsumerConfig;
using Gem::Courtier::GWebsocketConsumerT;
using Gem::Courtier::Consumers::GWebsocketClientT;
using namespace std::chrono_literals;

namespace {

constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;
constexpr auto PATIENCE = 10s; // how long a test waits for something that is due soon

using consumer_type = GWebsocketConsumerT<GFaultyContainer>;
using client_type = GWebsocketClientT<GFaultyContainer>;

/** @brief Polls @p condition until it holds or PATIENCE has passed @return Whether it held */
template <typename condition_type>
bool eventually(condition_type condition) {
    const auto give_up_at = std::chrono::steady_clock::now() + PATIENCE;
    while(not condition() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

/** @brief @param n_threads I/O and session threads @param ping The ping interval in seconds
 *  @return The configuration of a server on a free port */
GWebsocketConsumerConfig serverConfig(std::size_t n_threads, std::size_t ping = 1) {
    return GWebsocketConsumerConfig{
        .ip = "127.0.0.1",
        .port = 0,
        .serializationMode = BIN,
        .nThreads = n_threads,
        .pingInterval = ping,
    };
}

/** @brief A started server whose batches wait for ever (nothing is given up on time)
 *  @param config Its configuration @param stop_grace Its stop grace @return The server */
std::shared_ptr<consumer_type>
startedServer(const GWebsocketConsumerConfig &config, std::chrono::milliseconds stop_grace) {
    auto consumer = std::make_shared<consumer_type>(config, stop_grace);
    Gem::Courtier::GNetworkedTimeoutConfig timeouts;
    timeouts.treatment = "wait_indefinitely";
    consumer->applyTimeoutConfig(timeouts);
    consumer->startServer();
    return consumer;
}

/** @brief Clients that run in threads of their own, against one server */
class Clients {
public:
    /** @brief Starts @p n clients of the server on @p port; each evaluation takes @p duration
     *  @param port The server's port @param n How many @param duration An evaluation */
    Clients(unsigned short port, std::size_t n, std::chrono::milliseconds duration) {
        GWebsocketConsumerConfig config = serverConfig(1);
        config.port = port;
        for(std::size_t c = 0; c < n; ++c) {
            auto client = std::make_shared<client_type>(config);
            client->setItemProcessor([this, duration](GFaultyContainer &item) {
                n_evaluating_.fetch_add(1);
                std::this_thread::sleep_for(duration);
                item.markAsProcessedWith({true});
            });
            clients_.push_back(client);
            auto done = std::make_shared<std::atomic<bool>>(false);
            done_.push_back(done);
            threads_.emplace_back([client, done] {
                client->run();
                done->store(true);
            });
        }
    }
    /** @brief Joins the clients' threads */
    ~Clients() {
        threads_.clear();
    }
    Clients(const Clients &) = delete;
    Clients(Clients &&) = delete;
    Clients &operator=(const Clients &) = delete;
    Clients &operator=(Clients &&) = delete;

    /** @brief @return Whether every client's run() has returned */
    [[nodiscard]] bool done() const {
        return std::ranges::all_of(done_, [](const auto &d) { return d->load(); });
    }
    /** @brief @return How many clients were told that the run has ended; after done() */
    [[nodiscard]] std::size_t nToldTheEnd() const {
        std::size_t n = 0;
        for(const auto &client : clients_) {
            n += client->closeRequested() ? 1 : 0;
        }
        return n;
    }
    /** @brief @return How many evaluations have begun */
    [[nodiscard]] std::size_t nEvaluating() const {
        return n_evaluating_.load();
    }

private:
    std::vector<std::shared_ptr<client_type>> clients_;
    std::vector<std::shared_ptr<std::atomic<bool>>> done_;
    std::atomic<std::size_t> n_evaluating_{0};
    std::vector<std::jthread> threads_; ///< Joined first
};

/** @brief Submits @p items to @p consumer; the batch may end with the run (its slots are then
 *  filled with clones of a template) @return The batch, running */
std::future<void>
submit(consumer_type &consumer, std::vector<std::unique_ptr<GFaultyContainer>> &items) {
    return std::async(std::launch::async, [&consumer, &items] {
        auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
        policy.fatal_on_none_returned = false;
        auto clone_template = std::make_unique<GFaultyContainer>(99);
        clone_template->markAsProcessedWith({true});
        consumer
            .processBatch(std::span(items.data(), items.size()), policy, std::move(clone_template));
    });
}

/** @brief @param f What is timed @return How long it took */
template <typename F>
std::chrono::milliseconds timed(F f) {
    const auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start
    );
}

} /* anonymous namespace */

/******************************************************************************/
/** T7.1: a client that is evaluating when the server stops is told that the run has ended. */
TEST_CASE(
    "courtier(websocket): a client that evaluates at the stop is told the run has ended",
    "[courtier][websocket][stop]"
) {
    auto consumer = startedServer(serverConfig(2), 1s);
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(0));
    {
        // The evaluation outlasts the grace: only the announcement reaches the client in time.
        Clients clients(consumer->getPort(), 1, 3000ms);
        auto batch = submit(*consumer, items);
        REQUIRE(eventually([&] { return clients.nEvaluating() == 1; }));
        consumer->stopServer();
        batch.get();
        REQUIRE(eventually([&] { return clients.done(); }));
        CHECK(clients.nToldTheEnd() == 1);
    }
}

/******************************************************************************/
/** T7.2: forty clients that wait for work, a server with one I/O thread: every client is told. */
TEST_CASE(
    "courtier(websocket): forty waiting clients are all told the run has ended",
    "[courtier][websocket][stop]"
) {
    constexpr std::size_t N = 40;
    auto consumer = startedServer(serverConfig(1), 5s);
    {
        Clients clients(consumer->getPort(), N, 10ms);
        REQUIRE(eventually([&] { return consumer->getNActiveSessions() == N; }));
        std::this_thread::sleep_for(200ms); // every client has asked and waits
        consumer->stopServer();
        REQUIRE(eventually([&] { return clients.done(); }));
        CHECK(clients.nToldTheEnd() == N);
    }
    CHECK(consumer->getNActiveSessions() == 0);
}

/******************************************************************************/
/** T7.3: forty clients that confirm at once: the stop takes far less than the grace. */
TEST_CASE(
    "courtier(websocket): a stop whose clients confirm takes far less than the grace",
    "[courtier][websocket][stop]"
) {
    constexpr std::size_t N = 40;
    constexpr auto GRACE = 10s;
    auto consumer = startedServer(serverConfig(2), GRACE);
    {
        Clients clients(consumer->getPort(), N, 10ms);
        REQUIRE(eventually([&] { return consumer->getNActiveSessions() == N; }));
        const auto took = timed([&] { consumer->stopServer(); });
        CHECK(took < 3s);
        REQUIRE(eventually([&] { return clients.done(); }));
    }
}

/******************************************************************************/
/** T7.4: a connection that never reads after its handshake costs the stop one grace, not more. */
TEST_CASE(
    "courtier(websocket): a client that never reads costs the stop one grace",
    "[courtier][websocket][stop]"
) {
    constexpr auto GRACE = 1500ms;
    auto consumer = startedServer(serverConfig(1, 0), GRACE);

    boost::asio::io_context ctx;
    boost::beast::websocket::stream<tcp::socket> silent(ctx);
    silent.next_layer().connect({boost::asio::ip::address_v4::loopback(), consumer->getPort()});
    silent.handshake("localhost", "/"); // and from here on it reads nothing
    REQUIRE(eventually([&] { return consumer->getNActiveSessions() == 1; }));

    const auto took = timed([&] { consumer->stopServer(); });
    CHECK(took >= GRACE);
    CHECK(took < GRACE + 1500ms);
}

/******************************************************************************/
namespace {

/**
 * A GWebsocketListener whose calls of on_peer are held until 200 ms after its stop() has begun:
 * a client whose session is on its way when the server stops.
 */
class HeldOnPeerListener final : public Gem::Courtier::Consumers::GListenerI {
public:
    /** @brief @param listener The websocket listener */
    explicit HeldOnPeerListener(std::unique_ptr<Gem::Courtier::Consumers::GWebsocketListener> listener)
      : listener_(std::move(listener)) { /* nothing */
    }
    /** @brief Holds every call of on_peer @param executor See GListenerI @param on_peer See GListenerI */
    void start(boost::asio::any_io_executor executor, on_peer_type on_peer) override {
        listener_->start(
            std::move(executor),
            [stopping = stopping_, entered = entered_, on_peer = std::move(on_peer)](
                std::shared_ptr<Gem::Courtier::Consumers::GPeerI> peer,
                Gem::Courtier::GWirePeerId id
            ) mutable {
                entered->store(true);
                const auto give_up_at = std::chrono::steady_clock::now() + PATIENCE;
                while(not stopping->load() && std::chrono::steady_clock::now() < give_up_at) {
                    std::this_thread::sleep_for(1ms);
                }
                std::this_thread::sleep_for(200ms);
                on_peer(std::move(peer), id);
            }
        );
    }
    /** @brief Lets the held calls go on soon, and stops the listener */
    void stop() override {
        stopping_->store(true);
        listener_->stop();
    }
    /** @brief Forwards */
    void endWork() override {
        listener_->endWork();
    }
    /** @brief Forwards @return The websocket listener's grace */
    [[nodiscard]] std::chrono::milliseconds stopGrace() const override {
        return listener_->stopGrace();
    }
    /** @brief Forwards @return The websocket listener's answer */
    [[nodiscard]] bool detectsLoss() const override {
        return listener_->detectsLoss();
    }
    /** @brief @return The port the websocket listener has bound */
    [[nodiscard]] unsigned short getPort() const {
        return listener_->getPort();
    }
    /** @brief @return Whether a call of on_peer has been held */
    [[nodiscard]] bool entered() const {
        return entered_->load();
    }

private:
    std::unique_ptr<Gem::Courtier::Consumers::GWebsocketListener> listener_;
    std::shared_ptr<std::atomic<bool>> stopping_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> entered_ = std::make_shared<std::atomic<bool>>(false);
};

} /* anonymous namespace */

/**
 * A client whose session is on its way when the server stops (the call of on_peer is held on the
 * listener's accept strand, its one I/O thread): stop() of the websocket listener waits for that
 * call, so the shell finds the session and the client is told that the run has ended
 * (GListenerI::stop()).
 */
TEST_CASE(
    "courtier(websocket): a client whose session is on its way at the stop is told the run has ended",
    "[courtier][websocket][stop]"
) {
    constexpr auto GRACE = 5s;
    auto held = std::make_unique<HeldOnPeerListener>(
        std::make_unique<Gem::Courtier::Consumers::GWebsocketListener>(
            "held listener",
            0,
            Gem::Courtier::Consumers::GWebsocketPeer::GOptions{
                .name = "held listener",
                .mode = BIN,
                .pingInterval = 1s
            },
            1,
            GRACE
        )
    );
    auto &listener = *held;
    Gem::Courtier::GPeerConsumerT<GFaultyContainer> shell{"Shell", std::move(held), 2, BIN};
    shell.startServer();
    {
        Clients clients(listener.getPort(), 1, 10ms);
        REQUIRE(eventually([&] { return listener.entered(); }));
        const auto took = timed([&] { shell.stopServer(); });
        CHECK(took < GRACE);
        REQUIRE(eventually([&] { return clients.done(); }));
        CHECK(clients.nToldTheEnd() == 1);
    }
}
