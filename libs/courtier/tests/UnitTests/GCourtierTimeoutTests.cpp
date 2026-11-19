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
 * Tier-1 tests for the courtier time limits -- the user's reclaim lease and give-up window, the
 * explicit put-back -- and the clone-from-template refill. The networked timeout
 * logic lives in GNetworkedConsumerT independently of any actual socket, so it is exercised here
 * deterministically by a test subclass acting as a simulated transport: a driver thread plays the
 * role of clients (checkout / process / checkin), and can ABANDON an item (never check it back in,
 * = a client that died mid-evaluation) or explicitly REQUEUE one (= a websocket session-death
 * put-back). In both cases every item must still come back, via the reclaim lease / the put-back.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GNetworkedConsumerT.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

using Catch::Matchers::ContainsSubstring;
using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
namespace c2 = Gem::Courtier;
using namespace std::chrono_literals;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;

std::vector<item_ptr> make_batch(
    std::size_t n,
    const std::vector<std::size_t> &faulty = {},
    fault_mode fm = fault_mode::THROW_PROCESSING
) {
    std::vector<item_ptr> v;
    v.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        const bool f = std::ranges::contains(faulty, i);
        v.push_back(std::make_unique<GFaultyContainer>(i, f ? fm : fault_mode::NONE));
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

/** @brief A GNetworkedConsumerT with no real transport: it exposes the checkout/checkin/requeue
 *  endpoints so a test driver thread can play "clients" against the dispatch_ machinery. */
class SimNetConsumer : public c2::GNetworkedConsumerT<GFaultyContainer> {
public:
    using item_type = GFaultyContainer;
    using c2::GNetworkedConsumerT<GFaultyContainer>::checkout;
    using c2::GNetworkedConsumerT<GFaultyContainer>::checkin;
    using c2::GNetworkedConsumerT<GFaultyContainer>::requeue;
};

/** @brief How the driver mistreats a chosen item the first time it sees it. */
enum class misbehave {
    abandon,
    requeue_it
};

/**
 * Runs @p batch through a SimNetConsumer with a driver thread that processes items, but the FIRST
 * time it sees an item selected by @p victim it abandons or requeues it (per @p mode) -- simulating
 * a client death (reclaimed by the lease) or a session-death put-back. Returns once every slot is
 * resolved.
 */
template <typename victim_pred_type>
void run_with_misbehaviour(std::vector<item_ptr> &batch, misbehave mode, victim_pred_type victim) {
    SimNetConsumer consumer;
    c2::GNetworkedTimeoutConfig limits;
    limits.reclaim_lease_ms = 30; // reclaim an abandoned item quickly
    consumer.applyTimeoutConfig(limits);

    std::atomic<bool> finished{false};
    std::jthread worker([&] {
        consumer.processBatch(
            std::span<item_ptr>(batch.data(), batch.size()),
            c2::GSubmissionPolicy::full_success_or_fatal()
        );
        finished.store(true);
    });

    std::set<std::size_t> mistreated_once;
    while(not finished.load()) {
        auto p = consumer.checkout();
        if(not p) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        const std::size_t id = p->get_stored_number();
        if(victim(id) && mistreated_once.insert(id).second) {
            if(mode == misbehave::requeue_it) {
                consumer.requeue(
                    p->getCorrelationId()
                ); // immediate put-back (as a websocket session-death would do)
            }
            // mode == abandon: simply drop it -- never check it in; the reclaim lease must recover it
            continue;
        }
        p->process();
        consumer.checkin(std::move(p));
    }
    worker.join();
}

/** @brief A SimNetConsumer that also exposes the late-return buffer knobs/observers,
 *  so a test can enable the buffer and inspect what it holds / has dropped. */
class LateNetConsumer : public SimNetConsumer {
public:
    using c2::GNetworkedConsumerT<GFaultyContainer>::setLateReturnBuffer;
    using c2::GNetworkedConsumerT<GFaultyContainer>::lateReturnBufferSize;
    using c2::GNetworkedConsumerT<GFaultyContainer>::lateReturnDroppedCount;
};

/** @brief Runs a fresh n-item batch fully to completion on @p consumer (every slot processed and
 *  checked in), which advances the consumer's dispatch-round epoch by one. Used to age late-buffer
 *  entries past their TTL. */
void run_full_batch(LateNetConsumer &consumer, std::size_t n) {
    auto batch = make_batch(n);
    std::atomic<bool> finished{false};
    std::jthread worker([&] {
        consumer.processBatch(
            std::span<item_ptr>(batch.data(), batch.size()),
            c2::GSubmissionPolicy::full_success_or_fatal()
        );
        finished.store(true);
    });
    while(not finished.load()) {
        auto p = consumer.checkout();
        if(not p) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        p->process();
        consumer.checkin(std::move(p));
    }
    worker.join();
}

/** @brief Delivers a single LATE return (a result whose batch is not currently active) to @p consumer.
 *  With no batch active, any correlation id is "inactive", so this exercises the late-return path. */
void deliver_late(LateNetConsumer &consumer, std::size_t stored, c2::CORRELATION_ID_TYPE corr) {
    // The late-return path (checkin -> inactive batch_id -> buffer) does not inspect processing state,
    // so a bare item with the chosen correlation id is enough to exercise it.
    auto p = std::make_unique<GFaultyContainer>(stored, fault_mode::NONE);
    p->setCorrelationId(corr);
    consumer.checkin(std::move(p));
}

/** @brief Delivers a single late PAYLOAD-ONLY return: a RESULT frame that names a slot by its
 *  correlation id but carries no work item, so the consumer must apply it to the retained original. */
template <typename consumer_type>
void deliver_late_payloadonly(consumer_type &consumer, c2::CORRELATION_ID_TYPE corr) {
    c2::GCommandContainerT<typename consumer_type::item_type> frame{c2::GFrameKind::RESULT};
    c2::GProcessingOutcome outcome;
    outcome.status = c2::processingStatus::PROCESSED;
    outcome.correlation_id = corr;
    // No item exists here; the id names this process, as a checkout clone's would
    outcome.submission_uuid = {c2::detail::lineageSalt(), 0};
    frame.setOutcome(outcome);
    frame.setLibraryPayload({}); // the library's own return payload (empty for the demo containers)
    consumer.checkin(std::move(frame));
}

/** @brief Runs an n-item batch but ABANDONS the victim slots forever (never checks them in).
 *  The batch therefore retires with those slots still MISSING, which -- with the late-return
 *  buffer enabled -- makes the consumer retain a clone of each victim's original (keyed by its
 *  correlation id) so a later return can be applied to it. The other slots are processed normally;
 *  the user's give-up window (short) ends the batch. The copy handed out
 *  for each victim is kept, as a slow worker would keep it, and returned unprocessed in the order
 *  of @p victims; checking it in later is a late return of a slot that had not returned when its
 *  batch ended. In the first dispatch round the batch_id is 0, so a victim slot's correlation id
 *  is simply its slot index. */
template <typename consumer_type>
std::vector<std::unique_ptr<typename consumer_type::item_type>> run_abandoning_forever(
    consumer_type &consumer,
    std::vector<std::unique_ptr<typename consumer_type::item_type>> &batch,
    const std::vector<std::size_t> &victims
) {
    using batch_item_ptr = std::unique_ptr<typename consumer_type::item_type>;
    c2::GNetworkedTimeoutConfig limits;
    limits.give_up_window_ms = 300;
    consumer.applyTimeoutConfig(limits);
    std::vector<batch_item_ptr> slow(victims.size());
    std::atomic<bool> finished{false};
    std::jthread worker([&] {
        consumer.processBatch(
            std::span<batch_item_ptr>(batch.data(), batch.size()),
            c2::GSubmissionPolicy::clone_on_partial_return()
        );
        finished.store(true);
    });
    while(not finished.load()) {
        auto p = consumer.checkout();
        if(not p) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        const auto victim = std::ranges::find(victims, p->get_stored_number());
        if(victim != victims.end()) {
            // Never check it in -> stays unreturned -> retired MISSING -> retained
            auto &held = slow[static_cast<std::size_t>(victim - victims.begin())];
            if(not held) {
                held = std::move(p);
            }
            continue;
        }
        p->process();
        consumer.checkin(std::move(p));
    }
    worker.join();
    return slow;
}

/******************************************************************************/
// A work item whose late result can be made to fail on being applied to its retained original:
// GLateFaultItem's grafting of the retained state (a late result that carries its item) and its
// wire seam's applyReturn() (a payload-only late result) throw while armed.

/// As many applications of a returned result to a GLateFaultItem throw as this says
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> applicationsToFail{0};

/** @brief Uses up one armed application fault. @return Whether one was left */
bool consumeApplicationFault() {
    int left = applicationsToFail.load();
    while(left > 0 && not applicationsToFail.compare_exchange_weak(left, left - 1)) {
    }
    return left > 0;
}

/** @brief A work item with an id whose result can be made to fail on being applied (see
 *  applicationsToFail): the grafting of its retained state throws, and so does its wire seam's
 *  applyReturn() (below). */
class GLateFaultItem final : public c2::GProcessingContainerT<GLateFaultItem, bool> {
    friend struct Gem::Weft::access;

    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<c2::GProcessingContainerT<GLateFaultItem, bool>>(
            ar,
            "GProcessingContainerT_GLateFaultItem",
            *this
        );
        Gem::Common::archive_named(ar, "stored_number_", stored_number_);
    }

public:
    /** @brief An item with an id. @param stored_number Tells the items apart */
    explicit GLateFaultItem(std::size_t stored_number)
      : c2::GProcessingContainerT<GLateFaultItem, bool>(1)
      , stored_number_(stored_number) { /* nothing */
    }

    /** @brief @return The id of this item */
    [[nodiscard]] std::size_t get_stored_number() const {
        return stored_number_;
    }

private:
    /** @brief For loading only */
    GLateFaultItem()
      : c2::GProcessingContainerT<GLateFaultItem, bool>(1) { /* nothing */
    }
    /** @brief Nothing to compute */
    void process_([[maybe_unused]] const std::vector<bool> &res_vec) override { /* nothing */
    }
    /** @brief Throws while an application fault is armed */
    void graftRetainedStateFrom_([[maybe_unused]] const c2::GProcessable &original) override {
        if(consumeApplicationFault()) {
            throw std::runtime_error("this test makes the grafting of the retained state fail");
        }
    }

    std::size_t stored_number_ = 0; ///< Tells the items apart
};

} /* anonymous namespace */

namespace Gem::Courtier {
/** @brief The wire seam of GLateFaultItem: the default one, except that applying a return throws
 *  while an application fault is armed. */
template <>
struct GWireProtocolT<GLateFaultItem> {
    using payload_type = GNoLibraryPayload;
    static std::uint32_t workTag() {
        return 0;
    }
    static bool buildReturn(
        [[maybe_unused]] const GLateFaultItem &item,
        std::uint32_t &tag,
        [[maybe_unused]] payload_type &payload
    ) {
        tag = 0;
        return true;
    }
    static void applyReturn(
        GLateFaultItem &slot,
        [[maybe_unused]] std::uint32_t tag,
        [[maybe_unused]] const payload_type *payload,
        const GLateFaultItem *item
    ) {
        if(consumeApplicationFault()) {
            throw std::runtime_error("this test makes the application of a return fail");
        }
        if(item != nullptr) {
            slot.absorbResultsFrom(*item);
        }
    }
};
} /* namespace Gem::Courtier */

namespace {

/** @brief A networked consumer of GLateFaultItem with no real transport (as SimNetConsumer). */
class LateFaultConsumer : public c2::GNetworkedConsumerT<GLateFaultItem> {
public:
    using item_type = GLateFaultItem;
    using c2::GNetworkedConsumerT<GLateFaultItem>::checkout;
    using c2::GNetworkedConsumerT<GLateFaultItem>::checkin;
    using c2::GNetworkedConsumerT<GLateFaultItem>::setLateReturnBuffer;
    using c2::GNetworkedConsumerT<GLateFaultItem>::lateReturnBufferSize;
    using c2::GNetworkedConsumerT<GLateFaultItem>::lateReturnDroppedCount;
};

/** @brief Makes @p n GLateFaultItem work items with the ids 0 .. n-1.
 *  @param n The number of items
 *  @return The items */
std::vector<std::unique_ptr<GLateFaultItem>> make_late_fault_batch(std::size_t n) {
    std::vector<std::unique_ptr<GLateFaultItem>> v;
    v.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        v.push_back(std::make_unique<GLateFaultItem>(i));
    }
    return v;
}

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE(
    "courtier(timeout): the reclaim lease recovers abandoned (dead-client) items",
    "[courtier][timeout]"
) {
    auto batch = make_batch(40);
    run_with_misbehaviour(batch, misbehave::abandon, [](std::size_t id) { return id % 4 == 0; });
    CHECK(count_processed(batch) == 40); // every item came back despite the "deaths"
}

TEST_CASE(
    "courtier(timeout): an explicit requeue (session-death put-back) loses nothing",
    "[courtier][timeout]"
) {
    auto batch = make_batch(40);
    run_with_misbehaviour(batch, misbehave::requeue_it, [](std::size_t id) { return id % 3 == 0; });
    CHECK(count_processed(batch) == 40);
}

TEST_CASE(
    "courtier(timeout): every item abandoned once is still recovered",
    "[courtier][timeout]"
) {
    auto batch = make_batch(24);
    run_with_misbehaviour(batch, misbehave::abandon, [](std::size_t) { return true; });
    CHECK(count_processed(batch) == 24);
}

/******************************************************************************/

TEST_CASE(
    "courtier(clone): unresolved slots are refilled from the supplied template",
    "[courtier][clone]"
) {
    // A representative, already-evaluated template the algorithm hands down.
    constexpr std::size_t TEMPLATE_ID = 9999;
    auto tmpl = std::make_unique<GFaultyContainer>(TEMPLATE_ID, fault_mode::NONE);
    tmpl->set_processing_status(Gem::Courtier::processingStatus::DO_PROCESS);
    tmpl->process(); // a valid, evaluated template

    auto consumer = std::make_shared<c2::GStdThreadConsumerT<GFaultyContainer>>(
        c2::GStdThreadConsumerConfig{.nThreads = 4}
    );

    const std::vector<std::size_t> faulty{2, 5, 9};
    auto batch = make_batch(12, faulty, fault_mode::THROW_PROCESSING);

    consumer->processBatch(
        std::span<std::unique_ptr<GFaultyContainer>>(batch.data(), batch.size()),
        c2::GSubmissionPolicy::clone_on_partial_return(),
        std::move(tmpl)
    );

    CHECK(count_processed(batch) == 12); // no slot left unevaluated
    // The throwing slots must hold clones of the TEMPLATE (not of a surviving sibling).
    for(std::size_t const i : faulty) {
        CHECK(batch[i]->get_stored_number() == TEMPLATE_ID);
        CHECK(batch[i]->is_processed());
    }
    // A non-faulty slot keeps its own identity.
    CHECK(batch[0]->get_stored_number() == 0);
}

/******************************************************************************/
// Late-return buffer mechanism: a result that arrives after its batch finished/timed out is
// no longer silently dropped -- with the buffer enabled it is parked (bounded + TTL'd), and every
// drop (disabled buffer, capacity overflow, TTL expiry) is counted observably. Only the return of a
// slot that had not returned when its batch ended is parked, so these tests let a batch retire with
// slots abandoned by a slow worker (run_abandoning_forever) and check in that worker's copies late.

TEST_CASE(
    "courtier(late): a late return for a finished batch is buffered and not silently dropped",
    "[courtier][latereturn]"
) {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 8);

    auto batch = make_batch(3);
    auto slow = run_abandoning_forever(consumer, batch, /*victims*/ {2});
    REQUIRE(slow[0]);
    slow[0]->process();
    consumer.checkin(std::move(slow[0])); // its batch has retired -> a late arrival

    CHECK(consumer.lateReturnBufferSize() == 1);   // held for a later getLateReturns() to reap
    CHECK(consumer.lateReturnDroppedCount() == 0); // nothing dropped
}

TEST_CASE(
    "courtier(late): getLateReturns() drains the buffer and transfers the items",
    "[courtier][latereturn]"
) {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 8);

    auto batch = make_batch(4);
    auto slow = run_abandoning_forever(consumer, batch, /*victims*/ {2, 1});
    REQUIRE(slow[0]);
    REQUIRE(slow[1]);
    for(auto &late : slow) {
        late->process();
        consumer.checkin(std::move(late));
    }
    REQUIRE(consumer.lateReturnBufferSize() == 2);

    auto reaped = consumer.getLateReturns(); // a submitter's reaper drains via this hook
    REQUIRE(reaped.size() == 2);
    CHECK(consumer.lateReturnBufferSize() == 0); // buffer emptied
    CHECK(reaped[0]->get_stored_number() == 2);  // FIFO / arrival order preserved
    CHECK(reaped[1]->get_stored_number() == 1);
    CHECK(consumer.getLateReturns().empty()); // a second drain yields nothing
}

TEST_CASE(
    "courtier(late): buffering disabled (default) counts the drop but holds nothing",
    "[courtier][latereturn]"
) {
    LateNetConsumer consumer; // default cap == 0 -> buffering disabled

    deliver_late(consumer, 1, 0);
    deliver_late(consumer, 2, 1);

    CHECK(consumer.lateReturnBufferSize() == 0);   // nothing retained
    CHECK(consumer.lateReturnDroppedCount() == 2); // but the drops are observable, not silent
}

TEST_CASE(
    "courtier(late): the buffer is capacity-bounded and evicts and counts the oldest",
    "[courtier][latereturn]"
) {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(
        /*cap*/ 2,
        /*ttl_rounds*/ 1000
    ); // large TTL: only capacity evicts here

    // Two rounds, because the retained originals are bounded by the same cap of 2.
    auto first_batch = make_batch(4);
    auto first_slow = run_abandoning_forever(consumer, first_batch, /*victims*/ {1, 2});
    for(auto &late : first_slow) {
        REQUIRE(late);
        late->process();
        consumer.checkin(std::move(late));
    }
    REQUIRE(consumer.lateReturnBufferSize() == 2);
    auto second_batch = make_batch(3);
    auto second_slow = run_abandoning_forever(consumer, second_batch, /*victims*/ {1});
    REQUIRE(second_slow[0]);
    second_slow[0]->process();
    consumer.checkin(std::move(second_slow[0])); // exceeds the cap of 2 -> oldest evicted

    CHECK(consumer.lateReturnBufferSize() == 2);
    CHECK(consumer.lateReturnDroppedCount() == 1);
    auto reaped = consumer.getLateReturns();
    REQUIRE(reaped.size() == 2);
    CHECK(reaped[0]->get_stored_number() == 2); // the first round's slot 1 was the oldest
    CHECK(reaped[1]->get_stored_number() == 1);
}

TEST_CASE(
    "courtier(late): a buffered entry is evicted once it ages past its TTL in rounds",
    "[courtier][latereturn]"
) {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 100, /*ttl_rounds*/ 2); // TTL of 2 dispatch rounds

    auto batch = make_batch(3);
    auto slow = run_abandoning_forever(consumer, batch, /*victims*/ {2}); // epoch -> 1
    REQUIRE(slow[0]);
    slow[0]->process();
    consumer.checkin(std::move(slow[0])); // buffered at epoch 1
    REQUIRE(consumer.lateReturnBufferSize() == 1);

    run_full_batch(consumer, 1); // epoch -> 2: age 1 < 2, still held
    CHECK(consumer.lateReturnBufferSize() == 1);

    run_full_batch(consumer, 1); // epoch -> 3: age 2 >= 2, evicted on retiring
    CHECK(consumer.lateReturnBufferSize() == 0);
    CHECK(consumer.lateReturnDroppedCount() == 1);
}

/******************************************************************************/

/******************************************************************************/
// Payload-only late returns: a slow-but-alive worker's late payload-only return is applicable to a
// retained original (the un-returned clone the consumer keeps when a batch retires MISSING), so it is
// reaped rather than dropped; only one with no retained original to apply it to is dropped.
/******************************************************************************/

TEST_CASE(
    "courtier(late): a payload-only late return is applied to a retained original",
    "[courtier][latereturn]"
) {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(
        /*cap*/ 8,
        /*ttl_rounds*/ 100
    ); // buffering on -> originals are retained

    // Abandon slot 2 forever: the batch retires with it MISSING, so its original (stored id 2) is retained
    // under its correlation id (slot index, batch_id 0).
    auto batch = make_batch(3);
    run_abandoning_forever(consumer, batch, /*victims*/ {2});
    REQUIRE(consumer.retainedOriginalCount() == 1); // the un-returned original was retained

    // The slow worker's result finally arrives carrying only a payload, no work item. It must be applied
    // to the retained original (which supplies the item, stored id 2) and parked -- not dropped.
    deliver_late_payloadonly(consumer, /*corr*/ 2);
    CHECK(consumer.lateReturnBufferSize() == 1);
    CHECK(consumer.lateReturnDroppedCount() == 0);
    CHECK(consumer.retainedOriginalCount() == 0); // the retained original became the parked return

    auto reaped = consumer.getLateReturns();
    REQUIRE(reaped.size() == 1);
    CHECK(reaped[0]->get_stored_number() == 2); // the item came from the retained original
    CHECK(reaped[0]->is_processed());           // ... carrying the returned outcome
}

TEST_CASE(
    "courtier(late): a payload-only late return with no retained original is dropped",
    "[courtier][latereturn]"
) {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 100);

    // No batch ever ran, so nothing is retained: a payload-only late return has nothing to be applied
    // to and must be dropped (counted), never parked (there would be no item for the algorithm to reap).
    deliver_late_payloadonly(consumer, /*corr*/ 12345);
    CHECK(consumer.lateReturnBufferSize() == 0);
    CHECK(consumer.lateReturnDroppedCount() == 1);
}

/******************************************************************************/
// The retained-originals store is itself TTL-bounded: a clone kept so a slow worker's later payload-only
// return can be applied must not be held forever if that worker never returns. The late_returns_ FIFO
// aging is pinned above; this is its retained-store counterpart (evictRetainedOriginals_locked), so the
// Phase-6 aging-store consolidation has the retention aging characterized directly, not just indirectly.
/******************************************************************************/

TEST_CASE(
    "courtier(late): a retained original ages out of the retention store past its TTL",
    "[courtier][latereturn]"
) {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(
        /*cap*/ 8,
        /*ttl_rounds*/ 2
    ); // small TTL: the retained clone must age out

    // Abandon slot 2 forever so the batch retires with it MISSING -> its original is retained (as in the
    // applied case above), but here the slow worker NEVER returns a result to apply.
    auto batch = make_batch(3);
    run_abandoning_forever(consumer, batch, /*victims*/ {2});
    REQUIRE(consumer.retainedOriginalCount() == 1);

    // Advance the dispatch-round epoch well past the TTL. With no payload-only return ever arriving, the
    // retained clone must be swept rather than held indefinitely.
    for(int round = 0; round < 4; ++round) {
        run_full_batch(consumer, 1);
    }
    CHECK(consumer.retainedOriginalCount() == 0); // aged out, not retained forever
    CHECK(consumer.lateReturnBufferSize() == 0);  // no result ever arrived, so nothing was buffered
}

/******************************************************************************/
// A late return is parked only for a slot that had not returned when its batch ended. A second copy
// of a result whose slot was completed in time is a duplicate: it is dropped and counted, never
// handed to the algorithm as an extra work item.
/******************************************************************************/

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- two linear sections; the count comes from the assertion macros
TEST_CASE(
    "courtier(late): a late duplicate of a result that was completed in time is not parked",
    "[courtier][latereturn]"
) {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 100); // buffering on

    SECTION("a second copy of a completed slot's result is dropped and counted") {
        auto batch = make_batch(2);
        item_ptr kept_item;
        c2::GProcessingOutcome kept_outcome;
        std::atomic<bool> finished{false};
        std::jthread worker([&] {
            consumer.processBatch(
                std::span<item_ptr>(batch.data(), batch.size()),
                c2::GSubmissionPolicy::full_success_or_fatal()
            );
            finished.store(true);
        });
        while(not finished.load()) {
            auto p = consumer.checkout();
            if(not p) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            p->process();
            if(not kept_item) {
                // A deep copy of the first returned item and its outcome, to send the result twice
                kept_item = std::make_unique<GFaultyContainer>(*p);
                kept_outcome = p->processingOutcome();
            }
            consumer.checkin(std::move(p));
        }
        worker.join();
        REQUIRE(kept_item);
        REQUIRE(count_processed(batch) == 2);
        REQUIRE(consumer.retainedOriginalCount() == 0); // both slots returned in time
        const auto dropped_before = consumer.lateReturnDroppedCount();

        // The same result arrives again, carrying its item, after its batch has ended.
        c2::GCommandContainerT<GFaultyContainer> frame{c2::GFrameKind::RESULT};
        frame.setOutcome(kept_outcome);
        frame.setItem(std::move(kept_item));
        consumer.checkin(std::move(frame));

        CHECK(consumer.lateReturnBufferSize() == 0);
        CHECK(consumer.lateReturnDroppedCount() == dropped_before + 1);
    }

    SECTION("control: the late return of a slot that had not returned is parked") {
        auto batch = make_batch(3);
        auto slow = run_abandoning_forever(consumer, batch, /*victims*/ {2});
        REQUIRE(slow.size() == 1);
        REQUIRE(slow[0]);
        REQUIRE(consumer.retainedOriginalCount() == 1);

        slow[0]->process();
        consumer.checkin(std::move(slow[0]));

        CHECK(consumer.lateReturnBufferSize() == 1);
        CHECK(consumer.lateReturnDroppedCount() == 0);
        CHECK(consumer.retainedOriginalCount() == 0);
    }
}

/******************************************************************************/
// A late result that cannot be applied to its retained original (the grafting of the retained
// state or the library's seam throws) is dropped and counted, like a late result with nothing to
// be applied to. It must not leave checkin(): in a server that would end the session's handler and
// the server with it, while the same result arriving in time only fails its item.
/******************************************************************************/

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; the count comes from the assertion macros
TEST_CASE(
    "courtier(late): a late result that cannot be applied is dropped and the consumer goes on",
    "[courtier][latereturn]"
) {
    const bool carries_item = GENERATE(true, false);
    INFO((carries_item ? "the late result carries its item" : "the late result is payload-only"));
    LateFaultConsumer consumer;
    consumer.setLateReturnBuffer(
        /*cap*/ 8,
        /*ttl_rounds*/ 100
    ); // buffering on -> originals are retained

    auto batch = make_late_fault_batch(3);
    auto slow = run_abandoning_forever(consumer, batch, /*victims*/ {2});
    REQUIRE(slow[0]);
    REQUIRE(consumer.retainedOriginalCount() == 1);

    applicationsToFail.store(1); // the late result's application throws
    if(carries_item) {
        slow[0]->process();
        CHECK_NOTHROW(consumer.checkin(std::move(slow[0])));
    }
    else {
        CHECK_NOTHROW(deliver_late_payloadonly(consumer, /*corr*/ 2));
    }
    CHECK(applicationsToFail.load() == 0);         // the application did throw
    CHECK(consumer.lateReturnDroppedCount() == 1); // the result is dropped and counted
    CHECK(consumer.lateReturnBufferSize() == 0);   // nothing is parked
    CHECK(consumer.retainedOriginalCount() == 0);  // and its retained original is gone with it

    // The consumer goes on: a following batch runs as usual
    auto next = make_late_fault_batch(3);
    static_cast<void>(run_abandoning_forever(consumer, next, /*victims*/ {}));
    for(const auto &item : next) {
        CHECK(item->is_processed());
    }
}

/******************************************************************************/
// The user's two time limits, read from a config file. They are a SERVER-side transport concern --
// when an unreturned item is handed out again or given up -- orthogonal to the algorithm's
// GSubmissionPolicy. Because an evaluation may run from microseconds to weeks, neither has a
// default: both are 0, no limit.
TEST_CASE(
    "courtier(timeout): the two time limits are read from the config file",
    "[courtier][timeout][config]"
) {
    SECTION("an absent config file materializes the two limits, both 0") {
        const auto path =
            std::filesystem::temp_directory_path() / "geneva_nettimeout_defaults.json";
        std::filesystem::remove(path);
        c2::GNetworkedTimeoutConfig cfg;
        cfg.load(path.string()); // absent -> written with defaults, then read back
        CHECK(cfg.reclaim_lease_ms == 0);
        CHECK(cfg.give_up_window_ms == 0);
        std::filesystem::remove(path);
    }

    SECTION("non-default values round-trip through the JSON config file") {
        const auto path =
            std::filesystem::temp_directory_path() / "geneva_nettimeout_roundtrip.json";
        std::filesystem::remove(path);
        c2::GNetworkedTimeoutConfig w;
        w.reclaim_lease_ms = 1234;
        w.give_up_window_ms = 54'321;
        w.load(path.string()); // absent -> writes THESE values, reads them back

        c2::GNetworkedTimeoutConfig r;
        r.load(path.string()); // file now present -> reads the written values into a fresh struct
        CHECK(r.reclaim_lease_ms == 1234);
        CHECK(r.give_up_window_ms == 54'321);
        std::filesystem::remove(path);
    }

    SECTION("a negative value is refused, naming its key") {
        SimNetConsumer consumer;
        c2::GNetworkedTimeoutConfig lease;
        lease.reclaim_lease_ms = -1;
        CHECK_THROWS_WITH(
            consumer.applyTimeoutConfig(lease),
            ContainsSubstring("reclaim_lease_ms")
        );
        c2::GNetworkedTimeoutConfig window;
        window.give_up_window_ms = -5;
        CHECK_THROWS_WITH(
            consumer.applyTimeoutConfig(window),
            ContainsSubstring("give_up_window_ms")
        );
    }
}

/******************************************************************************/
// Routing and bookkeeping of returns inside an ACTIVE batch: the range of the slot index, a return
// for a slot that was put back in the meantime, the dispatch state of slots outside a batch, and
// the counters that make these paths observable.

namespace {

/** @brief A SimNetConsumer that also exposes dispatch_(), so a test can register a batch that
 *  carries slots which do not take part in it (processBatch() itself marks every non-null slot for
 *  processing). */
class RawDispatchConsumer : public SimNetConsumer {
public:
    using c2::GNetworkedConsumerT<GFaultyContainer>::dispatch_;
};

/** @brief Blocks until @p consumer hands out an item (a batch must be, or become, active). */
template <typename consumer_type>
item_ptr checkoutBlocking(consumer_type &consumer) {
    while(true) {
        if(auto p = consumer.checkout()) {
            return p;
        }
        std::this_thread::sleep_for(1ms);
    }
}

} /* anonymous namespace */

TEST_CASE(
    "courtier(routing): two slots more than 2^16 apart do not share a correlation id",
    "[courtier][routing]"
) {
    // With a 16-bit slot index, slot 70000 aliased slot 70000 - 65536 = 4464: one result landed in
    // the other's item and the second slot never completed. Only these two slots are populated, so
    // the span is large but the batch is two items.
    constexpr std::size_t LOW = 4464;
    constexpr std::size_t HIGH = LOW + 65536;
    std::vector<item_ptr> batch(HIGH + 1);
    batch[LOW] = std::make_unique<GFaultyContainer>(LOW, fault_mode::NONE);
    batch[HIGH] = std::make_unique<GFaultyContainer>(HIGH, fault_mode::NONE);

    SimNetConsumer consumer;
    std::jthread worker([&] {
        consumer.processBatch(
            std::span<item_ptr>(batch.data(), batch.size()),
            c2::GSubmissionPolicy::full_success_or_fatal()
        );
    });

    auto first = checkoutBlocking(consumer);
    auto second = checkoutBlocking(consumer);
    CHECK(first->getCorrelationId() != second->getCorrelationId());

    // Return the HIGH slot first: with aliased ids it would have been written into the LOW slot.
    if(first->get_stored_number() != HIGH) {
        std::swap(first, second);
    }
    first->process();
    consumer.checkin(std::move(first));
    second->process();
    consumer.checkin(std::move(second));
    worker.join();

    CHECK(batch[LOW]->is_processed());
    CHECK(batch[HIGH]->is_processed());
    CHECK(batch[LOW]->get_stored_number() == LOW);
    CHECK(batch[HIGH]->get_stored_number() == HIGH);
    CHECK(consumer.droppedReturnCount() == 0);
    CHECK(SimNetConsumer::maxBatchSize() == std::size_t{16'777'216}); // 2^24
}

TEST_CASE("courtier(routing): a return for a put-back slot is accepted", "[courtier][routing]") {
    auto batch = make_batch(1);
    SimNetConsumer consumer;

    std::jthread worker([&] {
        consumer.processBatch(
            std::span<item_ptr>(batch.data(), batch.size()),
            c2::GSubmissionPolicy::full_success_or_fatal()
        );
    });

    auto p = checkoutBlocking(consumer);
    consumer.requeue(p->getCorrelationId()); // the slot is PENDING again, as after a lease reclaim
    CHECK(consumer.requeuedCount() == 1);

    p->process();
    consumer.checkin(std::move(p)); // the first holder returns after all

    // The slot must be done now. Before the fix the return was dropped and the slot served again.
    auto again = consumer.checkout();
    CHECK(again == nullptr);
    if(again) { // let the batch finish so the test ends instead of hanging
        again->process();
        consumer.checkin(std::move(again));
    }
    worker.join();

    CHECK(count_processed(batch) == 1);
    CHECK(consumer.droppedReturnCount() == 0);
}

TEST_CASE(
    "courtier(routing): a slot outside the batch is set to dispatch state NONE",
    "[courtier][routing]"
) {
    auto batch = make_batch(2);
    batch[0]->set_processing_status(c2::processingStatus::DO_PROCESS);
    // Slot 1 does not take part (it is not DO_PROCESS) but carries the state an earlier, timed-out
    // round left on it.
    batch[1]->setDispatchState(c2::dispatchState::IN_FLIGHT);

    RawDispatchConsumer consumer;
    std::jthread worker([&] {
        consumer.dispatch_(std::span<item_ptr>(batch.data(), batch.size()));
    });

    auto p = checkoutBlocking(consumer); // the batch is registered once this returns
    CHECK(p->get_stored_number() == 0);
    CHECK(batch[1]->getDispatchState() == c2::dispatchState::NONE);

    p->process();
    consumer.checkin(std::move(p));
    worker.join();
    CHECK(batch[0]->is_processed());
}

TEST_CASE("courtier(routing): duplicates and empty checkouts are counted", "[courtier][routing]") {
    SimNetConsumer consumer;
    CHECK(consumer.checkout() == nullptr); // nothing registered yet
    CHECK(consumer.emptyCheckoutCount() == 1);

    auto batch = make_batch(2);
    std::jthread worker([&] {
        consumer.processBatch(
            std::span<item_ptr>(batch.data(), batch.size()),
            c2::GSubmissionPolicy::full_success_or_fatal()
        );
    });

    auto a = checkoutBlocking(consumer);
    auto b = checkoutBlocking(consumer);
    a->process();
    auto duplicate = std::make_unique<GFaultyContainer>(*a); // same correlation id as a
    consumer.checkin(std::move(a));
    consumer.checkin(std::move(duplicate)); // the batch is still active (b is out): a duplicate
    CHECK(consumer.droppedReturnCount() == 1);

    b->process();
    consumer.checkin(std::move(b));
    worker.join();
    CHECK(count_processed(batch) == 2);
    CHECK(consumer.requeuedCount() == 0);
}

/******************************************************************************/
// A clone that throws at checkout. The slot used to be marked IN_FLIGHT and counted out of the
// pending set before the clone was made; when the clone threw, the exception left checkout() with
// the slot in flight and nobody holding it, and without a time limit the batch never ended.

namespace {

/** @brief A RawDispatchConsumer that can also be stopped, so a test whose batch does not finish can
 *  still end instead of hanging. */
class StoppableRawDispatchConsumer : public RawDispatchConsumer {
public:
    using c2::GNetworkedConsumerT<GFaultyContainer>::requestStop;
};

/** @brief Stops @p consumer at the end of a scope, so that a batch that still waits when a test
 *  fails half-way is released and its submitter can be joined. */
class StopAtEnd {
public:
    explicit StopAtEnd(StoppableRawDispatchConsumer &consumer)
      : consumer_(consumer) {
    }
    ~StopAtEnd() {
        consumer_.requestStop();
    }
    StopAtEnd(const StopAtEnd &) = delete;
    StopAtEnd &operator=(const StopAtEnd &) = delete;
    StopAtEnd(StopAtEnd &&) = delete;
    StopAtEnd &operator=(StopAtEnd &&) = delete;

private:
    StoppableRawDispatchConsumer &consumer_;
};

} /* anonymous namespace */

TEST_CASE(
    "courtier(checkout): an item whose clone throws is flagged as failed and the batch completes",
    "[courtier][routing]"
) {
    constexpr std::size_t N = 4;
    constexpr std::size_t BAD = 1;               // the item whose clone throws
    const bool standard = GENERATE(true, false); // a std::exception, or an int
    INFO((standard ? "the clone throws a std::exception" : "the clone throws an int"));
    auto batch = make_batch(N);
    for(auto &p : batch) {
        p->set_processing_status(c2::processingStatus::DO_PROCESS);
    }

    StoppableRawDispatchConsumer consumer; // no give-up window can hide a stuck slot (the default)
    consumer.setCloneFunction([standard](const item_ptr &src) -> item_ptr {
        if(src->get_stored_number() == BAD) {
            if(standard) {
                throw std::runtime_error("clone refused by the test");
            }
            throw 42; // NOLINT(hicpp-exception-baseclass,cert-err60-cpp) -- not a std::exception, on purpose
        }
        return std::make_unique<GFaultyContainer>(*src);
    });

    std::atomic<bool> finished{false};
    std::jthread worker([&] {
        consumer.dispatch_(std::span<item_ptr>(batch.data(), batch.size()));
        finished.store(true);
    });

    std::size_t n_thrown = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while(not finished.load() && std::chrono::steady_clock::now() < deadline) {
        item_ptr p;
        try {
            p = consumer.checkout();
        }
        catch(...) {
            ++n_thrown;
        }
        if(not p) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        p->process();
        consumer.checkin(std::move(p));
    }
    const bool completed = finished.load();
    if(not completed) {
        consumer.requestStop(); // let dispatch_ return so that the test ends instead of hanging
    }
    worker.join();

    CHECK(completed);
    CHECK(n_thrown == 0);
    CHECK(batch[BAD]->getProcessingStatus() == c2::processingStatus::ERROR_FLAGGED);
    CHECK(
        batch[BAD]->getStoredErrorDescriptions().find(
            standard ? "clone refused by the test" : "not a std::exception"
        ) != std::string::npos
    );
    CHECK(batch[BAD]->getDispatchState() == c2::dispatchState::DONE);
    for(std::size_t k = 0; k < N; ++k) {
        if(k != BAD) {
            CHECK(batch[k]->is_processed());
        }
    }
}

/******************************************************************************/
// A result whose work item another server process handed out. Batch and slot numbers start again
// in every process, so a client that outlived its server and connected to this one can bring back
// a result whose correlation id names a slot of this consumer. The lineage id of the item it was
// handed names the process that handed it out; such a result must not reach the slot.

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- the count is the assertion macros
TEST_CASE(
    "courtier(routing): a result whose item another server process handed out is dropped",
    "[courtier][routing]"
) {
    constexpr double FOREIGN_TIME = 4242.0; // the processing time the foreign result reports
    const c2::SUBMISSION_UUID_TYPE foreign_uuid{c2::detail::lineageSalt() ^ 1U, 42};

    auto batch = make_batch(1);
    StoppableRawDispatchConsumer consumer; // no give-up window ends the batch behind our back
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 8); // a misrouted return could be parked

    auto submitter = std::async(std::launch::async, [&] {
        consumer.processBatch(
            std::span<item_ptr>(batch.data(), batch.size()),
            c2::GSubmissionPolicy::full_success_or_fatal()
        );
    });
    // Declared after the submitter, so it is destroyed first: a test that fails half-way releases
    // the waiting batch, and the future can be joined.
    const StopAtEnd stop_at_end{consumer};

    auto p = checkoutBlocking(consumer);
    p->process();

    auto processing_time = [](const item_ptr &item) {
        return std::get<1>(item->getProcessingTimes());
    };

    SECTION("a foreign result that carries its item") {
        auto foreign_item = std::make_unique<GFaultyContainer>(*p);
        c2::GProcessingOutcome foreign = p->processingOutcome();
        foreign.processing_time = FOREIGN_TIME;
        foreign_item->applyProcessingOutcome(foreign); // the item reports the foreign value, too
        foreign.submission_uuid = foreign_uuid;
        c2::GCommandContainerT<GFaultyContainer> frame{c2::GFrameKind::RESULT};
        frame.setOutcome(foreign);
        frame.setItem(std::move(foreign_item));
        consumer.checkin(std::move(frame));
    }

    SECTION("a foreign payload-only result") {
        c2::GProcessingOutcome foreign = p->processingOutcome();
        foreign.processing_time = FOREIGN_TIME;
        foreign.submission_uuid = foreign_uuid;
        c2::GCommandContainerT<GFaultyContainer> frame{c2::GFrameKind::RESULT};
        frame.setOutcome(foreign);
        frame.setLibraryPayload({});
        consumer.checkin(std::move(frame));
    }

    REQUIRE(consumer.foreignReturnCount() == 1);
    REQUIRE(consumer.lateReturnBufferSize() == 0);
    REQUIRE(consumer.droppedReturnCount() == 0);
    REQUIRE(submitter.wait_for(200ms) == std::future_status::timeout); // the submitter still waits
    REQUIRE(processing_time(batch[0]) != FOREIGN_TIME);

    // The result of this process completes the batch with its own value.
    consumer.checkin(std::move(p));
    REQUIRE(submitter.wait_for(5s) == std::future_status::ready);
    submitter.get();
    CHECK(batch[0]->is_processed());
    CHECK(processing_time(batch[0]) != FOREIGN_TIME);
    CHECK(consumer.foreignReturnCount() == 1);
}
