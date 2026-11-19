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

// The server's session over any peer (GPeerSessionT), on the in-process transport: the session
// against a stand-in for the consumer (the hooks), with a test role at the client's end.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include "GDemoProcessingContainers.hpp"
#include "GInProcessPeer.hpp"
#include "GInProcessTestSupport.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GServerSessionLogic.hpp"
#include "courtier/GWireCodec.hpp"
#include "courtier/GWireSerializationContext.hpp"
#include "courtier/transport/GPeerI.hpp"
#include "courtier/transport/GPeerSessionT.hpp"

using Gem::Courtier::GCheckoutResult;
using Gem::Courtier::GCommandContainerT;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GFrameKind;
using Gem::Courtier::GWakeFn;
using Gem::Courtier::GWireBlobId;
using Gem::Courtier::GWireBlobRegistry;
using Gem::Courtier::GWirePeerId;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerEnd;
using Gem::Courtier::Consumers::GPeerHandlers;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Tests::closeName;
using Gem::Courtier::Tests::endName;
using Gem::Courtier::Tests::GInProcessPeer;
using Gem::Courtier::Tests::IoThreads;
using namespace std::chrono_literals;

namespace {

constexpr auto PATIENCE = 5s; // how long a test waits for something that is due at once
constexpr auto QUIET = 300ms; // how long a test watches for something that must not come
constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

using session_type = Gem::Courtier::Consumers::GPeerSessionT<GFaultyContainer>;

/** @brief Waits, at most PATIENCE, for @p condition @return Whether it held in time */
template <typename condition_type>
bool eventually(condition_type condition) {
    return Gem::Courtier::Tests::eventually(condition, PATIENCE);
}

/******************************************************************************/
/**
 * A stand-in for the consumer: a list of item numbers that are pending, the wake functions of the
 * parked sessions, and the hooks of each session. Like GNetworkedConsumerT it wakes as many parked
 * callers as items became available, skipping one that is gone, and a session's lease puts back
 * what the session held when the session goes.
 */
class FakeConsumer {
public:
    /** @brief Makes items available and wakes the parked sessions for them
     *  @param numbers The numbers of the items */
    void offer(const std::vector<std::size_t> &numbers) {
        {
            const std::scoped_lock lock(mutex_);
            for(auto number : numbers) {
                pending_.push_back(number);
            }
        }
        this->wake_(numbers.size());
    }

    /** @brief The next checkout finds the consumer stopped, as one does that runs at the moment
     *  requestStop() sets the flag: after the session has looked at the stop */
    void stopAtNextCheckout() {
        const std::scoped_lock lock(mutex_);
        stop_at_checkout_ = true;
    }

    /** @brief Stops the consumer and wakes every parked session, as requestStop() does */
    void stop() {
        std::size_t n_parked = 0;
        {
            const std::scoped_lock lock(mutex_);
            stopped_ = true;
            n_parked = parked_.size();
        }
        this->wake_(n_parked);
    }

    /** @brief @return The hooks of one session */
    session_type::SessionHooks hooks() {
        auto lease = std::make_shared<Lease>(*this);
        return session_type::SessionHooks{
            .getPayloadItem =
                [this, lease](GWakeFn wake) {
                    GCheckoutResult<GFaultyContainer> result;
                    const std::scoped_lock lock(mutex_);
                    if(stop_at_checkout_) {
                        stopped_ = true;
                    }
                    if(stopped_) {
                        return result;
                    }
                    if(pending_.empty()) {
                        parked_.push_back(std::move(wake));
                        result.parked = true;
                        return result;
                    }
                    const std::size_t number = pending_.front();
                    pending_.pop_front();
                    lease->held.insert(number);
                    result.item = std::make_unique<GFaultyContainer>(number);
                    result.item->setCorrelationId(number);
                    return result;
                },
            .putPayloadItem =
                [this, lease](GCommandContainerT<GFaultyContainer> frame) {
                    const std::scoped_lock lock(mutex_);
                    lease->held.erase(frame.outcome().correlation_id);
                    ++n_checked_in_;
                },
            .failPayloadItem =
                [this, lease](Gem::Courtier::CORRELATION_ID_TYPE id, const std::string &) {
                    const std::scoped_lock lock(mutex_);
                    lease->held.erase(id);
                    ++n_failed_;
                },
            .checkServerStopped =
                [this] {
                    const std::scoped_lock lock(mutex_);
                    return stopped_;
                },
            .serverSignOn = [this](bool on) { n_sessions_.fetch_add(on ? 1 : -1); },
        };
    }

    /** @brief @return How many sessions are signed on */
    [[nodiscard]] int nSessions() const {
        return n_sessions_.load();
    }
    /** @brief @return How many sessions are parked */
    [[nodiscard]] std::size_t nParked() const {
        const std::scoped_lock lock(mutex_);
        return parked_.size();
    }
    /** @brief @return How many results have been checked in */
    [[nodiscard]] std::size_t nCheckedIn() const {
        const std::scoped_lock lock(mutex_);
        return n_checked_in_;
    }
    /** @brief @return How many items have ended as failed */
    [[nodiscard]] std::size_t nFailed() const {
        const std::scoped_lock lock(mutex_);
        return n_failed_;
    }
    /** @brief @return How many items are pending */
    [[nodiscard]] std::size_t nPending() const {
        const std::scoped_lock lock(mutex_);
        return pending_.size();
    }

private:
    /** @brief What one session holds; put back when the session's hooks go */
    struct Lease {
        explicit Lease(FakeConsumer &consumer)
          : consumer_(consumer) { /* nothing */
        }
        ~Lease() {
            this->giveBack();
        }
        Lease(const Lease &) = delete;
        Lease(Lease &&) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease &operator=(Lease &&) = delete;

        /** @brief Puts back every item held */
        void giveBack() {
            std::vector<std::size_t> numbers;
            {
                const std::scoped_lock lock(consumer_.mutex_);
                numbers.assign(held.begin(), held.end());
                held.clear();
            }
            if(not numbers.empty()) {
                consumer_.offer(numbers);
            }
        }

        std::set<std::size_t> held; ///< Under the consumer's mutex

    private:
        FakeConsumer &consumer_; ///< Outlives every session
    };

    /** @brief Wakes up to @p n parked sessions, skipping those that are gone @param n How many */
    void wake_(std::size_t n) {
        while(n > 0) {
            GWakeFn wake;
            {
                const std::scoped_lock lock(mutex_);
                if(parked_.empty()) {
                    return;
                }
                wake = std::move(parked_.front());
                parked_.pop_front();
            }
            if(wake()) {
                --n;
            }
        }
    }

    mutable std::mutex mutex_;
    std::deque<std::size_t> pending_; ///< Items that can be checked out
    std::deque<GWakeFn> parked_;      ///< Wake functions of parked sessions, oldest first
    bool stopped_ = false;
    bool stop_at_checkout_ = false;
    std::size_t n_checked_in_ = 0;
    std::size_t n_failed_ = 0;
    std::atomic<int> n_sessions_{0};
};

/******************************************************************************/
/** @brief The client's end of a session's connection: sends frames, records what arrives. */
class ClientEnd : public std::enable_shared_from_this<ClientEnd> {
public:
    /** @brief Starts @p peer for this end @param peer The client's end of the pair */
    void start(const std::shared_ptr<GPeerI> &peer) {
        peer_ = peer;
        auto self = this->shared_from_this();
        peer->start(
            GPeerHandlers{
                .onOpen = {},
                .onMessage =
                    [self](std::string &&message) {
                        const std::string bytes{std::move(message)};
                        GCommandContainerT<GFaultyContainer> frame{GFrameKind::NONE};
                        Gem::Courtier::wireDecode(bytes, frame, nullptr, self->mode_);
                        const std::scoped_lock lock(self->mutex_);
                        if(frame.kind() == GFrameKind::HELLO) {
                            self->hello_first_ = self->kinds_.empty() && self->n_hellos_ == 0;
                            ++self->n_hellos_;
                            return; // the greeting is counted apart from the answers
                        }
                        self->kinds_.push_back(frame.kind());
                    },
                .onClosed =
                    [self](GPeerEnd end, GPeerClose how, std::string_view) {
                        const std::scoped_lock lock(self->mutex_);
                        self->closed_ = endName(end) + ":" + closeName(how);
                    },
            }
        );
    }

    /** @brief The wire format of what this end sends and reads; before start()
     *  @param mode The format */
    void setMode(Gem::Common::serializationMode mode) {
        mode_ = mode;
    }

    /** @brief @param kind A frame kind @param id The item a RESULT names @return A frame of it,
     *  encoded (a RESULT carries the item @p id) */
    [[nodiscard]] std::string encode(GFrameKind kind, std::size_t id = 0) const {
        GCommandContainerT<GFaultyContainer> frame{kind};
        if(kind == GFrameKind::RESULT) {
            frame.reset(kind, std::make_unique<GFaultyContainer>(id));
            Gem::Courtier::GProcessingOutcome outcome;
            outcome.correlation_id = id;
            frame.setOutcome(outcome);
        }
        return Gem::Courtier::wireEncode(frame, nullptr, mode_);
    }

    /** @brief Sends a frame of the given kind (on the peer's executor) @param kind Its kind */
    void send(GFrameKind kind) {
        this->sendBytes(this->encode(kind));
    }

    /** @brief Sends a message as it is (on the peer's executor) @param bytes The message */
    void sendBytes(std::string bytes) {
        if(auto peer = peer_.lock()) {
            boost::asio::post(peer->executor(), [peer, bytes = std::move(bytes)]() mutable {
                peer->send(Gem::Courtier::Consumers::makeMessage(std::move(bytes)));
            });
        }
    }

    /** @brief Closes this end @param how What the session is told */
    void close(GPeerClose how) {
        if(auto peer = peer_.lock()) {
            peer->close(how);
        }
    }

    /** @brief @param kind A frame kind @return How many frames of it have arrived */
    [[nodiscard]] std::size_t count(GFrameKind kind) const {
        const std::scoped_lock lock(mutex_);
        std::size_t n = 0;
        for(auto k : kinds_) {
            n += (k == kind) ? 1 : 0;
        }
        return n;
    }
    /** @brief @return How many frames have arrived, the HELLO not counted */
    [[nodiscard]] std::size_t nFrames() const {
        const std::scoped_lock lock(mutex_);
        return kinds_.size();
    }
    /** @brief @return How many HELLOs have arrived */
    [[nodiscard]] std::size_t nHellos() const {
        const std::scoped_lock lock(mutex_);
        return n_hellos_;
    }
    /** @brief @return Whether the first frame that arrived was a HELLO */
    [[nodiscard]] bool helloFirst() const {
        const std::scoped_lock lock(mutex_);
        return hello_first_;
    }
    /** @brief @return "<end>:<how>" once the connection has ended, empty before */
    [[nodiscard]] std::string closed() const {
        const std::scoped_lock lock(mutex_);
        return closed_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<GFrameKind> kinds_; ///< The kinds of the frames that arrived, in order (no HELLO)
    std::size_t n_hellos_ = 0;      ///< HELLOs that arrived
    bool hello_first_ = false;      ///< The first frame that arrived was a HELLO
    std::string closed_;            ///< How the connection ended
    std::weak_ptr<GPeerI> peer_;    ///< The client's end, weakly (rule 4)
    Gem::Common::serializationMode mode_ = BIN; ///< The wire format
};

/******************************************************************************/
/** @brief One session and its client, on the given executors */
struct Connection {
    std::shared_ptr<ClientEnd> client = std::make_shared<ClientEnd>(); ///< The client's role
    std::shared_ptr<GPeerI> clientPeer; ///< Held by the test so that it can be closed
    std::weak_ptr<GPeerI> serverPeer;   ///< The session's end
};

/**
 * @brief Starts a session for a new in-process connection.
 * @param consumer The stand-in for the consumer
 * @param server_executor Where the session runs
 * @param client_executor Where the client's end runs
 * @param registry The blob registry, or nullptr
 * @param peer_id The connection's id in the registry
 * @param mode The wire format of both ends
 * @return The connection
 */
Connection connect(
    FakeConsumer &consumer,
    const boost::asio::any_io_executor &server_executor,
    const boost::asio::any_io_executor &client_executor,
    GWireBlobRegistry *registry = nullptr,
    GWirePeerId peer_id = 1,
    Gem::Common::serializationMode mode = BIN
) {
    auto [server_end, client_end] =
        GInProcessPeer::makePair(server_executor, {}, client_executor, {});
    Connection connection;
    connection.clientPeer = client_end;
    connection.serverPeer = server_end;
    std::make_shared<session_type>(consumer.hooks(), mode, registry, peer_id)->start(server_end);
    connection.client->setMode(mode);
    connection.client->start(client_end);
    return connection;
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * A session that cannot serve a request is parked; woken for items, it answers every request it
 * owes.
 */
TEST_CASE(
    "GPeerSessionT: the requests a parked session owes are answered when it is woken",
    "[courtier][peer][session]"
) {
    FakeConsumer consumer;
    IoThreads server;
    IoThreads client;
    {
        auto connection = connect(consumer, server.executor(), client.executor());
        connection.client->send(GFrameKind::PULL);
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return consumer.nParked() == 1; }));
        CHECK(connection.client->nFrames() == 0);

        consumer.offer({1, 2});
        REQUIRE(eventually([&] { return connection.client->count(GFrameKind::WORK) == 2; }));
        CHECK(connection.client->nFrames() == 2);
        CHECK(consumer.nPending() == 0);

        connection.client->close(GPeerClose::NORMAL);
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
    }
}

/******************************************************************************/
/**
 * A wake-up stands for an item. A session woken after its connection has ended still takes the
 * item and gives it back when it goes, which wakes the next parked session: no item is lost and
 * no session waits for an item that is there.
 */
TEST_CASE(
    "GPeerSessionT: a session woken after its connection ended passes its item on",
    "[courtier][peer][session]"
) {
    FakeConsumer consumer;
    // The sessions run on a context this test drives itself, so that the order of their handlers
    // is known: the close of the first connection is handled before its wake-up. The client's
    // threads are joined before that context goes.
    boost::asio::io_context server_ctx;
    IoThreads client;
    auto first = connect(consumer, server_ctx.get_executor(), client.executor(), nullptr, 1);
    auto second = connect(consumer, server_ctx.get_executor(), client.executor(), nullptr, 2);

    const auto drive_until = [&](auto condition) {
        const auto give_up_at = std::chrono::steady_clock::now() + PATIENCE;
        while(not condition() && std::chrono::steady_clock::now() < give_up_at) {
            server_ctx.restart();
            server_ctx.run_for(5ms);
        }
        return condition();
    };

    first.client->send(GFrameKind::PULL);
    REQUIRE(drive_until([&] { return consumer.nParked() == 1; }));
    second.client->send(GFrameKind::PULL);
    REQUIRE(drive_until([&] { return consumer.nParked() == 2; }));

    // The first client closes; its close waits on the sessions' context, not handled yet.
    first.client->close(GPeerClose::NORMAL);
    std::atomic<bool> close_sent{false};
    boost::asio::post(first.clientPeer->executor(), [&] { close_sent.store(true); });
    REQUIRE(eventually([&] { return close_sent.load(); }));

    // One item: the first session, parked longest, is woken for it, behind its close.
    consumer.offer({7});
    CHECK(consumer.nParked() == 1);

    REQUIRE(drive_until([&] { return second.client->count(GFrameKind::WORK) == 1; }));
    CHECK(first.client->nFrames() == 0);
    CHECK(consumer.nPending() == 0);
    CHECK(consumer.nSessions() == 1);

    second.client->close(GPeerClose::NORMAL);
    REQUIRE(drive_until([&] { return consumer.nSessions() == 0; }));
    // Nothing of the sessions may outlive the context they ran on
    server_ctx.restart();
    server_ctx.run_for(QUIET);
}

/******************************************************************************/
/** A RESULT is checked in and asks for nothing: one PULL after it is answered once. */
TEST_CASE("GPeerSessionT: a RESULT is checked in and owes nothing", "[courtier][peer][session]") {
    FakeConsumer consumer;
    IoThreads server;
    IoThreads client;
    {
        auto connection = connect(consumer, server.executor(), client.executor());
        consumer.offer({1, 2});
        connection.client->send(GFrameKind::RESULT);
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return connection.client->count(GFrameKind::WORK) >= 1; }));
        REQUIRE(eventually([&] { return consumer.nCheckedIn() == 1; }));
        std::this_thread::sleep_for(QUIET);
        CHECK(connection.client->count(GFrameKind::WORK) == 1);
        CHECK(consumer.nPending() == 1);

        connection.client->close(GPeerClose::NORMAL);
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
    }
}

/******************************************************************************/
/**
 * When the server stops, a session that owes answers does not give them: it closes its connection
 * with the word that the run has ended.
 */
TEST_CASE(
    "GPeerSessionT: at the stop a session with owed requests closes with RUN_ENDED",
    "[courtier][peer][session]"
) {
    FakeConsumer consumer;
    IoThreads server;
    IoThreads client;
    {
        auto connection = connect(consumer, server.executor(), client.executor());
        connection.client->send(GFrameKind::PULL);
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return consumer.nParked() == 1; }));

        consumer.stop();
        REQUIRE(eventually([&] { return not connection.client->closed().empty(); }));
        CHECK(connection.client->closed() == "CLOSED_THERE:RUN_ENDED");
        CHECK(connection.client->nFrames() == 0);
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
    }
}

/******************************************************************************/
/**
 * The stop can come between the session's look at it and its checkout: the consumer then hands out
 * neither an item nor a promise. The request is not answered; the connection is closed with
 * RUN_ENDED.
 */
TEST_CASE(
    "GPeerSessionT: a stop that comes during the checkout closes with RUN_ENDED and answers "
    "nothing",
    "[courtier][peer][session]"
) {
    FakeConsumer consumer;
    IoThreads server;
    IoThreads client;
    {
        auto connection = connect(consumer, server.executor(), client.executor());
        REQUIRE(eventually([&] { return consumer.nSessions() == 1; }));
        consumer.stopAtNextCheckout();
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return not connection.client->closed().empty(); }));
        CHECK(connection.client->closed() == "CLOSED_THERE:RUN_ENDED");
        CHECK(connection.client->nFrames() == 0);
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
    }
}

/******************************************************************************/
/** A session forgets its client's blob state, and signs off, when it goes. */
TEST_CASE(
    "GPeerSessionT: a session releases the blob state of its client in its destructor",
    "[courtier][peer][session]"
) {
    FakeConsumer consumer;
    GWireBlobRegistry registry;
    const GWireBlobId blob{17, 4};
    constexpr GWirePeerId PEER = 5;
    IoThreads server;
    IoThreads client;
    {
        auto connection = connect(consumer, server.executor(), client.executor(), &registry, PEER);
        REQUIRE(eventually([&] { return consumer.nSessions() == 1; }));
        registry.markPeerHasBlob(PEER, blob);
        REQUIRE(registry.peerHasBlob(PEER, blob));

        connection.client->close(GPeerClose::NORMAL);
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
        CHECK_FALSE(registry.peerHasBlob(PEER, blob));
    }
}

/******************************************************************************/
/** A session greets its client with HELLO, the first message of the connection, before any answer. */
TEST_CASE(
    "GPeerSessionT: a session greets its client with HELLO first",
    "[courtier][peer][session]"
) {
    FakeConsumer consumer;
    IoThreads server;
    IoThreads client;
    {
        auto connection = connect(consumer, server.executor(), client.executor());
        consumer.offer({1});
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return connection.client->count(GFrameKind::WORK) == 1; }));
        CHECK(connection.client->nHellos() == 1);
        CHECK(connection.client->helloFirst());

        connection.client->close(GPeerClose::NORMAL);
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
    }
}

/******************************************************************************/
/**
 * A RESULT whose payload cannot be read fails the item its head names; nothing is answered (it
 * asked for nothing), the connection goes on, and the client's blob state is forgotten: its blobs
 * are sent in full again. Under JSON as under the binary codec.
 */
TEST_CASE(
    "GPeerSessionT: a RESULT whose payload cannot be read fails its item and resets the blobs",
    "[courtier][peer][session]"
) {
    const auto mode = GENERATE(BIN, Gem::Common::serializationMode::GEM_JSON);
    constexpr GWirePeerId PEER = 1;
    FakeConsumer consumer;
    GWireBlobRegistry registry;
    const GWireBlobId blob{17, 4};
    IoThreads server;
    IoThreads client;
    {
        auto connection =
            connect(consumer, server.executor(), client.executor(), &registry, PEER, mode);
        consumer.offer({1, 2});
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return connection.client->count(GFrameKind::WORK) == 1; }));
        registry.markPeerHasBlob(PEER, blob);

        const std::string result = connection.client->encode(GFrameKind::RESULT, 1);
        connection.client->sendBytes(result.substr(0, result.size() - 8)); // the payload is cut
        REQUIRE(eventually([&] { return consumer.nFailed() == 1; }));
        CHECK(consumer.nCheckedIn() == 0);
        CHECK_FALSE(registry.peerHasBlob(PEER, blob));

        connection.client->send(GFrameKind::PULL); // the connection goes on
        REQUIRE(eventually([&] { return connection.client->count(GFrameKind::WORK) == 2; }));
        CHECK(connection.client->nFrames() == 2);
        CHECK(connection.client->closed().empty());

        connection.client->close(GPeerClose::NORMAL);
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
    }
}

/******************************************************************************/
/**
 * A RESULT without a result -- a client that could not read its item, or could not return it,
 * sends the outcome alone -- is checked in, and the client's blobs are sent in full again: the item
 * it could not read may have carried one.
 */
TEST_CASE(
    "GPeerSessionT: a RESULT without a result is checked in and resets the blobs",
    "[courtier][peer][session]"
) {
    constexpr GWirePeerId PEER = 1;
    FakeConsumer consumer;
    GWireBlobRegistry registry;
    const GWireBlobId blob{17, 4};
    IoThreads server;
    IoThreads client;
    {
        auto connection = connect(consumer, server.executor(), client.executor(), &registry, PEER);
        consumer.offer({1});
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return connection.client->count(GFrameKind::WORK) == 1; }));
        registry.markPeerHasBlob(PEER, blob);

        GCommandContainerT<GFaultyContainer> failed{GFrameKind::WORK};
        Gem::Courtier::GProcessingOutcome outcome;
        outcome.correlation_id = 1;
        failed.setOutcome(outcome);
        Gem::Courtier::makeFailedResult(failed, failed.outcome(), "it could not be read");
        connection.client->sendBytes(Gem::Courtier::wireEncode(failed, nullptr, BIN));
        REQUIRE(eventually([&] { return consumer.nCheckedIn() == 1; }));
        CHECK_FALSE(registry.peerHasBlob(PEER, blob));
        CHECK(consumer.nFailed() == 0);

        connection.client->close(GPeerClose::NORMAL);
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
    }
}

/******************************************************************************/
/**
 * A message whose head cannot be read is the one protocol error: the session does not know what it
 * was, answers nothing, ends the connection with ERROR, and the lease gives back what the client
 * held. Under JSON, a message that is not JSON at all is such a message.
 */
TEST_CASE(
    "GPeerSessionT: a message whose head cannot be read ends the connection with ERROR",
    "[courtier][peer][session]"
) {
    const auto mode = GENERATE(BIN, Gem::Common::serializationMode::GEM_JSON);
    FakeConsumer consumer;
    IoThreads server;
    IoThreads client;
    {
        auto connection = connect(consumer, server.executor(), client.executor(), nullptr, 1, mode);
        consumer.offer({1});
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return connection.client->count(GFrameKind::WORK) == 1; }));
        REQUIRE(consumer.nPending() == 0);

        connection.client->sendBytes(
            mode == BIN ? std::string("x") : std::string("this is not JSON")
        );
        REQUIRE(eventually([&] { return connection.client->closed() == "CLOSED_THERE:ERROR"; }));
        CHECK(connection.client->nFrames() == 1); // no answer
        // The lease gives back when the session goes, after its close has gone out: the client
        // may hear the close first. What the client held is back:
        CHECK(eventually([&] { return consumer.nPending() == 1; }));
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
    }
}

/******************************************************************************/
/**
 * Messages that arrive after the session has closed its connection, but before its close has gone
 * out, are still handed to it (GPeerI.hpp, rule 6): the client sent them before it knew of the
 * close. A result among them is checked in; nothing is answered, and nothing is checked out.
 */
TEST_CASE(
    "GPeerSessionT: after its own close a session checks a result in and answers nothing",
    "[courtier][peer][session]"
) {
    FakeConsumer consumer;
    IoThreads server;
    IoThreads client;
    {
        auto connection = connect(consumer, server.executor(), client.executor());
        consumer.offer({1, 2});
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return connection.client->count(GFrameKind::WORK) == 1; }));
        auto server_end = connection.serverPeer.lock();
        REQUIRE(server_end);

        // The strand of the session's end is held while the client sends a message whose head
        // cannot be read (the session closes with ERROR on it), a RESULT and a PULL: all three
        // have arrived before the session's close goes out.
        const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
        boost::asio::post(server_end->executor(), [held = release]() { held->wait_for(PATIENCE); });
        connection.client->sendBytes("x");
        connection.client->send(GFrameKind::RESULT);
        connection.client->send(GFrameKind::PULL);
        const auto sent = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
        boost::asio::post(connection.clientPeer->executor(), [sent] { sent->count_down(); });
        REQUIRE(sent->wait_for(PATIENCE));
        release->count_down();

        REQUIRE(eventually([&] { return connection.client->closed() == "CLOSED_THERE:ERROR"; }));
        CHECK(consumer.nCheckedIn() == 1);        // the RESULT
        CHECK(connection.client->nFrames() == 1); // the first WORK; the PULL is not answered
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
        // A session signs off before its lease gives back. What the client held is back, and
        // nothing is out:
        CHECK(eventually([&] { return consumer.nPending() == 2; }));
    }
}

/******************************************************************************/
/**
 * An item whose message is longer than the transport can carry fails, and only it: it is not handed
 * to the peer (which would end the connection, and the next client would meet the item again). The
 * connection stays open, and the request is still owed.
 */
TEST_CASE(
    "GPeerSessionT: an item longer than the transport can carry fails and the connection stays",
    "[courtier][peer][session]"
) {
    FakeConsumer consumer;
    IoThreads server;
    IoThreads client;
    {
        auto [server_end, client_end] = GInProcessPeer::makePair(
            server.executor(),
            // the HELLO fits, every WORK is longer
            {.maxMessageBytes = Gem::Courtier::buildHello<GFaultyContainer>(BIN, 0ms).size()},
            client.executor(),
            {}
        );
        Connection connection;
        connection.clientPeer = client_end;
        connection.serverPeer = server_end;
        std::make_shared<session_type>(consumer.hooks(), BIN, nullptr, 1)->start(server_end);
        connection.client->start(client_end);

        consumer.offer({1});
        connection.client->send(GFrameKind::PULL);
        REQUIRE(eventually([&] { return consumer.nFailed() == 1; }));
        REQUIRE(eventually([&] { return consumer.nParked() == 1; })); // the request is still owed
        CHECK(connection.client->nFrames() == 0);
        CHECK(connection.client->closed().empty()); // the connection goes on

        connection.client->close(GPeerClose::NORMAL);
        REQUIRE(eventually([&] { return consumer.nSessions() == 0; }));
    }
}
