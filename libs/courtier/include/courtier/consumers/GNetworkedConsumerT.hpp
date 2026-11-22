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

#pragma once

#include "common/GGlobalDefines.hpp"

// Standard headers
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Geneva headers
#include "common/GErrorStreamer.hpp" // g_error_streamer (a refused time limit)
#include "common/GExceptions.hpp"    // gem_exception (a refused time limit)
#include "common/GLogger.hpp"        // glogger << ... << GWARNING (late-return drop warning)
#include "common/concurrency/GAgingStoreT.hpp" // the shared aging keyed+FIFO store (late-return facility)
#include "common/concurrency/GMPMCQueueT.hpp"            // the list of parked callers
#include "common/concurrency/GThreadSafeKeyedStoreT.hpp" // the per-session in-flight store (CheckoutLease)
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GCommandContainerT.hpp"       // the RESULT frame checkin() consumes
#include "courtier/GCourtierEnums.hpp"           // CORRELATION_ID_TYPE, dispatchState
#include "courtier/GCourtierHelperFunctions.hpp" // describeException()
#include "courtier/GProcessable.hpp"             // detail::lineageSalt (the foreign-return check)
#include "courtier/GServerSessionLogic.hpp"      // GWakeFn, GCheckoutResult
#include "courtier/GWireProtocolT.hpp"           // the library seam that interprets a return
#include "courtier/consumers/GNetworkedTimeoutConfig.hpp" // the user's two time limits

namespace Gem::Courtier {

/******************************************************************************/
/**
 * Base class for networked courtier consumers (websocket, MPI). It owns the work machinery
 * that turns the synchronous reconcile-the-span contract of GBaseConsumerT into the asynchronous,
 * checkout/return world of remote clients -- and it does so for MANY concurrent submitters at once
 * (the fan-in design), so several submitters can each submit to the SAME shared client pool
 * simultaneously (e.g. Geneva's meta-optimization over a population of inner algorithms).
 *
 *  - dispatch_(items) registers the round's items as a new BATCH (the scheduling state lives ON each
 *    item -- its dispatchState -- so there are no per-item side queues; each call BORROWS the caller's
 *    round vector, it never owns/copies the items). It then blocks until every slot of ITS batch
 *    reaches DONE, or until the user's give-up window elapses if one is set; items left DO_PROCESS ==
 *    MISSING are resubmitted/cloned/failed by the inherited reconciliation loop.
 *  - checkout()/checkin() are the queue endpoints a transport's session calls. checkout() serves the
 *    next pending slot, round-robin INTERLEAVED across all currently-active batches (so concurrent
 *    submitters make progress together, not one whole batch after another). checkin() matches a
 *    returned (deserialized) result to its slot by id and writes it back.
 *
 * The wire correlation token is `(batch_id, slot)`: batch_id selects WHICH submitter's batch a
 * result belongs to (so two concurrent batches' slot 0s never collide), slot the index within that
 * batch. A result whose batch_id is no
 * longer active (a late arrival from a batch that already timed out / finished) never reaches a
 * live slot -- this is what makes resubmission and lease-reclaim safe, and replaces the former
 * per-round generation. A batch holds at most maxBatchSize() items (the range of the slot index).
 *
 * Borrow contract: the
 * batch's vector is BORROWED for the duration of dispatch_; the consumer reads/serializes/schedules
 * through it but never takes ownership. It writes into the items in place -- a returned result in
 * checkin() (applyProcessingOutcome(), then GWireProtocolT::applyReturn()), a failure, and each
 * item's dispatch state -- but never replaces, frees or moves one during dispatch_: the submitter
 * stays the sole owner of its items, and their addresses do not change there. (Under the clone
 * policy the inherited reconciliation may replace an item whose type cannot load content in place,
 * see GBaseConsumerT::processBatch().)
 *
 * Time limits (applyTimeoutConfig()): by default there is none. A dispatch_ waits until its batch is
 * DONE or the consumer stops, however long an evaluation takes: that is the user's problem's business,
 * and nothing here guesses it. A lost client is noticed by its connection (both transports give signs
 * of life), and the session puts back what it held at once (CheckoutLease). A client that hangs inside
 * an evaluation while its process still answers is not noticed that way. The user may set two limits,
 * each 0 (none) by default:
 *  - the reclaim lease: an item in flight longer than this is put back and handed out again, on every
 *    transport. The first result back is taken; a later duplicate is dropped and counted.
 *  - the give-up window: a batch without any result for longer than this, counted from its start or
 *    its last result, is given up; its open slots are left DO_PROCESS == MISSING for the reconciliation
 *    loop, which resubmits them and then applies the submission policy.
 * The wait itself has no period: it sleeps until the batch is done, a stop, or the nearer of the
 * two limits is due.
 *
 * @tparam processable_type The work-item type the consumer schedules to remote clients and reconciles
 */
template <typename processable_type>
class GNetworkedConsumerT : public GBaseConsumerT<processable_type> {
public:
    using item_ptr = typename GBaseConsumerT<processable_type>::item_ptr;
    using clock = std::chrono::steady_clock;

    GNetworkedConsumerT() = default;
    ~GNetworkedConsumerT() override = default;

    /***************************************************************************/
    /**
     * @brief Sets the user's two time limits (typically read from a config file). Both are off
     * (0) by default; see the class comment.
     *
     * This is the SERVER-side "when is an item handed out again, or given up" concern; it is
     * orthogonal to the algorithm's GSubmissionPolicy (what to do about an item that was given up).
     *
     * @param cfg The parsed configuration to apply
     * @throws gem_exception if a value is negative or too large for the clock (the exception
     *         names the key); the limits are then left as they were
     */
    void applyTimeoutConfig(const GNetworkedTimeoutConfig &cfg) {
        const auto lease = checkedLimit_("reclaim_lease_ms", cfg.reclaim_lease_ms);
        const auto window = checkedLimit_("give_up_window_ms", cfg.give_up_window_ms);
        {
            std::scoped_lock const lk(mtx_);
            reclaim_lease_ = lease;
            give_up_window_ = window;
        }
        cv_done_.notify_all(); // a waiting batch takes the new limits at once
    }

    /***************************************************************************/
    /**
     * @brief Configures the bounded late-return buffer.
     *
     * A result that arrives after its batch already finished/timed out is normally dropped (its
     * batch_id is no longer active). With this buffer enabled, such a late arrival -- a genuinely
     * distinct evaluation that simply came back too late to be used this round -- is parked instead
     * of discarded, so the submitter can reap it via getLateReturns() (Geneva's
     * GOptimizationAlgorithmBase enables it via enableLateReturns() and drains it through
     * getLateReturns() in its getOldWorkItems()). The buffer is bounded two ways: @p cap (max items
     * held; 0 DISABLES buffering, the default) and @p ttl_rounds (a held item is evicted after this
     * many dispatch rounds). Each entry carries the dispatch-round "epoch" at which it was
     * buffered, used only for TTL eviction; the batch id is a 40-bit, process-wide, non-wrapping
     * counter, so a retired batch id is never re-minted and a buffered late return can never be
     * confused with a fresh batch. Evictions (cap or TTL) are counted (lateReturnDroppedCount())
     * and warned once -- never silently lost.
     *
     * Enabling this buffer ALSO makes dispatch_ retain a clone of every un-returned original
     * (keyed by correlation id, bounded by the same cap + ttl_rounds). A late return is parked only
     * when such a retained original exists for its correlation id; the retained clone is consumed
     * by it:
     * @li A late return that CARRIES its work item is parked as it came, with the retained state
     *     the wire strips grafted back from the retained original.
     * @li A PAYLOAD-ONLY late return (a RESULT frame with only a library payload, no work item) is
     *     applied TO the retained clone, which is then parked like a full return.
     *
     * A late return with NO matching retained original is DROPPED and counted in
     * lateReturnDroppedCount(), whether or not it carries its item: either its slot was completed
     * in time, so the frame duplicates a result the batch already used, or its original has
     * outlived the ttl. Parking it would hand the algorithm a result it already has, or one that
     * no longer belongs to a live round.
     *
     * @param cap Maximum number of late items held; 0 disables buffering (the default)
     * @param ttl_rounds A held item is evicted after this many dispatch rounds
     */
    void setLateReturnBuffer(std::size_t cap, std::uint64_t ttl_rounds) {
        late_store_.configure(cap, ttl_rounds);
    }
    /** @brief Number of late returns currently held in the buffer (reaped by the submitter via
     *  getLateReturns()).
     *  @return The count of late items currently buffered */
    [[nodiscard]] std::size_t lateReturnBufferSize() const {
        return late_store_.parkedSize();
    }
    /** @brief Number of un-returned originals currently retained so a later payload-only return can be
     *  applied to it (see setLateReturnBuffer()). Mostly for tests/diagnostics.
     *  @return The count of retained originals currently held */
    [[nodiscard]] std::size_t retainedOriginalCount() const {
        return late_store_.retainedSize();
    }
    /** @brief Total late returns dropped since construction -- an observable, non-silent drop count.
     *  Counts cap/TTL evictions, arrivals while buffering is disabled, late returns (carrying
     *  their item or payload-only) with no matching retained original (see setLateReturnBuffer()),
     *  and late returns that could not be applied to their retained original.
     *  @return The running total of dropped late returns */
    [[nodiscard]] std::uint64_t lateReturnDroppedCount() const {
        std::scoped_lock const lk(mtx_);
        return late_dropped_count_;
    }

    /** @brief GBaseConsumerT hook: enable/size the late-return buffer (delegates to
     *  setLateReturnBuffer; note that late returns with nothing retained are never buffered -- see
     *  there).
     *  @param cap Maximum number of late items held; 0 disables buffering
     *  @param ttl_rounds A held item is evicted after this many dispatch rounds */
    void enableLateReturns(std::size_t cap, std::uint64_t ttl_rounds) override {
        setLateReturnBuffer(cap, ttl_rounds);
    }

    /** @brief GBaseConsumerT hook: drain the late-return buffer, transferring the held items to the
     *  caller (the submitter; Geneva's algorithms reap them after each submission). FIFO / arrival
     *  order. Every parked item is a complete work item: a full late return with the retained state
     *  of its retained original grafted back, or a payload-only late return applied to the retained
     *  original it named on arrival (see setLateReturnBuffer()).
     *  @return The buffered late items in arrival order (ownership transferred; the buffer is emptied) */
    std::vector<item_ptr> getLateReturns() override {
        return late_store_.drainParked();
    }

    /** @brief A networked consumer has clients, and one that does not count them cannot tell how
     *  many are connected: std::nullopt, unless a derived consumer knows (GPeerConsumerT).
     *  @return std::nullopt */
    [[nodiscard]] std::optional<std::size_t> connectedClients() const override {
        return std::nullopt;
    }

    /***************************************************************************/
    /** @brief The largest batch a single dispatch round can hold: the range of the slot index in
     *  the correlation id. A larger span is refused with a fatal error rather than routed wrongly.
     *  @return The maximum number of items in one batch */
    [[nodiscard]] static constexpr std::size_t maxBatchSize() noexcept {
        return MAX_BATCH_SIZE;
    }

    /** @brief Number of in-flight items put back to pending since construction: by the user's
     *  reclaim lease, or by a session that lost its client.
     *  @return The running total of re-queued items */
    [[nodiscard]] std::uint64_t requeuedCount() const {
        std::scoped_lock const lk(mtx_);
        return n_requeued_;
    }
    /** @brief Number of returns for an ACTIVE batch that were not applied since construction: a
     *  duplicate of a slot that is already done, or a slot that does not take part in its batch.
     *  (A return for a batch that is no longer active is a late return -- see
     *  lateReturnDroppedCount().)
     *  @return The running total of dropped returns */
    [[nodiscard]] std::uint64_t droppedReturnCount() const {
        std::scoped_lock const lk(mtx_);
        return n_dropped_returns_;
    }
    /** @brief Number of returns dropped since construction because their work item was handed out
     *  by another server process (a client that outlived its server and connected to this one).
     *  @return The running total of foreign returns */
    [[nodiscard]] std::uint64_t foreignReturnCount() const {
        std::scoped_lock const lk(mtx_);
        return n_foreign_returns_;
    }
    /** @brief Number of checkouts that found nothing pending since construction. A transport
     *  answers each of them with "no work".
     *  @return The running total of empty checkouts */
    [[nodiscard]] std::uint64_t emptyCheckoutCount() const {
        std::scoped_lock const lk(mtx_);
        return n_empty_checkouts_;
    }
    /** @brief The end-of-run figures (GBaseConsumerT::recordEndOfRun()): the connected clients, if
     *  the consumer can tell (connectedClients()), and the counters above -- requeued items,
     *  dropped, foreign and dropped late returns, empty checkouts
     *  @return The figures, by name */
    [[nodiscard]] typename GBaseConsumerT<processable_type>::figures_type
    endOfRunFigures() const override {
        typename GBaseConsumerT<processable_type>::figures_type figures;
        if(const std::optional<std::size_t> clients = this->connectedClients()) {
            figures.emplace_back("clients_connected", *clients);
        }
        std::scoped_lock const lk(mtx_);
        figures.emplace_back("requeued", n_requeued_);
        figures.emplace_back("dropped_returns", n_dropped_returns_);
        figures.emplace_back("foreign_returns", n_foreign_returns_);
        figures.emplace_back("late_returns_dropped", late_dropped_count_);
        figures.emplace_back("empty_checkouts", n_empty_checkouts_);
        return figures;
    }
    /** @brief Number of callers that are parked right now, waiting to be told about work (see
     *  checkoutOrPark()). A caller that has gone away while parked is counted until the next
     *  announcement.
     *  @return The current length of the list of parked callers */
    [[nodiscard]] std::size_t parkedCount() const {
        return parked_.size();
    }

protected:
    /***************************************************************************/
    /**
     * A RAII checkout lease for transports with PERSISTENT, per-client sessions (e.g. the websocket
     * consumer, whose client keeps an open connection while it evaluates). A session holds one of these
     * and records EVERY item it currently has in flight (a prefetching client may hold several at once);
     * if the session is destroyed while still holding unreturned items -- the client disconnected
     * mid-evaluation -- the lease requeues all of them for other clients immediately, a liveness-driven
     * put-back that needs no time limit.
     *
     * Items are keyed by their (batch_id, slot) correlation id, so add()/remove() pair up regardless of
     * the order results come back in, and each remembers which checkout of its slot the session got
     * (GCheckoutResult::checkout). A put-back names that checkout: if the slot has been checked out
     * again since (the user's reclaim lease put it back and another session took it), the put-back does nothing,
     * so a lease gives back only its own checkout. The in-flight store is thread-safe: although a
     * single session drives its add()/remove() on one strand, the destructor may run on whichever io
     * thread releases the last reference, so the two must not race.
     */
    struct CheckoutLease {
        // Items travel by unique_ptr, so the lease cannot co-own them: it tracks only the in-flight
        // correlation ids (a borrow). The owning copy stays in the consumer's batch; on abandon the
        // lease asks the consumer to requeue those ids.
        Gem::Common::Concurrency::
            GThreadSafeKeyedStoreT<Gem::Courtier::CORRELATION_ID_TYPE, std::uint64_t>
                in_flight;
        /// Puts back one item: its correlation id and the checkout the session got
        std::move_only_function<void(Gem::Courtier::CORRELATION_ID_TYPE, std::uint64_t)> on_abandon;

        CheckoutLease() = default;
        CheckoutLease(const CheckoutLease &) = delete;
        CheckoutLease &operator=(const CheckoutLease &) = delete;

        /** @brief Records the item just handed to the session, and its checkout.
         *  @param p The item handed to the session (null is ignored)
         *  @param checkout Which checkout of its slot it is (GCheckoutResult::checkout) */
        void add(const item_ptr &p, std::uint64_t checkout) {
            if(not p) {
                return;
            }
            in_flight.set(p->getCorrelationId(), checkout);
        }
        /** @brief Drops an id the session is done with (nothing left for the lease to reclaim).
         *  @param id The correlation id of the slot
         *  @return The checkout the session got, if the id was held */
        std::optional<std::uint64_t> remove(Gem::Courtier::CORRELATION_ID_TYPE id) {
            return in_flight.take(id);
        }
        /** @brief Puts back everything the session still holds, at its end. */
        void abandonAll() {
            // Atomically take everything still in flight; requeue outside the set's lock.
            if(on_abandon) {
                for(auto [id, checkout] : in_flight.drain()) {
                    on_abandon(id, checkout);
                }
            }
            else {
                in_flight.clear();
            }
        }
        ~CheckoutLease() {
            abandonAll();
        }
    };

    /***************************************************************************/
    /** @brief Hands the next pending slot's item to a calling session, or null if none is pending.
     *  Round-robin across all active batches; the item's dispatch state flips PENDING -> IN_FLIGHT and
     *  correlation rides its (batch_id, slot) id.
     *  @return A clone of the next pending item to ship, or nullptr if no slot is pending */
    item_ptr checkout() {
        std::scoped_lock const lk(mtx_);
        return checkout_locked();
    }

    /***************************************************************************/
    /** @brief Like checkout(), but a caller for whom nothing is pending is parked instead of sent
     *  away empty-handed: @p wake is kept and called once, when work has become available (a
     *  batch was registered, an item was put back) or the server stops. The woken caller asks
     *  again; it may find that others were faster and be parked once more.
     *
     *  For transports whose sessions can wait and can be reached later (the websocket consumer).
     *  The check for pending work and the parking happen under one lock, and work is announced
     *  only after it was made pending under that lock, so no wake-up is lost. As many callers are
     *  woken as items became available, the one that has waited longest first. A woken caller
     *  must therefore ask again even if it has no use for the item any more (its client is
     *  gone), and put back what it gets: that wakes the next one.
     *
     *  @param wake Called once when the caller should ask again (see GWakeFn for what it may do)
     *  @return An item; or `parked`, with @p wake kept; or neither once the server is stopping */
    Gem::Courtier::GCheckoutResult<processable_type> checkoutOrPark(Gem::Courtier::GWakeFn wake) {
        std::scoped_lock const lk(mtx_);
        if(stop_.load()) {
            return {};
        }
        if(total_pending_ > 0) {
            std::uint64_t checkout = 0;
            if(auto item = checkout_locked(&checkout)) {
                return {.item = std::move(item), .checkout = checkout};
            }
            // Every pending slot failed to clone and was completed as failed: nothing to hand
            // out after all, so the caller is parked like one that found nothing pending.
        }
        static_cast<void>(parked_.push(std::move(wake))); // unbounded queue: push never fails
        return {.parked = true};
    }

    /***************************************************************************/
    /** @brief Accepts a returned RESULT frame and writes it into its slot. The frame's (batch_id, slot)
     *  is decoded from the correlation id in its processing outcome. A return whose batch is no
     *  longer active (timed out / finished) takes the late-return path (bufferLateReturn_locked():
     *  parked, or dropped and counted -- also when it cannot be applied, so that it does not end
     *  the caller either); a return for a slot that is already DONE (a duplicate) or does not take
     *  part in its batch is dropped and counted. A return for a slot that was put back to PENDING
     *  in the meantime is accepted: the evaluation is as good as the one that would otherwise be
     *  repeated. A return that cannot be applied to its slot (the library's seam throws) ends the
     *  slot as failed. So only a failed allocation makes this throw.
     *
     *  The frame is split at exactly the layer boundary: courtier applies the GProcessingOutcome (status,
     *  errors, timings, routing) itself, and hands the library tag + payload (or the returned work item)
     *  to the GWireProtocolT seam, which knows what an evaluation RESULT means. Courtier never inspects
     *  the latter.
     *
     *  @param frame The deserialized RESULT frame (ownership transferred) */
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved) -- consumed in place
    void checkin(GCommandContainerT<processable_type> &&frame) {
        item_ptr returned = frame.releaseItem();
        std::scoped_lock const lk(mtx_);
        // A result whose item was handed out by another server process (a client that outlived
        // its server and found this one) must not reach a slot: batch and slot numbers start
        // again in every process.
        if(not handedOutHere(frame)) {
            recordForeignReturn_locked();
            return;
        }
        const Gem::Courtier::CORRELATION_ID_TYPE id = frame.outcome().correlation_id;
        auto it = batches_.find(decodeBatch(id));
        if(it == batches_.end()) {
            // Batch no longer active: a late arrival from a timed-out/finished batch. Instead of
            // dropping it silently, park it in the bounded late-return buffer for a later
            // getLateReturns() to reap. With buffering disabled (the default), or when it cannot
            // be applied to its retained original, this still counts the drop rather than losing
            // it without trace.
            bufferLateReturn_locked(frame, std::move(returned));
            return;
        }
        BatchState &b = it->second;
        using Gem::Courtier::dispatchState;
        const std::size_t slot = decodeSlot(id);
        const auto ds = (slot < b.items.size() && b.items[slot]) ? b.items[slot]->getDispatchState()
                                                                 : dispatchState::NONE;
        if(ds != dispatchState::IN_FLIGHT && ds != dispatchState::PENDING) {
            ++n_dropped_returns_; // out of range, not in the batch, or a duplicate of a done slot
            return;
        }
        if(ds == dispatchState::PENDING) {
            // The slot was put back (reclaim lease / session put-back) and its first holder returns
            // after all. Take the result: the slot leaves the pending set and is not served again.
            --b.pending;
            --total_pending_;
        }
        b.last_progress = clock::now(); // the give-up window counts from here

        // Reconcile the return INTO the live slot object in place, keeping its heap address (the
        // slot aliases the submitter's own item). Rather than swapping the returned object in --
        // which would free the original and RELOCATE the submitter's item -- we keep the original
        // and write only what came back into it. The original already holds the submitted data
        // and the retained state (omitted on the wire), and address stability is what lets a
        // concurrent per-item prefetch of the submitter hold a snapshot of the batch's addresses
        // across the submission.
        //
        // A result that cannot be written into its slot leaves the slot in an unknown state, so
        // the slot is not handed out again as it is. It is flagged as failed, and the submission
        // policy decides what happens to it, as it does for an evaluation that failed.
        try {
            b.items[slot]->applyProcessingOutcome(frame.outcome());
            Gem::Courtier::GWireProtocolT<processable_type>::applyReturn(
                *b.items[slot],
                frame.tag(),
                frame.libraryPayload(),
                returned.get()
            );
        }
        catch(...) {
            failSlot_locked(
                b,
                slot,
                frame.outcome(),
                "The returned result could not be applied to its slot: " +
                    Gem::Courtier::describeException(std::current_exception()),
                "checkin()"
            );
            return;
        }
        completeSlot_locked(b, slot);
    }

    /***************************************************************************/
    /** @brief Whether the item of a returned frame was handed out by this server process. The
     *  lineage id of the checkout clone names the process; a frame from another process (a client
     *  that outlived its server) carries an id whose batch and slot mean nothing here.
     *  @param frame A returned frame
     *  @return true if this process handed its item out */
    [[nodiscard]] static bool handedOutHere(const GCommandContainerT<processable_type> &frame) {
        return frame.outcome().submission_uuid[0] == Gem::Courtier::detail::lineageSalt();
    }

    /***************************************************************************/
    /** @brief Convenience overload for callers that already hold a processed work item rather than a
     *  wire frame (the local consumers and the test harnesses): wraps it in the RESULT frame its own
     *  outcome describes and checks that in.
     *  @param p The processed work item (ownership transferred); null is ignored */
    void checkin(item_ptr p) {
        if(not p) {
            return;
        }
        GCommandContainerT<processable_type> frame{Gem::Courtier::GFrameKind::RESULT};
        frame.setOutcome(p->processingOutcome());
        frame.setItem(std::move(p));
        checkin(std::move(frame));
    }

    /***************************************************************************/
    /** @brief Returns a slot's still-in-flight item to PENDING so another client picks it up
     *  immediately (the RAII put-back on a client disconnect). The batch+slot are located via the
     *  passed correlation id. A no-op if the batch moved on, the slot is no longer in flight, or --
     *  with @p checkout -- the slot has been checked out again since that checkout (another holder
     *  has it now; see CheckoutLease).
     *  @param id The (batch_id, slot) correlation id of the item to put back
     *  @param checkout The checkout that is given back (GCheckoutResult::checkout); none: whichever
     *         holds the slot */
    void requeue(
        Gem::Courtier::CORRELATION_ID_TYPE id,
        std::optional<std::uint64_t> checkout = std::nullopt
    ) {
        bool requeued = false;
        {
            std::scoped_lock const lk(mtx_);
            auto it = batches_.find(decodeBatch(id));
            requeued =
                it != batches_.end() && requeueSlot_locked(it->second, decodeSlot(id), checkout);
        }
        if(requeued) {
            announceWork_(1);
        }
    }

    /***************************************************************************/
    /** @brief Ends a slot that is in flight as failed (ERROR_FLAGGED, @p what added to its error
     *  descriptions; see failSlot_locked()), for an item that a session checked out and could not
     *  send: its encoding threw. The submission policy then treats it as any failed evaluation. A
     *  no-op if the batch has moved on, the slot is no longer in flight, or it has been checked out
     *  again since @p checkout.
     *  @param id The (batch_id, slot) correlation id of the item
     *  @param checkout The checkout that failed (GCheckoutResult::checkout)
     *  @param what Why it failed */
    void failInFlight(
        Gem::Courtier::CORRELATION_ID_TYPE id,
        std::uint64_t checkout,
        const std::string &what
    ) {
        std::scoped_lock const lk(mtx_);
        auto it = batches_.find(decodeBatch(id));
        if(it == batches_.end()) {
            return;
        }
        BatchState &b = it->second;
        const std::size_t slot = decodeSlot(id);
        if(slot >= b.items.size() || not b.items[slot] ||
           b.items[slot]->getDispatchState() != Gem::Courtier::dispatchState::IN_FLIGHT ||
           b.checkouts[slot] != checkout) {
            return;
        }
        failSlot_locked(b, slot, b.items[slot]->processingOutcome(), what, "failInFlight()");
    }

    /***************************************************************************/
    /** @brief Whether the server is being torn down (sessions use this to stop accepting work).
     *  @return true once teardown has been requested */
    [[nodiscard]] bool stopped() const noexcept {
        return stop_.load();
    }

    /** @brief Requests teardown -- subclasses call this from their stop path. Wakes every waiter
     *  and every parked caller. The flag is set under the lock, so a caller either sees it or is
     *  parked (or waiting) by the time the wake-up goes out. */
    void requestStop() noexcept {
        {
            std::scoped_lock const lk(mtx_);
            stop_.store(true);
        }
        announceWork_(parked_.size()); // everyone: nobody is parked once the flag is set
        cv_done_.notify_all();
    }

    /***************************************************************************/
    /**
     * Registers @p items as a fresh batch (state lives on the items themselves, the vector is merely
     * BORROWED), waits for them to return (or time out), and -- because checkin() writes results
     * straight into their slots -- simply deregisters and returns. Items that never reached DONE are
     * left DO_PROCESS == MISSING for the reconciliation loop. Safe to call concurrently from many
     * threads: each call owns a distinct batch_id; the shared client pool is served round-robin.
     *
     * @param items The round's work items, BORROWED (not owned) for the duration of the call; results are written back into their slots in place
     */
    // NOLINTNEXTLINE(readability-function-size) -- one coherent batch dispatch lifecycle (register under lock, wait with the user's limits, then deregister + retain late-return originals) sharing the same locked BatchState across phases; splitting would fragment the locking discipline the comments document
    void dispatch_(std::span<item_ptr> items) override {
        const std::size_t n = items.size();
        if(n == 0) {
            return;
        }
        if(n > MAX_BATCH_SIZE) {
            // The slot index would not fit the correlation id: two slots would share an id and a
            // result could be written into the wrong item. Refuse instead.
            this->fatal_(
                "GNetworkedConsumerT::dispatch_(): a batch of " + std::to_string(n) +
                " items exceeds the maximum of " + std::to_string(MAX_BATCH_SIZE) +
                " items per batch. Submit the items in several smaller batches."
            );
            return;
        }

        const auto start = clock::now();
        typename std::map<batch_key_t, BatchState>::iterator my_it;
        std::size_t n_announced = 0; // how many items this call has made pending
        {
            std::scoped_lock const lk(mtx_);
            const batch_key_t key = (next_batch_id_++ & BATCH_MASK);
            BatchState b;
            b.items = items;
            b.cursor = 0;
            b.checked_out_at.assign(n, start); // indexed by slot (the span index)
            b.checkouts.assign(n, 0);
            b.last_progress = start;
            // Select exactly the DO_PROCESS slots of the span for this batch: tag each with its
            // (batch_id, slot) correlation token (slot == span index) and mark it PENDING. Slots in any
            // other state -- already resolved, or null -- are set to dispatchState NONE and are
            // ignored by checkout()/checkin()/leaseSweep(), so the span may carry non-participating
            // slots without affecting routing or the write-back position. The reset matters: an
            // item keeps the state an earlier round left on it (IN_FLIGHT for a slot that timed
            // out), and a stale IN_FLIGHT would be picked up by this batch's lease sweep.
            std::size_t n_sel = 0;
            for(auto &&[k, item] : items | std::views::enumerate) {
                if(not item) {
                    continue;
                }
                if(item->getProcessingStatus() == Gem::Courtier::processingStatus::DO_PROCESS) {
                    item->setCorrelationId(encodeId(key, static_cast<std::size_t>(k)));
                    item->setDispatchState(Gem::Courtier::dispatchState::PENDING);
                    ++n_sel;
                }
                else {
                    item->setDispatchState(Gem::Courtier::dispatchState::NONE);
                }
            }
            b.target = n_sel;
            b.pending = n_sel;
            my_it = batches_.emplace(key, std::move(b)).first;
            total_pending_ += n_sel;
            n_announced = n_sel;
        }
        announceWork_(n_announced);

        // Wait until every slot of THIS batch is DONE or the consumer stops. With the user's
        // limits: give up once nothing has come back for longer than the give-up window, and put
        // back every slot that has been in flight longer than the reclaim lease. Without them the
        // wait has no time-out at all; with them it sleeps until the nearer of the two is due.
        {
            std::unique_lock<std::mutex> lk(mtx_);
            BatchState &b =
                my_it->second; // stable across waits: std::map refs survive other ins/erase
            while(true) {
                if(b.done == b.target || stop_.load()) {
                    break;
                }
                const auto now = clock::now();
                std::optional<clock::time_point> wake_at;
                if(give_up_window_ > std::chrono::milliseconds::zero()) {
                    if(now - b.last_progress > give_up_window_) {
                        break; // nothing came back for too long -> give up; unresolved slots MISSING
                    }
                    wake_at = b.last_progress + give_up_window_;
                }
                if(reclaim_lease_ > std::chrono::milliseconds::zero()) {
                    const auto [n_put_back, next_due] = leaseSweep_locked(b, now);
                    if(n_put_back > 0) {
                        // Items were put back: tell the callers, without the lock. The batch's last
                        // check-in may arrive in that window, and its notification would find nobody
                        // waiting -- so look at the batch again instead of going to sleep.
                        lk.unlock();
                        announceWork_(n_put_back);
                        lk.lock();
                        continue;
                    }
                    if(next_due && (not wake_at || *next_due < *wake_at)) {
                        wake_at = next_due;
                    }
                }
                if(wake_at) {
                    cv_done_.wait_until(lk, *wake_at);
                }
                else {
                    cv_done_.wait(lk);
                }
            }
            // Slots are already updated in place by checkin(); slots never reached DONE keep their
            // original DO_PROCESS item == MISSING for the policy loop. Drop the still-PENDING slots
            // from the global count and deregister the batch (its borrowed vector is about to leave
            // dispatch_'s scope), so any very late checkin() for it becomes a no-op.
            //
            // Before deregistering, retain a CLONE of each un-returned (MISSING) original, keyed by its
            // correlation id, so a slow-but-alive worker's later payload-only return still has something
            // to be applied to, and can be reaped via getLateReturns() instead of dropped.
            // Only when buffering is enabled; bounded by the same TTL + cap as the late-return buffer.
            if(late_store_.buffering()) {
                for(std::size_t k = 0; k < b.items.size(); ++k) {
                    // Retain only THIS batch's still-unreturned slots (PENDING/IN_FLIGHT == MISSING):
                    // a DONE slot already returned, and a NONE slot never participated in this batch.
                    const auto ds = b.items[k] ? b.items[k]->getDispatchState()
                                               : Gem::Courtier::dispatchState::NONE;
                    if(b.items[k] && (ds == Gem::Courtier::dispatchState::PENDING ||
                                      ds == Gem::Courtier::dispatchState::IN_FLIGHT)) {
                        late_store_.retain(
                            b.items[k]->getCorrelationId(),
                            this->clone_item_(b.items[k])
                        );
                    }
                }
            }
            total_pending_ -= b.pending;
            batches_.erase(my_it);
            // A dispatch round completed: advance the late-buffer epoch and age out any TTL-expired
            // entries (so stale late returns / retained originals are evicted even when none arrives).
            // Parked entries evicted here are genuine drops -> record them.
            recordLateDrop_locked(late_store_.advanceEpochAndEvict());
        }
    }

private:
    /***************************************************************************/
    // The work item's correlation id (a uint64) carries the wire correlation token: the high 40
    // bits hold the batch_id (which submitter's batch), the low 24 the slot index within it. The
    // batch_id is drawn from a single monotonic process-wide counter (next_batch_id_) on the shared
    // consumer, so two concurrent submitters never collide; at 40 bits it does not wrap in any
    // realistic run (2^40 dispatch rounds -- more than thirty years at a thousand rounds per
    // second), which is what makes late-return routing safe -- a stale return can never alias a
    // freshly minted batch. The slot field bounds a single batch to 2^24 items; dispatch_() refuses
    // a larger span (see maxBatchSize()).
    using batch_key_t = Gem::Courtier::CORRELATION_ID_TYPE;
    static constexpr Gem::Courtier::CORRELATION_ID_TYPE SLOT_BITS = GCORRELATIONIDSLOTBITS;
    static constexpr Gem::Courtier::CORRELATION_ID_TYPE SLOT_MASK = (1ull << SLOT_BITS) - 1ull;
    static constexpr Gem::Courtier::CORRELATION_ID_TYPE BATCH_BITS = GCORRELATIONIDBATCHBITS;
    static constexpr Gem::Courtier::CORRELATION_ID_TYPE BATCH_MASK = (1ull << BATCH_BITS) - 1ull;
    static constexpr std::size_t MAX_BATCH_SIZE = std::size_t{1} << SLOT_BITS;

    /** @brief Packs a batch_id and slot into the on-wire correlation id (batch in the high 40 bits,
     *  slot in the low 24).
     *  @param batch The batch id (masked to 40 bits)
     *  @param slot The slot index within the batch (masked to 24 bits)
     *  @return The combined (batch_id, slot) correlation id */
    static Gem::Courtier::CORRELATION_ID_TYPE encodeId(batch_key_t batch, std::size_t slot) {
        return ((batch & BATCH_MASK) << SLOT_BITS) |
               (static_cast<Gem::Courtier::CORRELATION_ID_TYPE>(slot) & SLOT_MASK);
    }
    /** @brief Extracts the batch id from a correlation id.
     *  @param id The (batch_id, slot) correlation id
     *  @return The batch id (high 40 bits) */
    static batch_key_t decodeBatch(Gem::Courtier::CORRELATION_ID_TYPE id) {
        return (id >> SLOT_BITS) & BATCH_MASK;
    }
    /** @brief Extracts the slot index from a correlation id.
     *  @param id The (batch_id, slot) correlation id
     *  @return The slot index (low 24 bits) */
    static std::size_t decodeSlot(Gem::Courtier::CORRELATION_ID_TYPE id) {
        return static_cast<std::size_t>(id & SLOT_MASK);
    }

    /***************************************************************************/
    /** @brief Parks a late arrival (a return for a batch that already finished/timed out) instead of
     *  dropping it silently -- but only when an original was retained for its correlation id, i.e.
     *  its slot had not returned when its batch ended. Without one (a duplicate of a slot completed
     *  in time, an original past its ttl, or buffering disabled: cap == 0, the default) the drop is
     *  merely COUNTED. A late return that cannot be applied to its retained original
     *  (applyProcessingOutcome(), GWireProtocolT::applyReturn() or graftRetainedStateFrom() throws:
     *  a payload that does not fit, a user type's own code) is dropped and counted as well, with a
     *  warning that names its id and what was thrown; like a return arriving in time that cannot be
     *  applied, it must not end the caller. A parked item is later reaped by the submitter via
     *  getLateReturns(). Caller holds mtx_.
     *  @param frame The late-arriving RESULT frame (its outcome routes it; its payload is applied)
     *  @param p The work item the frame carried, or null for a payload-only return */
    void bufferLateReturn_locked(const GCommandContainerT<processable_type> &frame, item_ptr p) {
        const Gem::Courtier::CORRELATION_ID_TYPE id = frame.outcome().correlation_id;
        // Without a retained original there is nothing to apply the return to: a payload-only
        // return names a work item it does not carry, and a return that carries its item belongs
        // to a slot that was completed in time (or whose original has outlived the buffer) -- the
        // frame is a duplicate, and parking it would hand the algorithm a result it already has.
        auto orig = late_store_.takeRetained(id);
        if(not orig) {
            recordLateDrop_locked(1);
            return;
        }
        try {
            if(not p) {
                // A PAYLOAD-ONLY return: its batch was deregistered, so checkin()'s usual slot is
                // gone, but dispatch_ retained a CLONE of the un-returned original (keyed by
                // correlation id) for exactly this case. Apply the return to that clone and park it.
                p = std::move(*orig);
                p->applyProcessingOutcome(frame.outcome());
                Gem::Courtier::GWireProtocolT<processable_type>::applyReturn(
                    *p,
                    frame.tag(),
                    frame.libraryPayload(),
                    nullptr
                );
            }
            else {
                // A late return that carries its own work item is complete except for the
                // retained state the wire strips; graft that back from the retained original.
                p->graftRetainedStateFrom(**orig);
            }
        }
        catch(...) {
            glogger << "In GNetworkedConsumerT: the late result with the id " << id << '\n'
                    << "could not be applied to its retained original and is dropped: "
                    << Gem::Courtier::describeException(std::current_exception()) << '\n'
                    << GWARNING;
            recordLateDrop_locked(1);
            return;
        }
        if(not late_store_.buffering()) {
            recordLateDrop_locked(1); // buffering off: count the drop, do not hold the item
            return;
        }
        // Park it; any TTL/cap eviction the park triggers is a genuine drop -> record it.
        recordLateDrop_locked(late_store_.park(std::move(p)));
    }

    /** @brief Records that @p n late returns were dropped (disabled-buffer arrival, no retained
     *  original, a return that could not be applied to it, or cap/TTL eviction) and warns ONCE so the loss is observable without log spam.
     *  Caller holds mtx_.
     *  @param n The number of late returns just dropped, added to the running total */
    void recordLateDrop_locked(std::uint64_t n) {
        if(n == 0) {
            return; // nothing dropped (e.g. an eviction sweep that freed nothing) -> no count, no warning
        }
        late_dropped_count_ += n;
        if(not late_drop_warned_) {
            late_drop_warned_ = true;
            glogger << "In GNetworkedConsumerT: a late work-item return (a result for a" << '\n'
                    << "batch that had already finished or timed out) was dropped. It came" << '\n'
                    << "too late to be used this round, or it duplicated a result that was" << '\n'
                    << "used. Enable/enlarge the late-return buffer (setLateReturnBuffer)" << '\n'
                    << "to retain the late ones for the submitter to reap; the" << '\n'
                    << "running drop total is available via lateReturnDroppedCount()." << '\n'
                    << GWARNING;
        }
    }

    /** @brief Records that a return was dropped because its work item was handed out by another
     *  server process, and warns ONCE. Caller holds mtx_. */
    void recordForeignReturn_locked() {
        ++n_foreign_returns_;
        if(not foreign_return_warned_) {
            foreign_return_warned_ = true;
            glogger << "In GNetworkedConsumerT: a result arrived whose work item was" << '\n'
                    << "handed out by another server process; it is dropped. This happens" << '\n'
                    << "when a client outlives its server and connects to a new one. The" << '\n'
                    << "total is available via foreignReturnCount()." << '\n'
                    << GWARNING;
        }
    }

    /***************************************************************************/
    /** @brief Per-batch scheduling state. `items` is a BORROWED pointer to the caller's round vector
     *  (valid only while that call's dispatch_ blocks); everything else is this batch's bookkeeping. */
    struct BatchState {
        std::span<item_ptr> items; ///< Borrowed batch span (not owned); empty by default
        std::size_t target = 0;    ///< Number of slots in this batch
        std::size_t done = 0;      ///< Slots that have reached DONE
        std::size_t pending = 0;   ///< Slots currently PENDING (awaiting a client)
        std::size_t cursor = 0;    ///< Next slot index to consider in checkout
        std::vector<clock::time_point> checked_out_at; ///< Per-slot checkout time (reclaim lease)
        std::vector<std::uint64_t> checkouts; ///< Per-slot: the number of its current checkout
        clock::time_point last_progress; ///< Start, or the most recent checkin (give-up window)
    };

    /***************************************************************************/
    /** @brief Serves the next PENDING slot, round-robin across active batches for fairness (so no
     *  single submitter starves the others). Caller holds mtx_.
     *
     *  The slot's item is cloned before anything about the slot changes. A slot whose clone throws
     *  cannot be shipped; it is completed as failed (see failSlot_locked()) and the scan goes on
     *  with the next slot, so the exception neither leaves the slot in flight with nobody holding
     *  it nor reaches the session that asked for work.
     *  @param checkout Receives which checkout of its slot this is, if not null
     *  @return A clone of the next pending item to ship, or nullptr if no slot is pending */
    item_ptr checkout_locked(std::uint64_t *checkout = nullptr) {
        if(total_pending_ == 0 || batches_.empty()) {
            ++n_empty_checkouts_;
            return nullptr;
        }
        // Start just past the last-served batch and walk the (ordered) map once, wrapping around.
        auto it = batches_.upper_bound(last_served_batch_);
        for(std::size_t scanned = 0; scanned < batches_.size(); ++scanned) {
            if(it == batches_.end()) {
                it = batches_.begin();
            }
            BatchState &b = it->second;
            while(true) {
                // Skip non-participating slots: null entries and anything not PENDING (see
                // dispatch_'s selection comment -- the span may carry null / already-resolved
                // slots).
                while(b.cursor < b.items.size() &&
                      (not b.items[b.cursor] || b.items[b.cursor]->getDispatchState() !=
                                                    Gem::Courtier::dispatchState::PENDING)) {
                    ++b.cursor;
                }
                if(b.cursor >= b.items.size()) {
                    break; // nothing pending in this batch
                }
                const std::size_t k = b.cursor++;
                // Hand the session a clone to serialize and ship; the owning copy stays in the
                // slot, marked IN_FLIGHT. Its correlation id rides the clone, so checkin() finds
                // the slot again. Clone via the consumer's type-generic helper (polymorphic functor
                // or copy-construct), before the slot is touched: a clone that throws leaves
                // nothing to undo but the cursor, which has passed a slot that is about to be DONE.
                item_ptr clone;
                try {
                    clone = this->clone_item_(b.items[k]);
                }
                catch(...) {
                    --b.pending;
                    --total_pending_;
                    failSlot_locked(
                        b,
                        k,
                        b.items[k]->processingOutcome(),
                        "The work item could not be cloned for dispatch: " +
                            Gem::Courtier::describeException(std::current_exception()),
                        "checkout()"
                    );
                    continue; // on to the next PENDING slot of this batch
                }
                b.items[k]->setDispatchState(Gem::Courtier::dispatchState::IN_FLIGHT);
                b.checked_out_at[k] = clock::now();
                ++b.checkouts[k];
                if(checkout != nullptr) {
                    *checkout = b.checkouts[k];
                }
                --b.pending;
                --total_pending_;
                last_served_batch_ = it->first;
                return clone;
            }
            ++it;
        }
        ++n_empty_checkouts_;
        return nullptr;
    }

    /***************************************************************************/
    /** @brief Marks slot @p slot of batch @p b DONE and counts it; wakes the waiting dispatch_
     *  calls when that completes the batch. Caller holds mtx_.
     *  @param b The batch owning the slot
     *  @param slot The slot index that has been resolved */
    void completeSlot_locked(BatchState &b, std::size_t slot) {
        b.items[slot]->setDispatchState(Gem::Courtier::dispatchState::DONE);
        ++b.done;
        if(b.done == b.target) {
            cv_done_.notify_all(); // each waiting dispatch_ re-checks its own batch
        }
    }

    /***************************************************************************/
    /** @brief Ends slot @p slot of batch @p b as failed: the item gets @p outcome with status
     *  ERROR_FLAGGED and @p what appended to its error descriptions, a warning is logged, and the
     *  slot is completed (completeSlot_locked()). The submission policy then treats the item like
     *  any evaluation that failed. Used where the item cannot go on: a return that cannot be
     *  applied (checkin()), an item that cannot be cloned for dispatch (checkout) and one that a
     *  session cannot encode (failInFlight()). The caller
     *  holds mtx_ and has already taken the slot out of the pending counts if it was PENDING.
     *  @param b The batch owning the slot
     *  @param slot The slot index to fail
     *  @param outcome The lifecycle record to write onto the item (status and text are added here)
     *  @param what Why the item failed
     *  @param where The function that failed it (for the log) */
    void failSlot_locked(
        BatchState &b,
        std::size_t slot,
        Gem::Courtier::GProcessingOutcome outcome,
        const std::string &what,
        const char *where
    ) {
        outcome.status = Gem::Courtier::processingStatus::ERROR_FLAGGED;
        outcome.error_descriptions += what;
        b.items[slot]->applyProcessingOutcome(outcome);
        glogger << "In GNetworkedConsumerT::" << where << ": " << what << '\n'
                << "The work item is flagged as failed." << '\n'
                << GWARNING;
        completeSlot_locked(b, slot);
    }

    /***************************************************************************/
    /** @brief Flips a slot IN_FLIGHT -> PENDING (a put-back) within batch @p b and rewinds its cursor
     *  so checkout re-finds it. Caller holds mtx_.
     *  @param b The batch owning the slot
     *  @param slot The slot index to flip back to PENDING
     *  @param checkout If set, the slot is flipped only while this is its current checkout
     *  @return true if the slot was flipped; false if it was out of range, not currently IN_FLIGHT,
     *          or checked out again since @p checkout */
    bool requeueSlot_locked(
        BatchState &b,
        std::size_t slot,
        std::optional<std::uint64_t> checkout = std::nullopt
    ) {
        if(slot >= b.items.size() || not b.items[slot] ||
           b.items[slot]->getDispatchState() != Gem::Courtier::dispatchState::IN_FLIGHT ||
           (checkout && *checkout != b.checkouts[slot])) {
            return false;
        }
        b.items[slot]->setDispatchState(Gem::Courtier::dispatchState::PENDING);
        ++b.pending;
        ++total_pending_;
        ++n_requeued_;
        if(slot < b.cursor) {
            b.cursor = slot;
        }
        return true;
    }

    /***************************************************************************/
    /** @brief Puts back every slot of batch @p b that has been IN_FLIGHT longer than the user's
     *  reclaim lease (PENDING again, for re-service), and finds when the next slot becomes due.
     *  A slot that is PENDING now is checked out later than @p now, so it is due no earlier
     *  than @p now plus the lease: a checkout needs no wake-up of the waiting dispatch_. Caller
     *  holds mtx_; only called with a lease set.
     *  @param b The batch to sweep
     *  @param now The current time; each slot's checkout time is compared with the lease against it
     *  @return How many slots were put back (the caller then announces that much work), and when
     *          the next slot becomes due (none if no slot is in flight or pending) */
    [[nodiscard]] std::pair<std::size_t, std::optional<clock::time_point>>
    leaseSweep_locked(BatchState &b, clock::time_point now) {
        std::size_t n_put_back = 0;
        std::optional<clock::time_point> next_due;
        if(b.pending > 0) {
            next_due = now + reclaim_lease_;
        }
        for(std::size_t k = 0; k < b.items.size(); ++k) {
            if(not b.items[k] ||
               b.items[k]->getDispatchState() != Gem::Courtier::dispatchState::IN_FLIGHT) {
                continue;
            }
            const auto due = b.checked_out_at[k] + reclaim_lease_;
            if(now > due) {
                n_put_back += requeueSlot_locked(b, k) ? 1 : 0;
            }
            else if(not next_due || due < *next_due) {
                next_due = due;
            }
        }
        return {n_put_back, next_due};
    }

    /***************************************************************************/
    /** @brief Tells those who wait for work that @p n_items have become available: up to
     *  @p n_items parked callers. Called WITHOUT mtx_, by
     *  every place that made work pending (or set the stop flag) under it.
     *
     *  One caller is woken per item, not everyone: with many small batches and many idle
     *  clients, waking all costs several cores for wake-ups that find nothing. A parked caller
     *  that is gone does not count (its wake function says so), and one that was reached but
     *  cannot serve passes its item on by putting it back (see checkoutOrPark()). The length of
     *  the list is read once, so that a caller that parks again while this loop runs is not
     *  woken a second time in the same round.
     *
     *  noexcept on purpose: it runs in destructors (a session that ends puts its items back),
     *  and a wake-up that cannot be delivered for lack of memory has no sensible continuation.
     *
     *  @param n_items How many items became available */
    void announceWork_(std::size_t n_items) noexcept {
        for(std::size_t in_list = parked_.size(); in_list > 0 && n_items > 0; --in_list) {
            auto wake = parked_.try_pop();
            if(not wake) {
                break; // another announcement was faster
            }
            if((*wake)()) {
                --n_items;
            }
        }
    }

    /***************************************************************************/
    /** @brief A time limit of the config file as a duration, refused if it is negative or too
     *  large for the clock (a malformed configuration ends loudly, not quietly).
     *  @param key The config key, named in the exception
     *  @param ms The value in milliseconds (0: no limit)
     *  @return The limit
     *  @throws gem_exception if @p ms is negative or too large */
    static std::chrono::milliseconds checkedLimit_(const char *key, std::int64_t ms) {
        // Half the clock's range: a deadline (a time point plus the limit) must not overflow.
        constexpr auto max_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration::max()).count() /
            2;
        if(ms < 0 || ms > max_ms) {
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GNetworkedConsumerT::applyTimeoutConfig(): Error!" << '\n'
                << "The value " << ms << " of \"" << key << "\" is not a time limit: it must be"
                << '\n'
                << "0 (no limit) or a positive number of milliseconds up to " << max_ms << "."
                << '\n'
            );
        }
        return std::chrono::milliseconds(ms);
    }

    mutable std::mutex mtx_;
    std::condition_variable cv_done_; ///< Signalled when a batch's last slot reaches DONE

    std::map<batch_key_t, BatchState> batches_; ///< All currently-active batches, keyed by batch_id
    batch_key_t next_batch_id_ = 0;             ///< Monotonic batch_id source (masked to 40 bits)
    batch_key_t last_served_batch_ = 0; ///< Round-robin cursor across batches (for fairness)
    std::size_t total_pending_ = 0;     ///< Slots PENDING across ALL batches (cv predicate)
    /// The callers parked by checkoutOrPark(), each as its wake function. Pushed under mtx_,
    /// popped without it (see announceWork_()).
    Gem::Common::Concurrency::GMPMCQueueT<Gem::Courtier::GWakeFn, 0> parked_;

    // --- late-return facility: a result that arrives after its batch finished/timed out is PARKED
    //     for a later getLateReturns() to reap instead of dropped, and a clone of each un-returned
    //     original is RETAINED (keyed by correlation id) so a late payload-only return can still be
    //     applied. Both live in one shared aging store (FIFO + keyed faces, one epoch + cap + TTL);
    //     the epoch advances once per retired batch. Disabled by default (cap == 0); when enabled,
    //     the submitter's reaper (Geneva's GOptimizationAlgorithmBase, via enableLateReturns() /
    //     getLateReturns()) drains the FIFO. The apply-or-drop policy and the drop accounting below
    //     are the consumer's; the store is invoked under mtx_, so its operations stay consistent
    //     with the batch bookkeeping. ---
    Gem::Common::Concurrency::GAgingStoreT<Gem::Courtier::CORRELATION_ID_TYPE, item_ptr>
        late_store_;
    std::uint64_t late_dropped_count_ =
        0; ///< Total late items dropped (disabled/cap/TTL) -- observable

    // --- counters for diagnostics and tests (guarded by mtx_; see the getters) ---
    std::uint64_t n_requeued_ = 0;        ///< In-flight items put back to pending
    std::uint64_t n_dropped_returns_ = 0; ///< Returns for an active batch that were not applied
    std::uint64_t n_foreign_returns_ = 0; ///< Returns whose item another server process handed out
    std::uint64_t n_empty_checkouts_ = 0; ///< Checkouts that found nothing pending
    bool late_drop_warned_ = false;      ///< One-shot warning guard for the first dropped late item
    bool foreign_return_warned_ = false; ///< One-shot warning guard for the first foreign return

    std::atomic<bool> stop_{false};

    // --- the user's two time limits (applyTimeoutConfig()); 0: none, the default ---
    std::chrono::milliseconds reclaim_lease_{0}; ///< An item in flight longer than this is put back
    std::chrono::milliseconds give_up_window_{
        0
    }; ///< A batch without a result this long is given up
};

/******************************************************************************/

} /* namespace Gem::Courtier */
