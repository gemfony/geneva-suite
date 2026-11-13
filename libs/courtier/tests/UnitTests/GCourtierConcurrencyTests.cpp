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

/**
 * Fan-in (multi-submitter) tests for the courtier consumers. Several optimization algorithms can
 * submit to the SAME consumer concurrently -- the use case being a meta-optimization over a
 * population of inner algorithms, each submitting its individuals in parallel to one shared client
 * pool. Each submitter's processBatch() must complete, and -- crucially -- every result must return
 * to EXACTLY its own batch's slot (no cross-batch corruption), which the (batch_id, slot) correlation
 * token guarantees.
 *
 * The networked consumer is exercised deterministically through a SimNetConsumer (no real socket): N
 * driver threads play the shared client pool (checkout / process / checkin) against M concurrent
 * processBatch() submitters. The local thread-pool consumer is exercised directly (concurrent
 * processBatch() onto one shared pool).
 *
 * The second half covers parked pulls (checkoutOrPark()): a caller for whom nothing is pending
 * leaves a wake function instead of going away empty-handed, and every place that makes work
 * available calls the parked ones.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <semaphore>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GNetworkedConsumerT.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
namespace c2 = Gem::Courtier;
using namespace std::chrono_literals;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;

// A unique, decodable stored number per (batch, slot), so we can verify each result came home to its
// own batch's slot and was not mis-routed to another concurrent batch.
constexpr std::size_t kBatchStride = 100'000;
std::size_t tag(std::size_t batch, std::size_t slot) { return batch * kBatchStride + slot; }

std::vector<item_ptr> make_tagged_batch(std::size_t batch, std::size_t n,
                                        const std::vector<std::size_t> &faulty = {}) {
    std::vector<item_ptr> v;
    v.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        const bool f = std::ranges::contains(faulty, i);
        v.push_back(std::make_unique<GFaultyContainer>(
            tag(batch, i), f ? fault_mode::THROW_PROCESSING : fault_mode::NONE));
    }
    return v;
}

std::size_t count_processed(const std::vector<item_ptr> &v) {
    std::size_t c = 0;
    for(const auto &it : v) {
        if(it && it->is_processed()) {
            ++c;
        }
    }
    return c;
}

/** @brief A GNetworkedConsumerT with no real transport: exposes checkout/checkin so a pool of test
 *  driver threads can play "the shared client pool" against many concurrent dispatch_ submitters. */
class SimNetConsumer : public c2::GNetworkedConsumerT<GFaultyContainer> {
public:
    using c2::GNetworkedConsumerT<GFaultyContainer>::checkout;
    using c2::GNetworkedConsumerT<GFaultyContainer>::checkin;
    // For the tests of parked pulls
    using c2::GNetworkedConsumerT<GFaultyContainer>::checkoutOrPark;
    using c2::GNetworkedConsumerT<GFaultyContainer>::requeue;
    using c2::GNetworkedConsumerT<GFaultyContainer>::requestStop;
    using c2::GNetworkedConsumerT<GFaultyContainer>::setLateReturnBuffer;
    using c2::GNetworkedConsumerT<GFaultyContainer>::lateReturnBufferSize;
};

/** @brief Waits until @p condition holds, for at most five seconds.
 *  @return Whether the condition held in the end */
template <typename ConditionF>
bool eventually(ConditionF condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while(not condition()) {
        if(std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

/** @brief Stops @p consumer at the end of a scope. Declared after a submitter thread, it releases
 *  a batch that still waits when a test fails half-way, so the thread can be joined and the test
 *  ends instead of hanging. After a batch has completed it does nothing of interest. */
struct GStopAtEnd {
    SimNetConsumer &consumer;
    explicit GStopAtEnd(SimNetConsumer &to_stop)
      : consumer(to_stop) {
    }
    ~GStopAtEnd() {
        consumer.requestStop();
    }
    GStopAtEnd(const GStopAtEnd &) = delete;
    GStopAtEnd &operator=(const GStopAtEnd &) = delete;
};

/** @brief Evaluates a checked-out item and hands it back, as a client would. */
void evaluateAndReturn(SimNetConsumer &consumer, item_ptr item) {
    item->process();
    consumer.checkin(std::move(item));
}

/** @brief Runs @p batch through @p consumer on a thread of its own and says when it is done. */
std::jthread
submit(SimNetConsumer &consumer, std::vector<item_ptr> &batch, std::atomic<bool> &done) {
    return std::jthread([&] {
        consumer.processBatch(
            std::span<item_ptr>(batch.data(), batch.size()),
            c2::GSubmissionPolicy::full_success_or_fatal()
        );
        done.store(true);
    });
}

} /* anonymous namespace */

/******************************************************************************/

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent fan-in scenario: spawns a shared client pool plus per-batch submitter threads against one networked consumer, then verifies each batch's own slots come back correctly routed; splitting would scatter the capturing lambdas
TEST_CASE("courtier(fanin): concurrent submitters to one networked consumer, results not cross-routed",
          "[courtier][concurrency][fanin]") {
    constexpr std::size_t M = 6;  // concurrent submitters (inner algorithms)
    constexpr std::size_t N = 25; // items per batch
    constexpr std::size_t CLIENTS = 4;

    SimNetConsumer consumer;

    // Each submitter owns a batch (the span borrows it, so it must outlive processBatch()).
    std::vector<std::vector<item_ptr>> batches;
    batches.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        batches.push_back(make_tagged_batch(b, N));
    }

    std::atomic<std::size_t> active{M};

    // The shared "client pool": pull any pending item from any batch, process it, hand it back.
    std::vector<std::jthread> clients;
    clients.reserve(CLIENTS);
    for(std::size_t c = 0; c < CLIENTS; ++c) {
        clients.emplace_back([&] {
            while(active.load() > 0) {
                auto p = consumer.checkout();
                if(not p) {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                try {
                    p->process();
                }
                catch(...) {
                    // process() sets the item's status (here always PROCESSED) before any re-throw;
                    // a real client swallows the re-throw and ships the item back. Mirror that.
                }
                consumer.checkin(std::move(p));
            }
        });
    }

    // The submitters: each blocks in processBatch() until its own batch is whole again.
    std::vector<std::jthread> submitters;
    submitters.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        submitters.emplace_back([&, b] {
            consumer.processBatch(
                std::span<item_ptr>(batches[b].data(), batches[b].size()),
                c2::GSubmissionPolicy::full_success_or_fatal());
            --active;
        });
    }

    for(auto &s : submitters) {
        s.join();
    }
    for(auto &c : clients) {
        c.join();
    }

    // Every batch fully evaluated, and every slot still holds ITS OWN item (correct (batch,slot)
    // routing -- a broken router would land batch B's result in batch A's slot, changing its tag).
    for(auto const& [b, batch] : batches | std::views::enumerate) {
        CHECK(count_processed(batch) == N);
        for(auto const& [i, item] : batch | std::views::enumerate) {
            CHECK(item->get_stored_number() == tag(b, i));
            CHECK(item->is_processed());
        }
    }
}

/******************************************************************************/

TEST_CASE("courtier(fanin): concurrent submitters with failures stay size-preserved per batch",
          "[courtier][concurrency][fanin]") {
    // Concurrent batches, each with a couple of throwing items, under clone-on-partial-return: each
    // batch must come back full (failed slots refilled from a successful sibling) and independent.
    constexpr std::size_t M = 5;
    constexpr std::size_t N = 20;

    SimNetConsumer consumer;

    std::vector<std::vector<item_ptr>> batches;
    batches.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        batches.push_back(make_tagged_batch(b, N, {3, 11})); // two throwing slots per batch
    }

    std::atomic<std::size_t> active{M};
    std::vector<std::jthread> clients;
    for(std::size_t c = 0; c < 4; ++c) {
        clients.emplace_back([&] {
            while(active.load() > 0) {
                auto p = consumer.checkout();
                if(not p) {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                try {
                    p->process(); // a faulty item sets itself EXCEPTION_CAUGHT, then re-throws
                }
                catch(...) {
                    // Swallow the re-throw (as a real client does); the EXCEPTION_CAUGHT status
                    // is already on the item, so checkin reports it as FAILED for the policy loop.
                }
                consumer.checkin(std::move(p));
            }
        });
    }

    std::vector<std::jthread> submitters;
    for(std::size_t b = 0; b < M; ++b) {
        submitters.emplace_back([&, b] {
            consumer.processBatch(
                std::span<item_ptr>(batches[b].data(), batches[b].size()),
                c2::GSubmissionPolicy::clone_on_partial_return());
            --active;
        });
    }
    for(auto &s : submitters) {
        s.join();
    }
    for(auto &c : clients) {
        c.join();
    }

    for(std::size_t b = 0; b < M; ++b) {
        CHECK(batches[b].size() == N);       // size preserved
        CHECK(count_processed(batches[b]) == N); // every slot ends up evaluated (clones refill failures)
    }
}

/******************************************************************************/

TEST_CASE("courtier(fanin): concurrent submitters to one local thread consumer",
          "[courtier][concurrency][fanin]") {
    // The local thread-pool consumer shares one pool across submitters; concurrent processBatch()
    // calls must each wait for only their OWN items (per-batch wait), not the whole pool.
    constexpr std::size_t M = 8;
    constexpr std::size_t N = 30;

    auto consumer = std::make_shared<c2::GStdThreadConsumerT<GFaultyContainer>>(4);

    std::vector<std::vector<item_ptr>> batches;
    batches.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        batches.push_back(make_tagged_batch(b, N));
    }

    std::vector<std::jthread> submitters;
    submitters.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        submitters.emplace_back([&, b] {
            consumer->processBatch(std::span<item_ptr>(batches[b].data(), batches[b].size()),
                                   c2::GSubmissionPolicy::full_success_or_fatal());
        });
    }
    for(auto &s : submitters) {
        s.join();
    }

    for(auto const& [b, batch] : batches | std::views::enumerate) {
        CHECK(count_processed(batch) == N);
        for(auto const& [i, item] : batch | std::views::enumerate) {
            CHECK(item->get_stored_number() == tag(b, i));
        }
    }
}

/******************************************************************************/

/******************************************************************************/
// Parked pulls

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; the count comes from the assertion macros
TEST_CASE(
    "courtier(park): a new batch wakes every parked caller exactly once",
    "[courtier][park]"
) {
    constexpr std::size_t CALLERS = 10;
    SimNetConsumer consumer;

    std::vector<std::atomic<int>> woken(CALLERS);
    for(auto &count : woken) {
        const auto result = consumer.checkoutOrPark([&count] { ++count; });
        REQUIRE(result.parked);
        REQUIRE_FALSE(result.item);
    }
    CHECK(consumer.parkedCount() == CALLERS);
    CHECK(consumer.emptyCheckoutCount() == 0); // a parked caller was not sent away empty-handed

    auto batch = make_tagged_batch(0, 3);
    std::atomic<bool> done{false};
    auto submitter = submit(consumer, batch, done);
    const GStopAtEnd stop_at_end(consumer);

    REQUIRE(eventually([&] {
        return std::ranges::all_of(woken, [](const auto &count) { return count.load() == 1; });
    }));
    CHECK(consumer.parkedCount() == 0);

    // The woken callers ask again: three find an item, the fourth is parked once more.
    for(std::size_t i = 0; i < batch.size(); ++i) {
        auto result = consumer.checkoutOrPark([] {});
        REQUIRE(result.item);
        evaluateAndReturn(consumer, std::move(result.item));
    }
    CHECK(consumer.checkoutOrPark([] {}).parked);

    submitter.join();
    CHECK(count_processed(batch) == batch.size());
    for(const auto &count : woken) {
        CHECK(count.load() == 1);
    }
}

/******************************************************************************/

TEST_CASE(
    "courtier(park): a stop wakes every parked caller and parks nobody afterwards",
    "[courtier][park]"
) {
    SimNetConsumer consumer;
    std::atomic<int> woken{0};
    for(int i = 0; i < 5; ++i) {
        const auto result = consumer.checkoutOrPark([&woken] { ++woken; });
        REQUIRE(result.parked);
    }

    consumer.requestStop();

    CHECK(woken.load() == 5);
    CHECK(consumer.parkedCount() == 0);

    const auto late_caller = consumer.checkoutOrPark([&woken] { ++woken; });
    CHECK_FALSE(late_caller.parked);
    CHECK_FALSE(late_caller.item);
    CHECK(consumer.parkedCount() == 0);
    CHECK(woken.load() == 5);
}

/******************************************************************************/

TEST_CASE("courtier(park): an item that is put back wakes a parked caller", "[courtier][park]") {
    SimNetConsumer consumer;
    auto batch = make_tagged_batch(0, 1);
    std::atomic<bool> done{false};
    auto submitter = submit(consumer, batch, done);
    const GStopAtEnd stop_at_end(consumer);

    // The first caller takes the only item and then loses its client.
    c2::GCheckoutResult<GFaultyContainer> first;
    REQUIRE(eventually([&] {
        first = consumer.checkoutOrPark([] {});
        return static_cast<bool>(first.item);
    }));

    std::atomic<int> woken{0};
    const auto parked_caller = consumer.checkoutOrPark([&woken] { ++woken; });
    REQUIRE(parked_caller.parked);
    CHECK(woken.load() == 0);

    consumer.requeue(first.item->getCorrelationId());
    CHECK(woken.load() == 1);

    auto second = consumer.checkoutOrPark([] {});
    REQUIRE(second.item);
    evaluateAndReturn(consumer, std::move(second.item));

    submitter.join();
    CHECK(count_processed(batch) == 1);
    CHECK(consumer.requeuedCount() == 1);
}

/******************************************************************************/
// The lease sweep puts an abandoned item back while a batch waits, and announces it without the
// consumer's lock. If the batch's last result is checked in during that announcement, nobody waits
// for the notification; the batch must notice all the same, without sleeping through another sweep
// interval. The interval is an hour here, so a batch that sleeps does not return.
//
// The arrangement: batch X has one item, which a client takes and abandons. A parked caller serves
// whatever it is woken for, right inside the wake function (a real caller must not do that; here it
// places the check-in exactly in the unlocked window). A second batch, Y, wakes the caller, which
// completes Y; that completion wakes the wait of X, whose sweep puts the abandoned item back and
// announces it -- and the caller completes X from inside that announcement.
TEST_CASE(
    "courtier(park): a batch completed during the lease sweep's announcement ends at once",
    "[courtier][park]"
) {
    SimNetConsumer consumer;
    consumer.setLeaseBootstrap(5ms);
    consumer.setLeaseBounds(1ms, 5s);
    consumer.setSweepTick(std::chrono::hours(1));

    auto batch_x = make_tagged_batch(0, 1);
    std::atomic<bool> x_done{false};
    auto submitter_x = submit(consumer, batch_x, x_done);

    c2::GCheckoutResult<GFaultyContainer> abandoned;
    REQUIRE(eventually([&] {
        abandoned = consumer.checkoutOrPark([] {});
        return static_cast<bool>(abandoned.item);
    }));
    std::this_thread::sleep_for(50ms); // well past the lease

    // Serves until nothing is pending, then parks again with itself as the wake function.
    std::function<void()> serve = [&] {
        while(true) {
            auto result = consumer.checkoutOrPark([&serve] { serve(); });
            if(not result.item) {
                return;
            }
            evaluateAndReturn(consumer, std::move(result.item));
        }
    };
    serve(); // nothing pending (the item of X is in flight): parks

    auto batch_y = make_tagged_batch(1, 1);
    std::atomic<bool> y_done{false};
    auto submitter_y = submit(consumer, batch_y, y_done);

    const bool both_done = eventually([&] { return x_done.load() && y_done.load(); });
    if(not both_done) {
        consumer.requestStop(); // release the batch that sleeps, so the test can end
    }
    submitter_x.join();
    submitter_y.join();

    CHECK(both_done);
    CHECK(count_processed(batch_x) == 1);
    CHECK(count_processed(batch_y) == 1);
    CHECK(consumer.requeuedCount() == 1);
}

/******************************************************************************/
// A session checks a result in first and asks for work second. A late result must reach the late
// buffer also when the caller is parked right afterwards.
TEST_CASE("courtier(park): a late return is kept when its caller parks next", "[courtier][park]") {
    SimNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 8);

    auto late = std::make_unique<GFaultyContainer>(42, fault_mode::NONE);
    late->setCorrelationId(0); // no batch is active, so any id is a late one
    consumer.checkin(std::move(late));
    CHECK(consumer.checkoutOrPark([] {}).parked);

    CHECK(consumer.lateReturnBufferSize() == 1);
    CHECK(consumer.parkedCount() == 1);
}

/******************************************************************************/
// Many sessions that park and are woken, against several submitters: nothing is lost, nothing is
// evaluated twice into a wrong slot, and no caller is ever sent away empty-handed. This is the test
// for the thread sanitizer.
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- one scenario: a pool of parking sessions against several submitters
TEST_CASE(
    "courtier(park): parking sessions against concurrent submitters",
    "[courtier][park][concurrency]"
) {
    constexpr std::size_t SESSIONS = 8;
    constexpr std::size_t SUBMITTERS = 4;
    constexpr std::size_t ROUNDS = 25;
    constexpr std::size_t ITEMS = 12;

    SimNetConsumer consumer;
    std::atomic<std::size_t> n_evaluated{0};
    std::atomic<std::size_t> n_never_woken{0};

    std::vector<std::jthread> sessions;
    sessions.reserve(SESSIONS);
    for(std::size_t s = 0; s < SESSIONS; ++s) {
        sessions.emplace_back([&] {
            // Shared with the wake function, which may outlive a session that gave up waiting.
            auto news = std::make_shared<std::binary_semaphore>(0);
            while(true) {
                auto result = consumer.checkoutOrPark([news] { news->release(); });
                if(result.item) {
                    evaluateAndReturn(consumer, std::move(result.item));
                    ++n_evaluated;
                }
                else if(result.parked) {
                    // Woken exactly once per parking. The limit only keeps a failing test from
                    // hanging; the whole test takes a fraction of a second.
                    if(not news->try_acquire_for(30s)) {
                        ++n_never_woken;
                        return;
                    }
                }
                else {
                    return; // the server is stopping
                }
            }
        });
    }

    std::atomic<std::size_t> n_complete{0};
    {
        std::vector<std::jthread> submitters;
        submitters.reserve(SUBMITTERS);
        for(std::size_t m = 0; m < SUBMITTERS; ++m) {
            submitters.emplace_back([&, m] {
                for(std::size_t round = 0; round < ROUNDS; ++round) {
                    auto batch = make_tagged_batch(m, ITEMS);
                    consumer.processBatch(
                        std::span<item_ptr>(batch.data(), batch.size()),
                        c2::GSubmissionPolicy::full_success_or_fatal()
                    );
                    if(count_processed(batch) == ITEMS) {
                        ++n_complete;
                    }
                }
            });
        }
    }

    consumer.requestStop();
    sessions.clear(); // joins: every session was woken by the stop

    CHECK(n_complete.load() == SUBMITTERS * ROUNDS);
    CHECK(n_evaluated.load() == SUBMITTERS * ROUNDS * ITEMS);
    CHECK(n_never_woken.load() == 0);
    CHECK(consumer.emptyCheckoutCount() == 0);
    CHECK(consumer.parkedCount() == 0);
}
