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
// held items, the parallel evaluations, the item processor, the busy signal, and scarce work.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>

#include <unistd.h> // getpid()

#include "GInProcessPeer.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "GInProcessTestSupport.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "GDemoProcessingContainers.hpp"
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
using Gem::Courtier::Tests::GInProcessPeer;
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
 * @brief The teardown of a test world: waits, at most PATIENCE, until @p done is set (its client
 * threads have returned from run()). A client that does not end is the sign of a failure the test
 * has already reported (a REQUIRE that failed, say); joining it would hang the process until
 * ctest's timeout, and Catch2's report of the failure would be lost. So the process ends here,
 * after the report has been flushed, with a failure.
 * @param done Whether the clients have ended @param world Which world, for the message
 */
template <typename done_type>
void endsOrExits(const done_type &done, std::string_view world) {
    if(eventually([&done] { return done(); })) {
        return;
    }
    std::cout.flush();
    std::cerr << world << ": a client did not end within " << PATIENCE.count()
              << " s of the teardown; the test process ends here (see the failure above)\n";
    std::cerr.flush();
    std::_Exit(EXIT_FAILURE);
}

/******************************************************************************/
/**
 * A batch that runs in a thread of its own while a test drives its clients. If the test leaves
 * before the batch is done (a REQUIRE that failed), the batch would wait for its items for ever,
 * and its future, destroyed first, with it: so the server is stopped first, which ends the batch.
 */
class BatchRun {
public:
    /** @brief @param run The batch, running @param stop Stops its server (idempotent) */
    BatchRun(std::future<void> run, std::function<void()> stop)
      : run_(std::move(run))
      , stop_(std::move(stop)) { /* nothing */
    }
    /** @brief Stops the server if the batch is not done; the future then waits for it */
    ~BatchRun() {
        if(run_.valid() && run_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            try {
                stop_();
            }
            catch(...) { // NOLINT(bugprone-empty-catch) -- a test's teardown goes on
            }
        }
    }
    BatchRun(const BatchRun &) = delete;
    BatchRun(BatchRun &&) = default;
    BatchRun &operator=(const BatchRun &) = delete;
    BatchRun &operator=(BatchRun &&) = delete;

    /** @brief Waits for the batch and rethrows what it threw */
    void get() {
        run_.get();
    }
    /** @brief @param patience How long @return The state of the batch after waiting so long */
    template <typename duration_type>
    [[nodiscard]] std::future_status wait_for(duration_type patience) const {
        return run_.wait_for(patience);
    }

private:
    std::future<void> run_;
    std::function<void()> stop_;
};

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
        endsOrExits([this] { return this->clientsDone(); }, "World");
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

/** @brief @return The options of a look-ahead client: depth 2, one evaluation at a time */
GPeerClientOptions lookAhead() {
    return GPeerClientOptions{
        .name = "look-ahead client",
        .mode = BIN,
        .prefetchDepth = 2,
        .parallelEvaluations = 1,
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
                .parallelEvaluations = 1
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
/** A client evaluates no more items at once than its parallel evaluations allow, and uses its
 *  item processor for every one. */
TEST_CASE(
    "GPeerClientT: the parallel evaluations bound the evaluations and the item processor does them",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    world.client(
        GPeerClientOptions{
            .name = "wide client",
            .mode = BIN,
            .prefetchDepth = 4,
            .parallelEvaluations = 2
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
/** A client with an evaluation slot free is not busy: it waits for its next item, and a polling
 *  transport must not look less often for it. Only with every slot taken is it busy. */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE(
    "GPeerClientT: a client is busy only while every evaluation slot is taken",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    World world;
    world.client(
        GPeerClientOptions{
            .name = "two slots",
            .mode = BIN,
            .prefetchDepth = 2,
            .parallelEvaluations = 2
        },
        evaluations,
        200ms,
        watch,
        AS_IS
    );
    // The client's end is looked at only briefly: it must not outlive the client's io context.
    const auto told_busy = [&watch] {
        const auto end = watch->last();
        return end == nullptr ? std::size_t{0} : end->nToldBusy();
    };
    REQUIRE(eventually([&] { return watch->last() != nullptr; }));

    // One item: one slot is taken, one is free -- never busy
    auto one = makeBatch(1);
    REQUIRE(world.runBatch(one));
    CHECK(told_busy() == 0);

    // Two items: both slots taken -- busy
    auto two = makeBatch(2);
    REQUIRE(world.runBatch(two));
    CHECK(evaluations->mostAtOnce() == 2);
    CHECK(told_busy() >= 1);
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
                .parallelEvaluations = 2,
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
        .parallelEvaluations = 1,
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
 * A look-ahead client (one evaluation at a time, depth 2) decodes the item it holds ahead while it
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
 * two parallel evaluations, limit 2: two items are evaluated and two held; the first result lets a held item start,
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
            .parallelEvaluations = 2
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
/** A client cannot evaluate more items than it holds, nor none: m above n, and m = 0, are refused
 *  when the client is built, with a message that names both. */
TEST_CASE(
    "GPeerClientT: more parallel evaluations than the prefetch depth are refused",
    "[courtier][peer][client]"
) {
    const auto build = [](std::size_t depth, std::size_t parallel) {
        return std::make_shared<client_type>(
            std::make_unique<NeverConnector>(),
            GPeerClientOptions{
                .name = "client",
                .mode = BIN,
                .prefetchDepth = depth,
                .parallelEvaluations = parallel
            }
        );
    };
    CHECK_THROWS_WITH(
        build(2, 3),
        Catch::Matchers::ContainsSubstring("evaluate 3 items at the same time") &&
            Catch::Matchers::ContainsSubstring("it holds at most 2")
    );
    CHECK_THROWS_WITH(build(2, 0), Catch::Matchers::ContainsSubstring("evaluate 0 items"));
    CHECK_NOTHROW(build(3, 3));
    CHECK_NOTHROW(build(3, 1));
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
    const auto ran = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    boost::asio::post(executor, [ran] { ran->count_down(); });
    CHECK(ran->wait_for(PATIENCE));
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
    Gem::Common::Concurrency::GCompletionLatchT opened{1}; ///< Counted down once to open it
    std::atomic<std::size_t> waiting{0};                   ///< Evaluations that arrived

    /** @brief Opens the gate (once) */
    void open() {
        if(not open_.exchange(true)) {
            opened.count_down();
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
        std::ignore = gate->opened.wait_for(PATIENCE);
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

/** @brief A directory of its own for the files of one test; removed with what is in it */
class UndeliveredDir {
public:
    UndeliveredDir()
      : path_(
            std::filesystem::temp_directory_path() /
            ("geneva-undelivered-test-" + std::to_string(::getpid()) + "-" +
             std::to_string(counter().fetch_add(1)))
        ) {
        std::filesystem::create_directories(path_);
    }
    ~UndeliveredDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    UndeliveredDir(const UndeliveredDir &) = delete;
    UndeliveredDir(UndeliveredDir &&) = delete;
    UndeliveredDir &operator=(const UndeliveredDir &) = delete;
    UndeliveredDir &operator=(UndeliveredDir &&) = delete;

    /** @brief @return Its path */
    [[nodiscard]] const std::filesystem::path &path() const {
        return path_;
    }
    /** @brief @return The files in it */
    [[nodiscard]] std::vector<std::filesystem::path> files() const {
        std::vector<std::filesystem::path> found;
        for(const auto &entry : std::filesystem::directory_iterator(path_)) {
            found.push_back(entry.path());
        }
        return found;
    }

private:
    /** @brief @return A count that tells the directories of one process apart */
    static std::atomic<int> &counter() {
        static std::atomic<int> n{0};
        return n;
    }
    std::filesystem::path path_; ///< Where it is
};

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
    const UndeliveredDir dir;
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "unlucky client", .mode = BIN, .undeliveredDir = dir.path()},
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
    CHECK(dir.files().size() == 1); // the result given up is saved
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
    // The last RETURN waited at the server for an answer: the client cannot know that it arrived,
    // sends it again, gives it up at the resend limit and saves it
    const UndeliveredDir dir;
    World world;
    auto client = world.client(
        GPeerClientOptions{
            .name = "limited client",
            .mode = BIN,
            .reconnectLimit = 3s,
            .contactTime = 300ms,
            .undeliveredDir = dir.path()
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
std::atomic<std::size_t> gatedDecodes{0}; ///< Work items whose decoding on a client has begun
std::atomic<std::size_t> heldDecode{1};   ///< Which decode waits (0: the first)
std::atomic<bool> decodeHeld{false};      ///< That decode waits while this is set
std::atomic<bool> decodeWaits{false};     ///< That decode has begun to wait
std::atomic<std::size_t> processedSaves{0}; ///< Encodings of an evaluated item so far
/// Which encoding of an evaluated item waits (0: the first); none by default
std::atomic<std::size_t> heldSave{std::numeric_limits<std::size_t>::max()};
std::atomic<bool> saveHeld{false};  ///< That encoding waits while this is set
std::atomic<bool> saveWaits{false}; ///< That encoding has begun to wait
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/** @brief A work item whose decode on a client waits as long as the test holds it: the
 *  heldDecode-th one (0: the first) of the process; and whose heldSave-th encoding as an evaluated
 *  item (a result) waits likewise */
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
        if constexpr(Gem::Common::archive_is_saving_v<archive_type>) {
            if(this->is_processed() && processedSaves.fetch_add(1) == heldSave.load()) {
                saveWaits.store(true);
                Gem::Courtier::Tests::eventually(
                    [] { return not saveHeld.load(); },
                    std::chrono::milliseconds(10'000)
                );
            }
        }
        else {
            if(this->is_due_for_processing() && gatedDecodes.fetch_add(1) == heldDecode.load()) {
                decodeWaits.store(true);
                Gem::Courtier::Tests::eventually(
                    [] { return not decodeHeld.load(); },
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
 * The decode is held for longer than the halt poll's interval: the limits are not looked at while
 * the lost connection's messages are decoded (a client with contact gave up at its first loss
 * when the poll fell into the drain, because the end of its last contact was not yet noted).
 */
TEST_CASE(
    "GPeerClientT: an answer decoded after a loss is booked against the lost connection",
    "[courtier][peer][client]"
) {
    gatedDecodes.store(0);
    heldDecode.store(1);
    decodeHeld.store(true);
    decodeWaits.store(false);
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

        REQUIRE(eventually([] { return decodeWaits.load(); }));
        watch->last()->sever();
        REQUIRE(eventually([&] { return client->losses() == 1; }));
        // The loss waits for the decode: no new attempt is made meanwhile (the first pause would
        // be over long before the halt poll runs), and the halt poll, which runs during the
        // drain, finds no limit run out.
        const auto polls = client->haltPolls();
        REQUIRE(eventually([&] { return client->haltPolls() > polls; }));
        CHECK(client->connections() == 1);
        CHECK_FALSE(client->terminalErrorFlagged());
        decodeHeld.store(false);

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
    CHECK_FALSE(client->terminalErrorFlagged());
}

/**
 * A second loss long after the first: the reconnect limit counts from the end of the connection
 * that was just lost, also while its messages are still being decoded -- not from the end of the
 * first one, which lies further back than the limit.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: a second loss long after the first is not held against the reconnect limit",
    "[courtier][peer][client]"
) {
    gatedDecodes.store(0);
    heldDecode.store(3); // the second item of the second batch
    decodeHeld.store(true);
    decodeWaits.store(false);
    using held_client_type = Gem::Courtier::Consumers::GPeerClientT<GHeldDecodeItem>;
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    auto watch = std::make_shared<GInProcessPeerWatch>();
    const auto makeItems = [] {
        std::vector<std::unique_ptr<GHeldDecodeItem>> items;
        items.push_back(std::make_unique<GHeldDecodeItem>());
        items.push_back(std::make_unique<GHeldDecodeItem>());
        return items;
    };
    auto first = makeItems();
    auto second = makeItems();
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
            GPeerClientOptions{.name = "held client", .mode = BIN, .reconnectLimit = 2s}
        );
        const std::jthread runner([client] { client->run(); });
        shell.processBatch(
            std::span(first.data(), first.size()),
            Gem::Courtier::GSubmissionPolicy::full_success_or_fatal()
        );
        watch->last()->sever(); // the first loss, of a connection with contact
        REQUIRE(eventually([&] { return client->connections() == 2; }));
        std::this_thread::sleep_for(2500ms); // longer than the limit since the first loss

        auto batch = std::async(std::launch::async, [&] {
            shell.processBatch(
                std::span(second.data(), second.size()),
                Gem::Courtier::GSubmissionPolicy::full_success_or_fatal()
            );
        });
        REQUIRE(eventually([] { return decodeWaits.load(); }));
        watch->last()->sever(); // the second loss, while an answer of it is decoded
        REQUIRE(eventually([&] { return client->losses() == 2; }));
        const auto polls = client->haltPolls();
        REQUIRE(eventually([&] { return client->haltPolls() > polls; })); // during the drain
        CHECK_FALSE(client->terminalErrorFlagged());
        decodeHeld.store(false);

        done = batch.wait_for(PATIENCE) == std::future_status::ready;
        if(not done) {
            shell.stopServer();
        }
        batch.get();
        shell.stopServer();
    }
    CHECK(done);
    CHECK(client->connections() == 3);
    CHECK_FALSE(client->terminalErrorFlagged());
}

/******************************************************************************/
// An answer of a connection is booked against that connection only, and the client that halts
// without a connection books the answers still being decoded before it removes anything: an
// answer decoded later must not take a result off the list that nothing has proven.

namespace {

/** @brief A connector whose attempts wait while it is held, then go to the in-process connector it
 *  wraps. Held from the test thread; released there. */
class GatedConnector final : public Gem::Courtier::Consumers::GConnectorI {
public:
    /** @brief @param inner The connector the attempts go to */
    explicit GatedConnector(std::unique_ptr<GInProcessConnector> inner)
      : inner_(std::move(inner)) { /* nothing */
    }
    /** @brief An attempt goes to the inner connector, or waits while held
     *  @param executor Where it runs @param done What is told the outcome */
    void connect(boost::asio::any_io_executor executor, done_type done) override {
        const std::scoped_lock lock(state_->mutex);
        if(state_->held) {
            state_->work.emplace(boost::asio::make_work_guard(executor)); // as a transport would
            state_->executor = std::move(executor);
            state_->done = std::move(done);
            state_->waiting = true;
            return;
        }
        inner_->connect(std::move(executor), std::move(done));
    }
    /** @brief Gives up an attempt, also one that waits */
    void cancel() override {
        const std::scoped_lock lock(state_->mutex);
        state_->done = nullptr;
        state_->waiting = false;
        state_->work.reset();
        inner_->cancel();
    }
    /** @brief @return true */
    [[nodiscard]] bool reconnectable() const override {
        return true;
    }

    /** @brief What the test holds on to: the gate, after the client has taken the connector */
    struct State {
        std::mutex mutex;
        bool held = false;
        bool waiting = false;
        boost::asio::any_io_executor executor;
        done_type done;
        /// Keeps the client's executor busy while an attempt waits
        std::optional<boost::asio::executor_work_guard<boost::asio::any_io_executor>> work;
    };
    /** @brief @return The gate */
    [[nodiscard]] std::shared_ptr<State> state() const {
        return state_;
    }
    /** @brief Attempts wait from now on @param state The gate */
    static void hold(const std::shared_ptr<State> &state) {
        const std::scoped_lock lock(state->mutex);
        state->held = true;
    }
    /** @brief @param state The gate @return Whether an attempt waits at it */
    [[nodiscard]] static bool waiting(const std::shared_ptr<State> &state) {
        const std::scoped_lock lock(state->mutex);
        return state->waiting;
    }
    /** @brief Lets a waiting attempt go on; it is refused (reported on the client's executor)
     *  @param state The gate */
    static void release(const std::shared_ptr<State> &state) {
        const std::scoped_lock lock(state->mutex);
        state->held = false;
        if(state->waiting && state->done) {
            state->waiting = false;
            boost::asio::post(state->executor, [done = std::move(state->done)]() mutable {
                done(nullptr, "the server refuses");
            });
        }
        state->work.reset();
    }

private:
    std::unique_ptr<GInProcessConnector> inner_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

/** @brief A server's end that replaces one message it sends -- the n-th of all ends of its
 *  listener, counted from 0 -- by a frame a client cannot accept (a RESULT, which only clients
 *  send) */
class CorruptingPeer final
  : public GPeerI
  , public std::enable_shared_from_this<CorruptingPeer> {
public:
    /** @brief @param inner The end it wraps @param sent The count shared by the listener's ends
     *  @param at Which message @param corrupted Set once it has been replaced */
    CorruptingPeer(
        const std::shared_ptr<GPeerI> &inner,
        std::shared_ptr<std::atomic<std::size_t>> sent,
        std::size_t at,
        std::shared_ptr<std::atomic<bool>> corrupted
    )
      : inner_(inner)
      , executor_(inner->executor())
      , sent_(std::move(sent))
      , at_(at)
      , corrupted_(std::move(corrupted)) { /* nothing */
    }
    /** @brief Forwards; the handlers it installs on the wrapped end own this end (as a role is
     *  owned, GPeerI.hpp rule 4) @param handlers See GPeerI */
    void start(GPeerHandlers handlers) override {
        auto inner = inner_.lock();
        if(not inner) {
            return;
        }
        inner->start(
            GPeerHandlers{
                .onOpen = std::move(handlers.onOpen),
                .onMessage =
                    [self = this->shared_from_this(),
                     on_message = std::move(handlers.onMessage)](std::string &&message) mutable {
                        on_message(std::move(message));
                    },
                .onClosed = std::move(handlers.onClosed),
            }
        );
    }
    /** @brief Forwards, or sends the bad frame @param message See GPeerI */
    void send(Gem::Courtier::Consumers::GMessage message) override {
        if(sent_->fetch_add(1) == at_) {
            const GCommandContainerT<GHeldDecodeItem> bad{GFrameKind::RESULT};
            message = Gem::Courtier::Consumers::makeMessage(
                Gem::Courtier::wireEncode(bad, nullptr, BIN)
            );
            corrupted_->store(true);
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
    /** @brief @return The wrapped end's executor, kept */
    [[nodiscard]] boost::asio::any_io_executor executor() override {
        return executor_;
    }

private:
    std::weak_ptr<GPeerI> inner_;
    boost::asio::any_io_executor executor_;
    std::shared_ptr<std::atomic<std::size_t>> sent_;
    std::size_t at_;
    std::shared_ptr<std::atomic<bool>> corrupted_;
};

/** @brief A listener whose ends are CorruptingPeers; otherwise the in-process one */
class CorruptingListener final : public GListenerI {
public:
    /** @brief @param listener The in-process listener @param at Which message is replaced
     *  @param corrupted Set once it has been */
    CorruptingListener(
        std::shared_ptr<GInProcessListener> listener,
        std::size_t at,
        std::shared_ptr<std::atomic<bool>> corrupted
    )
      : listener_(std::move(listener))
      , at_(at)
      , corrupted_(std::move(corrupted)) { /* nothing */
    }
    /** @brief Wraps the ends @param executor See GListenerI @param on_peer See GListenerI */
    void start(boost::asio::any_io_executor executor, on_peer_type on_peer) override {
        listener_->start(
            std::move(executor),
            [sent = sent_, at = at_, corrupted = corrupted_, on_peer = std::move(on_peer)](
                const std::shared_ptr<GPeerI> &peer,
                GWirePeerId id
            ) mutable { on_peer(std::make_shared<CorruptingPeer>(peer, sent, at, corrupted), id); }
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
    std::shared_ptr<GInProcessListener> listener_;
    std::size_t at_;
    std::shared_ptr<std::atomic<bool>> corrupted_;
    std::shared_ptr<std::atomic<std::size_t>> sent_ = std::make_shared<std::atomic<std::size_t>>(0);
};

/** @brief A shell for GHeldDecodeItem and one client of it, in-process; the batches end with the
 *  run if they cannot be completed (their slots are filled with clones) */
class HeldWorld {
public:
    using held_client_type = Gem::Courtier::Consumers::GPeerClientT<GHeldDecodeItem>;

    /** @brief @param options The client's settings @param processor How it evaluates (empty: at
     *  once) @param corrupt_at Which message of the server is replaced by a frame a client cannot
     *  accept (CorruptingPeer); none by default */
    explicit HeldWorld(
        GPeerClientOptions options,
        Gem::Courtier::GItemProcessorT<GHeldDecodeItem> processor = {},
        std::size_t corrupt_at = std::numeric_limits<std::size_t>::max()
    )
      : shell_(
            "Shell",
            std::make_unique<CorruptingListener>(listener_, corrupt_at, corrupted_),
            2,
            BIN
        ) {
        Gem::Courtier::GNetworkedTimeoutConfig config;
        config.treatment = "wait_indefinitely";
        shell_.applyTimeoutConfig(config);
        shell_.startServer();
        auto gated = std::make_unique<GatedConnector>(
            std::make_unique<GInProcessConnector>(route_, true, watch_)
        );
        gate_ = gated->state();
        client_ = std::make_shared<held_client_type>(std::move(gated), std::move(options));
        if(processor) {
            client_->setItemProcessor(std::move(processor));
        }
        runner_ = std::jthread([client = client_, done = done_] {
            client->run();
            done->store(true);
        });
    }
    /** @brief Stops the server (the client ends with it) and joins the client */
    ~HeldWorld() {
        try {
            shell_.stopServer();
            GatedConnector::release(gate_);
        }
        catch(...) { // NOLINT(bugprone-empty-catch) -- a test's teardown goes on
        }
        client_->flagCloseRequested();
        endsOrExits([this] { return done_->load(); }, "HeldWorld");
        runner_ = std::jthread{};
    }
    HeldWorld(const HeldWorld &) = delete;
    HeldWorld(HeldWorld &&) = delete;
    HeldWorld &operator=(const HeldWorld &) = delete;
    HeldWorld &operator=(HeldWorld &&) = delete;

    /** @brief @param items A batch @return It, running; not fatal if it cannot be completed */
    BatchRun batch(std::vector<std::unique_ptr<GHeldDecodeItem>> &items) {
        return {std::async(std::launch::async, [this, &items] {
            auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
            policy.fatal_on_none_returned = false;
            auto clone_template = std::make_unique<GHeldDecodeItem>();
            clone_template->markAsProcessedWith({true});
            shell_.processBatch(
                std::span(items.data(), items.size()),
                policy,
                std::move(clone_template)
            );
        }), [this] { shell_.stopServer(); }};
    }

    [[nodiscard]] held_client_type &client() {
        return *client_;
    }
    [[nodiscard]] Gem::Courtier::GPeerConsumerT<GHeldDecodeItem> &shell() {
        return shell_;
    }
    [[nodiscard]] GInProcessRoute &route() {
        return *route_;
    }
    [[nodiscard]] GInProcessPeerWatch &watch() {
        return *watch_;
    }
    /** @brief @return The server end of the last connection, if it still exists */
    [[nodiscard]] std::shared_ptr<GInProcessPeer> serverEnd() {
        return listener_->lastServerEnd();
    }
    [[nodiscard]] const std::shared_ptr<GatedConnector::State> &gate() const {
        return gate_;
    }
    [[nodiscard]] bool clientDone() const {
        return done_->load();
    }
    /** @brief @return Whether the server has sent the bad frame */
    [[nodiscard]] bool corrupted() const {
        return corrupted_->load();
    }

private:
    std::shared_ptr<GInProcessListener> listener_ =
        std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    std::shared_ptr<GInProcessRoute> route_ = std::make_shared<GInProcessRoute>(listener_);
    std::shared_ptr<GInProcessPeerWatch> watch_ = std::make_shared<GInProcessPeerWatch>();
    std::shared_ptr<std::atomic<bool>> corrupted_ = std::make_shared<std::atomic<bool>>(false);
    Gem::Courtier::GPeerConsumerT<GHeldDecodeItem> shell_;
    std::shared_ptr<GatedConnector::State> gate_;
    std::shared_ptr<held_client_type> client_;
    std::shared_ptr<std::atomic<bool>> done_ = std::make_shared<std::atomic<bool>>(false);
    std::jthread runner_;
};

/** @brief @param n How many @return n held-decode items */
std::vector<std::unique_ptr<GHeldDecodeItem>> heldItems(std::size_t n) {
    std::vector<std::unique_ptr<GHeldDecodeItem>> items;
    items.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        items.push_back(std::make_unique<GHeldDecodeItem>());
    }
    return items;
}

} /* anonymous namespace */

/**
 * Look-ahead, depth 2: item A is evaluated while the request P is with the server, so A goes out
 * as a RESULT behind P. The server answers P with B, and the connection is lost while B is being
 * decoded. The client halts during the drain; the attempt it then makes waits. When B's decode
 * is booked it proves P and nothing after it: A is still unproven and is saved when the last
 * attempt fails. (The answer used to be decoded after the halt had removed P from the list, and
 * took A instead.)
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: an answer decoded after a halt during the drain proves only its request",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    gatedDecodes.store(0);
    heldDecode.store(1); // B
    decodeHeld.store(true);
    decodeWaits.store(false);
    {
        HeldWorld world(GPeerClientOptions{
            .name = "look-ahead client",
            .mode = BIN,
            .prefetchDepth = 2,
            .parallelEvaluations = 1,
            .lookAhead = true,
            .undeliveredDir = dir.path()
        });
        auto first = heldItems(1);
        world.batch(first).get(); // A, a RESULT; P waits at the server
        auto second = heldItems(1);
        auto batch = world.batch(second); // B answers P
        REQUIRE(eventually([] { return decodeWaits.load(); }));
        GatedConnector::hold(world.gate()); // (no attempt runs now)
        world.route().clear();
        world.watch().last()->sever();
        REQUIRE(eventually([&] { return world.client().losses() == 1; }));
        world.client().flagCloseRequested();
        const auto polls = world.client().haltPolls();
        // The halt poll has run during the drain
        REQUIRE(eventually([&] { return world.client().haltPolls() > polls; }));
        const auto booked = world.client().messagesBooked();
        decodeHeld.store(false);
        REQUIRE(eventually([&] { return world.client().messagesBooked() > booked; })); // B
        REQUIRE(eventually([&] { return GatedConnector::waiting(world.gate()); })); // the last attempt
        GatedConnector::release(world.gate());
        REQUIRE(eventually([&] { return world.clientDone(); }));
        world.shell().stopServer();
        batch.get();
    }
    CHECK(dir.files().size() == 1);
}

/**
 * Depth 3, two evaluations at a time: A and C are evaluated, B's decode is held (it answers the
 * third request P). The client halts; the first of A and C to finish goes out as a RESULT behind
 * P. The connection is lost; the client winds up without a connection. B's decode, booked only
 * now, proves P and nothing after it: both results are saved when the last attempt fails.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: an answer decoded after a loss while halting proves only its request",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    gatedDecodes.store(0);
    heldDecode.store(2); // B, the third item
    decodeHeld.store(true);
    decodeWaits.store(false);
    auto permits = std::make_shared<std::counting_semaphore<4>>(0);
    auto started = std::make_shared<std::atomic<int>>(0);
    {
        HeldWorld world(
            GPeerClientOptions{
                .name = "wide client",
                .mode = BIN,
                .prefetchDepth = 3,
                .parallelEvaluations = 2,
                .undeliveredDir = dir.path()
            },
            [permits, started](GHeldDecodeItem &item) {
                started->fetch_add(1);
                permits->acquire();
                item.markAsProcessedWith({true});
            }
        );
        auto items = heldItems(3);
        auto batch = world.batch(items);
        REQUIRE(eventually([&] { return started->load() == 2 && decodeWaits.load(); }));
        world.client().flagCloseRequested();
        const auto server_end = world.serverEnd();
        REQUIRE(server_end);
        const std::size_t delivered = server_end->nDelivered();
        permits->release(); // the first result: a RESULT behind P
        REQUIRE(eventually([&] { return server_end->nDelivered() > delivered; }));
        GatedConnector::hold(world.gate());
        world.route().clear();
        const auto client_end = world.watch().last();
        REQUIRE(client_end);
        client_end->sever(); // halted: the client winds up without a connection
        REQUIRE(eventually([&] { return client_end->ended(); }));
        const auto booked = world.client().messagesBooked();
        decodeHeld.store(false);
        REQUIRE(eventually([&] { return world.client().messagesBooked() > booked; })); // B
        permits->release(); // the second result
        REQUIRE(eventually([&] { return GatedConnector::waiting(world.gate()); }));
        GatedConnector::release(world.gate());
        REQUIRE(eventually([&] { return world.clientDone(); }));
        world.shell().stopServer();
        batch.get();
    }
    CHECK(dir.files().size() == 2);
}

/******************************************************************************/
// A halt while a connection is being opened is a halt without a connection.

namespace {

/** @brief A peer that never opens, and whose close is confirmed at once (CLOSED_HERE): a
 *  connection that a client gives up while it is being opened. It counts what is sent on it. */
class NeverOpenPeer final
  : public GPeerI
  , public std::enable_shared_from_this<NeverOpenPeer> {
public:
    /** @brief @param executor The client's executor */
    explicit NeverOpenPeer(boost::asio::any_io_executor executor)
      : executor_(std::move(executor)) { /* nothing */
    }
    /** @brief Keeps the handlers; never opens @param handlers The role's */
    void start(GPeerHandlers handlers) override {
        handlers_ = std::move(handlers);
        work_.emplace(boost::asio::make_work_guard(executor_));
        started_->store(true);
    }
    /** @brief Counts @param message Dropped */
    void send([[maybe_unused]] Gem::Courtier::Consumers::GMessage message) override {
        n_sent_->fetch_add(1);
    }
    /** @brief The other end confirms at once @param how What the close says */
    void close(GPeerClose how) override {
        if(closed_.exchange(true)) {
            return;
        }
        boost::asio::post(executor_, [self = this->shared_from_this(), how] {
            self->end_(how);
        });
    }
    /** @brief Ends at once */
    void abort() override {
        this->end_(GPeerClose::NORMAL);
    }
    /** @brief @return The client's executor */
    [[nodiscard]] boost::asio::any_io_executor executor() override {
        return executor_;
    }
    /** @brief @return Whether it was started */
    [[nodiscard]] std::shared_ptr<std::atomic<bool>> started() const {
        return started_;
    }
    /** @brief @return How many messages were sent on it */
    [[nodiscard]] std::shared_ptr<std::atomic<int>> nSent() const {
        return n_sent_;
    }

private:
    void end_(GPeerClose how) {
        auto handlers = std::move(handlers_);
        handlers_ = GPeerHandlers{};
        work_.reset();
        if(handlers.onClosed) {
            handlers.onClosed(GPeerEnd::CLOSED_HERE, how, "closed and confirmed");
        }
    }

    boost::asio::any_io_executor executor_;
    GPeerHandlers handlers_;
    std::optional<boost::asio::executor_work_guard<boost::asio::any_io_executor>> work_;
    std::atomic<bool> closed_{false};
    std::shared_ptr<std::atomic<bool>> started_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<int>> n_sent_ = std::make_shared<std::atomic<int>>(0);
};

/** @brief The second connection is a NeverOpenPeer (the connector keeps it, as a transport keeps
 *  its peers); every other one goes to the in-process server */
class SecondNeverOpens final : public Gem::Courtier::Consumers::GConnectorI {
public:
    /** @brief @param inner The connector of the other connections */
    explicit SecondNeverOpens(std::unique_ptr<GInProcessConnector> inner)
      : inner_(std::move(inner)) { /* nothing */
    }
    /** @brief @param executor Where it runs @param done What is told the outcome */
    void connect(boost::asio::any_io_executor executor, done_type done) override {
        if(n_connects_++ != 1) {
            inner_->connect(std::move(executor), std::move(done));
            return;
        }
        peer_ = std::make_shared<NeverOpenPeer>(executor);
        made_->store(peer_);
        boost::asio::post(executor, [peer = peer_, done = std::move(done)]() mutable {
            done(peer, {});
        });
    }
    /** @brief Gives up the inner connector's attempt */
    void cancel() override {
        inner_->cancel();
    }
    /** @brief @return true */
    [[nodiscard]] bool reconnectable() const override {
        return true;
    }
    /** @brief @return Where the NeverOpenPeer made is shown (from any thread) */
    [[nodiscard]] std::shared_ptr<std::atomic<std::shared_ptr<NeverOpenPeer>>> made() const {
        return made_;
    }

private:
    std::unique_ptr<GInProcessConnector> inner_;
    std::size_t n_connects_ = 0;
    std::shared_ptr<NeverOpenPeer> peer_;
    std::shared_ptr<std::atomic<std::shared_ptr<NeverOpenPeer>>> made_ =
        std::make_shared<std::atomic<std::shared_ptr<NeverOpenPeer>>>();
};

} /* anonymous namespace */

/**
 * A result is sent on a connection whose server end loses it, and the connection is then lost.
 * The client halts while its next connection is being opened: that is a halt without a
 * connection. The client gives the opening connection up (nothing is sent on it) and winds up --
 * its evaluations have ended -- with one last attempt, which delivers the result. (The client
 * used to close the opening connection and end with its close: the result was saved although a
 * connection to deliver it on could be made, and a halt while opening was a second way to end
 * without a connection.)
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: a halt while a connection is being opened winds up with a last attempt",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    auto evaluations = std::make_shared<Evaluations>();
    // A server's end loses what comes after its first message: on the first connection the
    // result (after the request), on the last attempt nothing (the result comes first)
    World world(GInProcessListener::Options{
        .serverFaults = Gem::Courtier::Tests::GInProcessFaults{.deliverAtMost = 1}
    });
    auto watch = std::make_shared<GInProcessPeerWatch>();
    auto returned = std::make_shared<std::atomic<bool>>(false);
    watch->setOnMade([returned](GInProcessPeer &end) {
        end.setTransmitHook([returned](std::string_view bytes) {
            if(isReturn(bytes)) {
                returned->store(true);
            }
        });
    });
    auto connector = std::make_unique<SecondNeverOpens>(std::make_unique<GInProcessConnector>(
        std::make_shared<GInProcessRoute>(world.listener()),
        true,
        watch
    ));
    auto made = connector->made();
    auto client = std::make_shared<client_type>(
        std::move(connector),
        GPeerClientOptions{.name = "halting client", .mode = BIN, .undeliveredDir = dir.path()}
    );
    client->setItemProcessor(countingProcessor(evaluations, 0ms));
    auto done = std::make_shared<std::atomic<bool>>(false);
    {
        const std::jthread runner([client, done] {
            client->run();
            done->store(true);
        });
        auto items = makeBatch(1);
        auto batch = std::async(std::launch::async, [&] { return world.runBatch(items); });
        REQUIRE(eventually([&] { return returned->load(); })); // sent, and lost there
        watch->last()->sever();
        REQUIRE(eventually([&] {
            const auto peer = made->load();
            return peer != nullptr && peer->started()->load();
        }));
        client->flagCloseRequested(); // while the second connection is being opened
        REQUIRE(eventually([&] { return done->load(); }));
        CHECK(made->load()->nSent()->load() == 0);
        CHECK(batch.get()); // the last attempt delivered the result
    }
    CHECK(evaluations->of(0) == 1);
    CHECK(client->connections() == 2); // the first, and the last attempt
    CHECK(dir.files().empty());
}

/******************************************************************************/
// One way to end without a connection: a close that waited for a result to be encoded once more,
// and a protocol error found while the lost connection's messages are decoded, end the client by
// the same rule as a halt without a connection (a last attempt, then the results are saved).

/**
 * A halt while a result is evaluated: the result was encoded as a RETURN and is encoded once more
 * as a RESULT (held here), and the close waits for it. The connection is lost meanwhile. When the
 * result is ready, the client has no connection: it makes its last attempt (refused here) and
 * saves the result. (The close used to survive the loss; the client then ended without saving.)
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: a close that waits for a result does not survive the loss of its connection",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    gatedDecodes.store(0);
    heldDecode.store(std::numeric_limits<std::size_t>::max());
    processedSaves.store(0);
    heldSave.store(1); // the second encoding of the processed item: the RESULT
    saveHeld.store(true);
    saveWaits.store(false);
    auto permits = std::make_shared<std::counting_semaphore<4>>(0);
    auto started = std::make_shared<std::atomic<int>>(0);
    {
        HeldWorld world(
            GPeerClientOptions{.name = "halting client", .mode = BIN, .undeliveredDir = dir.path()},
            [permits, started](GHeldDecodeItem &item) {
                started->fetch_add(1);
                permits->acquire();
                item.markAsProcessedWith({true});
            }
        );
        auto items = heldItems(1);
        auto batch = world.batch(items);
        REQUIRE(eventually([&] { return started->load() == 1; }));
        world.client().flagCloseRequested(); // the compute thread still encodes a RETURN
        permits->release();
        REQUIRE(eventually([] { return saveWaits.load(); }));
        const auto polls = world.client().haltPolls();
        // The halt poll has seen the halt: the close waits for the result
        REQUIRE(eventually([&] { return world.client().haltPolls() > polls; }));
        GatedConnector::hold(world.gate());
        const auto client_end = world.watch().last();
        REQUIRE(client_end);
        client_end->sever();
        REQUIRE(eventually([&] { return client_end->ended(); }));
        saveHeld.store(false);
        // The result has come: the client, without a connection, makes its last attempt
        REQUIRE(eventually([&] { return GatedConnector::waiting(world.gate()); }));
        GatedConnector::release(world.gate());
        REQUIRE(eventually([&] { return world.clientDone(); }));
        world.shell().stopServer();
        batch.get();
    }
    saveHeld.store(false);
    heldSave.store(std::numeric_limits<std::size_t>::max());
    CHECK(dir.files().size() == 1);
}

/**
 * Depth 2: A is evaluated and returned, B's decode is held, and the server's answer to A's RETURN
 * is a frame a client cannot accept (a protocol error), which is decoded after B. The connection
 * is lost while B is held. The protocol error is found while the lost connection's messages are
 * decoded: the client ends without a connection by the same rule -- A's result (unproven: the
 * answer to its request was the bad frame) and B's are saved when the last attempt is refused.
 * (The client used to end at once and drop A.)
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: a protocol error found in the drain ends the client by the rule of a halt",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    gatedDecodes.store(0);
    heldDecode.store(1); // B
    decodeHeld.store(true);
    decodeWaits.store(false);
    {
        HeldWorld world(
            GPeerClientOptions{
                .name = "client",
                .mode = BIN,
                .prefetchDepth = 2,
                .undeliveredDir = dir.path()
            },
            {},
            2 // the server's third message (the answer to A's RETURN) is a bad frame
        );
        auto items = heldItems(3);
        auto batch = world.batch(items);
        REQUIRE(eventually([] { return decodeWaits.load(); }));
        REQUIRE(eventually([&] { return world.corrupted(); }));
        // The bad frame (the server's third message) waits behind B
        const auto client_end = world.watch().last();
        REQUIRE(client_end);
        REQUIRE(eventually([&] { return client_end->nDelivered() == 3; }));
        GatedConnector::hold(world.gate());
        client_end->sever();
        REQUIRE(eventually([&] { return world.client().losses() == 1; }));
        decodeHeld.store(false);
        // The protocol error is found in the drain: the client makes its last attempt
        REQUIRE(eventually([&] { return GatedConnector::waiting(world.gate()); }));
        GatedConnector::release(world.gate());
        REQUIRE(eventually([&] { return world.clientDone(); }));
        world.shell().stopServer();
        batch.get();
    }
    CHECK(dir.files().size() == 2);
}

/******************************************************************************/
/**
 * After its own close with ERROR (here: a frame a client cannot accept) the client starts nothing
 * new: of the items it holds (depth 3, one evaluation at a time) only the one being evaluated is
 * evaluated, and its result is saved; the held one is dropped (the server puts it back), and no
 * file is written for it.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: after its close with ERROR a client starts no evaluation",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    gatedDecodes.store(0);
    heldDecode.store(std::numeric_limits<std::size_t>::max());
    auto permits = std::make_shared<std::counting_semaphore<4>>(0);
    auto started = std::make_shared<std::atomic<int>>(0);
    {
        HeldWorld world(
            GPeerClientOptions{
                .name = "client",
                .mode = BIN,
                .prefetchDepth = 3,
                .undeliveredDir = dir.path()
            },
            [permits, started](GHeldDecodeItem &item) {
                started->fetch_add(1);
                permits->acquire();
                item.markAsProcessedWith({true});
            },
            2 // the server's third message is a bad frame
        );
        auto items = heldItems(3);
        auto batch = world.batch(items);
        REQUIRE(eventually([&] { return started->load() == 1 && world.corrupted(); }));
        // The bad frame (the server's third message) is decoded: the client closes
        REQUIRE(eventually([&] { return world.client().messagesBooked() >= 3; }));
        permits->release(3);
        REQUIRE(eventually([&] { return world.clientDone(); }));
        world.shell().stopServer();
        batch.get();
    }
    CHECK(started->load() == 1);
    CHECK(dir.files().size() == 1);
}

/******************************************************************************/
// A finished result that can no longer be delivered is saved: the evaluated item, one file each,
// in the client's undeliveredDir. Evaluations are never cut; a client that halts without a
// connection makes one last attempt to deliver what it has finished.

namespace {

/** @brief @param file A saved item @return The item it holds, loaded as the type loads; empty if it
 *  cannot be loaded */
std::shared_ptr<GFaultyContainer> loadSaved(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return Gem::Common::sharedPtrFromString<GFaultyContainer>(bytes, BIN);
}

/** @brief A batch that may end without its results: its slots are then filled with clones of a
 *  template (not fatal) @param world Where it runs @param items Its items @return It, running */
BatchRun openBatch(World &world, std::vector<std::unique_ptr<GFaultyContainer>> &items) {
    return {std::async(std::launch::async, [&world, &items] {
        auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
        policy.fatal_on_none_returned = false;
        auto clone_template = std::make_unique<GFaultyContainer>(99);
        clone_template->markAsProcessedWith({true});
        world.shell()
            .processBatch(std::span(items.data(), items.size()), policy, std::move(clone_template));
    }), [&world] { world.shell().stopServer(); }};
}

} /* anonymous namespace */

/** A client that halts while it has no connection and evaluates an item lets the evaluation end and
 *  delivers its result with one last attempt; nothing is saved. */
TEST_CASE(
    "GPeerClientT: a halt without a connection delivers the running evaluation with a last attempt",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "halting client", .mode = BIN, .undeliveredDir = dir.path()},
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
    client->flagCloseRequested();
    // From here on no attempt is made, whether the halt poll has run or not: an attempt is made
    // only by the retry timer, which looks at the halt itself, and an attempt under way was
    // refused (the route is cleared) long before the route is set again. The wait is not
    // load-bearing.
    std::this_thread::sleep_for(1500ms);
    const std::size_t attempts_before = world.listener()->nConnections();
    world.route().set(world.listener());
    std::this_thread::sleep_for(500ms);
    CHECK(world.listener()->nConnections() == attempts_before); // it does not try while it halts

    gate->open();
    REQUIRE(batch.get());
    REQUIRE(eventually([&] { return world.clientsDone(); }));
    CHECK(evaluations->of(0) == 1);
    CHECK(client->connections() == 2); // the last attempt
    CHECK(dir.files().empty());
}

/** The same, and the last attempt fails: the evaluated item is saved, and it loads back. */
TEST_CASE(
    "GPeerClientT: a result that cannot be delivered after a halt is saved as the evaluated item",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(41));
    {
        World world;
        auto client = world.client(
            GPeerClientOptions{.name = "halting client", .mode = BIN, .undeliveredDir = dir.path()},
            evaluations,
            0ms,
            watch,
            withGate(evaluations, gate)
        );
        auto batch = openBatch(world, items);
        REQUIRE(eventually([&] { return gate->waiting.load() == 1; }));
        world.route().clear();
        watch->last()->sever();
        REQUIRE(eventually([&] { return client->losses() == 1; }));
        client->flagCloseRequested();
        std::this_thread::sleep_for(1500ms);
        gate->open();
        REQUIRE(eventually([&] { return world.clientsDone(); }));
        CHECK(client->connections() == 1); // the last attempt was refused
        world.shell().stopServer();
        batch.get();
    }
    const auto files = dir.files();
    REQUIRE(files.size() == 1);
    INFO("saved as " << files.front().string());
    const auto saved = loadSaved(files.front());
    REQUIRE(saved);
    CHECK(saved->get_stored_number() == 41);
    CHECK(saved->is_processed());
}

/** A client halts while connected and sends its last result before its close; the server reads
 *  neither and does not confirm the close. Only a confirmed close proves the result (GPeerI.hpp,
 *  rule 6): it is saved, and it loads back. */
TEST_CASE(
    "GPeerClientT: a result sent before a close the server does not confirm is saved",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(23));
    {
        // The server's end reads the client's first message (its request) and nothing after it,
        // and ignores the client's close.
        World world(GInProcessListener::Options{
            .serverFaults =
                Gem::Courtier::Tests::GInProcessFaults{.deliverAtMost = 1, .acknowledgeClose = false},
            .closeGrace = 300ms
        });
        auto client = world.client(
            GPeerClientOptions{.name = "halting client", .mode = BIN, .undeliveredDir = dir.path()},
            evaluations,
            0ms,
            nullptr,
            withGate(evaluations, gate)
        );
        auto batch = openBatch(world, items);
        REQUIRE(eventually([&] { return gate->waiting.load() == 1; }));
        client->flagCloseRequested(); // a halt while connected: the result goes out, then the close
        gate->open();
        REQUIRE(eventually([&] { return world.clientsDone(); }));
        CHECK(client->connectionEnd() == GPeerEnd::CLOSED_HERE_UNCONFIRMED);
        CHECK(client->connections() == 1);
        CHECK(client->resultsSaved() == 1);
        world.shell().stopServer();
        batch.get();
    }
    const auto files = dir.files();
    REQUIRE(files.size() == 1);
    INFO("saved as " << files.front().string());
    const auto saved = loadSaved(files.front());
    REQUIRE(saved);
    CHECK(saved->get_stored_number() == 23);
    CHECK(saved->is_processed());
}

/** The server's end closes with ERROR (as a session does after a message it cannot read) while a
 *  client that halts sends the result of its last evaluation and closes. A result that arrives
 *  after the server's end was closed, but before its close went out, is handed to the session
 *  and checked in; one that arrives after the server's close went out is dropped there -- and
 *  then the two closes have crossed, which confirms neither (GPeerI.hpp, rule 6), so the client
 *  saves it. Either way the result is not lost, and it is evaluated once. */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario in two orders; Catch2's macros count
TEST_CASE(
    "GPeerClientT: a result is not lost when the server's end closes as the client closes",
    "[courtier][peer][client]"
) {
    const bool server_close_out_first = GENERATE(true, false);
    const UndeliveredDir dir;
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    auto client_close_out = std::make_shared<std::atomic<bool>>(false);
    watch->setOnMade([client_close_out](GInProcessPeer &end) {
        end.setTransmitHook([client_close_out](std::string_view payload) {
            if(payload.size() == 1) {
                client_close_out->store(true); // a close's word (a frame is longer)
            }
        });
    });
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(17));
    bool batch_done = false;
    {
        World world;
        auto client = world.client(
            GPeerClientOptions{.name = "halting client", .mode = BIN, .undeliveredDir = dir.path()},
            evaluations,
            0ms,
            watch,
            withGate(evaluations, gate)
        );
        auto batch = openBatch(world, items);
        REQUIRE(eventually([&] { return gate->waiting.load() == 1; }));
        auto server_end = world.listener()->lastServerEnd();
        REQUIRE(server_end);

        // The strand of the server's end is held: what the client sends waits there, and so does
        // the server's own close, in the order of the calls.
        const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
        boost::asio::post(server_end->executor(), [held = release]() {
            held->wait_for(PATIENCE);
        });
        if(server_close_out_first) {
            server_end->close(GPeerClose::ERROR); // goes out before the result arrives
        }
        client->flagCloseRequested(); // the client closes once its evaluation has ended
        gate->open();
        REQUIRE(eventually([&] { return client_close_out->load(); })); // the result, the close
        if(not server_close_out_first) {
            server_end->close(GPeerClose::ERROR); // after the result arrived, before it is read
        }
        release->count_down();
        REQUIRE(eventually([&] { return world.clientsDone(); }));
        // Done at once when the session checked the result in; never done when it was dropped
        batch_done = batch.wait_for(server_close_out_first ? 0s : PATIENCE) ==
                     std::future_status::ready;
        world.shell().stopServer();
        batch.get();
    }
    CHECK(evaluations->of(17) == 1);
    if(server_close_out_first) {
        CHECK_FALSE(batch_done);
        REQUIRE(dir.files().size() == 1); // the closes crossed: the result is saved
        const auto saved = loadSaved(dir.files().front());
        REQUIRE(saved);
        CHECK(saved->get_stored_number() == 17);
    }
    else {
        CHECK(batch_done); // the session was handed the result after its close and checked it in
        CHECK(dir.files().empty());
    }
}

/** The reconnect limit runs out while the client holds a finished result: it is saved. */
TEST_CASE(
    "GPeerClientT: a result held when the reconnect limit runs out is saved",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(7));
    {
        World world;
        auto client = world.client(
            GPeerClientOptions{
                .name = "limited client",
                .mode = BIN,
                .reconnectLimit = 1s,
                .contactTime = 300ms,
                .undeliveredDir = dir.path()
            },
            evaluations,
            0ms,
            watch,
            withGate(evaluations, gate)
        );
        auto batch = openBatch(world, items);
        REQUIRE(eventually([&] { return gate->waiting.load() == 1; }));
        std::this_thread::sleep_for(1200ms); // open for the contact time: contact
        world.route().clear();
        watch->last()->sever();
        gate->open(); // finished while there is no connection
        REQUIRE(eventually([&] { return world.clientsDone(); }));
        CHECK(client->terminalErrorFlagged());
        world.shell().stopServer();
        batch.get();
    }
    const auto files = dir.files();
    REQUIRE(files.size() == 1);
    const auto saved = loadSaved(files.front());
    REQUIRE(saved);
    CHECK(saved->get_stored_number() == 7);
}

/**
 * A result counts its sends only where it was handed to an open connection: one that finished
 * while there was no connection is sent on the next one for the first time, and a loss of that
 * connection does not bring it to the resend limit. Here it is sent on a second connection, which
 * is lost before anything answers it, and delivered on a third.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: a result is counted as sent only on an open connection",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(11));
    World world;
    auto client = world.client(
        GPeerClientOptions{.name = "client", .mode = BIN},
        evaluations,
        0ms,
        watch,
        withGate(evaluations, gate)
    );
    auto batch = openBatch(world, items);
    REQUIRE(eventually([&] { return gate->waiting.load() == 1; }));
    world.route().clear();
    watch->last()->sever();
    REQUIRE(eventually([&] { return client->losses() == 1; }));
    gate->open(); // finished while there is no connection: appended, not sent
    std::this_thread::sleep_for(200ms);
    world.route().set(world.listener());
    REQUIRE(eventually([&] { return client->connections() == 2; }));
    REQUIRE(batch.wait_for(PATIENCE) == std::future_status::ready); // the server has the result
    batch.get();
    watch->last()->sever(); // nothing has answered the result's request on this connection
    REQUIRE(eventually([&] { return client->losses() == 2 && client->connections() == 3; }));
    CHECK(client->resultsGivenUp() == 0);
    CHECK(client->resultsResent() == 1); // on the third connection only
    CHECK(evaluations->of(11) == 1);
    client->flagCloseRequested();
}

/** After the server has announced the end of the run nobody needs the result: nothing is saved. */
TEST_CASE(
    "GPeerClientT: a result finished after the end of the run is not saved",
    "[courtier][peer][client]"
) {
    const UndeliveredDir dir;
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(3));
    {
        World world;
        world.client(
            GPeerClientOptions{.name = "late client", .mode = BIN, .undeliveredDir = dir.path()},
            evaluations,
            0ms,
            nullptr,
            withGate(evaluations, gate)
        );
        auto batch = openBatch(world, items);
        REQUIRE(eventually([&] { return gate->waiting.load() == 1; }));
        world.shell().stopServer(); // RUN_ENDED while the item is evaluated
        batch.get();
        gate->open();
        REQUIRE(eventually([&] { return world.clientsDone(); }));
    }
    CHECK(dir.files().empty());
}

/** A directory that cannot be written: a warning, and the client ends as it would. */
TEST_CASE(
    "GPeerClientT: a result that cannot be saved is reported and the client ends",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(5));
    World world;
    auto client = world.client(
        GPeerClientOptions{
            .name = "client",
            .mode = BIN,
            .undeliveredDir = "/proc/geneva-cannot-be-written-here"
        },
        evaluations,
        0ms,
        watch,
        withGate(evaluations, gate)
    );
    auto batch = openBatch(world, items);
    REQUIRE(eventually([&] { return gate->waiting.load() == 1; }));
    world.route().clear();
    watch->last()->sever();
    REQUIRE(eventually([&] { return client->losses() == 1; }));
    client->flagCloseRequested();
    gate->open();
    CHECK(eventually([&] { return world.clientsDone(); }));
    CHECK(client->resultsUnsaved() == 1);
    world.shell().stopServer();
    batch.get();
}

/******************************************************************************/
// WORK that arrives after the client's own close (and before the end of the connection: GPeerI.hpp,
// rule 6 lets it through) starts nothing, and the server hands its item out again.

namespace {

/** @brief What a LateWorkPeer shares with the test */
struct LateWork {
    std::atomic<std::size_t> nWork{0};      ///< WORK frames that reached the end
    std::atomic<bool> held{false};          ///< The second WORK frame is held back
    std::atomic<bool> delivered{false};     ///< ... and was handed to the client after its close()
};

/** @brief A client's end that holds the second WORK frame back and hands it to the client right
 *  after the client's close(), on the end's executor -- as a transport hands on what arrives after
 *  close() until its own close has gone out. It is owned by the handlers it installs on the end it
 *  wraps, as a role is (rule 4). */
class LateWorkPeer final
  : public GPeerI
  , public std::enable_shared_from_this<LateWorkPeer> {
public:
    /** @brief @param inner The end it wraps @param late Shared with the test */
    LateWorkPeer(const std::shared_ptr<GPeerI> &inner, std::shared_ptr<LateWork> late)
      : inner_(inner)
      , executor_(inner->executor())
      , late_(std::move(late)) { /* nothing */
    }
    /** @brief Wraps the client's onMessage @param handlers See GPeerI */
    void start(GPeerHandlers handlers) override {
        auto inner = inner_.lock();
        if(not inner) {
            return;
        }
        on_message_ =
            std::make_shared<std::move_only_function<void(std::string &&)>>(std::move(handlers.onMessage));
        inner->start(
            GPeerHandlers{
                .onOpen = std::move(handlers.onOpen),
                .onMessage =
                    [self = this->shared_from_this()](std::string &&message) {
                        if(kindOf(message) == GFrameKind::WORK &&
                           self->late_->nWork.fetch_add(1) == 1) {
                            self->held_ = std::move(message);
                            self->late_->held.store(true);
                            return;
                        }
                        (*self->on_message_)(std::move(message));
                    },
                .onClosed = std::move(handlers.onClosed),
            }
        );
    }
    /** @brief Forwards @param message See GPeerI */
    void send(Gem::Courtier::Consumers::GMessage message) override {
        if(auto inner = inner_.lock()) {
            inner->send(std::move(message));
        }
    }
    /** @brief Forwards, then hands the held WORK to the client (on the client's thread)
     *  @param how See GPeerI */
    void close(GPeerClose how) override {
        if(auto inner = inner_.lock()) {
            inner->close(how);
        }
        if(held_) {
            boost::asio::post(
                executor_,
                [on_message = on_message_, late = late_, message = std::move(*held_)]() mutable {
                    (*on_message)(std::move(message));
                    late->delivered.store(true);
                }
            );
            held_.reset();
        }
    }
    /** @brief Forwards */
    void abort() override {
        if(auto inner = inner_.lock()) {
            inner->abort();
        }
    }
    /** @brief @return The wrapped end's executor */
    [[nodiscard]] boost::asio::any_io_executor executor() override {
        return executor_;
    }

private:
    std::weak_ptr<GPeerI> inner_;
    boost::asio::any_io_executor executor_;
    std::shared_ptr<LateWork> late_;
    std::shared_ptr<std::move_only_function<void(std::string &&)>> on_message_;
    std::optional<std::string> held_; ///< On the client's thread
};

/** @brief An in-process connector whose ends are LateWorkPeers */
class LateWorkConnector final : public Gem::Courtier::Consumers::GConnectorI {
public:
    /** @brief @param inner The in-process connector @param late Shared with the ends */
    LateWorkConnector(std::unique_ptr<GInProcessConnector> inner, std::shared_ptr<LateWork> late)
      : inner_(std::move(inner))
      , late_(std::move(late)) { /* nothing */
    }
    /** @brief @param executor Where it runs @param done What is told the outcome */
    void connect(boost::asio::any_io_executor executor, done_type done) override {
        inner_->connect(
            std::move(executor),
            [late = late_, done = std::move(done)](
                const std::shared_ptr<GPeerI> &peer,
                std::string_view error
            ) mutable {
                done(peer ? std::make_shared<LateWorkPeer>(peer, late) : nullptr, error);
            }
        );
    }
    /** @brief Forwards */
    void cancel() override {
        inner_->cancel();
    }
    /** @brief @return true */
    [[nodiscard]] bool reconnectable() const override {
        return true;
    }

private:
    std::unique_ptr<GInProcessConnector> inner_;
    std::shared_ptr<LateWork> late_;
};

} /* anonymous namespace */

/**
 * Depth 2, one evaluation at a time: the client evaluates item A; the WORK of item B is held back
 * by its end. The client halts: A's result goes out, then the close, and B's WORK reaches the
 * client after its close(). The client starts nothing; the server puts B back when the connection
 * ends, and a second client evaluates it.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: work that arrives after the client's close is not evaluated and goes elsewhere",
    "[courtier][peer][client]"
) {
    auto first = std::make_shared<Evaluations>();
    auto second = std::make_shared<Evaluations>();
    auto gate = std::make_shared<Gate>();
    auto late = std::make_shared<LateWork>();
    std::vector<std::unique_ptr<GFaultyContainer>> items = makeBatch(2);
    bool batch_done = false;
    {
        World world;
        auto client = std::make_shared<client_type>(
            std::make_unique<LateWorkConnector>(
                std::make_unique<GInProcessConnector>(
                    std::make_shared<GInProcessRoute>(world.listener()),
                    true,
                    nullptr
                ),
                late
            ),
            GPeerClientOptions{
                .name = "halting client",
                .mode = BIN,
                .prefetchDepth = 2,
                .parallelEvaluations = 1
            }
        );
        client->setItemProcessor(gatedProcessor(first, gate));
        auto done = std::make_shared<std::atomic<bool>>(false);
        const std::jthread runner([client, done] {
            client->run();
            done->store(true);
        });
        auto batch = openBatch(world, items);
        REQUIRE(eventually([&] { return gate->waiting.load() == 1 && late->held.load(); }));
        client->flagCloseRequested(); // A's result, then the close
        gate->open();
        REQUIRE(eventually([&] { return late->delivered.load(); })); // B, after the close()
        REQUIRE(eventually([&] { return done->load(); }));
        world.client(GPeerClientOptions{.name = "second client", .mode = BIN}, second, 0ms, nullptr, AS_IS);
        batch_done = batch.wait_for(PATIENCE) == std::future_status::ready;
        world.shell().stopServer();
        batch.get();
    }
    CHECK(batch_done);
    CHECK(first->total.load() == 1);  // A only
    CHECK(second->total.load() == 1); // B, given out again
    for(std::size_t i = 0; i < items.size(); ++i) {
        INFO("item " << i);
        CHECK(first->of(i) + second->of(i) == 1);
    }
}
