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

// The client over any connector (GPeerClientT), against the server's shell (GPeerConsumerT) on the
// in-process transport: look-ahead, RESULT, the halt with an item limit, the end of the run with
// held items, the compute width, the item processor, the busy signal, and scarce work.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>

#include "GInProcessPeer.hpp"
#include "GInProcessTestSupport.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GProcessingContainerT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/GWireCodec.hpp"
#include "courtier/consumers/GNetworkedTimeoutConfig.hpp"
#include "courtier/consumers/GPeerConsumerT.hpp"
#include "courtier/transport/GPeerClientT.hpp"
#include "courtier/transport/GPeerI.hpp"

using Gem::Courtier::GCommandContainerT;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GFrameKind;
using Gem::Courtier::GWirePeerId;
using Gem::Courtier::Consumers::GListenerI;
using Gem::Courtier::Consumers::GPeerClientOptions;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerEnd;
using Gem::Courtier::Consumers::GPeerHandlers;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Tests::GInProcessConnector;
using Gem::Courtier::Tests::GInProcessListener;
using Gem::Courtier::Tests::GInProcessListenerRef;
using Gem::Courtier::Tests::GInProcessPeerWatch;
using Gem::Courtier::Tests::GInProcessRoute;
using Gem::Courtier::Tests::IoThreads;
using namespace std::chrono_literals;

namespace {

constexpr auto PATIENCE = 10s; // how long a test waits for something that is due soon
constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

using client_type = Gem::Courtier::Consumers::GPeerClientT<GFaultyContainer>;

/** @brief Waits, at most PATIENCE, for @p condition @return Whether it held in time */
template <typename condition_type>
bool eventually(condition_type condition) {
    return Gem::Courtier::Tests::eventually(condition, PATIENCE);
}

/******************************************************************************/
/**
 * What the server's ends of all connections saw: requests (PULL and RETURN) that have no answer
 * (WORK or NO_WORK) yet, per connection and at most, and the RESULTs that arrived.
 */
struct ServerCounts {
    std::atomic<std::size_t> maxUnanswered{0}; ///< The most requests without an answer, anywhere
    std::atomic<std::size_t> nResults{0};      ///< RESULT frames received
    std::atomic<std::size_t> nReturns{0};      ///< RETURN frames received
    std::atomic<GFrameKind> lastResult{GFrameKind::NONE}; ///< The kind of the last result received
    std::atomic<std::size_t> nWork{0};                    ///< WORK frames sent
    std::atomic<std::size_t> overAnswered{0}; ///< Answers sent beyond the requests received

    /** @brief Notes the unanswered requests of one connection @param n Their number */
    void unanswered(std::size_t n) {
        std::size_t seen = maxUnanswered.load();
        while(n > seen && not maxUnanswered.compare_exchange_weak(seen, n)) {
        }
    }
};

/** @brief @param bytes An encoded frame @return Its kind */
GFrameKind kindOf(const std::string &bytes) {
    GCommandContainerT<GFaultyContainer> frame{GFrameKind::NONE};
    Gem::Courtier::wireDecode(bytes, frame, nullptr, BIN);
    return frame.kind();
}

/**
 * A server's end that counts: it stands between the session and the in-process end, and notes
 * every request that arrives and every answer the session sends. Everything happens on the
 * strand of the end it wraps, so its own counts need no lock. It is owned by the handlers it
 * installs on that end and refers to it weakly, as a role does (GPeerI.hpp, rule 4).
 */
class CountingPeer final
  : public GPeerI
  , public std::enable_shared_from_this<CountingPeer> {
public:
    /** @brief @param inner The end it wraps @param counts Where it reports */
    CountingPeer(const std::shared_ptr<GPeerI> &inner, std::shared_ptr<ServerCounts> counts)
      : inner_(inner)
      , executor_(inner->executor())
      , counts_(std::move(counts)) { /* nothing */
    }

    /** @brief Installs counting handlers around @p handlers on the wrapped end */
    void start(GPeerHandlers handlers) override {
        auto inner = inner_.lock();
        if(not inner) {
            return;
        }
        auto self = this->shared_from_this();
        inner->start(
            GPeerHandlers{
                .onOpen = std::move(handlers.onOpen),
                .onMessage =
                    [self,
                     on_message = std::move(handlers.onMessage)](std::string &&message) mutable {
                        const GFrameKind kind = kindOf(message);
                        if(kind == GFrameKind::PULL || kind == GFrameKind::RETURN ||
                           kind == GFrameKind::UNREADABLE) {
                            ++self->requests_;
                            self->counts_->unanswered(self->requests_ - self->answers_);
                        }
                        if(kind == GFrameKind::RETURN) {
                            self->counts_->nReturns.fetch_add(1);
                            self->counts_->lastResult.store(kind);
                        }
                        if(kind == GFrameKind::RESULT) {
                            self->counts_->nResults.fetch_add(1);
                            self->counts_->lastResult.store(kind);
                        }
                        on_message(std::move(message));
                    },
                .onClosed = std::move(handlers.onClosed),
            }
        );
    }
    /** @brief Counts an answer and forwards @param message The message */
    void send(Gem::Courtier::Consumers::GMessage message) override {
        const GFrameKind kind = kindOf(*message);
        if(kind == GFrameKind::WORK || kind == GFrameKind::NO_WORK) {
            ++answers_;
            if(answers_ > requests_) {
                counts_->overAnswered.fetch_add(1);
            }
        }
        if(kind == GFrameKind::WORK) {
            counts_->nWork.fetch_add(1);
        }
        if(auto inner = inner_.lock()) {
            inner->send(std::move(message));
        }
    }
    /** @brief Forwards @param how See GPeerI */
    void close(GPeerClose how) override {
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
    /** @brief @return The wrapped end's executor, kept from start() on: GPeerI::executor() always
     *  answers, also once the wrapped end has gone (a wake-up may post to it then) */
    [[nodiscard]] boost::asio::any_io_executor executor() override {
        return executor_;
    }

private:
    std::weak_ptr<GPeerI> inner_;          ///< The wrapped end
    boost::asio::any_io_executor executor_; ///< The wrapped end's executor, kept
    std::shared_ptr<ServerCounts> counts_; ///< Where it reports
    std::size_t requests_ = 0;             ///< Requests received (on the strand)
    std::size_t answers_ = 0;              ///< Answers sent (on the strand)
};

/** @brief A listener that wraps every server end in a CountingPeer; otherwise the in-process one */
class CountingListener final : public GListenerI {
public:
    /** @brief @param listener The in-process listener @param counts Where the ends report */
    CountingListener(
        std::shared_ptr<GInProcessListener> listener,
        std::shared_ptr<ServerCounts> counts
    )
      : listener_(std::move(listener))
      , counts_(std::move(counts)) { /* nothing */
    }
    /** @brief Wraps the ends @param executor See GListenerI @param on_peer See GListenerI */
    void start(boost::asio::any_io_executor executor, on_peer_type on_peer) override {
        listener_->start(
            std::move(executor),
            [counts = counts_,
             on_peer =
                 std::move(on_peer)](const std::shared_ptr<GPeerI> &peer, GWirePeerId id) mutable {
                on_peer(std::make_shared<CountingPeer>(peer, counts), id);
            }
        );
    }
    /** @brief Forwards */
    void stop() override {
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
    /** @brief Forwards @return The in-process listener's answer */
    [[nodiscard]] bool detectsLoss() const override {
        return listener_->detectsLoss();
    }

private:
    std::shared_ptr<GInProcessListener> listener_; ///< The in-process listener
    std::shared_ptr<ServerCounts> counts_;         ///< Where the ends report
};

/******************************************************************************/
/** @brief Evaluations as a client's item processor sees them: per item, at the same time, in all */
struct Evaluations {
    std::mutex mutex;
    std::map<std::size_t, std::size_t> perItem; ///< Item number -> how often it was evaluated
    std::size_t now = 0;                        ///< Evaluated at this moment
    std::size_t most = 0;                       ///< The most at the same time
    std::atomic<std::size_t> total{0};          ///< All evaluations

    /** @brief @param item The item's number @return How often it was evaluated */
    std::size_t of(std::size_t item) {
        const std::scoped_lock lock(mutex);
        auto it = perItem.find(item);
        return it == perItem.end() ? 0 : it->second;
    }
    /** @brief @return The most evaluations at the same time */
    std::size_t mostAtOnce() {
        const std::scoped_lock lock(mutex);
        return most;
    }
};

/**
 * @brief An item processor that counts, takes @p duration, and marks the item processed.
 * @param evaluations Where it counts @param duration How long an evaluation takes
 */
Gem::Courtier::GItemProcessorT<GFaultyContainer> countingProcessor(
    const std::shared_ptr<Evaluations> &evaluations,
    std::chrono::milliseconds duration
) {
    return [evaluations, duration](GFaultyContainer &item) {
        {
            const std::scoped_lock lock(evaluations->mutex);
            ++evaluations->perItem[item.get_stored_number()];
            ++evaluations->now;
            evaluations->most = std::max(evaluations->most, evaluations->now);
        }
        evaluations->total.fetch_add(1);
        std::this_thread::sleep_for(duration);
        {
            const std::scoped_lock lock(evaluations->mutex);
            --evaluations->now;
        }
        item.markAsProcessedWith({true});
    };
}

/******************************************************************************/
/** @brief A server's shell with a counting in-process listener, and clients that run in threads */
class World {
public:
    /** @brief @param options What the in-process pairs are made with */
    explicit World(GInProcessListener::Options options = {})
      : listener_(std::make_shared<GInProcessListener>(options))
      , counts_(std::make_shared<ServerCounts>())
      , shell_("Shell", std::make_unique<CountingListener>(listener_, counts_), 2, BIN) {
        Gem::Courtier::GNetworkedTimeoutConfig config;
        config.treatment = "wait_indefinitely";
        shell_.applyTimeoutConfig(config);
        shell_.startServer();
    }

    /** @brief Stops the server and joins the clients */
    ~World() {
        shell_.stopServer();
        threads_.clear();
    }
    World(const World &) = delete;
    World(World &&) = delete;
    World &operator=(const World &) = delete;
    World &operator=(World &&) = delete;

    /**
     * @brief Makes a client and runs it in a thread of its own.
     * @param options Its settings
     * @param evaluations Where its item processor counts
     * @param duration How long an evaluation takes
     * @param watch Where its connector records the client end (may be empty)
     * @param setup Done with the client before it runs
     * @return The client
     */
    template <typename setup_type>
    std::shared_ptr<client_type> client(
        GPeerClientOptions options,
        const std::shared_ptr<Evaluations> &evaluations,
        std::chrono::milliseconds duration,
        const std::shared_ptr<GInProcessPeerWatch> &watch,
        setup_type setup
    ) {
        auto client = std::make_shared<client_type>(
            std::make_unique<GInProcessConnector>(route_, true, watch),
            std::move(options)
        );
        client->setItemProcessor(countingProcessor(evaluations, duration));
        setup(*client);
        auto &done = done_.emplace_back(std::make_shared<std::atomic<bool>>(false));
        threads_.emplace_back([client, done] {
            client->run();
            done->store(true);
        });
        return client;
    }

    /** @brief @return The listener of the shell */
    [[nodiscard]] const std::shared_ptr<GInProcessListener> &listener() const {
        return listener_;
    }
    /** @brief @return Where the clients connect to */
    [[nodiscard]] GInProcessRoute &route() {
        return *route_;
    }
    /** @brief @return The shell */
    Gem::Courtier::GPeerConsumerT<GFaultyContainer> &shell() {
        return shell_;
    }
    /** @brief @return What the server's ends counted */
    ServerCounts &counts() {
        return *counts_;
    }
    /** @brief @return What the server's ends counted, to be read after this world has gone */
    [[nodiscard]] std::shared_ptr<ServerCounts> sharedCounts() const {
        return counts_;
    }
    /** @brief @return Whether every client's run() has returned */
    bool clientsDone() const {
        return std::ranges::all_of(done_, [](const auto &d) { return d->load(); });
    }

    /**
     * @brief Submits @p items and waits for the batch, at most PATIENCE; stops the server if it
     * is not done by then (which ends the batch).
     * @return Whether the batch was done in time
     */
    bool runBatch(std::vector<std::unique_ptr<GFaultyContainer>> &items) {
        auto batch = std::async(std::launch::async, [&] {
            shell_.processBatch(
                std::span(items.data(), items.size()),
                Gem::Courtier::GSubmissionPolicy::clone_on_partial_return()
            );
        });
        const bool done = batch.wait_for(PATIENCE) == std::future_status::ready;
        if(not done) {
            shell_.stopServer();
        }
        batch.get();
        return done;
    }

private:
    std::shared_ptr<GInProcessListener> listener_;
    /// Where the clients' connectors connect to (the listener, until a test changes it)
    std::shared_ptr<GInProcessRoute> route_ = std::make_shared<GInProcessRoute>(listener_);
    std::shared_ptr<ServerCounts> counts_;
    Gem::Courtier::GPeerConsumerT<GFaultyContainer> shell_;
    std::vector<std::shared_ptr<std::atomic<bool>>> done_;
    std::vector<std::jthread> threads_; ///< Joined first, while the shell still exists
};

/** @brief @param n How many @param first The first number @return n items, numbered */
std::vector<std::unique_ptr<GFaultyContainer>> makeBatch(std::size_t n, std::size_t first = 0) {
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        items.push_back(std::make_unique<GFaultyContainer>(first + i));
    }
    return items;
}

/** @brief A client set up without anything more */
constexpr auto AS_IS = [](client_type &) {};

/** @brief @return The options of a look-ahead client: depth 2, width 1 */
GPeerClientOptions lookAhead() {
    return GPeerClientOptions{
        .name = "look-ahead client",
        .mode = BIN,
        .prefetchDepth = 2,
        .computeWidth = 1,
        .lookAhead = true
    };
}

} /* anonymous namespace */

/******************************************************************************/
/** With look-ahead a client never has more than one request without an answer. */
TEST_CASE(
    "GPeerClientT: with look-ahead at most one request is unanswered",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    world.client(lookAhead(), evaluations, 5ms, nullptr, AS_IS);
    auto items = makeBatch(20);
    REQUIRE(world.runBatch(items));
    CHECK(evaluations->total.load() == 20);
    CHECK(world.counts().maxUnanswered.load() == 1);
    CHECK(evaluations->mostAtOnce() == 1);
}

/******************************************************************************/
/**
 * A result that is finished while the client's request is unanswered (the server has no item for
 * it) asks for nothing: it travels as a RESULT.
 */
TEST_CASE(
    "GPeerClientT: a result finished while a request is unanswered is a RESULT",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    world.client(lookAhead(), evaluations, 50ms, nullptr, AS_IS);
    auto items = makeBatch(1);
    REQUIRE(world.runBatch(items));
    CHECK(world.counts().nResults.load() == 1);
    CHECK(world.counts().nReturns.load() == 0);
    CHECK(world.counts().maxUnanswered.load() == 1);
}

/******************************************************************************/
/**
 * A client with an item limit sends its last result -- as a RESULT, it asks for nothing more -- and
 * closes; the server has every result. The client (depth 1, no look-ahead) expects to ask with its
 * results, so its last one is encoded as a RETURN and must be encoded once more.
 */
TEST_CASE(
    "GPeerClientT: a client with an item limit sends its last result and closes",
    "[courtier][peer][client]"
) {
    constexpr std::uint32_t LIMIT = 3;
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "limited client", .mode = BIN, .prefetchDepth = 1},
        evaluations,
        20ms,
        nullptr,
        [](client_type &c) { c.setProcessMax(LIMIT); }
    );
    auto items = makeBatch(LIMIT);
    REQUIRE(world.runBatch(items));
    REQUIRE(eventually([&] { return world.clientsDone(); }));
    CHECK(evaluations->total.load() == LIMIT);
    CHECK(world.counts().nReturns.load() == LIMIT - 1);
    CHECK(world.counts().nResults.load() == 1);
    CHECK(world.counts().lastResult.load() == GFrameKind::RESULT);
    CHECK(client->connectionEnd() == GPeerEnd::CLOSED_HERE);
}

/******************************************************************************/
/**
 * When the server announces the end of the run, a client drops the items it holds and has not
 * begun to evaluate; the one being evaluated runs to its end.
 */
TEST_CASE(
    "GPeerClientT: after RUN_ENDED a held item is not evaluated",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto items = makeBatch(2);
    {
        World world;
        auto client = world.client(
            GPeerClientOptions{
                .name = "holding client",
                .mode = BIN,
                .prefetchDepth = 2,
                .computeWidth = 1
            },
            evaluations,
            500ms,
            nullptr,
            AS_IS
        );
        // The batch ends with the run, before a result can come back: its slots are filled with
        // clones of a template, so that this is not fatal here.
        auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
        policy.fatal_on_none_returned = false;
        auto clone_template = std::make_unique<GFaultyContainer>(99);
        clone_template->markAsProcessedWith({true});
        auto batch = std::async(std::launch::async, [&] {
            world.shell().processBatch(
                std::span(items.data(), items.size()),
                policy,
                std::move(clone_template)
            );
        });
        // One item is evaluated, the other held (or still being decoded): the run ends now.
        REQUIRE(eventually([&] { return evaluations->total.load() == 1; }));
        REQUIRE(eventually([&] { return world.counts().nWork.load() == 2; }));
        std::this_thread::sleep_for(50ms);
        world.shell().stopServer();
        batch.get();
        REQUIRE(eventually([&] { return world.clientsDone(); }));
    }
    CHECK(evaluations->total.load() == 1);
    CHECK(evaluations->of(0) + evaluations->of(1) == 1);
}

/******************************************************************************/
/** A client evaluates no more items at once than its compute width allows, and uses its item
 *  processor for every one. */
TEST_CASE(
    "GPeerClientT: the compute width bounds the evaluations and the item processor does them",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    world.client(
        GPeerClientOptions{
            .name = "wide client",
            .mode = BIN,
            .prefetchDepth = 4,
            .computeWidth = 2
        },
        evaluations,
        10ms,
        nullptr,
        AS_IS
    );
    auto items = makeBatch(24);
    REQUIRE(world.runBatch(items));
    CHECK(evaluations->total.load() == 24);
    CHECK(evaluations->mostAtOnce() == 2);
    std::size_t n_by_processor = 0;
    for(std::size_t i = 0; i < items.size(); ++i) {
        n_by_processor += evaluations->of(i);
    }
    CHECK(n_by_processor == 24);
}

/******************************************************************************/
/** A client tells its peer that it is busy while it evaluates an item, and when it is not. */
TEST_CASE(
    "GPeerClientT: the client tells its peer while it evaluates",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    World world;
    world.client(lookAhead(), evaluations, 400ms, watch, AS_IS);
    // The client's end is looked at only briefly: it must not outlive the client's io context.
    const auto told = [&watch](bool busy) {
        const auto end = watch->last();
        return end != nullptr && end->ownerBusy() == busy;
    };
    REQUIRE(eventually([&] { return watch->last() != nullptr; }));
    CHECK(told(false));

    auto items = makeBatch(1);
    auto batch = std::async(std::launch::async, [&] { return world.runBatch(items); });
    REQUIRE(eventually([&] { return evaluations->total.load() == 1; }));
    // The flag is set on the io thread after the evaluation has been handed to the pool, so it may
    // come a little after the evaluation has begun; the evaluation lasts 400 ms, and the flag is
    // cleared only after it.
    CHECK(eventually([&] { return told(true); }));
    REQUIRE(batch.get());
    REQUIRE(eventually([&] { return told(false); }));
}

/******************************************************************************/
/** Scarce work: two items, four look-ahead clients, each waiting with one request. Each client has
 *  at most one request unanswered, so a woken session hands its client one item, and each item is
 *  evaluated once. Which clients get the items is not fixed: a wake-up does not hand an item over,
 *  it makes a parked session ask again, and a client that got the first item asks at once (its
 *  look-ahead PULL); its session may take the second item before the other woken session has
 *  asked. */
TEST_CASE(
    "GPeerClientT: with look-ahead scarce work is evaluated once and one request at a time",
    "[courtier][peer][client]"
) {
    World world;
    std::vector<std::shared_ptr<Evaluations>> evaluations;
    for(int c = 0; c < 4; ++c) {
        evaluations.push_back(std::make_shared<Evaluations>());
        world.client(lookAhead(), evaluations.back(), 20ms, nullptr, AS_IS);
    }
    REQUIRE(eventually([&] { return world.shell().getNActiveSessions() == 4; }));
    std::this_thread::sleep_for(100ms); // every client has asked
    auto items = makeBatch(2);
    REQUIRE(world.runBatch(items));
    std::size_t n_evaluated = 0;
    for(std::size_t i = 0; i < items.size(); ++i) {
        for(const auto &e : evaluations) {
            n_evaluated += e->of(i);
        }
    }
    CHECK(n_evaluated == 2);
    CHECK(world.counts().nWork.load() == 2);
    CHECK(world.counts().maxUnanswered.load() == 1);
}

/******************************************************************************/
// A result whose RETURN may no longer ask by the time its bytes reach the io thread, because a
// refill PULL went out meanwhile (from the back-off timer after a NO_WORK). The server's end is a
// script here, so that it can answer NO_WORK, which the server's shell never does for a request it
// can park.

namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> noWorkSent{false}; ///< The scripted server has answered with NO_WORK
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// How long the client takes to encode its evaluated item: longer than the longest back-off after
/// a NO_WORK (GCLIENTNOWORKPAUSEUPPERBOUNDARYMSEC)
constexpr auto SLOW_ENCODE = 800ms;
/// How long an evaluation waits, once the client has counted the NO_WORK, before it ends: the
/// client has published that it may ask again by then, and its refill PULL comes after the
/// shortest back-off (GCLIENTNOWORKPAUSELOWERBOUNDARYMSEC, 50 ms) at the earliest
constexpr auto AFTER_NO_WORK = 5ms;

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
/// How often an evaluated GSlowReturnItem was encoded (the second encoding is the point of the case)
std::atomic<std::size_t> slowEncodes{0};
/// The NO_WORK count of the client under test; set before it runs
std::function<std::uint64_t()> noWorkSeenByClient;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/**
 * A work item that is encoded slowly once it has been evaluated (on its way back), and whose
 * evaluation ends only after the scripted server has sent its NO_WORK.
 */
class GSlowReturnItem final : public Gem::Courtier::GProcessingContainerT<GSlowReturnItem, bool> {
    friend struct Gem::Weft::access;

    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<
            Gem::Courtier::GProcessingContainerT<GSlowReturnItem, bool>>(
            ar,
            "GProcessingContainerT_GSlowReturnItem",
            *this
        );
        if constexpr(Gem::Common::archive_is_saving_v<archive_type>) {
            if(this->is_processed()) {
                slowEncodes.fetch_add(1);
                std::this_thread::sleep_for(SLOW_ENCODE);
            }
        }
    }

public:
    /** @brief The default constructor (also for loading) */
    GSlowReturnItem()
      : Gem::Courtier::GProcessingContainerT<GSlowReturnItem, bool>(1) { /* nothing */
    }

private:
    /** @brief Ends after the client has counted the NO_WORK */
    void process_([[maybe_unused]] const std::vector<bool> &res_vec) override {
        while(not noWorkSent.load() || noWorkSeenByClient() == 0) {
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(AFTER_NO_WORK);
    }
};

using slow_client_type = Gem::Courtier::Consumers::GPeerClientT<GSlowReturnItem>;

/**
 * The server's end as a script: the first request is answered with an item, the second with
 * NO_WORK, every later one is kept waiting. It counts the requests without an answer and the
 * results.
 */
class ScriptedServer final : public std::enable_shared_from_this<ScriptedServer> {
public:
    /** @brief Starts @p peer with this script as its role @param peer The server's end */
    void start(const std::shared_ptr<GPeerI> &peer) {
        peer_ = peer;
        auto self = this->shared_from_this();
        peer->start(
            GPeerHandlers{
                .onOpen = {},
                .onMessage = [self](std::string &&message) { self->received_(std::move(message)); },
                .onClosed = [](GPeerEnd, GPeerClose, std::string_view) {},
            }
        );
    }
    /** @brief Ends the run for the client */
    void endRun() {
        if(auto peer = peer_.lock()) {
            peer->close(GPeerClose::RUN_ENDED);
        }
    }

    std::atomic<std::size_t> maxUnanswered{0}; ///< The most requests without an answer
    std::atomic<std::size_t> nReturns{0};      ///< RETURN frames received
    std::atomic<std::size_t> nResults{0};      ///< RESULT frames received

private:
    /** @brief On the end's strand @param message A message of the client */
    void received_(std::string &&message) {
        const std::string bytes = std::move(message);
        GCommandContainerT<GSlowReturnItem> frame{GFrameKind::NONE};
        Gem::Courtier::wireDecode(bytes, frame, nullptr, BIN);
        const GFrameKind kind = frame.kind();
        if(kind == GFrameKind::RETURN) {
            nReturns.fetch_add(1);
        }
        if(kind == GFrameKind::RESULT) {
            nResults.fetch_add(1);
        }
        if(kind != GFrameKind::PULL && kind != GFrameKind::RETURN) {
            return;
        }
        ++requests_;
        maxUnanswered.store(std::max(maxUnanswered.load(), requests_ - answers_));
        auto peer = peer_.lock();
        if(not peer || requests_ > 2) {
            return; // kept waiting
        }
        ++answers_;
        if(requests_ == 1) {
            auto item = std::make_unique<GSlowReturnItem>();
            item->mark_as_due_for_processing();
            const GCommandContainerT<GSlowReturnItem> work{GFrameKind::WORK, std::move(item)};
            peer->send(
                Gem::Courtier::Consumers::makeMessage(Gem::Courtier::wireEncode(work, nullptr, BIN))
            );
            return;
        }
        const GCommandContainerT<GSlowReturnItem> no_work{GFrameKind::NO_WORK};
        peer->send(
            Gem::Courtier::Consumers::makeMessage(Gem::Courtier::wireEncode(no_work, nullptr, BIN))
        );
        noWorkSent.store(true);
    }

    std::weak_ptr<GPeerI> peer_; ///< The server's end
    std::size_t requests_ = 0;   ///< Requests received (on the strand)
    std::size_t answers_ = 0;    ///< Answers sent (on the strand)
};

} /* anonymous namespace */

/**
 * With look-ahead, a NO_WORK leaves the client without a request; its result is then encoded as a
 * RETURN. While that encoding takes its time, the back-off timer sends a refill PULL. The RETURN
 * may no longer ask: it is encoded once more and arrives as a RESULT, so that the server never has
 * two requests of this client without an answer.
 */
TEST_CASE(
    "GPeerClientT: a result whose request a refill PULL took over arrives as a RESULT",
    "[courtier][peer][client]"
) {
    noWorkSent.store(false);
    auto server = std::make_shared<ScriptedServer>();
    {
        IoThreads io{2};
        auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
        listener->start(io.executor(), [server](const std::shared_ptr<GPeerI> &peer, GWirePeerId) {
            server->start(peer);
        });
        auto client = std::make_shared<slow_client_type>(
            std::make_unique<GInProcessConnector>(std::make_shared<GInProcessRoute>(listener)),
            GPeerClientOptions{
                .name = "slowly returning client",
                .mode = BIN,
                .prefetchDepth = 2,
                .computeWidth = 2,
                .lookAhead = true
            }
        );
        slowEncodes.store(0);
        // a plain pointer: the evaluation ends before the client does (run() returns after it)
        noWorkSeenByClient = [c = client.get()] { return c->noWorkCount(); };
        std::jthread runner([client] { client->run(); });
        REQUIRE(eventually([&] { return server->nResults.load() + server->nReturns.load() > 0; }));
        std::this_thread::sleep_for(100ms); // anything that would follow the result
        CHECK(server->nResults.load() == 1);
        CHECK(server->nReturns.load() == 0);
        CHECK(server->maxUnanswered.load() == 1);
        CHECK(slowEncodes.load() == 2); // encoded as a RETURN first, then once more as a RESULT
        server->endRun();
        runner.join();
        listener->stop();
        listener->endWork();
    }
}

/******************************************************************************/
// The decoding of the item a look-ahead client holds ahead. A watched item says when it is decoded
// and can be made slow to decode; its evaluation can wait for the second item to be decoded.

namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<std::size_t> decodesStarted{0};   ///< Work items whose decoding has begun
std::atomic<std::size_t> decodesDone{0};      ///< Work items decoded
std::atomic<int> decodeMs{0};                 ///< How long decoding a work item takes
std::atomic<bool> firstWaitsForSecond{false}; ///< The first evaluation waits for the second decode
std::atomic<bool> decodedMeanwhile{
    false
}; ///< The second item was decoded during the first evaluation
std::atomic<std::size_t> evaluatingNow{0};  ///< Evaluations in progress
std::atomic<std::size_t> evaluatingMost{0}; ///< The most evaluations at the same time
std::atomic<std::size_t> nEvaluated{0};     ///< Evaluations begun
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/** @brief Resets what the watched items record @param decode_ms How long a decode takes
 *  @param first_waits Whether the first evaluation waits for the second decode */
void resetWatch(int decode_ms, bool first_waits) {
    decodesStarted.store(0);
    decodesDone.store(0);
    decodeMs.store(decode_ms);
    firstWaitsForSecond.store(first_waits);
    decodedMeanwhile.store(false);
    evaluatingNow.store(0);
    evaluatingMost.store(0);
    nEvaluated.store(0);
}

/** @brief A work item that records its decoding on the client and its evaluations */
class GWatchedItem final : public Gem::Courtier::GProcessingContainerT<GWatchedItem, bool> {
    friend struct Gem::Weft::access;

    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<Gem::Courtier::GProcessingContainerT<GWatchedItem, bool>>(
            ar,
            "GProcessingContainerT_GWatchedItem",
            *this
        );
        if constexpr(not Gem::Common::archive_is_saving_v<archive_type>) {
            if(this->is_due_for_processing()) { // a work item, on the client
                decodesStarted.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(decodeMs.load()));
                decodesDone.fetch_add(1);
            }
        }
    }

public:
    /** @brief The default constructor (also for loading) */
    GWatchedItem()
      : Gem::Courtier::GProcessingContainerT<GWatchedItem, bool>(1) { /* nothing */
    }

private:
    /** @brief Counts; the first evaluation may wait for the second item to be decoded */
    void process_([[maybe_unused]] const std::vector<bool> &res_vec) override {
        const std::size_t now = evaluatingNow.fetch_add(1) + 1;
        std::size_t most = evaluatingMost.load();
        while(now > most && not evaluatingMost.compare_exchange_weak(most, now)) {
        }
        if(nEvaluated.fetch_add(1) == 0 && firstWaitsForSecond.load()) {
            decodedMeanwhile.store(
                Gem::Courtier::Tests::eventually(
                    [] { return decodesDone.load() >= 2; },
                    std::chrono::milliseconds(1000)
                )
            );
        }
        else {
            std::this_thread::sleep_for(50ms);
        }
        evaluatingNow.fetch_sub(1);
    }
};

/** @brief @return The options of the look-ahead client of a WatchWorld */
GPeerClientOptions watchedClient() {
    return GPeerClientOptions{
        .name = "watched client",
        .mode = BIN,
        .prefetchDepth = 2,
        .computeWidth = 1,
        .lookAhead = true
    };
}

/** @brief A server's shell for items of the given type, and one client
 *  @tparam item_type The work item */
template <typename item_type>
class WatchWorld {
public:
    /** @brief @param options The settings of the client */
    explicit WatchWorld(GPeerClientOptions options = watchedClient())
      : listener_(std::make_shared<GInProcessListener>(GInProcessListener::Options{}))
      , shell_("Shell", std::make_unique<GInProcessListenerRef>(listener_), 2, BIN)
      , client_(
            std::make_shared<Gem::Courtier::Consumers::GPeerClientT<item_type>>(
                std::make_unique<GInProcessConnector>(std::make_shared<GInProcessRoute>(listener_)),
                std::move(options)
            )
        ) {
        Gem::Courtier::GNetworkedTimeoutConfig config;
        config.treatment = "wait_indefinitely";
        shell_.applyTimeoutConfig(config);
        shell_.startServer();
        runner_ = std::jthread([client = client_] { client->run(); });
    }
    /** @brief Stops the server and joins the client */
    ~WatchWorld() {
        shell_.stopServer();
        runner_ = std::jthread{};
    }
    WatchWorld(const WatchWorld &) = delete;
    WatchWorld(WatchWorld &&) = delete;
    WatchWorld &operator=(const WatchWorld &) = delete;
    WatchWorld &operator=(WatchWorld &&) = delete;

    /** @brief @return The shell */
    Gem::Courtier::GPeerConsumerT<item_type> &shell() {
        return shell_;
    }

    /** @brief Submits @p n items; the batch may end with the run (its slots are then filled with
     *  clones of a template) @param n How many @return The batch, running */
    std::future<void> submit(std::size_t n) {
        items_.clear();
        for(std::size_t i = 0; i < n; ++i) {
            items_.push_back(std::make_unique<item_type>());
        }
        return std::async(std::launch::async, [this] {
            auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
            policy.fatal_on_none_returned = false;
            auto clone_template = std::make_unique<item_type>();
            clone_template->markAsProcessedWith({true});
            shell_.processBatch(
                std::span(items_.data(), items_.size()),
                policy,
                std::move(clone_template)
            );
        });
    }

private:
    std::shared_ptr<GInProcessListener> listener_;
    Gem::Courtier::GPeerConsumerT<item_type> shell_;
    std::shared_ptr<Gem::Courtier::Consumers::GPeerClientT<item_type>> client_;
    std::vector<std::unique_ptr<item_type>> items_;
    std::jthread runner_;
};

} /* anonymous namespace */

/******************************************************************************/
/**
 * A look-ahead client (compute width 1, depth 2) decodes the item it holds ahead while it
 * evaluates the other one, so that the item is ready when the evaluation ends; it still evaluates
 * one item at a time.
 */
TEST_CASE(
    "GPeerClientT: with look-ahead the next item is decoded while one is evaluated",
    "[courtier][peer][client]"
) {
    resetWatch(0, true);
    {
        WatchWorld<GWatchedItem> world;
        auto batch = world.submit(2);
        REQUIRE(batch.wait_for(PATIENCE) == std::future_status::ready);
        batch.get();
    }
    CHECK(decodedMeanwhile.load());
    CHECK(nEvaluated.load() == 2);
    CHECK(evaluatingMost.load() == 1);
}

/******************************************************************************/
/**
 * When the server announces the end of the run while an item is still being decoded, that item is
 * not evaluated once it is decoded.
 */
TEST_CASE(
    "GPeerClientT: after RUN_ENDED an item still being decoded is not evaluated",
    "[courtier][peer][client]"
) {
    resetWatch(500, false);
    {
        WatchWorld<GWatchedItem> world;
        auto batch = world.submit(2);
        // The first item is decoded and evaluated; the second is being decoded: the run ends now.
        REQUIRE(eventually([] { return decodesStarted.load() == 2; }));
        REQUIRE(decodesDone.load() == 1);
        world.shell().stopServer();
        batch.get();
    }
    CHECK(decodesDone.load() == 2);
    CHECK(nEvaluated.load() == 1);
}

/******************************************************************************/
// A work item that carries a blob (a layout, as a genome does) and can be made to fail its first
// load on the client before the blob is read.

namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> poisonedLoadsLeft{0}; ///< How many loads of a poisoned item still fail
std::atomic<int> failedLoads{0};       ///< Loads of a work item that failed, for whatever reason
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/**
 * A work item with a blob that travels send-once (by value the first time a client gets it, by id
 * afterwards), as GGenome's layout does. A poisoned item fails to load, before its blob is read,
 * as long as poisonedLoadsLeft says so.
 */
class GLayoutItem final : public Gem::Courtier::GProcessingContainerT<GLayoutItem, bool> {
    friend struct Gem::Weft::access;

    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        Gem::Common::archive_named_base<Gem::Courtier::GProcessingContainerT<GLayoutItem, bool>>(
            ar,
            "GProcessingContainerT_GLayoutItem",
            *this
        );
        archive_named(ar, "poisoned_", poisoned_);
        if constexpr(not Gem::Common::archive_is_saving_v<archive_type>) {
            if(poisoned_ && poisonedLoadsLeft.fetch_sub(1) > 0) {
                failedLoads.fetch_add(1);
                throw std::runtime_error("this test makes the first load of the item fail");
            }
        }

        const auto *ctx = Gem::Courtier::GWireSerializationScope::current();
        const Gem::Courtier::GWireBlobId id{7, 7};
        bool interned = false;
        bool present = true;
        if constexpr(Gem::Common::archive_is_saving_v<archive_type>) {
            interned =
                ctx != nullptr && ctx->enabled && ctx->may_intern_blobs && ctx->registry != nullptr;
            if(interned) {
                if(not ctx->registry->has(id)) {
                    ctx->registry->put(id, layout_);
                }
                present = not ctx->registry->peerHasBlob(ctx->peer, id);
                if(present) {
                    ctx->registry->markPeerHasBlob(ctx->peer, id);
                }
            }
        }
        archive_named(ar, "interned", interned);
        archive_named(ar, "present", present);
        if(present) {
            archive_named(ar, "layout_", layout_);
            if constexpr(not Gem::Common::archive_is_saving_v<archive_type>) {
                if(interned && ctx != nullptr && ctx->registry != nullptr) {
                    ctx->registry->put(id, layout_);
                }
            }
            return;
        }
        if constexpr(not Gem::Common::archive_is_saving_v<archive_type>) {
            if(ctx == nullptr || ctx->registry == nullptr ||
               not ctx->registry->tryGet(id, layout_)) {
                failedLoads.fetch_add(1);
                throw std::runtime_error(
                    "the layout is referenced by an id the client does not know"
                );
            }
        }
    }

public:
    /** @brief The default constructor (also for loading) */
    GLayoutItem()
      : Gem::Courtier::GProcessingContainerT<GLayoutItem, bool>(1) { /* nothing */
    }

private:
    /** @brief Nothing to compute */
    void process_([[maybe_unused]] const std::vector<bool> &res_vec) override { /* nothing */
    }

    bool poisoned_ = true; ///< Whether a load of this item may fail (see poisonedLoadsLeft)
    std::string layout_ = "layout"; ///< The blob
};

} /* anonymous namespace */

/******************************************************************************/
/**
 * A client that cannot load an item tells the server (UNREADABLE) and stays. The server puts the
 * item back at once and sends the blob in full again: the item's blob never reached the client, and
 * an item that referred to it by id could not be loaded either. Every item is processed; exactly one
 * load failed.
 */
TEST_CASE(
    "GPeerClientT: an item the client cannot load comes back and the next brings its blob",
    "[courtier][peer][client]"
) {
    poisonedLoadsLeft.store(1);
    failedLoads.store(0);
    bool done = false;
    {
        WatchWorld<GLayoutItem> world(
            GPeerClientOptions{.name = "layout client", .mode = BIN, .prefetchDepth = 1}
        );
        auto batch = world.submit(3);
        done = batch.wait_for(PATIENCE) == std::future_status::ready;
        if(not done) {
            world.shell().stopServer();
        }
        batch.get();
    }
    CHECK(done);
    CHECK(failedLoads.load() == 1);
}

/******************************************************************************/
// Items that cannot be encoded: by the server on their way out, or by the client on their way back.

namespace {

/** @brief Arms save faults for the length of a test, and disarms them at its end */
struct ArmedSaveFaults {
    ArmedSaveFaults() {
        GFaultyContainer::armSaveFaults(std::numeric_limits<std::size_t>::max());
    }
    ~ArmedSaveFaults() {
        GFaultyContainer::armSaveFaults(0);
    }
    ArmedSaveFaults(const ArmedSaveFaults &) = delete;
    ArmedSaveFaults(ArmedSaveFaults &&) = delete;
    ArmedSaveFaults &operator=(const ArmedSaveFaults &) = delete;
    ArmedSaveFaults &operator=(ArmedSaveFaults &&) = delete;
};

/** @brief @param mode How item 0 fails @return Item 0, which fails so, and item 1, which does not */
std::vector<std::unique_ptr<GFaultyContainer>> oneBadOneGood(Gem::Courtier::fault_mode mode) {
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(0, mode));
    items.push_back(std::make_unique<GFaultyContainer>(1));
    return items;
}

} /* anonymous namespace */

/**
 * An item the server cannot encode ends as failed (the policy then fills its slot with a clone of
 * the good one); it is not put back and served again. The client's other item is evaluated once.
 */
TEST_CASE(
    "GPeerClientT: an item the server cannot encode ends as failed",
    "[courtier][peer][client]"
) {
    const ArmedSaveFaults armed;
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    world.client(
        GPeerClientOptions{.name = "client", .mode = BIN, .prefetchDepth = 2},
        evaluations,
        5ms,
        nullptr,
        AS_IS
    );
    auto items = oneBadOneGood(Gem::Courtier::fault_mode::THROW_SAVING);
    REQUIRE(world.runBatch(items));
    CHECK(evaluations->of(0) == 0);
    CHECK(evaluations->of(1) == 1);
    CHECK(world.counts().nWork.load() == 1);
    CHECK(world.shell().requeuedCount() == 0);
    REQUIRE(items[0] != nullptr);
    CHECK(items[0]->get_stored_number() == 1); // the failed slot holds a clone of the good item
}

/**
 * A result the client cannot encode is returned as failed, as an evaluation that threw is: the
 * client stays, the item is not served again, and the slot ends as failed.
 */
TEST_CASE(
    "GPeerClientT: a result the client cannot encode is returned as failed",
    "[courtier][peer][client]"
) {
    const ArmedSaveFaults armed;
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "client", .mode = BIN, .prefetchDepth = 1},
        evaluations,
        5ms,
        nullptr,
        AS_IS
    );
    auto items = oneBadOneGood(Gem::Courtier::fault_mode::THROW_SAVING_RESULT);
    REQUIRE(world.runBatch(items));
    CHECK(evaluations->of(0) == 1);
    CHECK(evaluations->of(1) == 1);
    CHECK(world.counts().nWork.load() == 2);
    CHECK(world.shell().requeuedCount() == 0);
    CHECK_FALSE(world.clientsDone());
    REQUIRE(items[0] != nullptr);
    CHECK(items[0]->get_stored_number() == 1);
}

/******************************************************************************/
/**
 * A client that reaches its item limit starts no evaluation after it, sends the results of those
 * that run as they finish, and closes when none runs: nothing it evaluated is put back. Depth 4,
 * width 2, limit 2: two items are evaluated and two held; the first result lets a held item start,
 * the second reaches the limit and the other held item is dropped; the third result is sent before
 * the close. A second client then evaluates the rest; no item is evaluated twice.
 */
TEST_CASE(
    "GPeerClientT: at its item limit a client starts nothing new and sends what it evaluates",
    "[courtier][peer][client]"
) {
    auto limited = std::make_shared<Evaluations>();
    auto other = std::make_shared<Evaluations>();
    World world;
    world.client(
        GPeerClientOptions{
            .name = "limited client",
            .mode = BIN,
            .prefetchDepth = 4,
            .computeWidth = 2
        },
        limited,
        300ms,
        nullptr,
        [](client_type &c) { c.setProcessMax(2); }
    );
    auto items = makeBatch(8);
    auto batch = std::async(std::launch::async, [&] { return world.runBatch(items); });
    const bool limited_done = eventually([&] { return world.clientsDone(); });
    const std::size_t n_results = world.counts().nResults.load() + world.counts().nReturns.load();
    world.client(GPeerClientOptions{.name = "other client", .mode = BIN}, other, 5ms, nullptr, AS_IS);
    REQUIRE(batch.get());
    REQUIRE(limited_done);
    CHECK(limited->total.load() == 3);
    CHECK(n_results == 3);
    for(std::size_t i = 0; i < items.size(); ++i) {
        INFO("item " << i);
        CHECK(limited->of(i) + other->of(i) == 1);
    }
}

/******************************************************************************/
// A client that is still connecting notices a halt: the connector's attempt is given up and run()
// returns.

namespace {

/** @brief A connector whose attempt never completes; it keeps the executor busy until cancel() */
class NeverConnector final : public Gem::Courtier::Consumers::GConnectorI {
public:
    /** @brief Keeps the attempt, and work on @p executor @param executor Where it would run
     *  @param done Never called */
    void connect(boost::asio::any_io_executor executor, done_type done) override {
        done_ = std::move(done);
        work_.emplace(boost::asio::make_work_guard(executor));
    }
    /** @brief Gives the attempt up */
    void cancel() override {
        n_cancels_->fetch_add(1);
        done_ = nullptr;
        work_.reset();
    }
    /** @brief @return true */
    [[nodiscard]] bool reconnectable() const override {
        return true;
    }
    /** @brief @return How often cancel() was called */
    [[nodiscard]] std::shared_ptr<std::atomic<int>> cancels() const {
        return n_cancels_;
    }

private:
    done_type done_;
    std::optional<boost::asio::executor_work_guard<boost::asio::any_io_executor>> work_;
    std::shared_ptr<std::atomic<int>> n_cancels_ = std::make_shared<std::atomic<int>>(0);
};

} /* anonymous namespace */

TEST_CASE(
    "GPeerClientT: a client that is still connecting notices a halt",
    "[courtier][peer][client]"
) {
    auto connector = std::make_unique<NeverConnector>();
    auto cancels = connector->cancels();
    auto client = std::make_shared<client_type>(
        std::move(connector),
        GPeerClientOptions{.name = "connecting client", .mode = BIN}
    );
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::jthread runner([client, done] {
        client->run();
        done->store(true);
    });
    std::this_thread::sleep_for(100ms);
    const auto asked = std::chrono::steady_clock::now();
    client->flagCloseRequested();
    const bool returned = Gem::Courtier::Tests::eventually(
        [&done] { return done->load(); },
        std::chrono::milliseconds(5000)
    );
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - asked
    );
    INFO("run() returned after " << took.count() << " ms");
    REQUIRE(returned);
    CHECK(took < 2500ms);
    CHECK(cancels->load() >= 1);
}

/******************************************************************************/
/** The test double that counts on the server's side answers executor() also once the end it wraps
 *  has gone (GPeerI: from any thread; a wake-up may post to it then). */
TEST_CASE(
    "GPeerClientT tests: the counting server end keeps its executor",
    "[courtier][peer][client]"
) {
    IoThreads io{1};
    auto ends = Gem::Courtier::Tests::GInProcessPeer::makePair(io.executor(), {}, io.executor(), {}, 1s);
    auto counting = std::make_shared<CountingPeer>(ends.first, std::make_shared<ServerCounts>());
    ends.first.reset();
    ends.second.reset(); // neither end was started: they are gone
    const auto executor = counting->executor();
    REQUIRE(static_cast<bool>(executor));
    std::promise<void> ran;
    boost::asio::post(executor, [&ran] { ran.set_value(); });
    CHECK(ran.get_future().wait_for(PATIENCE) == std::future_status::ready);
}

/******************************************************************************/
// Connecting: a client may start before its server; it gives up at its first-contact limit, also
// inside an attempt that never completes; an open connection is not held against the limit.

TEST_CASE(
    "GPeerClientT: a client whose attempt never completes ends at its first-contact limit",
    "[courtier][peer][client]"
) {
    auto connector = std::make_unique<NeverConnector>();
    auto cancels = connector->cancels();
    auto client = std::make_shared<client_type>(
        std::move(connector),
        GPeerClientOptions{.name = "limited client", .mode = BIN, .firstContactLimit = 1s}
    );
    auto done = std::make_shared<std::atomic<bool>>(false);
    const std::jthread runner([client, done] {
        client->run();
        done->store(true);
    });
    const bool returned =
        Gem::Courtier::Tests::eventually([&done] { return done->load(); }, 5000ms);
    if(not returned) {
        client->flagCloseRequested(); // so that the runner can be joined
    }
    REQUIRE(returned);
    CHECK(client->terminalErrorFlagged());
    CHECK(cancels->load() >= 1);
}

TEST_CASE(
    "GPeerClientT: a client started before its server connects once the server is there",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    world.route().clear(); // connects are refused
    world.client(
        GPeerClientOptions{.name = "early client", .mode = BIN},
        evaluations,
        1ms,
        nullptr,
        AS_IS
    );
    std::this_thread::sleep_for(1500ms); // several attempts are refused meanwhile
    CHECK_FALSE(world.clientsDone());

    world.route().set(world.listener());
    auto items = makeBatch(5);
    REQUIRE(world.runBatch(items));
    CHECK(evaluations->total.load() == 5);
}

TEST_CASE(
    "GPeerClientT: a client that reaches no server ends at its first-contact limit",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    world.route().clear();
    auto client = world.client(
        GPeerClientOptions{.name = "limited client", .mode = BIN, .firstContactLimit = 1s},
        evaluations,
        1ms,
        nullptr,
        AS_IS
    );
    REQUIRE(eventually([&] { return world.clientsDone(); }));
    CHECK(client->terminalErrorFlagged());
}

TEST_CASE(
    "GPeerClientT: an open connection without work is not held against the first-contact limit",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "parked client", .mode = BIN, .firstContactLimit = 1s},
        evaluations,
        1ms,
        nullptr,
        AS_IS
    );
    std::this_thread::sleep_for(2500ms); // parked: no answer, and less than the contact time
    CHECK_FALSE(world.clientsDone());

    auto items = makeBatch(3);
    REQUIRE(world.runBatch(items));
    CHECK(evaluations->total.load() == 3);
    CHECK_FALSE(client->terminalErrorFlagged());
}

/******************************************************************************/
// A lost connection: the client connects again, sends what the server may not have read, and goes
// on; what it holds is evaluated and delivered on the next connection.

namespace {

/** @brief A gate an item processor waits at until the test opens it */
struct Gate {
    std::promise<void> opened;                                     ///< Set to open it
    std::shared_future<void> waited = opened.get_future().share(); ///< Where processors wait
    std::atomic<std::size_t> waiting{0};                           ///< Evaluations that arrived

    /** @brief Opens the gate (once) */
    void open() {
        if(not open_.exchange(true)) {
            opened.set_value();
        }
    }

private:
    std::atomic<bool> open_{false};
};

/**
 * @brief An item processor that counts (as countingProcessor()), and waits at @p gate before it
 * marks the item processed.
 * @param evaluations Where it counts @param gate Where it waits
 */
Gem::Courtier::GItemProcessorT<GFaultyContainer>
gatedProcessor(const std::shared_ptr<Evaluations> &evaluations, const std::shared_ptr<Gate> &gate) {
    return [evaluations, gate](GFaultyContainer &item) {
        {
            const std::scoped_lock lock(evaluations->mutex);
            ++evaluations->perItem[item.get_stored_number()];
        }
        evaluations->total.fetch_add(1);
        gate->waiting.fetch_add(1);
        gate->waited.wait_for(PATIENCE);
        item.markAsProcessedWith({true});
    };
}

/** @brief @param options The settings of a client @return A client that is set up with a gated
 *  processor @param evaluations Where it counts @param gate Where it waits */
auto withGate(const std::shared_ptr<Evaluations> &evaluations, const std::shared_ptr<Gate> &gate) {
    return [evaluations, gate](client_type &client) {
        client.setItemProcessor(gatedProcessor(evaluations, gate));
    };
}

/** @brief Whether @p bytes are a RETURN frame (a close's payload is not a frame)
 *  @param bytes What an in-process end hands over @return true for a RETURN */
bool isReturn(std::string_view bytes) {
    try {
        return kindOf(std::string(bytes)) == GFrameKind::RETURN;
    }
    catch(...) {
        return false;
    }
}

/** @brief A listener whose server ends close every connection as soon as it is open, and its
 *  threads. It refers to nothing else. */
class DroppingServer {
public:
    /** @brief @param how What the close says; NORMAL: closed at once, ERROR: closed with ERROR on
     *  the first message */
    explicit DroppingServer(GPeerClose how = GPeerClose::NORMAL) {
        listener_->start(io_.executor(), [how](const std::shared_ptr<GPeerI> &peer, GWirePeerId) {
            const std::weak_ptr<GPeerI> weak = peer;
            peer->start(
                GPeerHandlers{
                    .onOpen = {},
                    .onMessage =
                        // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
                    [weak, how](std::string &&) {
                        if(auto end = weak.lock(); end && how == GPeerClose::ERROR) {
                            end->close(GPeerClose::ERROR);
                        }
                    },
                    .onClosed = [](GPeerEnd, GPeerClose, std::string_view) {},
                }
            );
            if(how == GPeerClose::NORMAL) {
                peer->close(GPeerClose::NORMAL);
            }
        });
    }
    /** @brief Stops the listener while its threads run */
    ~DroppingServer() {
        try {
            listener_->stop();
        }
        catch(...) { // NOLINT(bugprone-empty-catch) -- it does not throw here
        }
        listener_->endWork();
    }
    DroppingServer(const DroppingServer &) = delete;
    DroppingServer(DroppingServer &&) = delete;
    DroppingServer &operator=(const DroppingServer &) = delete;
    DroppingServer &operator=(DroppingServer &&) = delete;

    /** @brief @return The listener (for a route) */
    [[nodiscard]] const std::shared_ptr<GInProcessListener> &listener() const {
        return listener_;
    }

private:
    IoThreads io_{2};
    std::shared_ptr<GInProcessListener> listener_ =
        std::make_shared<GInProcessListener>(GInProcessListener::Options{});
};

} /* anonymous namespace */

/******************************************************************************/
/** A result that is finished while the client has no connection is delivered on the next one, once:
 *  the item is not evaluated a second time. */
TEST_CASE(
    "GPeerClientT: a result finished while there is no connection is delivered once",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "losing client", .mode = BIN},
        evaluations,
        0ms,
        watch,
        withGate(evaluations, gate)
    );
    auto items = makeBatch(1);
    auto batch = std::async(std::launch::async, [&] { return world.runBatch(items); });
    REQUIRE(eventually([&] { return gate->waiting.load() == 1; }));

    world.route().clear();
    watch->last()->sever();
    REQUIRE(eventually([&] { return client->losses() == 1; }));
    gate->open(); // the result is finished while every attempt is refused
    std::this_thread::sleep_for(300ms);
    CHECK(client->connections() == 1);

    world.route().set(world.listener());
    REQUIRE(batch.get());
    CHECK(evaluations->of(0) == 1);
    CHECK(client->connections() == 2);
    CHECK(client->resultsResent() == 0); // it had never been sent
}

/******************************************************************************/
/** A client at depth 1 whose request waits at the server (no work) loses its connection: on the
 *  next one it asks again and works. */
TEST_CASE(
    "GPeerClientT: a parked client at depth 1 works after a loss",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "parked client", .mode = BIN},
        evaluations,
        1ms,
        watch,
        AS_IS
    );
    REQUIRE(eventually([&] { return client->connections() == 1; }));
    std::this_thread::sleep_for(100ms); // its PULL waits at the server
    watch->last()->sever();
    REQUIRE(eventually([&] { return client->connections() == 2; }));

    auto items = makeBatch(3);
    REQUIRE(world.runBatch(items));
    CHECK(evaluations->total.load() == 3);
}

/******************************************************************************/
/** The new peer of a client that evaluates is told so. */
TEST_CASE(
    "GPeerClientT: a new connection is told that the client is busy",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "busy client", .mode = BIN},
        evaluations,
        0ms,
        watch,
        withGate(evaluations, gate)
    );
    auto items = makeBatch(1);
    auto batch = std::async(std::launch::async, [&] { return world.runBatch(items); });
    REQUIRE(eventually([&] { return gate->waiting.load() == 1; }));
    auto first = watch->last();
    REQUIRE(first);
    CHECK(eventually([&] { return first->ownerBusy(); }));

    first->sever();
    REQUIRE(eventually([&] { return client->connections() == 2; }));
    auto second = watch->last();
    REQUIRE(second);
    CHECK(second != first);
    CHECK(eventually([&] { return second->ownerBusy(); }));

    gate->open();
    REQUIRE(batch.get());
    CHECK(evaluations->of(0) == 1);
}

/******************************************************************************/
/** A RETURN that ends every connection it is sent on is given up after the resend limit; the server
 *  hands its item out again, and the batch is done. */
TEST_CASE(
    "GPeerClientT: a result lost on every connection is given up at the resend limit",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    auto cuts = std::make_shared<std::atomic<int>>(0);
    watch->setOnMade([cuts](Gem::Courtier::Tests::GInProcessPeer &end) {
        end.setTransmitHook([&end, cuts](std::string_view bytes) {
            if(isReturn(bytes) && cuts->load() < Gem::Courtier::GCLIENTRESENDLIMIT) {
                cuts->fetch_add(1);
                end.sever(); // the RETURN is not delivered
            }
        });
    });
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "unlucky client", .mode = BIN},
        evaluations,
        1ms,
        watch,
        AS_IS
    );
    auto items = makeBatch(1);
    REQUIRE(world.runBatch(items));
    CHECK(cuts->load() == Gem::Courtier::GCLIENTRESENDLIMIT);
    CHECK(client->resultsGivenUp() == 1);
    CHECK(client->losses() == Gem::Courtier::GCLIENTRESENDLIMIT);
    CHECK(evaluations->of(0) == 2); // handed out again by the server
}

/******************************************************************************/
/** A server that closes with ERROR (a message whose kind it could not read) ends the connection,
 *  not the client: it connects again. */
TEST_CASE(
    "GPeerClientT: a connection the server closes with ERROR is a loss",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    const DroppingServer erring(GPeerClose::ERROR);
    World world;
    world.route().set(erring.listener());
    auto client = world.client(
        GPeerClientOptions{.name = "client", .mode = BIN},
        evaluations,
        1ms,
        nullptr,
        AS_IS
    );
    REQUIRE(eventually([&] { return client->losses() >= 1; }));
    world.route().set(world.listener());
    auto items = makeBatch(2);
    REQUIRE(world.runBatch(items));
    CHECK(evaluations->total.load() == 2);
    CHECK_FALSE(client->terminalErrorFlagged());
}

/******************************************************************************/
/**
 * Contact. A server that accepts and drops at once gives no contact: the pause grows, and the
 * reconnect limit, counted from the end of the last connection that had contact, ends the client.
 */
TEST_CASE(
    "GPeerClientT: a server that drops every connection at once does not keep a client",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    const DroppingServer dropping;
    World world;
    auto client = world.client(
        GPeerClientOptions{
            .name = "limited client",
            .mode = BIN,
            .reconnectLimit = 3s,
            .contactTime = 300ms
        },
        evaluations,
        1ms,
        watch,
        AS_IS
    );
    auto items = makeBatch(1);
    REQUIRE(world.runBatch(items)); // an answer: contact

    world.route().set(dropping.listener());
    const auto lost = std::chrono::steady_clock::now();
    watch->last()->sever();
    REQUIRE(eventually([&] { return world.clientsDone(); }));
    const auto took = std::chrono::steady_clock::now() - lost;
    CHECK(client->terminalErrorFlagged());
    CHECK(took >= 3s);
    CHECK(took < 6s);
    // Pauses of 0.125-0.25, 0.25-0.5, 0.5-1, 1-2 s ... within about 3 s
    INFO("connections dropped: " << dropping.listener()->nConnections());
    CHECK(dropping.listener()->nConnections() <= 7);
}

/**
 * A connection that had contact long ago (a parked client) and is then lost: the reconnect limit
 * counts from the loss, not from the last answer.
 */
TEST_CASE(
    "GPeerClientT: the reconnect limit counts from the end of the connection",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "patient client", .mode = BIN, .reconnectLimit = 1500ms},
        evaluations,
        1ms,
        watch,
        AS_IS
    );
    auto first = makeBatch(1);
    REQUIRE(world.runBatch(first));      // an answer: contact
    std::this_thread::sleep_for(2500ms); // parked for longer than the limit

    world.route().clear();
    watch->last()->sever();
    std::this_thread::sleep_for(500ms);
    world.route().set(world.listener());
    REQUIRE(eventually([&] { return client->connections() == 2; }));
    CHECK_FALSE(world.clientsDone());
    auto second = makeBatch(1, 1);
    REQUIRE(world.runBatch(second));
    CHECK_FALSE(client->terminalErrorFlagged());
}

/******************************************************************************/
/**
 * At most one answer per request read: with RESULTs (look-ahead), parked requests, a lost
 * connection and the end of the run. (UNREADABLE cannot be made here: the counting server end
 * decodes every message itself, which would use up the load fault that makes a client's load fail.)
 */
TEST_CASE("GPeerClientT: the server answers no message twice", "[courtier][peer][client]") {
    auto evaluations = std::make_shared<Evaluations>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    std::shared_ptr<ServerCounts> counts;
    {
        World world;
        counts = world.sharedCounts();
        auto client = world.client(lookAhead(), evaluations, 5ms, watch, AS_IS);
        auto items = makeBatch(6, 0);
        REQUIRE(world.runBatch(items));
        std::this_thread::sleep_for(100ms); // parked
        watch->last()->sever();
        REQUIRE(eventually([&] { return client->connections() == 2; }));
        auto more = makeBatch(4, 10);
        REQUIRE(world.runBatch(more));
        CHECK(world.counts().nResults.load() + world.counts().nReturns.load() >= 10);
    }
    CHECK(counts->overAnswered.load() == 0);
}

/******************************************************************************/
// The order of a loss: an answer that arrived before the connection was lost, and is decoded only
// afterwards, is booked against the lost connection first.

namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<std::size_t> gatedDecodes{0};   ///< Work items whose decoding on a client has begun
std::atomic<bool> secondDecodeHeld{false};  ///< The second decode waits while this is set
std::atomic<bool> secondDecodeWaits{false}; ///< The second decode has begun to wait
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/** @brief A work item whose second decode on a client waits as long as the test holds it */
class GHeldDecodeItem final : public Gem::Courtier::GProcessingContainerT<GHeldDecodeItem, bool> {
    friend struct Gem::Weft::access;

    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<
            Gem::Courtier::GProcessingContainerT<GHeldDecodeItem, bool>>(
            ar,
            "GProcessingContainerT_GHeldDecodeItem",
            *this
        );
        if constexpr(not Gem::Common::archive_is_saving_v<archive_type>) {
            if(this->is_due_for_processing() && gatedDecodes.fetch_add(1) == 1) {
                secondDecodeWaits.store(true);
                Gem::Courtier::Tests::eventually(
                    [] { return not secondDecodeHeld.load(); },
                    std::chrono::milliseconds(10'000)
                );
            }
        }
    }

public:
    /** @brief The default constructor (also for loading) */
    GHeldDecodeItem()
      : Gem::Courtier::GProcessingContainerT<GHeldDecodeItem, bool>(1) { /* nothing */
    }

private:
    /** @brief Nothing to compute */
    void process_([[maybe_unused]] const std::vector<bool> &res_vec) override { /* nothing */
    }
};

} /* anonymous namespace */

/**
 * The client returns its first result as a RETURN; the server answers it with the second item, and
 * the connection is lost while that answer is being decoded. The answer proves the RETURN: it is
 * not sent again on the next connection, and the second item is evaluated and delivered there.
 */
TEST_CASE(
    "GPeerClientT: an answer decoded after a loss is booked against the lost connection",
    "[courtier][peer][client]"
) {
    gatedDecodes.store(0);
    secondDecodeHeld.store(true);
    secondDecodeWaits.store(false);
    using held_client_type = Gem::Courtier::Consumers::GPeerClientT<GHeldDecodeItem>;
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    auto watch = std::make_shared<GInProcessPeerWatch>();
    std::vector<std::unique_ptr<GHeldDecodeItem>> items;
    items.push_back(std::make_unique<GHeldDecodeItem>());
    items.push_back(std::make_unique<GHeldDecodeItem>());
    std::shared_ptr<held_client_type> client;
    bool done = false;
    {
        Gem::Courtier::GPeerConsumerT<GHeldDecodeItem>
            shell("Shell", std::make_unique<GInProcessListenerRef>(listener), 2, BIN);
        Gem::Courtier::GNetworkedTimeoutConfig config;
        config.treatment = "wait_indefinitely";
        shell.applyTimeoutConfig(config);
        shell.startServer();
        client = std::make_shared<held_client_type>(
            std::make_unique<GInProcessConnector>(
                std::make_shared<GInProcessRoute>(listener),
                true,
                watch
            ),
            GPeerClientOptions{.name = "held client", .mode = BIN}
        );
        const std::jthread runner([client] { client->run(); });
        auto batch = std::async(std::launch::async, [&] {
            shell.processBatch(
                std::span(items.data(), items.size()),
                Gem::Courtier::GSubmissionPolicy::full_success_or_fatal()
            );
        });

        REQUIRE(eventually([] { return secondDecodeWaits.load(); }));
        watch->last()->sever();
        REQUIRE(eventually([&] { return client->losses() == 1; }));
        // The loss waits for the decode: no new attempt is made meanwhile (the first pause would
        // be over long before this).
        std::this_thread::sleep_for(600ms);
        CHECK(client->connections() == 1);
        secondDecodeHeld.store(false);

        done = batch.wait_for(PATIENCE) == std::future_status::ready;
        if(not done) {
            shell.stopServer();
        }
        batch.get();
        shell.stopServer();
    }
    CHECK(done);
    CHECK(client->connections() == 2);
    CHECK(client->resultsResent() == 0);
    CHECK(gatedDecodes.load() == 2);
}
