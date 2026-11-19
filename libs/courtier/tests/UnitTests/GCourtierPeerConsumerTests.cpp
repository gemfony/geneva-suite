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

// The server's shell over any listener (GPeerConsumerT), on the in-process transport: the end of a
// run, the bound on it, the items of a lost client and the registry of peers. The clients are test
// roles that send their requests by hand.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>

#include "GDemoProcessingContainers.hpp"
#include "GInProcessPeer.hpp"
#include "GInProcessTestSupport.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "common/concurrency/GThreadBudget.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/GWireCodec.hpp"
#include "courtier/consumers/GNetworkedTimeoutConfig.hpp"
#include "courtier/consumers/GPeerConsumerT.hpp"
#include "courtier/transport/GPeerI.hpp"

using Gem::Courtier::GCommandContainerT;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GFrameKind;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerEnd;
using Gem::Courtier::Consumers::GPeerHandlers;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Tests::closeName;
using Gem::Courtier::Tests::endName;
using Gem::Courtier::Tests::GInProcessConnector;
using Gem::Courtier::Tests::GInProcessFaults;
using Gem::Courtier::Tests::GInProcessListener;
using Gem::Courtier::Tests::GInProcessListenerRef;
using Gem::Courtier::Tests::GInProcessPeer;
using Gem::Courtier::Tests::GInProcessRoute;
using Gem::Courtier::Tests::IoThreads;
using namespace std::chrono_literals;

namespace {

constexpr auto PATIENCE = 5s; // how long a test waits for something that is due at once
constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

/** @brief Waits, at most PATIENCE, for @p condition @return Whether it held in time */
template <typename condition_type>
bool eventually(condition_type condition) {
    return Gem::Courtier::Tests::eventually(condition, PATIENCE);
}

/******************************************************************************/
/** @brief The shell under test, on the in-process listener */
class Shell final : public Gem::Courtier::GPeerConsumerT<GFaultyContainer> {
public:
    /** @brief @param listener The in-process listener the shell owns a reference to
     *  @param n_threads The session threads */
    Shell(const std::shared_ptr<GInProcessListener> &listener, std::size_t n_threads)
      : Shell(std::make_unique<GInProcessListenerRef>(listener), n_threads) { /* nothing */
    }

    /** @brief @param listener The listener the shell owns @param n_threads The session threads */
    Shell(std::unique_ptr<Gem::Courtier::Consumers::GListenerI> listener, std::size_t n_threads)
      : Gem::Courtier::GPeerConsumerT<GFaultyContainer>(
            "Shell",
            std::move(listener),
            n_threads,
            BIN
        ) {
        // No time limit (the default): a lost item must come back through the session's lease.
    }
};

/******************************************************************************/
/**
 * A client that sends its requests by hand: some PULLs when the connection is open, and for every
 * WORK either the RESULT of the evaluated item and a PULL for the next, or nothing (it holds the
 * item). It records what it got (the server's HELLO not counted) and how its connection ended.
 */
class ScriptedClient : public std::enable_shared_from_this<ScriptedClient> {
public:
    /** @brief @param n_pulls PULLs sent when the connection is open
     *  @param returns Whether a WORK is evaluated and returned
     *  @param foreign Whether a return claims that another server process handed its item out */
    ScriptedClient(std::size_t n_pulls, bool returns, bool foreign = false)
      : n_pulls_(n_pulls)
      , returns_(returns)
      , foreign_(foreign) { /* nothing */
    }

    /** @brief Connects through @p connector on @p executor and starts the peer it gets
     *  @param connector The connector, kept alive by the caller @param executor Where it runs */
    void connect(GInProcessConnector &connector, const boost::asio::any_io_executor &executor) {
        connector.connect(
            executor,
            [self =
                 this->shared_from_this()](const std::shared_ptr<GPeerI> &peer, std::string_view) {
                if(peer) {
                    self->start_(peer);
                }
            }
        );
    }

    /** @brief Sends a message as it is (on the peer's executor); the client must know its end
     *  (open()), or the test is wrong and is told so @param bytes The message */
    void sendBytes(std::string bytes) {
        auto peer = this->knownPeer_("sendBytes");
        boost::asio::post(peer->executor(), [peer, bytes = std::move(bytes)]() mutable {
            peer->send(Gem::Courtier::Consumers::makeMessage(std::move(bytes)));
        });
    }

    /** @brief Ends the connection at once, as a client that is lost does (on the peer's executor);
     *  the client must know its end */
    void abort() {
        auto peer = this->knownPeer_("abort");
        boost::asio::post(peer->executor(), [peer] { peer->abort(); });
    }

    /** @brief @return Whether the connection is open */
    [[nodiscard]] bool open() const {
        return open_.load();
    }
    /** @brief @return How many WORK frames arrived */
    [[nodiscard]] std::size_t nWork() const {
        return n_work_.load();
    }
    /** @brief @return How many frames arrived */
    [[nodiscard]] std::size_t nFrames() const {
        return n_frames_.load();
    }
    /** @brief @return "<end>:<how>" once the connection has ended, empty before */
    [[nodiscard]] std::string closed() const {
        const std::scoped_lock lock(mutex_);
        return closed_;
    }

private:
    /** @brief @return The peer, if there is one */
    std::shared_ptr<GPeerI> peer_() const {
        const std::scoped_lock lock(mutex_);
        return peer_weak_.lock();
    }

    /** @brief @param what The action that needs the peer @return The peer; if there is none, the
     *  test program ends at once with a message (an action of the test before the client knows its
     *  end would silently go nowhere). Not a throw: in a test that holds a batch in a std::async
     *  future, the exception would unwind into the future's destructor, which waits for a batch
     *  that only the server's stop ends, and the test would hang until ctest's timeout. (Catch2
     *  has no death tests, so this path of a wrong test is not tested itself.) */
    std::shared_ptr<GPeerI> knownPeer_(std::string_view what) const {
        auto peer = this->peer_();
        if(not peer) {
            std::cerr << "ScriptedClient::" << what
                      << ": the client does not know its end (yet); the test is wrong" << std::endl;
            std::abort();
        }
        return peer;
    }

    /** @brief Starts the peer (on its executor) @param peer The client's end */
    void start_(const std::shared_ptr<GPeerI> &peer) {
        {
            const std::scoped_lock lock(mutex_);
            peer_weak_ = peer;
        }
        auto self = this->shared_from_this();
        peer->start(
            GPeerHandlers{
                .onOpen =
                    [self] {
                        self->open_.store(true);
                        for(std::size_t i = 0; i < self->n_pulls_; ++i) {
                            self->send_(GCommandContainerT<GFaultyContainer>{GFrameKind::PULL});
                        }
                    },
                .onMessage = [self](std::string &&message) { self->received_(std::move(message)); },
                .onClosed =
                    [self](GPeerEnd end, GPeerClose how, std::string_view) {
                        self->open_.store(false);
                        const std::scoped_lock lock(self->mutex_);
                        self->closed_ = endName(end) + ":" + closeName(how);
                    },
            }
        );
    }

    /** @brief A message from the server, on the peer's executor @param message Its bytes */
    void received_(std::string &&message) {
        const std::string bytes{std::move(message)};
        GCommandContainerT<GFaultyContainer> frame{GFrameKind::NONE};
        Gem::Courtier::wireDecode(bytes, frame, nullptr, BIN);
        if(frame.kind() == GFrameKind::HELLO) {
            return;
        }
        n_frames_.fetch_add(1);
        if(frame.kind() != GFrameKind::WORK) {
            return;
        }
        n_work_.fetch_add(1);
        if(returns_) {
            Gem::Courtier::processWorkFrame(frame, "ScriptedClient");
            Gem::Courtier::makeReturnFrame(frame);
            if(foreign_) {
                auto outcome = frame.outcome();
                outcome.submission_uuid[0] ^= 1U; // another process's lineage, the same id
                frame.setOutcome(std::move(outcome));
            }
            this->send_(frame);
            this->send_(GCommandContainerT<GFaultyContainer>{GFrameKind::PULL});
        }
    }

    /** @brief Encodes and sends a frame, on the peer's executor @param frame The frame */
    void send_(const GCommandContainerT<GFaultyContainer> &frame) {
        if(auto peer = this->peer_()) {
            peer->send(
                Gem::Courtier::Consumers::makeMessage(
                    Gem::Courtier::wireEncode(frame, nullptr, BIN)
                )
            );
        }
    }

    const std::size_t n_pulls_;
    const bool returns_;
    const bool foreign_;
    mutable std::mutex mutex_;
    std::weak_ptr<GPeerI> peer_weak_; ///< Under mutex_
    std::string closed_;              ///< Under mutex_
    std::atomic<bool> open_{false};
    std::atomic<std::size_t> n_work_{0};
    std::atomic<std::size_t> n_frames_{0};
};

/** @brief @param n How many @return n items, numbered from 0 */
std::vector<std::unique_ptr<GFaultyContainer>> makeBatch(std::size_t n) {
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        items.push_back(std::make_unique<GFaultyContainer>(i));
    }
    return items;
}

/** @brief @param since A moment @return The milliseconds since then */
long long msSince(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - since
    )
        .count();
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * The end of a run: every client is told RUN_ENDED, the parked ones and those that wait for
 * nothing alike, and the shell does not wait out its grace when their connections end.
 */
TEST_CASE(
    "GPeerConsumerT: at the stop forty clients hear RUN_ENDED and the registry empties",
    "[courtier][peer][shell]"
) {
    constexpr std::size_t N_PARKED = 20;
    constexpr std::size_t N_IDLE = 20;
    auto listener =
        std::make_shared<GInProcessListener>(GInProcessListener::Options{.stopGrace = 3s});
    // The connector goes after the threads its client ends run on (GInProcessConnector)
    GInProcessConnector connector{std::make_shared<GInProcessRoute>(listener)};
    IoThreads clients;
    std::vector<std::shared_ptr<ScriptedClient>> roles;
    roles.reserve(N_PARKED + N_IDLE);
    {
        Shell shell{listener, 4};
        shell.startServer();
        for(std::size_t i = 0; i < N_PARKED + N_IDLE; ++i) {
            // The first ones ask for work that is not there and are parked; the others ask for
            // nothing (as a client does while it evaluates).
            roles.push_back(std::make_shared<ScriptedClient>(i < N_PARKED ? 1 : 0, true));
            roles.back()->connect(connector, clients.executor());
        }
        REQUIRE(eventually([&] { return shell.getNActiveSessions() == N_PARKED + N_IDLE; }));

        const auto stop_began = std::chrono::steady_clock::now();
        shell.stopServer();
        const auto stop_ms = msSince(stop_began);
        INFO("stopServer() took " << stop_ms << " ms");
        CHECK(stop_ms < 1500);
        CHECK(shell.getNActiveSessions() == 0);
        CHECK(listener->workEnded());
    }
    for(const auto &role : roles) {
        // The server's end has ended; the client's onClosed may still be due.
        CHECK(eventually([&] { return role->closed() == "CLOSED_THERE:RUN_ENDED"; }));
        CHECK(role->nFrames() == 0);
    }
}

/******************************************************************************/
/**
 * The connected clients (connectedClients(), what a benchmark waits for) are the sessions whose
 * client has spoken and whose connection has not ended. A client that has said nothing is not
 * counted; one whose connection has ended is no longer counted. A client that dies without its
 * connection ending, on a transport that does not notice the loss, is counted on (as the
 * query's documentation says) -- here until the end of the run.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- two linear scenarios; Catch2's macros count
TEST_CASE(
    "GPeerConsumerT: the connected clients are those that have spoken and are still connected",
    "[courtier][peer][shell]"
) {
    SECTION("a client that has said nothing is not counted; one that has ended is no longer") {
        auto listener =
            std::make_shared<GInProcessListener>(GInProcessListener::Options{.stopGrace = 300ms});
        GInProcessConnector connector{std::make_shared<GInProcessRoute>(listener)};
        IoThreads clients;
        auto silent = std::make_shared<ScriptedClient>(0, true);
        auto speaking = std::make_shared<ScriptedClient>(1, true);
        {
            Shell shell{listener, 2};
            shell.startServer();
            silent->connect(connector, clients.executor());
            REQUIRE(eventually([&] { return silent->open() && shell.getNActiveSessions() == 1; }));
            CHECK(shell.connectedClients() == std::optional<std::size_t>{0});

            speaking->connect(connector, clients.executor());
            REQUIRE(eventually([&] { return shell.connectedClients() == 1U; }));
            CHECK(shell.getNActiveSessions() == 2);

            speaking->abort(); // its connection ends; the server's end hears its close
            REQUIRE(eventually([&] { return shell.connectedClients() == 0U; }));
            shell.stopServer();
        }
    }

    SECTION("a client that dies unnoticed is counted until the transport notices") {
        // The server's ends ignore a close (the client's close never arrives, as from a host
        // that is gone), and the listener has no keep-alive
        auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{
            .serverFaults = GInProcessFaults{.ignoreClose = true},
            .stopGrace = 300ms
        });
        GInProcessConnector connector{std::make_shared<GInProcessRoute>(listener)};
        IoThreads clients;
        auto dying = std::make_shared<ScriptedClient>(1, true);
        auto living = std::make_shared<ScriptedClient>(1, true);
        {
            Shell shell{listener, 2};
            shell.startServer();
            dying->connect(connector, clients.executor());
            REQUIRE(eventually([&] { return shell.connectedClients() == 1U; }));
            dying->abort();
            REQUIRE(eventually([&] { return not dying->open(); }));
            living->connect(connector, clients.executor());
            REQUIRE(eventually([&] { return shell.connectedClients() == 2U; }));
            CHECK(shell.getNActiveSessions() == 2);
            shell.stopServer();
        }
    }
}

/******************************************************************************/
/**
 * A client that is lost while it holds items: its session puts them back at once, and another
 * client gets them. Nothing is given up on time here (no time limit), so only the lease can.
 */
TEST_CASE(
    "GPeerConsumerT: the items of a lost client are served to another one",
    "[courtier][peer][shell]"
) {
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    // The connector goes after the threads its client ends run on (GInProcessConnector)
    GInProcessConnector connector{std::make_shared<GInProcessRoute>(listener)};
    IoThreads clients;
    Shell shell{listener, 2};
    shell.startServer();

    auto items = makeBatch(4);
    auto batch = std::async(std::launch::async, [&] {
        shell.processBatch(
            std::span(items.data(), items.size()),
            Gem::Courtier::GSubmissionPolicy::clone_on_partial_return()
        );
    });

    // The first client takes two items and holds them, then is lost.
    auto lost = std::make_shared<ScriptedClient>(2, false);
    lost->connect(connector, clients.executor());
    REQUIRE(eventually([&] { return lost->nWork() == 2; }));
    lost->abort();
    REQUIRE(eventually([&] { return shell.getNActiveSessions() == 0; }));

    // The second client evaluates everything there is, the two put back among it.
    auto worker = std::make_shared<ScriptedClient>(1, true);
    worker->connect(connector, clients.executor());

    const bool done = batch.wait_for(PATIENCE) == std::future_status::ready;
    shell.stopServer(); // ends the batch if it is not done
    batch.get();
    REQUIRE(done);
    CHECK(worker->nWork() == 4);
    std::size_t n_processed = 0;
    for(const auto &item : items) {
        n_processed += (item && item->is_processed()) ? 1 : 0;
    }
    CHECK(n_processed == 4);
}

/******************************************************************************/
/** A shell that was never started stops at once and does not end its listener's work. */
TEST_CASE(
    "GPeerConsumerT: a shell that was never started has nothing to stop",
    "[courtier][peer][shell]"
) {
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    {
        Shell shell{listener, 1};
        shell.stopServer();
        CHECK(shell.getNActiveSessions() == 0);
    }
    CHECK_FALSE(listener->workEnded());
}

/******************************************************************************/
/**
 * A listener whose start() throws (a port that cannot be bound) leaves the shell one that was never
 * started: the exception reaches the caller, and neither stopServer() nor the destructor asks the
 * listener to stop or to end its work, or gives back threads that were never reserved; the stop
 * announces the end of the run to the clients that may wait for the server, once. A second
 * startServer() is refused.
 */
TEST_CASE(
    "GPeerConsumerT: a listener whose start throws leaves the shell never started",
    "[courtier][peer][shell]"
) {
    auto listener =
        std::make_shared<GInProcessListener>(GInProcessListener::Options{.startThrows = true});
    auto &budget = Gem::Common::Concurrency::threadBudget();
    const unsigned int reserved_before = budget.reserved();
    {
        Shell shell{listener, 3};
        CHECK_THROWS(shell.startServer());
        CHECK(budget.reserved() == reserved_before);
        CHECK_THROWS(shell.startServer());
        shell.stopServer();
        CHECK(listener->nStops() == 0);
        CHECK_FALSE(listener->workEnded());
        CHECK(listener->nAnnounced() == 1);
        CHECK(budget.reserved() == reserved_before);
    }
    CHECK(listener->nAnnounced() == 1);
    CHECK(listener->nStops() == 0);
    CHECK_FALSE(listener->workEnded());
    CHECK(budget.reserved() == reserved_before);
}

/******************************************************************************/
// A session that is made while the server stops: its peer's start() is held on the listener's
// thread, in on_peer (by a wrapper around the server's end), while stopServer() looks at the
// registry of peers.

namespace {

/** @brief What the wrapped server ends of one test share: the hold of start(), and what was seen */
struct StartHold {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;   ///< A start() is held (under mutex)
    bool closeSeen = false; ///< A close() has arrived (under mutex)
    std::optional<std::chrono::steady_clock::time_point> stopAt; ///< stop() of the listener
    std::atomic<std::size_t> closesBeforeStart{0}; ///< close() calls before start() had returned

    /** @brief Holds start() until a close arrives or HOLD after the listener's stop() */
    void hold() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        const auto give_up_at = std::chrono::steady_clock::now() + PATIENCE;
        while(not closeSeen && std::chrono::steady_clock::now() < give_up_at &&
              not(stopAt && std::chrono::steady_clock::now() >= *stopAt + HOLD)) {
            changed.wait_for(lock, 5ms);
        }
    }
    /** @brief Notes the listener's stop() */
    void stopped() {
        const std::scoped_lock lock(mutex);
        stopAt = std::chrono::steady_clock::now();
        changed.notify_all();
    }
    /** @brief Notes a close() @param before_start Whether start() had not returned */
    void closed(bool before_start) {
        if(before_start) {
            closesBeforeStart.fetch_add(1);
        }
        const std::scoped_lock lock(mutex);
        closeSeen = true;
        changed.notify_all();
    }
    /** @brief @return Whether a start() is held or was */
    bool wasEntered() {
        const std::scoped_lock lock(mutex);
        return entered;
    }

    static constexpr auto HOLD = 300ms; ///< How long a start() is held after the listener's stop()
};

/** @brief A server's end whose start() is held (StartHold); it records a close() before it.
 *  It is owned by the handlers it installs on the end it wraps, as a role is (rule 4). */
class HeldStartPeer final
  : public GPeerI
  , public std::enable_shared_from_this<HeldStartPeer> {
public:
    /** @brief @param inner The end it wraps @param hold What it shares with the test */
    HeldStartPeer(const std::shared_ptr<GPeerI> &inner, std::shared_ptr<StartHold> hold)
      : inner_(inner)
      , hold_(std::move(hold)) { /* nothing */
    }
    /** @brief Waits for the hold, then starts the wrapped end @param handlers See GPeerI */
    void start(GPeerHandlers handlers) override {
        hold_->hold();
        auto inner = inner_.lock();
        if(inner) {
            auto self = this->shared_from_this();
            inner->start(
                GPeerHandlers{
                    .onOpen = std::move(handlers.onOpen),
                    .onMessage = [self, on_message = std::move(handlers.onMessage)](
                                     std::string &&message
                                 ) mutable { on_message(std::move(message)); },
                    .onClosed = std::move(handlers.onClosed),
                }
            );
        }
        started_.store(true);
    }
    /** @brief Forwards @param message See GPeerI */
    void send(Gem::Courtier::Consumers::GMessage message) override {
        if(auto inner = inner_.lock()) {
            inner->send(std::move(message));
        }
    }
    /** @brief Records whether start() has returned, and forwards @param how See GPeerI */
    void close(GPeerClose how) override {
        hold_->closed(not started_.load());
        if(auto inner = inner_.lock()) {
            inner->close(how);
        }
    }
    /** @brief Forwards */
    void abort() override {
        if(auto inner = inner_.lock()) {
            inner->abort();
        }
    }
    /** @brief Forwards @return The wrapped end's executor */
    [[nodiscard]] boost::asio::any_io_executor executor() override {
        auto inner = inner_.lock();
        return inner ? inner->executor() : boost::asio::any_io_executor{};
    }

private:
    std::weak_ptr<GPeerI> inner_;      ///< The wrapped end
    std::shared_ptr<StartHold> hold_;  ///< Shared with the test
    std::atomic<bool> started_{false}; ///< start() has returned
};

/** @brief The in-process listener, with every server end wrapped in a HeldStartPeer */
class HeldStartListener final : public Gem::Courtier::Consumers::GListenerI {
public:
    /** @brief @param listener The in-process listener @param hold Shared with the ends */
    HeldStartListener(std::shared_ptr<GInProcessListener> listener, std::shared_ptr<StartHold> hold)
      : listener_(std::move(listener))
      , hold_(std::move(hold)) { /* nothing */
    }
    /** @brief Wraps the ends @param executor See GListenerI @param on_peer See GListenerI */
    void start(boost::asio::any_io_executor executor, on_peer_type on_peer) override {
        listener_->start(
            std::move(executor),
            [hold = hold_, on_peer = std::move(on_peer)](
                const std::shared_ptr<GPeerI> &peer,
                Gem::Courtier::GWirePeerId id
            ) mutable { on_peer(std::make_shared<HeldStartPeer>(peer, hold), id); }
        );
    }
    /** @brief Notes the stop, and forwards */
    void stop() override {
        hold_->stopped();
        listener_->stop();
    }
    /** @brief Forwards */
    void endWork() override {
        listener_->endWork();
    }
    /** @brief Forwards @return The in-process listener's grace */
    [[nodiscard]] std::chrono::milliseconds stopGrace() const override {
        return listener_->stopGrace();
    }
    /** @brief Forwards @return The listener's interval */
    [[nodiscard]] std::chrono::milliseconds keepAlive() const override {
        return listener_->keepAlive();
    }

private:
    std::shared_ptr<GInProcessListener> listener_; ///< The in-process listener
    std::shared_ptr<StartHold> hold_;              ///< Shared with the ends
};

} /* anonymous namespace */

/**
 * The shell never closes a peer before that peer has been started (GPeerI.hpp, rule 2): a session
 * enters the registry only after its peer's start(). The stop comes while the listener's thread is
 * held in the start() of a new peer for longer than the listener's stop() waits for it; the stop
 * neither closes that peer early nor waits its grace for a peer whose close was lost.
 */
TEST_CASE(
    "GPeerConsumerT: a peer is never closed before it has been started",
    "[courtier][peer][shell]"
) {
    constexpr auto STOP_GRACE = 1500ms;
    // The listener's own wait for the calls of on_peer gives up before the start() is let go: the
    // order of start and sign-on alone keeps rule 2 then.
    auto listener = std::make_shared<GInProcessListener>(
        GInProcessListener::Options{.stopGrace = STOP_GRACE, .stopBound = 100ms}
    );
    auto hold = std::make_shared<StartHold>();
    // The connector goes after the threads its client ends run on (GInProcessConnector)
    GInProcessConnector connector{std::make_shared<GInProcessRoute>(listener)};
    IoThreads clients;
    auto role = std::make_shared<ScriptedClient>(0, true);
    {
        Gem::Courtier::GPeerConsumerT<GFaultyContainer>
            shell{"Shell", std::make_unique<HeldStartListener>(listener, hold), 2, BIN};
        shell.startServer();
        role->connect(connector, clients.executor());
        REQUIRE(eventually([&] { return hold->wasEntered(); }));

        const auto stop_began = std::chrono::steady_clock::now();
        shell.stopServer();
        const auto stop_ms = msSince(stop_began);
        INFO("stopServer() took " << stop_ms << " ms");
        CHECK(listener->nStopBoundsHit() == 1);
        CHECK(hold->closesBeforeStart.load() == 0);
        CHECK(stop_ms < STOP_GRACE.count());
    }
}

/******************************************************************************/
/**
 * A client that connects while the thread the listener calls on_peer on is busy: its session is on
 * its way when the server stops. stop() of the listener waits for that call, so the shell finds the
 * session and the client hears that the run has ended (GListenerI::stop()).
 */
TEST_CASE(
    "GPeerConsumerT: a client whose session is on its way at the stop hears RUN_ENDED",
    "[courtier][peer][shell]"
) {
    constexpr auto STOP_GRACE = 3000ms;
    auto listener =
        std::make_shared<GInProcessListener>(GInProcessListener::Options{.stopGrace = STOP_GRACE});
    // The connector goes after the threads its client ends run on (GInProcessConnector)
    GInProcessConnector connector{std::make_shared<GInProcessRoute>(listener)};
    IoThreads clients;
    auto role = std::make_shared<ScriptedClient>(0, true);
    long long stop_ms = 0;
    {
        Shell shell{listener, 2};
        shell.startServer();
        const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
        auto holding = std::make_shared<std::atomic<bool>>(false);
        listener->holdAcceptThread([holding, release] {
            holding->store(true);
            release->wait();
        });
        const bool held = eventually([&] { return holding->load(); });
        role->connect(connector, clients.executor());
        const bool connected = eventually([&] { return listener->nConnections() == 1; });
        auto stop = std::async(std::launch::async, [&shell] {
            const auto stop_began = std::chrono::steady_clock::now();
            shell.stopServer();
            return msSince(stop_began);
        });
        const bool stopping = eventually([&] { return listener->nStops() == 1; });
        std::this_thread::sleep_for(100ms); // the stop waits for the call of on_peer now
        release->count_down();
        stop_ms = stop.get();
        REQUIRE(held);
        REQUIRE(connected);
        REQUIRE(stopping);
        CHECK(listener->nStopBoundsHit() == 0);
    }
    INFO("stopServer() took " << stop_ms << " ms");
    CHECK(eventually([&] { return role->closed() == "CLOSED_THERE:RUN_ENDED"; }));
    CHECK(stop_ms < STOP_GRACE.count());
}

/******************************************************************************/
/**
 * With the user's reclaim lease (a listener without a keep-alive), a session's lease gives back
 * only its own checkout: client A holds item X until the lease puts it back, client B takes it, then
 * A is lost. A's lease must not take X from B: a third client C gets nothing, and X has been handed
 * out twice, not three times.
 */
TEST_CASE(
    "GPeerConsumerT: a lost client's lease gives back only its own checkout",
    "[courtier][peer][shell]"
) {
    constexpr std::int64_t LEASE_MS = 2000;
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    GInProcessConnector connector{std::make_shared<GInProcessRoute>(listener)};
    IoThreads clients;
    Shell shell{listener, 2};
    Gem::Courtier::GNetworkedTimeoutConfig config;
    config.reclaim_lease_ms = LEASE_MS;
    shell.applyTimeoutConfig(config);
    shell.startServer();

    auto items = makeBatch(1);
    auto batch = std::async(std::launch::async, [&] {
        auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
        policy.fatal_on_none_returned = false;
        auto clone_template = std::make_unique<GFaultyContainer>(99);
        clone_template->markAsProcessedWith({true});
        shell
            .processBatch(std::span(items.data(), items.size()), policy, std::move(clone_template));
    });

    auto a = std::make_shared<ScriptedClient>(1, false);
    a->connect(connector, clients.executor());
    const bool a_has_it = eventually([&] { return a->nWork() == 1; });
    const bool put_back = Gem::Courtier::Tests::eventually(
        [&] { return shell.requeuedCount() == 1; },
        std::chrono::milliseconds(3 * LEASE_MS)
    );
    auto b = std::make_shared<ScriptedClient>(1, false);
    b->connect(connector, clients.executor());
    const bool b_has_it = eventually([&] { return b->nWork() == 1; });
    a->abort();
    const bool a_gone = eventually([&] { return shell.getNActiveSessions() == 1; });
    auto c = std::make_shared<ScriptedClient>(1, false);
    c->connect(connector, clients.executor());
    const bool c_there = eventually([&] { return shell.getNActiveSessions() == 2; });
    std::this_thread::sleep_for(200ms); // C would have its item by now
    const std::size_t c_work = c->nWork();
    const std::uint64_t n_requeued = shell.requeuedCount();

    shell.stopServer();
    batch.get();
    REQUIRE(a_has_it);
    REQUIRE(put_back);
    REQUIRE(b_has_it);
    REQUIRE(a_gone);
    REQUIRE(c_there);
    CHECK(c_work == 0);
    CHECK(n_requeued == 1);
}

/******************************************************************************/
/**
 * A result whose item another server process handed out (a client that outlived its server) is
 * dropped; when its id happens to match an item this session holds, that item stays in the lease:
 * when the client is then lost, the item is put back and another client evaluates it.
 */
TEST_CASE(
    "GPeerConsumerT: a foreign result does not free the item it names from the lease",
    "[courtier][peer][shell]"
) {
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    GInProcessConnector connector{std::make_shared<GInProcessRoute>(listener)};
    IoThreads clients;
    Shell shell{listener, 2};
    shell.startServer();

    auto items = makeBatch(1);
    auto batch = std::async(std::launch::async, [&] {
        // A batch that the stop ends is filled with clones of a template, so that this is not fatal
        auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
        policy.fatal_on_none_returned = false;
        auto clone_template = std::make_unique<GFaultyContainer>(99);
        clone_template->markAsProcessedWith({true});
        shell
            .processBatch(std::span(items.data(), items.size()), policy, std::move(clone_template));
    });
    auto stranger = std::make_shared<ScriptedClient>(1, true, true);
    stranger->connect(connector, clients.executor());
    const bool dropped = eventually([&] { return shell.foreignReturnCount() == 1; });
    stranger->abort();
    const bool gone = eventually([&] { return shell.getNActiveSessions() == 0; });
    auto worker = std::make_shared<ScriptedClient>(1, true);
    worker->connect(connector, clients.executor());

    const bool done = batch.wait_for(PATIENCE) == std::future_status::ready;
    shell.stopServer(); // ends the batch if it is not done
    batch.get();
    REQUIRE(dropped);
    REQUIRE(gone);
    CHECK(done);
    CHECK(worker->nWork() == 1);
}

/******************************************************************************/
/**
 * A step of stopServer() that throws (here the listener's stop()) does not leave the session
 * threads running: they are joined and the listener's work is ended before the exception reaches
 * the caller, and the shell can be destroyed.
 */
TEST_CASE(
    "GPeerConsumerT: a stop whose listener throws still joins the session threads",
    "[courtier][peer][shell]"
) {
    auto listener =
        std::make_shared<GInProcessListener>(GInProcessListener::Options{.stopThrows = true});
    auto &budget = Gem::Common::Concurrency::threadBudget();
    const unsigned int reserved_before = budget.reserved();
    {
        Shell shell{listener, 3};
        shell.startServer();
        CHECK_THROWS(shell.stopServer());
        CHECK(listener->workEnded());
        CHECK(budget.reserved() == reserved_before);
    } // the destructor returns: no session thread is left in run()
    CHECK(listener->nStops() == 1);
}

/******************************************************************************/
/**
 * A shell that has been stopped cannot be started: startServer() throws and starts nothing, so no
 * session thread is left that nothing would stop (the destructor's stop has nothing more to do).
 * The two calls take turns when they come from different threads.
 */
TEST_CASE(
    "GPeerConsumerT: a shell that has been stopped cannot be started",
    "[courtier][peer][shell]"
) {
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    auto &budget = Gem::Common::Concurrency::threadBudget();
    const unsigned int reserved_before = budget.reserved();
    {
        Shell shell{listener, 3};
        shell.stopServer();
        CHECK_THROWS(shell.startServer());
        CHECK(budget.reserved() == reserved_before);
    } // the destructor returns
    CHECK(listener->nStops() == 0);
    CHECK_FALSE(listener->workEnded());
}

/******************************************************************************/
/**
 * A shell is started once: a second startServer() after one that succeeded is refused with an
 * exception that says so, and changes nothing -- no further threads are reserved, and the stop
 * ends the run as after a single start.
 */
TEST_CASE(
    "GPeerConsumerT: a shell that has been started cannot be started again",
    "[courtier][peer][shell]"
) {
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    auto &budget = Gem::Common::Concurrency::threadBudget();
    const unsigned int reserved_before = budget.reserved();
    {
        Shell shell{listener, 3};
        shell.startServer();
        const unsigned int reserved_started = budget.reserved();
        std::string what;
        try {
            shell.startServer();
        }
        catch(const std::exception &e) {
            what = e.what();
        }
        CHECK(what.find("started already") != std::string::npos);
        CHECK(budget.reserved() == reserved_started);
        shell.stopServer();
        CHECK(listener->nStops() == 1);
        CHECK(listener->workEnded());
        CHECK(budget.reserved() == reserved_before);
    } // the destructor's stop has nothing more to do
    CHECK(listener->nStops() == 1);
}
