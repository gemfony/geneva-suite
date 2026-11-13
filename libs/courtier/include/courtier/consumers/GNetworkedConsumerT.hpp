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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

// Geneva headers
#include "common/GLogger.hpp" // glogger << ... << GWARNING (late-return drop warning)
#include "common/concurrency/GAgingStoreT.hpp" // the shared aging keyed+FIFO store (late-return facility)
#include "common/concurrency/GThreadSafeSetT.hpp" // the per-session in-flight borrow set (CheckoutLease)
#include "courtier/GCourtierEnums.hpp" // CORRELATION_ID_TYPE, dispatchState
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GCommandContainerT.hpp" // the RETURN frame checkin() consumes
#include "courtier/GWireProtocolT.hpp"     // the library seam that interprets a return
#include "courtier/consumers/GNetworkedTimeoutConfig.hpp" // timeoutTreatment + the config struct

namespace Gem::Courtier {

/******************************************************************************/
/**
 * Base class for networked courtier consumers (ASIO, websocket, MPI). It owns the work machinery
 * that turns the synchronous reconcile-the-span contract of GBaseConsumerT into the asynchronous,
 * checkout/return world of remote clients -- and it does so for MANY concurrent submitters at once
 * (the fan-in design), so a population of optimization algorithms can each submit to the SAME shared
 * client pool simultaneously (e.g. a meta-optimization over a population of inner algorithms).
 *
 *  - dispatch_(items) registers the round's items as a new BATCH (the scheduling state lives ON each
 *    item -- its dispatchState -- so there are no per-item side queues; each call BORROWS the caller's
 *    round vector, it never owns/copies the items). It then blocks until every slot of ITS batch
 *    reaches DONE or that batch's adaptive give-up window elapses; items left DO_PROCESS == MISSING are
 *    resubmitted/cloned/failed by the inherited reconciliation loop.
 *  - checkout()/checkin() are the queue endpoints a transport's session calls. checkout() serves the
 *    next pending slot, round-robin INTERLEAVED across all currently-active batches (so concurrent
 *    submitters make progress together, not one whole batch after another). checkin() matches a
 *    returned (deserialized) result to its slot by id and writes it back.
 *
 * The wire correlation token is `(batch_id, slot)` -- the reincarnation of the old broker's
 * bufferport routing: batch_id selects WHICH submitter's batch a result belongs to (so two concurrent
 * batches' slot 0s never collide), slot the index within that batch. A result whose batch_id is no
 * longer active (a late arrival from a batch that already timed out / finished) is silently dropped --
 * this is what makes resubmission and lease-reclaim safe, and replaces the former per-round generation.
 *
 * Borrow contract: the
 * batch's vector is BORROWED for the duration of dispatch_; the consumer reads/serializes/schedules
 * through it but never takes ownership. The only write is checkin() swapping a slot's pointer for the
 * deserialized result -- the population stays the sole owner of its individuals.
 *
 * Timeout / death-detection (per batch, user-selectable via applyTimeoutConfig()): each dispatch_ waits
 * until its batch is DONE or progress stalls past a give-up window; items left DO_PROCESS == MISSING are
 * resubmitted/cloned/failed by the inherited reconciliation loop. The give-up window (and the reclaim lease
 * below) follow the configured timeoutTreatment:
 *  - adaptive (the default): a multiple of the running mean return time, clamped -- scale-free, so it
 *    self-calibrates to an evaluation that may run from microseconds to days;
 *  - fixed: a user-set fixed window / lease (for a user who knows their time bound);
 *  - wait_indefinitely: never declare an item MISSING on time (for a reliable cluster + a need-all policy).
 * The give-up window only applies ONCE at least one result has ever been received: before that, dispatch_
 * waits INDEFINITELY, because under a batch-scheduling system the first worker may not enter the pool for
 * minutes or hours. While waiting, where the transport has no client-liveness signal (usesTimeLease(),
 * default true: ASIO/MPI), stuck in-flight items of that batch are reclaimed once they exceed the reclaim
 * LEASE. A transport that detects client death directly (the websocket consumer, via its persistent session
 * + CheckoutLease) reclaims immediately on disconnect and disables the time lease, so it honours the
 * give-up treatment but is not bound by the time lease.
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
    /** @brief Sets the lease used to reclaim a stuck in-flight item before the running mean is known.
     *  @param w The bootstrap reclaim lease (used until the first return time is observed) */
    void setLeaseBootstrap(std::chrono::milliseconds w) { lease_bootstrap_ = w; }
    /** @brief Sets the multiple of the running mean return time used as the reclaim lease.
     *  @param f The multiplier applied to the running mean return time to form the (clamped) reclaim lease */
    void setLeaseFactor(double f) { lease_factor_ = f; }
    /** @brief Sets the lower/upper clamp on the adaptive reclaim lease.
     *  @param lo The lower clamp on the reclaim lease
     *  @param hi The upper clamp on the reclaim lease */
    void setLeaseBounds(std::chrono::milliseconds lo, std::chrono::milliseconds hi) {
        min_lease_ = lo;
        max_lease_ = hi;
    }
    /** @brief Sets the poll interval at which dispatch_ re-evaluates the lease/stall while waiting.
     *  @param w The re-evaluation poll interval */
    void setSweepTick(std::chrono::milliseconds w) { sweep_tick_ = w; }
    /** @brief Selects the timeout treatment (adaptive / fixed / wait_indefinitely) directly.
     *  @param t The treatment governing when an unreturned item is declared MISSING */
    void setTimeoutTreatment(timeoutTreatment t) { treatment_ = t; }

    /***************************************************************************/
    /**
     * @brief Applies a networked-consumer timeout configuration (typically read from a config file).
     *
     * Sets the treatment and every adaptive/fixed knob from @p cfg. A derived consumer with an additional,
     * transport-specific timeout (e.g. the ASIO per-exchange session deadline) overrides this to consume the
     * relevant extra field and then calls the base. This is the SERVER-side "when is an item lost" transport
     * concern; it is orthogonal to the algorithm's GSubmissionPolicy (what to do about a lost item).
     *
     * @param cfg The parsed timeout configuration to apply
     */
    virtual void applyTimeoutConfig(const GNetworkedTimeoutConfig &cfg) {
        treatment_ = cfg.treatmentEnum();
        ema_alpha_ = cfg.ema_alpha;
        lease_factor_ = cfg.lease_factor;
        stall_factor_ = cfg.stall_factor;
        lease_bootstrap_ = std::chrono::milliseconds(cfg.lease_bootstrap_ms);
        min_lease_ = std::chrono::milliseconds(cfg.min_lease_ms);
        max_lease_ = std::chrono::milliseconds(cfg.max_lease_ms);
        min_stall_ = std::chrono::milliseconds(cfg.min_stall_ms);
        max_stall_ = std::chrono::milliseconds(cfg.max_stall_ms);
        sweep_tick_ = std::chrono::milliseconds(cfg.sweep_tick_ms);
        fixed_stall_window_ = std::chrono::milliseconds(cfg.fixed_stall_window_ms);
        fixed_lease_ = std::chrono::milliseconds(cfg.fixed_lease_ms);
    }

    /***************************************************************************/
    /**
     * @brief Configures the bounded late-return buffer.
     *
     * A result that arrives after its batch already finished/timed out is normally dropped (its
     * batch_id is no longer active). With this buffer enabled, such a late arrival -- a genuinely
     * distinct evaluation that simply came back too late to be used this round -- is parked instead
     * of discarded, so an optimization algorithm can reap it via getOldWorkItems() (the
     * GOptimizationAlgorithmBase reaper enables it via enableLateReturns() and drains it through
     * getLateReturns()/getOldWorkItems()). The buffer is bounded two ways: @p cap (max items held; 0 DISABLES buffering, the
     * default) and @p ttl_rounds (a held item is evicted after this many dispatch rounds). Each entry
     * carries the dispatch-round "epoch" at which it was buffered, used only for TTL eviction; the batch
     * id is a 48-bit, process-wide, non-wrapping counter, so a retired batch id is never re-minted and a
     * buffered late return can never be confused with a fresh batch. Evictions (cap or TTL) are counted
     * (lateReturnDroppedCount()) and warned once -- never silently lost.
     *
     * PAYLOAD-ONLY late returns: a RETURN frame may carry only a library payload (computed values) rather
     * than the whole work item, in which case there is no item to park -- the item it describes is the
     * one the server dispatched, and by the time a LATE return arrives its batch has been reconciled and
     * that item is gone. To make such a slow-but-alive worker's result usable, enabling this buffer ALSO
     * makes dispatch_ retain a clone of every un-returned original (keyed by correlation id, bounded by
     * the same cap + ttl_rounds). A late payload-only return is then applied TO that retained clone,
     * which is parked like a full return; only one with NO matching retained clone (e.g. one that
     * outlived the ttl) is DROPPED (and counted in lateReturnDroppedCount()), since there is nothing for
     * the algorithm to reap.
     *
     * @param cap Maximum number of late items held; 0 disables buffering (the default)
     * @param ttl_rounds A held item is evicted after this many dispatch rounds
     */
    void setLateReturnBuffer(std::size_t cap, std::uint64_t ttl_rounds) {
        late_store_.configure(cap, ttl_rounds);
    }
    /** @brief Number of late returns currently held in the buffer (reaped by the OA via getOldWorkItems()).
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
     *  Counts cap/TTL evictions, arrivals while buffering is disabled, and payload-only late returns
     *  with no matching retained original to apply it to (see setLateReturnBuffer()).
     *  @return The running total of dropped late returns */
    [[nodiscard]] std::uint64_t lateReturnDroppedCount() const {
        std::scoped_lock const lk(mtx_);
        return late_dropped_count_;
    }

    /** @brief GBaseConsumerT hook: enable/size the late-return buffer (delegates to setLateReturnBuffer;
     *  note that payload-only late returns with nothing retained are never buffered -- see there).
     *  @param cap Maximum number of late items held; 0 disables buffering
     *  @param ttl_rounds A held item is evicted after this many dispatch rounds */
    void enableLateReturns(std::size_t cap, std::uint64_t ttl_rounds) override {
        setLateReturnBuffer(cap, ttl_rounds);
    }

    /** @brief GBaseConsumerT hook: drain the late-return buffer, transferring the held items to the
     *  caller (the optimization algorithm reaps them in fixAfterJobSubmission). FIFO / arrival order.
     *  Every parked item is a complete work item: a full late return as-is, or a payload-only late
     *  return applied to the retained original it named on arrival (see setLateReturnBuffer()).
     *  @return The buffered late items in arrival order (ownership transferred; the buffer is emptied) */
    std::vector<item_ptr> getLateReturns() override {
        return late_store_.drainParked();
    }

protected:
    /***************************************************************************/
    /**
     * A RAII checkout lease for transports with PERSISTENT, per-client sessions (e.g. the websocket
     * consumer, whose client keeps an open connection while it evaluates). A session holds one of these
     * and records EVERY item it currently has in flight (a prefetching client may hold several at once);
     * if the session is destroyed while still holding unreturned items -- the client disconnected
     * mid-evaluation -- the lease requeues all of them for other clients immediately, a liveness-driven
     * put-back that does not wait out the time lease. Transports with one-shot, per-request connections
     * (ASIO) must NOT use this (their sessions end normally after every exchange); they rely on the time
     * lease instead -- see usesTimeLease().
     *
     * Items are keyed by their (batch_id, slot) correlation id, so add()/remove() pair up regardless of
     * the order results come back in. The in-flight set is a thread-safe GThreadSafeSetT: although a
     * single session drives its add()/remove() on one strand, the destructor may run on whichever io
     * thread releases the last reference, so the two must not race.
     */
    struct CheckoutLease {
        // Items travel by unique_ptr, so the lease cannot co-own them: it tracks only the in-flight
        // correlation ids (a borrow). The owning copy stays in the consumer's batch; on abandon the
        // lease asks the consumer to requeue those ids.
        Gem::Common::Concurrency::GThreadSafeSetT<Gem::Courtier::CORRELATION_ID_TYPE> in_flight;
        std::move_only_function<void(Gem::Courtier::CORRELATION_ID_TYPE)> on_abandon;

        CheckoutLease() = default;
        CheckoutLease(const CheckoutLease &) = delete;
        CheckoutLease &operator=(const CheckoutLease &) = delete;

        /** @brief Records the id of an item just handed to the session.
         *  @param p The item handed to the session (its correlation id is tracked as in-flight; null is ignored) */
        void add(const item_ptr &p) {
            if(not p) {
                return;
            }
            in_flight.insert(p->getCorrelationId());
        }
        /** @brief Drops an id the session returned normally (nothing left for the lease to reclaim).
         *  @param id The correlation id of the returned slot, dropped from the in-flight set */
        void remove(Gem::Courtier::CORRELATION_ID_TYPE id) {
            in_flight.erase(id);
        }
        ~CheckoutLease() {
            // Atomically take everything still in flight; requeue outside the set's lock.
            if(on_abandon) {
                for(auto id : in_flight.drain()) {
                    on_abandon(id);
                }
            }
            else {
                in_flight.clear();
            }
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
    /** @brief Like checkout(), but blocks up to @p wait for a slot to become available before
     *  giving up and returning null. Keeps a transport from churning "no data" responses during the
     *  gaps between batches.
     *  @param wait The maximum time to block waiting for a pending slot
     *  @return A clone of the next pending item, or nullptr if none became available within @p wait */
    item_ptr checkoutWait(std::chrono::milliseconds wait) {
        std::unique_lock<std::mutex> lk(mtx_);
        if(total_pending_ == 0) {
            cv_work_.wait_for(lk, wait, [this] { return total_pending_ > 0 || stop_.load(); });
        }
        return checkout_locked();
    }

    /***************************************************************************/
    /** @brief Accepts a returned RETURN frame and writes it into its slot. The frame's (batch_id, slot)
     *  is decoded from the correlation id in its processing outcome; a return whose batch is no longer
     *  active (timed out / finished) or whose slot is no longer IN_FLIGHT (a duplicate) is dropped,
     *  which is what makes resubmission safe.
     *
     *  The frame is split at exactly the layer boundary: courtier applies the GProcessingOutcome (status,
     *  errors, timings, routing) itself, and hands the library tag + payload (or the returned work item)
     *  to the GWireProtocolT seam, which knows what an evaluation RESULT means. Courtier never inspects
     *  the latter.
     *
     *  @param frame The deserialized RETURN frame (ownership transferred) */
    void checkin(GCommandContainerT<processable_type> &&frame) {
        item_ptr returned = frame.releaseItem();
        std::scoped_lock const lk(mtx_);
        const Gem::Courtier::CORRELATION_ID_TYPE id = frame.outcome().correlation_id;
        auto it = batches_.find(decodeBatch(id));
        if(it == batches_.end()) {
            // Batch no longer active: a late arrival from a timed-out/finished batch. Instead of
            // dropping it silently, park it in the bounded late-return buffer for a
            // later getOldWorkItems() to reap. With buffering disabled (the default) this still counts
            // the drop rather than losing it without trace.
            bufferLateReturn_locked(frame, std::move(returned));
            return;
        }
        BatchState &b = it->second;
        const std::size_t slot = decodeSlot(id);
        if(slot >= b.items.size() || not b.items[slot] ||
           b.items[slot]->getDispatchState() != Gem::Courtier::dispatchState::IN_FLIGHT) {
            return; // out of range, a null (non-participating) slot, or duplicate / not in flight
        }
        // Feed the (shared) adaptive timeout: how long this item took from checkout to return.
        const auto now = clock::now();
        recordReturnTime_(now - b.checked_out_at[slot]);
        b.last_progress = now;

        // Reconcile the return INTO the live slot object in place, keeping its heap address (the slot
        // aliases the live population element). Rather than swapping the returned object in -- which would
        // free the original and RELOCATE the population element -- we keep the original and write only
        // what came back into it. The original already holds the submitted parameters and the evolved
        // library-owned scratch (omitted on the wire), and address stability is what lets a concurrent
        // per-item prefetch hold a snapshot of population addresses across the submission.
        b.items[slot]->applyProcessingOutcome(frame.outcome());
        Gem::Courtier::GWireProtocolT<processable_type>::applyReturn(
            *b.items[slot], frame.tag(), frame.libraryPayload(), returned.get()
        );
        b.items[slot]->setDispatchState(Gem::Courtier::dispatchState::DONE);
        ++b.done;
        if(b.done == b.target) {
            cv_done_.notify_all(); // each waiting dispatch_ re-checks its own batch
        }
    }

    /***************************************************************************/
    /** @brief Convenience overload for callers that already hold a processed work item rather than a
     *  wire frame (the local consumers and the test harnesses): wraps it in the RETURN frame its own
     *  outcome describes and checks that in.
     *  @param p The processed work item (ownership transferred); null is ignored */
    void checkin(item_ptr p) {
        if(not p) {
            return;
        }
        GCommandContainerT<processable_type> frame{Gem::Courtier::GFrameKind::RETURN};
        frame.setOutcome(p->processingOutcome());
        frame.setItem(std::move(p));
        checkin(std::move(frame));
    }

    /***************************************************************************/
    /** @brief Returns a slot's still-in-flight item to PENDING so another client picks it up
     *  immediately (the RAII put-back on a client disconnect). The batch+slot are located via the
     *  passed correlation id. A no-op if the batch moved on or the slot is no longer in flight.
     *  @param id The (batch_id, slot) correlation id of the item to put back */
    void requeue(Gem::Courtier::CORRELATION_ID_TYPE id) {
        std::scoped_lock const lk(mtx_);
        auto it = batches_.find(decodeBatch(id));
        if(it == batches_.end()) {
            return;
        }
        if(requeueSlot_locked(it->second, decodeSlot(id))) {
            cv_work_.notify_one();
        }
    }

    /***************************************************************************/
    /** @brief Whether the server is being torn down (sessions use this to stop accepting work).
     *  @return true once teardown has been requested */
    [[nodiscard]] bool stopped() const noexcept { return stop_.load(); }

    /** @brief Requests teardown -- subclasses call this from their stop path. Wakes every waiter. */
    void requestStop() noexcept {
        stop_.store(true);
        cv_work_.notify_all();
        cv_done_.notify_all();
    }

    /***************************************************************************/
    /** @brief Whether this consumer reclaims stuck in-flight items via the TIME lease. True for
     *  transports with no per-client liveness signal (ASIO, MPI): an item that has been IN_FLIGHT
     *  longer than the adaptive lease is presumed lost and requeued. A transport that detects client
     *  death directly (the websocket consumer, via CheckoutLease on its persistent session) overrides
     *  this to false, so a live-but-slow client is never wrongly reclaimed.
     *  @return true if stuck in-flight items are reclaimed via the time lease (false for liveness-driven transports) */
    virtual bool usesTimeLease() const { return true; }

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
    // NOLINTNEXTLINE(readability-function-size) -- one coherent batch dispatch lifecycle (register under lock, wait with adaptive stall/lease reclamation, then deregister + retain late-return originals) sharing the same locked BatchState across phases; splitting would fragment the locking discipline the comments document
    void dispatch_(std::span<item_ptr> items) override {
        const std::size_t n = items.size();
        if(n == 0) {
            return;
        }

        const auto start = clock::now();
        typename std::map<batch_key_t, BatchState>::iterator my_it;
        {
            std::scoped_lock const lk(mtx_);
            const batch_key_t key = (next_batch_id_++ & BATCH_MASK);
            BatchState b;
            b.items = items;
            b.cursor = 0;
            b.checked_out_at.assign(n, start); // indexed by slot (the span index)
            b.last_progress = start;
            // Select exactly the DO_PROCESS slots of the span for this batch: tag each with its
            // (batch_id, slot) correlation token (slot == span index) and mark it PENDING. Slots in any
            // other state -- already resolved, or null -- are left in dispatchState NONE and are ignored
            // by checkout()/checkin()/leaseSweep(), so the span may carry non-participating slots without
            // affecting routing or the write-back position.
            std::size_t n_sel = 0;
            for(auto&& [k, item] : items | std::views::enumerate) {
                if(item &&
                   item->getProcessingStatus() == Gem::Courtier::processingStatus::DO_PROCESS) {
                    item->setCorrelationId(encodeId(key, static_cast<std::size_t>(k)));
                    item->setDispatchState(Gem::Courtier::dispatchState::PENDING);
                    ++n_sel;
                }
            }
            b.target = n_sel;
            b.pending = n_sel;
            my_it = batches_.emplace(key, std::move(b)).first;
            total_pending_ += n_sel;
        }
        cv_work_.notify_all(); // wake any session blocked in checkoutWait()

        // Wait until every slot of THIS batch is DONE, or its progress stalls past the (adaptive)
        // give-up window. The happy path wakes on the final checkin() and exits at once. While
        // waiting, where the transport has no liveness signal, periodically reclaim this batch's items
        // that have been in flight longer than the adaptive lease.
        {
            std::unique_lock<std::mutex> lk(mtx_);
            BatchState &b = my_it->second; // stable across wait_for: std::map refs survive other ins/erase
            while(true) {
                if(b.done == b.target || stop_.load()) {
                    break;
                }
                const auto now = clock::now();
                // Never give up while NO result has EVER been received: under a batch-scheduling system
                // the first worker may enter the pool arbitrarily later (minutes, even hours), so we
                // wait indefinitely for the first return. The give-up window only bounds how long we
                // keep waiting once workers ARE returning (it reclaims items handed to workers that
                // have since gone slow/dead) -- it must not bound the wait for workers to appear.
                if(n_return_samples_ > 0 && now - b.last_progress > currentStallWindow()) {
                    break; // progress stalled with live workers -> give up; unresolved slots MISSING
                }
                if(this->usesTimeLease()) {
                    leaseSweep_locked(b, now);
                }
                cv_done_.wait_for(lk, sweep_tick_);
            }
            // Slots are already updated in place by checkin(); slots never reached DONE keep their
            // original DO_PROCESS item == MISSING for the policy loop. Drop the still-PENDING slots
            // from the global count and deregister the batch (its borrowed vector is about to leave
            // dispatch_'s scope), so any very late checkin() for it becomes a no-op.
            //
            // Before deregistering, retain a CLONE of each un-returned (MISSING) original, keyed by its
            // correlation id, so a slow-but-alive worker's later payload-only return still has something
            // to be applied to, and can be reaped via getOldWorkItems() instead of dropped.
            // Only when buffering is enabled; bounded by the same TTL + cap as the late-return buffer.
            if(late_store_.buffering()) {
                for(std::size_t k = 0; k < b.items.size(); ++k) {
                    // Retain only THIS batch's still-unreturned slots (PENDING/IN_FLIGHT == MISSING):
                    // a DONE slot already returned, and a NONE slot never participated in this batch.
                    const auto ds = b.items[k] ? b.items[k]->getDispatchState()
                                               : Gem::Courtier::dispatchState::NONE;
                    if(b.items[k] && (ds == Gem::Courtier::dispatchState::PENDING ||
                                      ds == Gem::Courtier::dispatchState::IN_FLIGHT)) {
                        late_store_.retain(b.items[k]->getCorrelationId(),
                                           this->clone_item_(b.items[k]));
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
    // The work item's correlation id (a uint64) carries the wire correlation token: the high 48 bits
    // hold the batch_id (which submitter's batch), the low 16 the slot index within it. The batch_id is
    // drawn from a single monotonic process-wide counter (next_batch_id_) on the shared consumer, so two
    // concurrent submitters never collide; at 48 bits it does not wrap in any realistic run (2^48 dispatch
    // rounds), which is what makes late-return routing safe -- a stale return can never alias a freshly
    // minted batch. The slot field bounds a single batch to 2^16 items (ample for any Geneva population).
    using batch_key_t = Gem::Courtier::CORRELATION_ID_TYPE;
    static constexpr Gem::Courtier::CORRELATION_ID_TYPE SLOT_BITS = 16;
    static constexpr Gem::Courtier::CORRELATION_ID_TYPE SLOT_MASK = (1ull << SLOT_BITS) - 1ull;
    static constexpr Gem::Courtier::CORRELATION_ID_TYPE BATCH_MASK = (1ull << 48) - 1ull;

    /** @brief Packs a batch_id and slot into the on-wire correlation id (batch in the high 48 bits, slot in the low 16).
     *  @param batch The batch id (masked to 48 bits)
     *  @param slot The slot index within the batch (masked to 16 bits)
     *  @return The combined (batch_id, slot) correlation id */
    static Gem::Courtier::CORRELATION_ID_TYPE encodeId(batch_key_t batch, std::size_t slot) {
        return ((batch & BATCH_MASK) << SLOT_BITS)
               | (static_cast<Gem::Courtier::CORRELATION_ID_TYPE>(slot) & SLOT_MASK);
    }
    /** @brief Extracts the batch id from a correlation id.
     *  @param id The (batch_id, slot) correlation id
     *  @return The batch id (high 48 bits) */
    static batch_key_t decodeBatch(Gem::Courtier::CORRELATION_ID_TYPE id) {
        return (id >> SLOT_BITS) & BATCH_MASK;
    }
    /** @brief Extracts the slot index from a correlation id.
     *  @param id The (batch_id, slot) correlation id
     *  @return The slot index (low 16 bits) */
    static std::size_t decodeSlot(Gem::Courtier::CORRELATION_ID_TYPE id) {
        return static_cast<std::size_t>(id & SLOT_MASK);
    }

    /***************************************************************************/
    /** @brief Parks a late arrival (a return for a batch that already finished/timed out) instead of
     *  dropping it silently. With buffering disabled (cap == 0, the default) the drop is merely COUNTED;
     *  when enabled, a parked item is later reaped by the OA via getOldWorkItems(). Caller holds mtx_.
     *  @param frame The late-arriving RETURN frame (its outcome routes it; its payload is applied)
     *  @param p The work item the frame carried, or null for a payload-only return */
    void bufferLateReturn_locked(const GCommandContainerT<processable_type> &frame, item_ptr p) {
        const Gem::Courtier::CORRELATION_ID_TYPE id = frame.outcome().correlation_id;
        if(not p) {
            // A PAYLOAD-ONLY return names a work item it does not carry. A LATE one arrives after its
            // batch was deregistered, so checkin()'s usual slot is gone -- but when late-return buffering
            // is enabled, dispatch_ retained a CLONE of each un-returned original (keyed by correlation
            // id) for exactly this case. Apply the return to that clone and park the clone; with no
            // retained clone there is nothing the algorithm could reap, so the return is dropped.
            auto orig = late_store_.takeRetained(id);
            if(not orig) {
                recordLateDrop_locked(1); // no retained original -> nothing to apply the return to
                return;
            }
            p = std::move(*orig);
            p->applyProcessingOutcome(frame.outcome());
            Gem::Courtier::GWireProtocolT<processable_type>::applyReturn(
                *p, frame.tag(), frame.libraryPayload(), nullptr
            );
        }
        else {
            // A late return that carries its own work item is complete except for the library-owned
            // scratch the wire strips; graft that back from the retained original before discarding it.
            if(auto orig = late_store_.takeRetained(id)) {
                p->graftOaScratchFrom(**orig);
            }
        }
        if(not late_store_.buffering()) {
            recordLateDrop_locked(1); // buffering off: count the drop, do not hold the item
            return;
        }
        // Park it; any TTL/cap eviction the park triggers is a genuine drop -> record it.
        recordLateDrop_locked(late_store_.park(std::move(p)));
    }

    /** @brief Records that @p n late returns were dropped (disabled-buffer arrival, or cap/TTL
     *  eviction) and warns ONCE so the loss is observable without log spam. Caller holds mtx_.
     *  @param n The number of late returns just dropped, added to the running total */
    void recordLateDrop_locked(std::uint64_t n) {
        if(n == 0) {
            return; // nothing dropped (e.g. an eviction sweep that freed nothing) -> no count, no warning
        }
        late_dropped_count_ += n;
        if(not late_drop_warned_) {
            late_drop_warned_ = true;
            glogger << "In GNetworkedConsumerT: a late work-item return (a result for a batch that had" << '\n'
                    << "already finished or timed out) was dropped. These are genuinely-distinct" << '\n'
                    << "evaluations that arrived too late to be used this round. Enable/enlarge the" << '\n'
                    << "late-return buffer (setLateReturnBuffer) to retain them for the optimization" << '\n'
                    << "algorithm to reap; the running drop total is available via lateReturnDroppedCount()." << '\n'
                    << GWARNING;
        }
    }

    /***************************************************************************/
    /** @brief Per-batch scheduling state. `items` is a BORROWED pointer to the caller's round vector
     *  (valid only while that call's dispatch_ blocks); everything else is this batch's bookkeeping. */
    struct BatchState {
        std::span<item_ptr> items; ///< Borrowed batch span (not owned); empty by default
        std::size_t target = 0;                 ///< Number of slots in this batch
        std::size_t done = 0;                   ///< Slots that have reached DONE
        std::size_t pending = 0;                ///< Slots currently PENDING (awaiting a client)
        std::size_t cursor = 0;                 ///< Next slot index to consider in checkout
        std::vector<clock::time_point> checked_out_at; ///< Per-slot checkout time (lease + stats)
        clock::time_point last_progress;      ///< Time of the most recent checkin (stall basis)
    };

    /***************************************************************************/
    /** @brief Serves the next PENDING slot, round-robin across active batches for fairness (so no
     *  single submitter starves the others). Caller holds mtx_.
     *  @return A clone of the next pending item to ship, or nullptr if no slot is pending */
    item_ptr checkout_locked() {
        if(total_pending_ == 0 || batches_.empty()) {
            return nullptr;
        }
        // Start just past the last-served batch and walk the (ordered) map once, wrapping around.
        auto it = batches_.upper_bound(last_served_batch_);
        for(std::size_t scanned = 0; scanned < batches_.size(); ++scanned) {
            if(it == batches_.end()) {
                it = batches_.begin();
            }
            BatchState &b = it->second;
            // Skip non-participating slots: null entries and anything not PENDING (see dispatch_'s
            // selection comment -- the span may carry null / already-resolved slots).
            while(b.cursor < b.items.size() &&
                  (not b.items[b.cursor] ||
                   b.items[b.cursor]->getDispatchState() != Gem::Courtier::dispatchState::PENDING)) {
                ++b.cursor;
            }
            if(b.cursor < b.items.size()) {
                const std::size_t k = b.cursor++;
                b.items[k]->setDispatchState(Gem::Courtier::dispatchState::IN_FLIGHT);
                b.checked_out_at[k] = clock::now();
                --b.pending;
                --total_pending_;
                last_served_batch_ = it->first;
                // Hand the session a clone to serialize and ship; the owning copy stays in the slot,
                // marked IN_FLIGHT. Its correlation id rides the clone, so checkin() finds the slot again.
                // Clone via the consumer's type-generic helper (polymorphic functor or copy-construct).
                return this->clone_item_(b.items[k]);
            }
            ++it;
        }
        return nullptr;
    }

    /***************************************************************************/
    /** @brief Flips a slot IN_FLIGHT -> PENDING (a put-back) within batch @p b and rewinds its cursor
     *  so checkout re-finds it. Caller holds mtx_.
     *  @param b The batch owning the slot
     *  @param slot The slot index to flip back to PENDING
     *  @return true if the slot was flipped; false if it was out of range or not currently IN_FLIGHT */
    bool requeueSlot_locked(BatchState &b, std::size_t slot) {
        if(slot >= b.items.size() || not b.items[slot] ||
           b.items[slot]->getDispatchState() != Gem::Courtier::dispatchState::IN_FLIGHT) {
            return false;
        }
        b.items[slot]->setDispatchState(Gem::Courtier::dispatchState::PENDING);
        ++b.pending;
        ++total_pending_;
        if(slot < b.cursor) {
            b.cursor = slot;
        }
        return true;
    }

    /***************************************************************************/
    /** @brief Reclaims every slot of batch @p b that has been IN_FLIGHT longer than the adaptive
     *  lease, returning it to PENDING for re-service. Caller holds mtx_.
     *  @param b The batch to sweep
     *  @param now The current time, against which each slot's checkout time is compared to the lease */
    void leaseSweep_locked(BatchState &b, clock::time_point now) {
        const auto lease = currentLease();
        bool any = false;
        for(std::size_t k = 0; k < b.items.size(); ++k) {
            if(b.items[k] &&
               b.items[k]->getDispatchState() == Gem::Courtier::dispatchState::IN_FLIGHT &&
               (now - b.checked_out_at[k]) > lease) {
                any = requeueSlot_locked(b, k) || any;
            }
        }
        if(any) {
            cv_work_.notify_all();
        }
    }

    /***************************************************************************/
    /** @brief Folds one observed checkout->return duration into the running mean (an EMA).
     *  @param d The observed checkout-to-return duration of one item */
    void recordReturnTime_(clock::duration d) {
        const double ms = std::chrono::duration<double, std::milli>(d).count();
        if(n_return_samples_ == 0) {
            mean_return_ms_ = ms;
        }
        else {
            mean_return_ms_ = (ema_alpha_ * ms) + ((1.0 - ema_alpha_) * mean_return_ms_);
        }
        ++n_return_samples_;
    }

protected:
    /***************************************************************************/
    /** @brief The current reclaim lease: a multiple of the running mean return time (clamped), or a
     *  bootstrap value until the first return has been observed. Protected so a derived transport (and the
     *  timeout tests) can inspect the treatment-driven value.
     *  @return The current reclaim lease duration */
    std::chrono::milliseconds currentLease() const {
        switch(treatment_) {
            case timeoutTreatment::wait_indefinitely:
                // Never reclaim on time: no time lease can ever elapse (liveness-driven reclaim, e.g. a
                // websocket disconnect, still applies independently).
                return kNeverWindow_;
            case timeoutTreatment::fixed:
                return fixed_lease_;
            case timeoutTreatment::adaptive:
            default:
                if(n_return_samples_ == 0) {
                    return lease_bootstrap_;
                }
                const auto v =
                    std::chrono::milliseconds(static_cast<long long>(lease_factor_ * mean_return_ms_));
                return std::clamp(v, min_lease_, max_lease_);
        }
    }

    /***************************************************************************/
    /** @brief The current give-up window: how long a batch's dispatch_ tolerates NO progress before
     *  declaring the rest of that batch MISSING. A generous multiple of the running mean return time,
     *  clamped. Only ever consulted once n_return_samples_ > 0 (dispatch_ waits indefinitely before
     *  the first return ever arrives), so there is no pre-sample fallback here.
     *  @return The current give-up (stall) window duration */
    std::chrono::milliseconds currentStallWindow() const {
        switch(treatment_) {
            case timeoutTreatment::wait_indefinitely:
                // Never give up on time: the stall window can never elapse, so no item is declared MISSING
                // for lateness (a genuinely dead client is still detected by liveness where available).
                return kNeverWindow_;
            case timeoutTreatment::fixed:
                return fixed_stall_window_;
            case timeoutTreatment::adaptive:
            default:
                const auto v =
                    std::chrono::milliseconds(static_cast<long long>(stall_factor_ * mean_return_ms_));
                return std::clamp(v, min_stall_, max_stall_);
        }
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_done_; ///< Signalled when a batch's last slot reaches DONE
    std::condition_variable cv_work_; ///< Signalled when a slot becomes available

    std::map<batch_key_t, BatchState> batches_; ///< All currently-active batches, keyed by batch_id
    batch_key_t next_batch_id_ = 0;             ///< Monotonic batch_id source (masked to 48 bits)
    batch_key_t last_served_batch_ = 0;         ///< Round-robin cursor across batches (for fairness)
    std::size_t total_pending_ = 0;             ///< Slots PENDING across ALL batches (cv predicate)

    // --- late-return facility: a result that arrives after its batch finished/timed out is PARKED for a
    //     later getOldWorkItems() to reap instead of dropped, and a clone of each un-returned original is
    //     RETAINED (keyed by correlation id) so a late payload-only return can still be applied. Both live
    //     in one shared aging store (FIFO + keyed faces, one epoch + cap + TTL); the epoch advances once
    //     per retired batch. Disabled by default (cap == 0); when enabled, the OA-side reaper
    //     (GOptimizationAlgorithmBase, via enableLateReturns() / getOldWorkItems()) drains the FIFO. The
    //     apply-or-drop policy and the drop accounting below are the consumer's; the store is invoked
    //     under mtx_, so its operations stay consistent with the batch bookkeeping. ---
    Gem::Common::Concurrency::GAgingStoreT<Gem::Courtier::CORRELATION_ID_TYPE, item_ptr> late_store_;
    std::uint64_t late_dropped_count_ = 0;      ///< Total late items dropped (disabled/cap/TTL) -- observable
    bool late_drop_warned_ = false;             ///< One-shot warning guard for the first dropped late item

    double mean_return_ms_ = 0.0;       ///< Running (EMA) mean checkout->return time (consumer-wide)
    std::size_t n_return_samples_ = 0;  ///< Returns observed so far (across all batches)

    std::atomic<bool> stop_{false};

    // --- timeout treatment (see timeoutTreatment): adaptive (scale-free, the default) declares MISSING /
    //     reclaims relative to the running mean; fixed uses the two fixed windows below; wait_indefinitely
    //     never declares MISSING on time. Selected from the config file via applyTimeoutConfig(). ---
    timeoutTreatment treatment_ = timeoutTreatment::adaptive;
    std::chrono::milliseconds fixed_stall_window_{300'000}; ///< give-up window when treatment_ == fixed
    std::chrono::milliseconds fixed_lease_{300'000};        ///< reclaim lease when treatment_ == fixed
    // The "never" window/lease for wait_indefinitely. A finite but astronomically large value (100 years)
    // rather than milliseconds::max(): it is compared against a steady_clock (nanosecond) duration, and
    // max() would overflow int64 when the comparison promotes it to nanoseconds, whereas 100 years in ns
    // (~3.15e18) stays well within range and can never elapse in a real run.
    static constexpr std::chrono::milliseconds kNeverWindow_{100LL * 365 * 24 * 3600 * 1000};

    // --- adaptive-timeout configuration (sane defaults; tunable via the setters) ---
    double ema_alpha_ = 0.25;   ///< EMA weight for new return-time samples
    double lease_factor_ = 4.0; ///< Reclaim lease = lease_factor_ * mean return time (clamped)
    double stall_factor_ = 8.0; ///< Give-up window = stall_factor_ * mean return time (clamped)
    std::chrono::milliseconds lease_bootstrap_{10'000};     ///< Reclaim lease before any return is seen
    std::chrono::milliseconds min_lease_{200};              ///< Lower clamp on the reclaim lease
    std::chrono::milliseconds max_lease_{300'000};          ///< Upper clamp on the reclaim lease
    std::chrono::milliseconds min_stall_{2'000};            ///< Lower clamp on the give-up window
    std::chrono::milliseconds max_stall_{600'000};          ///< Upper clamp on the give-up window
    std::chrono::milliseconds sweep_tick_{50};              ///< dispatch_ re-evaluation poll interval
};

/******************************************************************************/

} /* namespace Gem::Courtier */
