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
// in-process transport: the requirements of the client -- its halts and the end of the run, the
// parallel evaluations and the item processor, items that cannot be read, processed or encoded,
// connecting and its limits, lost connections and the resend of results, its own close.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
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

#include "GCapturedLog.hpp"
#include "GDemoProcessingContainers.hpp"
#include "GInProcessPeer.hpp"
#include "GInProcessTestSupport.hpp"
#include "common/GGemCommit.hpp"
#include "common/GLogger.hpp"
#include "common/GModuleManifest.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GProcessingContainerT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/GWireCodec.hpp"
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
using Gem::Courtier::Tests::CapturedLog;
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
 * What the server's ends of all connections saw: PULLs that have no answer (WORK) yet, per
 * connection and at most, the RESULTs that arrived, and the frames of other kinds a client sent.
 */
struct ServerCounts {
    std::atomic<std::size_t> maxUnanswered{0}; ///< The most PULLs without an answer, anywhere
    std::atomic<std::size_t> nPulls{0};        ///< PULL frames received
    std::atomic<std::size_t> nResults{0};      ///< RESULT frames received
    std::atomic<std::size_t> nOther{0};        ///< Frames of any other kind received
    std::atomic<std::size_t> nWork{0};         ///< WORK frames sent
    std::atomic<std::size_t> overAnswered{0};  ///< WORKs sent beyond the PULLs received

    /** @brief Notes the unanswered requests of one connection @param n Their number */
    void unanswered(std::size_t n) {
        std::size_t seen = maxUnanswered.load();
        while(n > seen && not maxUnanswered.compare_exchange_weak(seen, n)) {
        }
    }
};

/** @brief @param bytes An encoded frame @return Its kind (read from its head) */
GFrameKind kindOf(const std::string &bytes) {
    GCommandContainerT<GFaultyContainer> frame{GFrameKind::NONE};
    std::ignore = Gem::Courtier::frameHeadFromString(std::string_view{bytes}, frame, BIN);
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
                        if(kind == GFrameKind::PULL) {
                            ++self->requests_;
                            self->counts_->nPulls.fetch_add(1);
                            self->counts_->unanswered(self->requests_ - self->answers_);
                        }
                        else if(kind == GFrameKind::RESULT) {
                            self->counts_->nResults.fetch_add(1);
                        }
                        else {
                            self->counts_->nOther.fetch_add(1);
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
        if(kind == GFrameKind::WORK) {
            ++answers_;
            if(answers_ > requests_) {
                counts_->overAnswered.fetch_add(1);
            }
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
    std::weak_ptr<GPeerI> inner_;           ///< The wrapped end
    boost::asio::any_io_executor executor_; ///< The wrapped end's executor, kept
    std::shared_ptr<ServerCounts> counts_;  ///< Where it reports
    std::size_t requests_ = 0;              ///< PULLs received (on the strand)
    std::size_t answers_ = 0;               ///< WORKs sent (on the strand)
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
    /** @brief Forwards @return The listener's interval */
    [[nodiscard]] std::chrono::milliseconds keepAlive() const override {
        return listener_->keepAlive();
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

} /* anonymous namespace */

/******************************************************************************/
/**
 * A client with an item limit sends its last result and closes; the server has every result. Every
 * result is a RESULT, and none asks for anything: the client asks with PULLs.
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
    CHECK(world.counts().nResults.load() == LIMIT);
    CHECK(world.counts().nOther.load() == 0);
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

/** @brief @return The options of the client of a WatchWorld: depth 2, one evaluation at a time */
GPeerClientOptions watchedClient() {
    return GPeerClientOptions{
        .name = "watched client",
        .mode = BIN,
        .prefetchDepth = 2,
        .parallelEvaluations = 1
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
 * A client that cannot load an item returns it as failed -- a RESULT of the ids in the head of its
 * WORK, without a result -- and stays. The server fails that slot (the policy fills it) and sends the
 * client its blobs in full again: the item's blob never reached the client, and an item that
 * referred to it by id could not be loaded either. The batch is done; exactly one load failed.
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
    // The client's close follows its last RESULT on the connection, and the server's end reads in
    // order: once the client's session has left the shell, every RESULT it sent has been counted.
    const bool limited_closed = eventually([&] { return world.shell().getNActiveSessions() == 0; });
    const std::size_t n_results = world.counts().nResults.load();
    world.client(
        GPeerClientOptions{.name = "other client", .mode = BIN},
        other,
        5ms,
        nullptr,
        AS_IS
    );
    REQUIRE(batch.get());
    REQUIRE(limited_done);
    REQUIRE(limited_closed);
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

/** @brief A listener whose server ends close every connection as soon as it is open, and its
 *  threads. It refers to nothing else. */
class DroppingServer {
public:
    /** @brief @param how What the close says; NORMAL: closed at once (no HELLO), ERROR: greeted with
     *  a HELLO and closed with ERROR on the client's first message */
    explicit DroppingServer(GPeerClose how = GPeerClose::NORMAL) {
        listener_->start(io_.executor(), [how](const std::shared_ptr<GPeerI> &peer, GWirePeerId) {
            const std::weak_ptr<GPeerI> weak = peer;
            peer->start(
                GPeerHandlers{
                    .onOpen =
                        [weak, how] {
                            if(auto end = weak.lock(); end && how == GPeerClose::ERROR) {
                                end->send(
                                    Gem::Courtier::Consumers::makeMessage(
                                        Gem::Courtier::buildHello<GFaultyContainer>(BIN, 0ms)
                                    )
                                );
                            }
                        },
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
    CHECK(world.listener()->nConnections() == 1);

    world.route().set(world.listener());
    REQUIRE(batch.get());
    CHECK(evaluations->of(0) == 1);
    CHECK(world.listener()->nConnections() == 2);
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
    REQUIRE(eventually([&] { return world.shell().parkedCount() == 1; })); // its PULL waits
    watch->last()->sever();
    REQUIRE(eventually([&] { return world.listener()->nConnections() == 2; }));

    auto items = makeBatch(3);
    REQUIRE(world.runBatch(items));
    CHECK(evaluations->total.load() == 3);
}

/******************************************************************************/
/** A server that closes with ERROR (a message whose head it could not read) ends the connection,
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
 * Contact. A server that accepts and drops at once sends no HELLO, so gives no contact: the pause grows, and the
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
        GPeerClientOptions{.name = "limited client", .mode = BIN, .reconnectLimit = 3s},
        evaluations,
        1ms,
        watch,
        AS_IS
    );
    auto items = makeBatch(1);
    REQUIRE(world.runBatch(items)); // a HELLO: contact

    world.route().set(dropping.listener()); // no HELLO there
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
    REQUIRE(world.runBatch(first));      // a HELLO: contact
    std::this_thread::sleep_for(2500ms); // parked for longer than the limit

    world.route().clear();
    watch->last()->sever();
    std::this_thread::sleep_for(500ms);
    world.route().set(world.listener());
    REQUIRE(eventually([&] { return world.listener()->nConnections() == 2; }));
    CHECK_FALSE(world.clientsDone());
    auto second = makeBatch(1, 1);
    REQUIRE(world.runBatch(second));
    CHECK_FALSE(client->terminalErrorFlagged());
}

namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<std::size_t> gatedDecodes{0};   ///< Work items whose decoding on a client has begun
std::atomic<std::size_t> heldDecode{1};     ///< Which decode waits (0: the first)
std::atomic<bool> decodeHeld{false};        ///< That decode waits while this is set
std::atomic<bool> decodeWaits{false};       ///< That decode has begun to wait
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

/**
 * @brief Holds nothing of GHeldDecodeItem, and counts from 0, from its construction on and again
 * after its destruction: a case that uses the holds declares one first and then sets the hold it
 * needs. A case that fails half-way (a REQUIRE) would otherwise leave its hold set for the next
 * case, whose encodings or decodes would then wait unasked.
 */
class HeldStateReset final {
public:
    /** @brief Resets the holds */
    HeldStateReset() {
        reset();
    }
    /** @brief Resets the holds again */
    ~HeldStateReset() {
        reset();
    }
    HeldStateReset(const HeldStateReset &) = delete;
    HeldStateReset(HeldStateReset &&) = delete;
    HeldStateReset &operator=(const HeldStateReset &) = delete;
    HeldStateReset &operator=(HeldStateReset &&) = delete;

private:
    /** @brief Nothing held, nothing counted */
    static void reset() {
        gatedDecodes.store(0);
        heldDecode.store(std::numeric_limits<std::size_t>::max());
        decodeHeld.store(false);
        decodeWaits.store(false);
        processedSaves.store(0);
        heldSave.store(std::numeric_limits<std::size_t>::max());
        saveHeld.store(false);
        saveWaits.store(false);
    }
};

} /* anonymous namespace */

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
    const HeldStateReset held_state;
    heldDecode.store(3); // the second item of the second batch
    decodeHeld.store(true);
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
        REQUIRE(eventually([&] { return listener->nConnections() == 2; }));
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
        // The halt poll looks at the limits while the decode is held
        std::this_thread::sleep_for(Gem::Courtier::GCLIENTHALTPOLLINTERVAL + 500ms);
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
                .onMessage = [self = this->shared_from_this(),
                              on_message = std::move(handlers.onMessage)](
                                 std::string &&message
                             ) mutable { on_message(std::move(message)); },
                .onClosed = std::move(handlers.onClosed),
            }
        );
    }
    /** @brief Forwards, or sends the bad frame @param message See GPeerI */
    void send(Gem::Courtier::Consumers::GMessage message) override {
        if(sent_->fetch_add(1) == at_) {
            const GCommandContainerT<GHeldDecodeItem> bad{GFrameKind::RESULT};
            message =
                Gem::Courtier::Consumers::makeMessage(Gem::Courtier::wireEncode(bad, nullptr, BIN));
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
    /** @brief Forwards @return The listener's interval */
    [[nodiscard]] std::chrono::milliseconds keepAlive() const override {
        return listener_->keepAlive();
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
        return {
            std::async(
                std::launch::async,
                [this, &items] {
                    auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
                    policy.fatal_on_none_returned = false;
                    auto clone_template = std::make_unique<GHeldDecodeItem>();
                    clone_template->markAsProcessedWith({true});
                    shell_.processBatch(
                        std::span(items.data(), items.size()),
                        policy,
                        std::move(clone_template)
                    );
                }
            ),
            [this] { shell_.stopServer(); }
        };
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

/******************************************************************************/
/**
 * After its own close with ERROR (here: a frame a client cannot accept) the client starts nothing
 * new: of the items it holds (depth 3, one evaluation at a time) only the one being evaluated is
 * evaluated; the held one is dropped (the server puts it back).
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; Catch2's macros count
TEST_CASE(
    "GPeerClientT: after its close with ERROR a client starts no evaluation",
    "[courtier][peer][client]"
) {
    const HeldStateReset held_state;
    auto permits = std::make_shared<std::counting_semaphore<4>>(0);
    auto started = std::make_shared<std::atomic<int>>(0);
    {
        HeldWorld world(
            GPeerClientOptions{.name = "client", .mode = BIN, .prefetchDepth = 3},
            [permits, started](GHeldDecodeItem &item) {
                started->fetch_add(1);
                permits->acquire();
                item.markAsProcessedWith({true});
            },
            3 // the server's fourth message (after the HELLO and two WORKs) is a bad frame
        );
        auto items = heldItems(3);
        auto batch = world.batch(items);
        REQUIRE(eventually([&] { return started->load() == 1 && world.corrupted(); }));
        // The bad frame is decoded (in-process, at once): the client ends
        std::this_thread::sleep_for(300ms);
        permits->release(3);
        REQUIRE(eventually([&] { return world.clientDone(); }));
        world.shell().stopServer();
        batch.get();
    }
    CHECK(started->load() == 1);
}

namespace {

/** @brief A batch that may end without its results: its slots are then filled with clones of a
 *  template (not fatal) @param world Where it runs @param items Its items @return It, running */
BatchRun openBatch(World &world, std::vector<std::unique_ptr<GFaultyContainer>> &items) {
    return {
        std::async(
            std::launch::async,
            [&world, &items] {
                auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
                policy.fatal_on_none_returned = false;
                auto clone_template = std::make_unique<GFaultyContainer>(99);
                clone_template->markAsProcessedWith({true});
                world.shell().processBatch(
                    std::span(items.data(), items.size()),
                    policy,
                    std::move(clone_template)
                );
            }
        ),
        [&world] { world.shell().stopServer(); }
    };
}

} /* anonymous namespace */

/**
 * The server's end closes with ERROR (as a session does after a message it cannot read) while a
 * client that halts sends the result of its last evaluation and closes. A result that arrives
 * after the server's end was closed, but before its close went out, is handed to the session and
 * checked in; one that arrives after the server's close went out is dropped there. The client
 * does not know which (delivery is best effort) and ends; a dropped result is not lost to the run:
 * the server hands the item out again when the connection ends, and another client evaluates it.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario in two orders; Catch2's macros count
TEST_CASE(
    "GPeerClientT: a result is not lost when the server's end closes as the client closes",
    "[courtier][peer][client]"
) {
    const bool server_close_out_first = GENERATE(true, false);
    auto evaluations = std::make_shared<Evaluations>();
    auto second = std::make_shared<Evaluations>();
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
    bool done_by_first = false;
    bool batch_done = false;
    {
        World world;
        auto client = world.client(
            GPeerClientOptions{.name = "halting client", .mode = BIN},
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
        boost::asio::post(server_end->executor(), [held = release]() { held->wait_for(PATIENCE); });
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
        // Done at once when the session checked the result in; not done when it was dropped
        done_by_first =
            batch.wait_for(server_close_out_first ? 0s : PATIENCE) == std::future_status::ready;
        if(not done_by_first) { // the item is handed out again, to a second client
            world.client(
                GPeerClientOptions{.name = "second client", .mode = BIN},
                second,
                0ms,
                nullptr,
                AS_IS
            );
        }
        batch_done = batch.wait_for(PATIENCE) == std::future_status::ready;
        world.shell().stopServer();
        batch.get();
    }
    CHECK(batch_done);
    CHECK(evaluations->of(17) == 1);
    CHECK(done_by_first == not server_close_out_first);
    CHECK(second->of(17) == (server_close_out_first ? 1U : 0U));
}

namespace {

/** @brief What a LateWorkPeer shares with the test */
struct LateWork {
    std::atomic<std::size_t> nWork{0};  ///< WORK frames that reached the end
    std::atomic<bool> held{false};      ///< The second WORK frame is held back
    std::atomic<bool> delivered{false}; ///< ... and was handed to the client after its close()
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
        on_message_ = std::make_shared<std::move_only_function<void(std::string &&)>>(
            std::move(handlers.onMessage)
        );
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
            ) mutable { done(peer ? std::make_shared<LateWorkPeer>(peer, late) : nullptr, error); }
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
        world.client(
            GPeerClientOptions{.name = "second client", .mode = BIN},
            second,
            0ms,
            nullptr,
            AS_IS
        );
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

/******************************************************************************/
/**
 * A result is always a RESULT, and it asks for nothing: the client asks for every item with a PULL,
 * and the server answers each PULL with one WORK. The client keeps no more PULLs unanswered than
 * its depth.
 */
TEST_CASE(
    "GPeerClientT: every result is a RESULT and every item is asked for with a PULL",
    "[courtier][peer][client]"
) {
    constexpr std::size_t N_ITEMS = 20;
    auto evaluations = std::make_shared<Evaluations>();
    std::shared_ptr<ServerCounts> counts;
    {
        World world;
        counts = world.sharedCounts();
        world.client(
            GPeerClientOptions{.name = "client", .mode = BIN, .prefetchDepth = 2},
            evaluations,
            5ms,
            nullptr,
            AS_IS
        );
        auto items = makeBatch(N_ITEMS);
        REQUIRE(world.runBatch(items));
    }
    CHECK(evaluations->total.load() == N_ITEMS);
    CHECK(counts->nResults.load() == N_ITEMS);
    CHECK(counts->nWork.load() == N_ITEMS);
    CHECK(counts->nOther.load() == 0);
    CHECK(counts->maxUnanswered.load() <= 2);
    CHECK(counts->overAnswered.load() == 0);
}

/******************************************************************************/
// A server of another build: its HELLO names another commit, or another program. The client ends
// with a terminal error, and asks for nothing. Where one side's program has no identity, the client
// goes on, and its log says so once per run.

namespace {

/// The commit a server of another build names in its HELLO (no build has it)
constexpr std::string_view OTHER_COMMIT = "0123456789abcdef0123456789abcdef01234567-dirty";
/// The Geneva release the test servers name in their HELLO
constexpr std::string_view OTHER_VERSION = "0.0.1-other";

/** @brief A listener whose server ends greet with a HELLO of a given build and program, and count
 *  what their clients send, and the HELLOs of this build among it; and its threads */
class GreetingServer {
public:
    /**
     * @param build The Geneva commit the HELLO names @param program The program it names
     * @param end_after 0: no connection is closed by the server; n: each connection is closed at the
     * client's first message, with ERROR on the first n - 1 connections and RUN_ENDED on the n-th
     * @param later_build The Geneva commit the HELLO names from the second connection on (empty:
     * @p build)
     */
    GreetingServer(
        std::string build,
        std::string program,
        std::size_t end_after = 0,
        std::string later_build = {}
    ) {
        listener_->start(
            io_.executor(),
            [first_build = std::move(build),
             later_build = std::move(later_build),
             program = std::move(program),
             end_after,
             received = received_,
             hellos = hellos_,
             connections = connections_](const std::shared_ptr<GPeerI> &peer, GWirePeerId) {
                const std::size_t connection = connections->fetch_add(1) + 1;
                const GPeerClose how =
                    connection < end_after ? GPeerClose::ERROR : GPeerClose::RUN_ENDED;
                const std::string build =
                    connection > 1 && not later_build.empty() ? later_build : first_build;
                const std::weak_ptr<GPeerI> weak = peer;
                peer->start(
                    GPeerHandlers{
                        .onOpen =
                            [weak, build, program] {
                                if(auto end = weak.lock()) {
                                    GCommandContainerT<GFaultyContainer> hello{GFrameKind::NONE};
                                    hello.setHello(build, program, 0ms, std::string(OTHER_VERSION));
                                    end->send(
                                        Gem::Courtier::Consumers::makeMessage(
                                            Gem::Courtier::container_to_string(hello, BIN)
                                        )
                                    );
                                }
                            },
                        .onMessage =
                            [weak, received, hellos, end_after, how](std::string &&message) {
                                received->fetch_add(1);
                                GCommandContainerT<GFaultyContainer> frame{GFrameKind::NONE};
                                try {
                                    Gem::Courtier::frameHeadFromString(message, frame, BIN);
                                }
                                catch(...) { // NOLINT(bugprone-empty-catch) -- counted above
                                }
                                if(frame.kind() == GFrameKind::HELLO &&
                                   frame.build() == Gem::Common::gemCommit() &&
                                   frame.release() == GEM_VERSION_STRING) {
                                    hellos->fetch_add(1);
                                }
                                if(auto end = weak.lock(); end && end_after > 0) {
                                    end->close(how);
                                }
                            },
                        .onClosed = [](GPeerEnd, GPeerClose, std::string_view) {},
                    }
                );
            }
        );
    }
    /** @brief Stops the listener while its threads run */
    ~GreetingServer() {
        try {
            listener_->stop();
        }
        catch(...) { // NOLINT(bugprone-empty-catch) -- it does not throw here
        }
        listener_->endWork();
    }
    GreetingServer(const GreetingServer &) = delete;
    GreetingServer(GreetingServer &&) = delete;
    GreetingServer &operator=(const GreetingServer &) = delete;
    GreetingServer &operator=(GreetingServer &&) = delete;

    /** @brief @return The listener (for a route) */
    [[nodiscard]] const std::shared_ptr<GInProcessListener> &listener() const {
        return listener_;
    }
    /** @brief @return How many messages the clients sent */
    [[nodiscard]] std::size_t received() const {
        return received_->load();
    }
    /** @brief @return How many of them were a HELLO of this build (a client that does not match) */
    [[nodiscard]] std::size_t hellos() const {
        return hellos_->load();
    }

private:
    IoThreads io_{2};
    std::shared_ptr<GInProcessListener> listener_ =
        std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    std::shared_ptr<std::atomic<std::size_t>> received_ =
        std::make_shared<std::atomic<std::size_t>>(0);
    std::shared_ptr<std::atomic<std::size_t>> hellos_ =
        std::make_shared<std::atomic<std::size_t>>(0);
    std::shared_ptr<std::atomic<std::size_t>> connections_ =
        std::make_shared<std::atomic<std::size_t>>(0);
};

/**
 * @brief Runs a client against @p server until it ends.
 * @param server The server it connects to
 * @return The client, which has ended (checked)
 */
std::shared_ptr<client_type> runClientAgainst(const GreetingServer &server) {
    auto evaluations = std::make_shared<Evaluations>();
    World world;
    world.route().set(server.listener());
    auto client = world.client(
        GPeerClientOptions{.name = "client", .mode = BIN},
        evaluations,
        1ms,
        nullptr,
        AS_IS
    );
    CHECK(eventually([&] { return world.clientsDone(); }));
    return client;
}

} /* anonymous namespace */

/******************************************************************************/
// The server alone sets the keep-alive interval: it names it in the HELLO, and the client hands it
// to its end (GPeerI::setKeepAlive()), which the in-process pair records.
TEST_CASE(
    "GPeerClientT: the client hands the keep-alive interval of the HELLO to its end",
    "[courtier][peer][client]"
) {
    auto evaluations = std::make_shared<Evaluations>();
    auto watch = std::make_shared<GInProcessPeerWatch>();
    World world(GInProcessListener::Options{.keepAlive = 250ms});
    world.client(GPeerClientOptions{.name = "client", .mode = BIN}, evaluations, 1ms, watch, AS_IS);
    REQUIRE(eventually([&] {
        const auto end = watch->last();
        return end != nullptr && end->keepAlive() == 250ms;
    }));
}

// The HELLO a server sends names the commit of its Geneva sources, and a client of the same build
// accepts it.
TEST_CASE(
    "GPeerClientT: the HELLO names the Geneva commit of the server",
    "[courtier][peer][client]"
) {
    const std::string hello = Gem::Courtier::buildHello<GFaultyContainer>(BIN, 0ms);
    GCommandContainerT<GFaultyContainer> got{GFrameKind::NONE};
    REQUIRE_NOTHROW(Gem::Courtier::container_from_string(hello, got, BIN));
    CHECK(got.kind() == GFrameKind::HELLO);
    CHECK(got.build() == Gem::Common::gemCommit());
    CHECK(got.release() == GEM_VERSION_STRING);
    CHECK(Gem::Courtier::helloMismatch(got).empty());
}

TEST_CASE("GPeerClientT: a server of another build ends the client", "[courtier][peer][client]") {
    auto evaluations = std::make_shared<Evaluations>();
    // What the client logs goes here for the duration of this test.
    auto log = std::make_shared<CapturedLog>();
    glogger.addLogTarget(log);
    const GreetingServer other{std::string(OTHER_COMMIT), ""};
    World world;
    world.route().set(other.listener());
    auto client = world.client(
        GPeerClientOptions{.name = "client", .mode = BIN},
        evaluations,
        1ms,
        nullptr,
        AS_IS
    );
    REQUIRE(eventually([&] { return world.clientsDone(); }));
    CHECK(client->terminalErrorFlagged());
    CHECK(client->losses() == 0);
    CHECK(client->connections() == 1); // it did not come back
    // The server counts on its own threads: the client may be done before they have run.
    CHECK(eventually([&] { return other.received() == 1; })); // it asked for nothing,
    CHECK(eventually([&] { return other.hellos() == 1; }));   // but said who it is
    // The log names both commits, and this build's version for the reader.
    CHECK(log->hasLineWith(
        {"The server was built from another commit",
         OTHER_COMMIT,
         OTHER_VERSION,
         Gem::Common::gemCommit(),
         GEM_VERSION_STRING}
    ));
    glogger.resetLogTargets();
}

// The HELLO is checked on every connection, not only on the first: a server that comes back as
// another build (an upgrade behind the same address) ends the client as well.
TEST_CASE(
    "GPeerClientT: a server that comes back as another build ends the client",
    "[courtier][peer][client]"
) {
    auto log = std::make_shared<CapturedLog>();
    glogger.addLogTarget(log);
    // The first connection names this build and ends with ERROR at the client's PULL; the second
    // names another build.
    const GreetingServer
        server{std::string(Gem::Common::gemCommit()), "", 2, std::string(OTHER_COMMIT)};
    const auto client = runClientAgainst(server);
    CHECK(client->terminalErrorFlagged());
    CHECK(client->connections() == 2); // it did not come back a third time
    // The server counts on its own threads: the client may be done before they have run.
    CHECK(eventually([&] { return server.received() == 2; })); // a PULL on the first, on the second
    CHECK(eventually([&] { return server.hellos() == 1; }));   // only its HELLO
    CHECK(log->hasLineWith({"The server was built from another commit", OTHER_COMMIT}));
    glogger.resetLogTargets();
}

// The identity of the program is one per process (Gem::Common::setProgramIdentity()), so this case
// runs in a ctest entry of its own (GCourtierPeerClientProgramTests) and sets it half way: first
// this client's program has none, then it has one.
TEST_CASE(
    "GPeerClientT: the program identity of the HELLO is checked where both sides have one",
    "[courtier][peer][client][programidentity]"
) {
    REQUIRE(Gem::Common::programIdentity().empty());
    const std::string own_commit{Gem::Common::gemCommit()};
    // What the clients log goes here for the duration of this test.
    auto log = std::make_shared<CapturedLog>();
    glogger.addLogTarget(log);

    // This client's program has no identity: not checked; the client says so once, though it is
    // greeted twice (the server ends the first connection with ERROR, the second with RUN_ENDED).
    {
        const GreetingServer server{own_commit, "the server's program", 2};
        const auto client = runClientAgainst(server);
        CHECK_FALSE(client->terminalErrorFlagged());
        CHECK(client->connections() == 2);
        CHECK(log->countLinesWith({"not checked", "this client's program has no identity"}) == 1);
    }

    Gem::Common::setProgramIdentity("this program");

    // The server's program has no identity: the same, from the other side.
    {
        const GreetingServer server{own_commit, "", 2};
        const auto client = runClientAgainst(server);
        CHECK_FALSE(client->terminalErrorFlagged());
        CHECK(client->connections() == 2);
        CHECK(log->countLinesWith({"not checked", "the server's program has no identity"}) == 1);
    }

    // The server's program has another identity: the client ends, as for another Geneva commit,
    // and the log names both identities.
    {
        const GreetingServer server{own_commit, "another program", 0};
        const auto client = runClientAgainst(server);
        CHECK(client->terminalErrorFlagged());
        CHECK(client->connections() == 1); // it did not come back
        // The server counts on its own threads: the client may be done before they have run.
        CHECK(eventually([&] { return server.received() == 1; })); // it asked for nothing,
        CHECK(eventually([&] { return server.hellos() == 1; }));   // but said who it is
        CHECK(
            log->hasLineWith({"The server runs another program", "another program", "this program"})
        );
    }

    // A server of this process names both texts, and a client of this process accepts them.
    const std::string hello = Gem::Courtier::buildHello<GFaultyContainer>(BIN, 0ms);
    GCommandContainerT<GFaultyContainer> got{GFrameKind::NONE};
    REQUIRE_NOTHROW(Gem::Courtier::container_from_string(hello, got, BIN));
    CHECK(got.build() == own_commit);
    CHECK(got.program() == "this program");
    CHECK(Gem::Courtier::helloMismatch(got).empty());
    CHECK(Gem::Courtier::helloProgramUnchecked(got).empty());
    glogger.resetLogTargets();
}

/******************************************************************************/
// No exception of any type leaves a compute thread: when what follows the evaluation throws
// something that is not a std::exception (here the library's return seam, GWireProtocolT::
// buildReturn()), the item is returned to the server as failed and the client goes on.

namespace {

/** @brief A work item whose return seam (GWireProtocolT::buildReturn(), below) throws something
 *  that is not a std::exception: what follows its evaluation fails on the compute thread. */
class GReturnThrowsItem final
  : public Gem::Courtier::GProcessingContainerT<GReturnThrowsItem, bool> {
    friend struct Gem::Weft::access;

    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<
            Gem::Courtier::GProcessingContainerT<GReturnThrowsItem, bool>>(
            ar,
            "GProcessingContainerT_GReturnThrowsItem",
            *this
        );
    }

public:
    /** @brief The default constructor (also for loading) */
    GReturnThrowsItem()
      : Gem::Courtier::GProcessingContainerT<GReturnThrowsItem, bool>(1) { /* nothing */
    }

private:
    /** @brief Nothing to evaluate */
    void process_([[maybe_unused]] const std::vector<bool> &res_vec) override { /* nothing */
    }
};

} /* anonymous namespace */

namespace Gem::Courtier {
/** @brief The wire seam of GReturnThrowsItem: the default one, except that building the return
 *  throws something that is not a std::exception. */
template <>
struct GWireProtocolT<GReturnThrowsItem> {
    using payload_type = GNoLibraryPayload;
    static std::uint32_t workTag() {
        return 0;
    }
    static bool buildReturn(
        [[maybe_unused]] const GReturnThrowsItem &item,
        [[maybe_unused]] std::uint32_t &tag,
        [[maybe_unused]] payload_type &payload
    ) {
        throw 42; // NOLINT(hicpp-exception-baseclass,cert-err60-cpp) -- not a std::exception on purpose
    }
    static void applyReturn(
        GReturnThrowsItem &slot,
        [[maybe_unused]] std::uint32_t tag,
        [[maybe_unused]] const payload_type *payload,
        const GReturnThrowsItem *item
    ) {
        if(item != nullptr) {
            slot.absorbResultsFrom(*item);
        }
    }
};
} /* namespace Gem::Courtier */

TEST_CASE(
    "GPeerClientT: an item that cannot be returned is returned as failed",
    "[courtier][peer][client]"
) {
    bool done = false;
    {
        WatchWorld<GReturnThrowsItem> world(
            GPeerClientOptions{.name = "client", .mode = BIN, .prefetchDepth = 1}
        );
        auto batch = world.submit(2); // both fail; the policy fills their slots
        done = batch.wait_for(PATIENCE) == std::future_status::ready;
        if(not done) {
            world.shell().stopServer();
        }
        batch.get();
        CHECK(world.shell().requeuedCount() == 0); // failed, not handed out again
    }
    CHECK(done);
}
