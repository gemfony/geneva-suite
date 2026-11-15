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
#include <future>
#include <map>
#include <memory>
#include <mutex>
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
using Gem::Courtier::Tests::GInProcessPeerWatch;
using namespace std::chrono_literals;

namespace {

constexpr auto PATIENCE = 10s; // how long a test waits for something that is due soon
constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

using client_type = Gem::Courtier::Consumers::GPeerClientT<GFaultyContainer>;

/** @brief Waits, at most PATIENCE, for @p condition @return Whether it held in time */
template <typename Condition>
bool eventually(Condition condition) {
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
                        if(kind == GFrameKind::PULL || kind == GFrameKind::RETURN) {
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
    void send(std::string message) override {
        const GFrameKind kind = kindOf(message);
        if(kind == GFrameKind::WORK || kind == GFrameKind::NO_WORK) {
            ++answers_;
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
    /** @brief Forwards @return The wrapped end's executor */
    [[nodiscard]] boost::asio::any_io_executor executor() override {
        auto inner = inner_.lock();
        return inner ? inner->executor() : boost::asio::any_io_executor{};
    }

private:
    std::weak_ptr<GPeerI> inner_;          ///< The wrapped end
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
    template <typename Setup>
    std::shared_ptr<client_type> client(
        GPeerClientOptions options,
        const std::shared_ptr<Evaluations> &evaluations,
        std::chrono::milliseconds duration,
        const std::shared_ptr<GInProcessPeerWatch> &watch,
        Setup setup
    ) {
        auto client = std::make_shared<client_type>(
            std::make_unique<GInProcessConnector>(listener_, true, watch),
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

    /** @brief @return The shell */
    Gem::Courtier::GPeerConsumerT<GFaultyContainer> &shell() {
        return shell_;
    }
    /** @brief @return What the server's ends counted */
    ServerCounts &counts() {
        return *counts_;
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
        // One item is evaluated, the other held (or, if the evaluation took the pool's only
        // thread first, still waiting to be decoded): the run ends now.
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
/**
 * With a compute width below the depth, an item that arrives while the pool's only thread evaluates
 * is decoded only when that evaluation ends. If the run has ended by then, it is not evaluated.
 */
TEST_CASE(
    "GPeerClientT: after RUN_ENDED an item still waiting to be decoded is not evaluated",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto first = makeBatch(1, 0);
    auto second = makeBatch(1, 1);
    {
        World world;
        world.client(
            GPeerClientOptions{
                .name = "decoding client",
                .mode = BIN,
                .prefetchDepth = 2,
                .computeWidth = 1
            },
            evaluations,
            500ms,
            nullptr,
            AS_IS
        );
        auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
        policy.fatal_on_none_returned = false;
        const auto submit = [&](std::vector<std::unique_ptr<GFaultyContainer>> &items) {
            auto clone_template = std::make_unique<GFaultyContainer>(99);
            clone_template->markAsProcessedWith({true});
            return std::async(std::launch::async, [&, t = std::move(clone_template)]() mutable {
                world.shell()
                    .processBatch(std::span(items.data(), items.size()), policy, std::move(t));
            });
        };
        // The first item is being evaluated when the second is sent: its decoding waits.
        auto first_batch = submit(first);
        REQUIRE(eventually([&] { return evaluations->total.load() == 1; }));
        auto second_batch = submit(second);
        REQUIRE(eventually([&] { return world.counts().nWork.load() == 2; }));
        world.shell().stopServer();
        first_batch.get();
        second_batch.get();
        REQUIRE(eventually([&] { return world.clientsDone(); }));
    }
    CHECK(evaluations->total.load() == 1);
    CHECK(evaluations->of(1) == 0);
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
    CHECK(told(true));
    REQUIRE(batch.get());
    REQUIRE(eventually([&] { return told(false); }));
}

/******************************************************************************/
/** Scarce work: two items, four look-ahead clients, each waiting with one request. The items go
 *  to two different clients. */
TEST_CASE(
    "GPeerClientT: with look-ahead scarce work goes to different clients",
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
    std::size_t n_clients_with_work = 0;
    for(const auto &e : evaluations) {
        CHECK(e->total.load() <= 1);
        n_clients_with_work += e->total.load();
    }
    CHECK(n_clients_with_work == 2);
}
