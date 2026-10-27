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
 * Tier-1 tests for the courtier timeout machinery -- the adaptive timeout / death-detection (the
 * reclaim lease + the explicit put-back) and the clone-from-template refill. The networked timeout
 * logic lives in GNetworkedConsumerT independently of any actual socket, so it is exercised here
 * deterministically by a test subclass acting as a simulated transport: a driver thread plays the
 * role of clients (checkout / process / checkin), and can ABANDON an item (never check it back in,
 * = a client that died mid-evaluation) or explicitly REQUEUE one (= a websocket session-death
 * put-back). In both cases every item must still come back, via the reclaim lease / the put-back.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include <span>

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

std::vector<item_ptr> make_batch(std::size_t n, const std::vector<std::size_t> &faulty = {},
                                 fault_mode fm = fault_mode::THROW_PROCESSING) {
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
    using c2::GNetworkedConsumerT<GFaultyContainer>::checkout;
    using c2::GNetworkedConsumerT<GFaultyContainer>::checkin;
    using c2::GNetworkedConsumerT<GFaultyContainer>::requeue;
    // Expose the treatment-driven give-up window / reclaim lease so the timeout-treatment test can read them.
    using c2::GNetworkedConsumerT<GFaultyContainer>::currentStallWindow;
    using c2::GNetworkedConsumerT<GFaultyContainer>::currentLease;
};

/** @brief How the driver mistreats a chosen item the first time it sees it. */
enum class misbehave { abandon, requeue_it };

/**
 * Runs @p batch through a SimNetConsumer with a driver thread that processes items, but the FIRST
 * time it sees an item selected by @p victim it abandons or requeues it (per @p mode) -- simulating
 * a client death (reclaimed by the lease) or a session-death put-back. Returns once every slot is
 * resolved.
 */
template <typename VictimPred>
void run_with_misbehaviour(std::vector<item_ptr> &batch, misbehave mode, VictimPred victim) {
    SimNetConsumer consumer;
    consumer.setLeaseBootstrap(30ms);  // reclaim an abandoned item quickly
    consumer.setLeaseBounds(10ms, 5s);
    consumer.setSweepTick(5ms);

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
                consumer.requeue(p->getCorrelationId()); // immediate put-back (as a websocket session-death would do)
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
        consumer.processBatch(std::span<item_ptr>(batch.data(), batch.size()),
                              c2::GSubmissionPolicy::full_success_or_fatal());
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

/** @brief Delivers a single late PAYLOAD-ONLY return: a RETURN frame that names a slot by its
 *  correlation id but carries no work item, so the consumer must apply it to the retained original. */
void deliver_late_payloadonly(LateNetConsumer &consumer, c2::CORRELATION_ID_TYPE corr) {
    c2::GCommandContainerT<GFaultyContainer> frame{c2::GFrameKind::RETURN};
    c2::GProcessingOutcome outcome;
    outcome.status = c2::processingStatus::PROCESSED;
    outcome.correlation_id = corr;
    frame.setOutcome(outcome);
    frame.setLibraryPayload({}); // the library's own return payload (empty for the demo containers)
    consumer.checkin(std::move(frame));
}

/** @brief Runs an n-item batch but ABANDONS the victim slot forever (never checks it in). The batch
 *  therefore retires with that slot still MISSING, which -- with the late-return buffer enabled -- makes
 *  the consumer retain a clone of the victim's original (keyed by its correlation id) so a later
 *  payload-only return can be applied to it. The other slots are processed normally (so at least one return is
 *  observed and the give-up window engages). The first (only) dispatch round has batch_id 0, so the
 *  victim slot's correlation id is simply its slot index. */
void run_abandoning_forever(LateNetConsumer &consumer, std::vector<item_ptr> &batch,
                            std::size_t victim_stored) {
    consumer.setLeaseBootstrap(20ms);
    consumer.setLeaseBounds(10ms, 200ms);
    consumer.setSweepTick(5ms);
    std::atomic<bool> finished{false};
    std::jthread worker([&] {
        consumer.processBatch(std::span<item_ptr>(batch.data(), batch.size()),
                              c2::GSubmissionPolicy::clone_on_partial_return());
        finished.store(true);
    });
    while(not finished.load()) {
        auto p = consumer.checkout();
        if(not p) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if(p->get_stored_number() == victim_stored) {
            continue; // never check it in -> stays unreturned -> retired MISSING -> retained
        }
        p->process();
        consumer.checkin(std::move(p));
    }
    worker.join();
}

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("courtier(timeout): the reclaim lease recovers abandoned (dead-client) items",
          "[courtier][timeout]") {
    auto batch = make_batch(40);
    run_with_misbehaviour(batch, misbehave::abandon, [](std::size_t id) { return id % 4 == 0; });
    CHECK(count_processed(batch) == 40); // every item came back despite the "deaths"
}

TEST_CASE("courtier(timeout): an explicit requeue (session-death put-back) loses nothing",
          "[courtier][timeout]") {
    auto batch = make_batch(40);
    run_with_misbehaviour(batch, misbehave::requeue_it, [](std::size_t id) { return id % 3 == 0; });
    CHECK(count_processed(batch) == 40);
}

TEST_CASE("courtier(timeout): every item abandoned once is still recovered",
          "[courtier][timeout]") {
    auto batch = make_batch(24);
    run_with_misbehaviour(batch, misbehave::abandon, [](std::size_t) { return true; });
    CHECK(count_processed(batch) == 24);
}

/******************************************************************************/

TEST_CASE("courtier(clone): unresolved slots are refilled from the supplied template",
          "[courtier][clone]") {
    // A representative, already-evaluated template the algorithm hands down.
    constexpr std::size_t TEMPLATE_ID = 9999;
    auto tmpl = std::make_unique<GFaultyContainer>(TEMPLATE_ID, fault_mode::NONE);
    tmpl->set_processing_status(Gem::Courtier::processingStatus::DO_PROCESS);
    tmpl->process(); // a valid, evaluated template

    auto consumer = std::make_shared<c2::GStdThreadConsumerT<GFaultyContainer>>(4);

    const std::vector<std::size_t> faulty{2, 5, 9};
    auto batch = make_batch(12, faulty, fault_mode::THROW_PROCESSING);

    consumer->processBatch(std::span<std::unique_ptr<GFaultyContainer>>(batch.data(), batch.size()),
                           c2::GSubmissionPolicy::clone_on_partial_return(), std::move(tmpl));

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
// drop (disabled buffer, capacity overflow, TTL expiry) is counted observably.

TEST_CASE("courtier(late): a late return for a finished batch is buffered, not silently dropped",
          "[courtier][latereturn]") {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 8);

    deliver_late(consumer, 42, /*corr*/ 0); // no batch active -> a late arrival

    CHECK(consumer.lateReturnBufferSize() == 1);   // held for a later getOldWorkItems() to reap
    CHECK(consumer.lateReturnDroppedCount() == 0);  // nothing dropped
}

TEST_CASE("courtier(late): getLateReturns() drains the buffer and transfers the items",
          "[courtier][latereturn]") {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 8);

    deliver_late(consumer, 100, 0);
    deliver_late(consumer, 101, 1);
    REQUIRE(consumer.lateReturnBufferSize() == 2);

    auto reaped = consumer.getLateReturns(); // the OA-side getOldWorkItems() drains via this hook
    REQUIRE(reaped.size() == 2);
    CHECK(consumer.lateReturnBufferSize() == 0);             // buffer emptied
    CHECK(reaped[0]->get_stored_number() == 100);            // FIFO / arrival order preserved
    CHECK(reaped[1]->get_stored_number() == 101);
    CHECK(consumer.getLateReturns().empty());                // a second drain yields nothing
}

TEST_CASE("courtier(late): buffering disabled (default) counts the drop but holds nothing",
          "[courtier][latereturn]") {
    LateNetConsumer consumer; // default cap == 0 -> buffering disabled

    deliver_late(consumer, 1, 0);
    deliver_late(consumer, 2, 1);

    CHECK(consumer.lateReturnBufferSize() == 0);    // nothing retained
    CHECK(consumer.lateReturnDroppedCount() == 2);  // but the drops are observable, not silent
}

TEST_CASE("courtier(late): the buffer is capacity-bounded, evicting and counting the oldest",
          "[courtier][latereturn]") {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 2, /*ttl_rounds*/ 1000); // large TTL: only capacity evicts here

    deliver_late(consumer, 10, 0);
    deliver_late(consumer, 11, 1);
    deliver_late(consumer, 12, 2); // exceeds the cap of 2 -> oldest evicted

    CHECK(consumer.lateReturnBufferSize() == 2);
    CHECK(consumer.lateReturnDroppedCount() == 1);
}

TEST_CASE("courtier(late): a buffered entry is evicted once it ages past its TTL in rounds",
          "[courtier][latereturn]") {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 100, /*ttl_rounds*/ 2); // TTL of 2 dispatch rounds

    deliver_late(consumer, 7, 0);                 // buffered at epoch 0
    REQUIRE(consumer.lateReturnBufferSize() == 1);

    run_full_batch(consumer, 1);                  // epoch -> 1: age 1 < 2, still held
    CHECK(consumer.lateReturnBufferSize() == 1);

    run_full_batch(consumer, 1);                  // epoch -> 2: age 2 >= 2, evicted on the retire sweep
    CHECK(consumer.lateReturnBufferSize() == 0);
    CHECK(consumer.lateReturnDroppedCount() == 1);
}

/******************************************************************************/

/******************************************************************************/
// Payload-only late returns: a slow-but-alive worker's late payload-only return is applicable to a
// retained original (the un-returned clone the consumer keeps when a batch retires MISSING), so it is
// reaped rather than dropped; only one with no retained original to apply it to is dropped.
/******************************************************************************/

TEST_CASE("courtier(late): a payload-only late return is applied to a retained original",
          "[courtier][latereturn]") {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 100); // buffering on -> originals are retained

    // Abandon slot 2 forever: the batch retires with it MISSING, so its original (stored id 2) is retained
    // under its correlation id (slot index, batch_id 0).
    auto batch = make_batch(3);
    run_abandoning_forever(consumer, batch, /*victim_stored*/ 2);
    REQUIRE(consumer.retainedOriginalCount() == 1); // the un-returned original was retained

    // The slow worker's result finally arrives carrying only a payload, no work item. It must be applied
    // to the retained original (which supplies the item, stored id 2) and parked -- not dropped.
    deliver_late_payloadonly(consumer, /*corr*/ 2);
    CHECK(consumer.lateReturnBufferSize() == 1);
    CHECK(consumer.lateReturnDroppedCount() == 0);
    CHECK(consumer.retainedOriginalCount() == 0); // the retained original became the parked return

    auto reaped = consumer.getLateReturns();
    REQUIRE(reaped.size() == 1);
    CHECK(reaped[0]->get_stored_number() == 2);  // the item came from the retained original
    CHECK(reaped[0]->is_processed());            // ... carrying the returned outcome
}

TEST_CASE("courtier(late): a payload-only late return with no retained original is dropped",
          "[courtier][latereturn]") {
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

TEST_CASE("courtier(late): a retained original ages out of the retention store past its TTL",
          "[courtier][latereturn]") {
    LateNetConsumer consumer;
    consumer.setLateReturnBuffer(/*cap*/ 8, /*ttl_rounds*/ 2); // small TTL: the retained clone must age out

    // Abandon slot 2 forever so the batch retires with it MISSING -> its original is retained (as in the
    // applied case above), but here the slow worker NEVER returns a result to apply.
    auto batch = make_batch(3);
    run_abandoning_forever(consumer, batch, /*victim_stored*/ 2);
    REQUIRE(consumer.retainedOriginalCount() == 1);

    // Advance the dispatch-round epoch well past the TTL. With no payload-only return ever arriving, the
    // retained clone must be swept rather than held indefinitely.
    for(int round = 0; round < 4; ++round) {
        run_full_batch(consumer, 1);
    }
    CHECK(consumer.retainedOriginalCount() == 0); // aged out, not retained forever
    CHECK(consumer.lateReturnBufferSize() == 0);   // no result ever arrived, so nothing was buffered
}

/******************************************************************************/
// Configurable timeout treatment (adaptive / fixed / wait_indefinitely), read from a config file. The
// treatment is a SERVER-side transport concern -- WHEN an unreturned item is declared MISSING -- orthogonal
// to the algorithm's GSubmissionPolicy. Because a Geneva evaluation may run from microseconds to days, the
// default is scale-free (adaptive); this pins the config round-trip and the treatment -> window/lease mapping.
TEST_CASE("courtier(timeout): the treatment is config-file selectable", "[courtier][timeout][config]") {
    SECTION("treatment string <-> enum") {
        CHECK(c2::timeoutTreatmentFromString("adaptive") == c2::timeoutTreatment::adaptive);
        CHECK(c2::timeoutTreatmentFromString("fixed") == c2::timeoutTreatment::fixed);
        CHECK(c2::timeoutTreatmentFromString("wait_indefinitely") == c2::timeoutTreatment::wait_indefinitely);
        CHECK(c2::timeoutTreatmentFromString("bogus") == c2::timeoutTreatment::adaptive); // unknown -> default
        CHECK(c2::to_string(c2::timeoutTreatment::fixed) == "fixed");
        CHECK(c2::to_string(c2::timeoutTreatment::wait_indefinitely) == "wait_indefinitely");
    }

    SECTION("an absent config file materializes the scale-free adaptive defaults") {
        const auto path = std::filesystem::temp_directory_path() / "geneva_nettimeout_defaults.json";
        std::filesystem::remove(path);
        c2::GNetworkedTimeoutConfig cfg;
        cfg.load(path.string()); // absent -> written with defaults, then read back
        CHECK(cfg.treatment == "adaptive");
        CHECK(cfg.treatmentEnum() == c2::timeoutTreatment::adaptive);
        CHECK(cfg.lease_factor == 4.0);
        CHECK(cfg.stall_factor == 8.0);
        std::filesystem::remove(path);
    }

    SECTION("non-default values round-trip through the JSON config file") {
        const auto path = std::filesystem::temp_directory_path() / "geneva_nettimeout_roundtrip.json";
        std::filesystem::remove(path);
        c2::GNetworkedTimeoutConfig w;
        w.treatment = "fixed";
        w.fixed_lease_ms = 1234;
        w.stall_factor = 3.5;
        w.session_timeout_ms = 0;
        w.load(path.string()); // absent -> writes THESE values, reads them back

        c2::GNetworkedTimeoutConfig r;
        r.load(path.string()); // file now present -> reads the written values into a fresh struct
        CHECK(r.treatment == "fixed");
        CHECK(r.treatmentEnum() == c2::timeoutTreatment::fixed);
        CHECK(r.fixed_lease_ms == 1234);
        CHECK(r.stall_factor == 3.5);
        CHECK(r.session_timeout_ms == 0);
        std::filesystem::remove(path);
    }

    SECTION("the treatment drives the give-up window and reclaim lease") {
        SimNetConsumer consumer; // fresh: adaptive, no return samples observed yet

        // adaptive default, before any sample: the lease is the bootstrap value (10s default), and the
        // give-up window is the lower stall clamp (mean == 0 -> clamped up to min_stall == 2s).
        CHECK(consumer.currentLease() == 10s);
        CHECK(consumer.currentStallWindow() == 2s);

        // fixed: the two configured constants are returned verbatim, regardless of observed timings.
        c2::GNetworkedTimeoutConfig fixed_cfg;
        fixed_cfg.treatment = "fixed";
        fixed_cfg.fixed_stall_window_ms = 7'000;
        fixed_cfg.fixed_lease_ms = 9'000;
        consumer.applyTimeoutConfig(fixed_cfg);
        CHECK(consumer.currentStallWindow() == 7s);
        CHECK(consumer.currentLease() == 9s);

        // wait_indefinitely: an "effectively never" window/lease (>> any real run) so nothing is ever
        // declared MISSING on time. It must stay comfortably below the overflow boundary of a nanosecond
        // clock comparison, but far above any plausible run length.
        c2::GNetworkedTimeoutConfig wait_cfg;
        wait_cfg.treatment = "wait_indefinitely";
        consumer.applyTimeoutConfig(wait_cfg);
        CHECK(consumer.currentStallWindow() > std::chrono::hours(24 * 365 * 50)); // > 50 years
        CHECK(consumer.currentLease() > std::chrono::hours(24 * 365 * 50));
    }
}
