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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <random>
#include <string>
#include <string_view>
#include <utility>

// Boost headers
#include <boost/asio.hpp>

// Geneva headers
#include "common/GLogger.hpp"
#include "common/concurrency/GThreadBudget.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GCourtierHelperFunctions.hpp" // describeException()
#include "courtier/GWireCodec.hpp"
#include "courtier/GWireSerializationContext.hpp"

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * The prefetch/compute pipeline of the networked clients (GPeerClientT). A client keeps up to
 * prefetch_depth_ work items in flight (requested-but-unanswered pulls + items computing on the
 * compute pool), overlap network transfer with evaluation, back off on NO_WORK and poll halt() on a
 * timer. That whole pipeline -- the in-flight bookkeeping, the compute-pool dispatch with its
 * work-guard discipline, the shared result prologue, the jittered NO_WORK refill timer and the
 * halt-poll timer -- lives here ONCE, as a CRTP base between GBaseClientT and the concrete client.
 *
 * Threads. The io thread moves the connection's bytes, answers its pings and keeps the books
 * (pending_pulls_, computing_, the timers); every decision about an item is taken there. It never
 * encodes or decodes a work item: a message is decoded on a strand of the compute pool
 * (dispatch_message_()), and the RETURN of an evaluated item is built and encoded on the compute
 * thread that evaluated it (dispatch_compute_()). However long that takes, the io thread stays free
 * for the connection.
 *
 * A concrete client contributes only its transport specifics through hooks it befriends the base
 * for:
 *
 *  - refill_()                              -- issue pulls until the pipeline is at depth
 *  - haltShutdown_()                        -- tear the transport down when halt() is reached
 *  - when_decoded_(kind)                    -- a message other than WORK has arrived
 *  - when_unreadable_(error)                -- a message could not be decoded
 *  - resultKindHint_() and sendResult_(bytes, error, frame, item)
 *                                           -- the concrete client decides whether a finished
 *                                              result is a RETURN (it asks for the next item) or a
 *                                              RESULT (it asks for nothing). The compute thread
 *                                              encodes it as the hint says (any thread);
 *                                              sendResult_() gets the bytes and the frame on the io
 *                                              thread, decides with the books and does them;
 *                                              item is the evaluated work item if the frame does
 *                                              not carry it (a library payload replaces it)
 *  - when_work_()                           -- optional: a WORK item has been counted (io thread)
 *  - when_busy_(busy)                       -- optional: computing_ has become, or stopped being,
 *                                              0 (io thread)
 *  - when_answered_(kind)                   -- optional: a message has been decoded (io thread),
 *                                              before the books are done; NONE if it could not be
 *                                              read. A client that has it keeps pending_pulls_
 *                                              itself; without it the base counts one request
 *                                              answered for every message that could be read
 *  - after_decoded_()                       -- optional: the books of a decoded message are done;
 *                                              n_decoding_ counts those still being decoded
 *  - when_polled_()                         -- optional: the halt poll found no halt condition
 *
 * Compute width. The pool evaluates at most compute_width_ items at a time (by default as many as
 * the prefetch depth). A WORK item that arrives while that many are evaluated is held on the io
 * thread (held_) and handed to the pool when an evaluation ends. Held items count towards the
 * prefetch depth; a concrete client may drop them (drop_held_()) when their results can no longer
 * be delivered, and with them every item still being decoded. With a width equal to the depth no
 * item is ever held. The messages are decoded on a strand of the same pool. With a width below the
 * depth the pool has one thread more than the width (poolThreads_()), so that an item that arrives
 * while compute_width_ items are evaluated is decoded at once and is ready when an evaluation ends;
 * the books on the io thread still let no more than compute_width_ items be evaluated at a time.
 *
 * @tparam derived_type The concrete client type (CRTP; must also inherit enable_shared_from_this)
 * @tparam processable_type The work-item type exchanged with the server (must be processable)
 */
template <typename derived_type, typename processable_type>
class GPrefetchingClientT : public Gem::Courtier::GBaseClientT<processable_type> {
protected:
    //-------------------------------------------------------------------------
    /** @brief Initializes the pipeline state.
     *
     *  @param client_name The concrete client's name, used in diagnostics
     *  @param prefetch_depth Maximum number of work items kept in flight at once (0 is treated as 1)
     *  @param compute_width How many items are evaluated at the same time; 0 (the default) means
     *         as many as the prefetch depth, more than the depth is the depth. A width below the
     *         depth lets the client hold items ahead of those it evaluates. */
    GPrefetchingClientT(
        std::string_view client_name,
        std::size_t prefetch_depth,
        std::size_t compute_width = 0
    )
      : client_name_(client_name)
      , prefetch_depth_(prefetch_depth == 0 ? 1 : prefetch_depth)
      , compute_width_(
            compute_width == 0 ? prefetch_depth_ : std::min(compute_width, prefetch_depth_)
        )
      , compute_pool_(this->poolThreads_()) { /* nothing */
    }

    //-------------------------------------------------------------------------
    /** @brief The destructor. Logs a shutdown summary (items processed, NO_WORK count, prefetch depth). */
    ~GPrefetchingClientT() override {
        glogger << '\n'
                << client_name_ << " is shutting down. Processed " << this->getNProcessed()
                << " items in total" << '\n'
                << "\"no data\" was received " << n_nodata_.load() << " times" << '\n'
                << "prefetch depth was " << prefetch_depth_ << '\n'
                << '\n'
                << GLOGGING;
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Decodes a message from the server off the io thread and hands the result back to it.
     * The message is decoded on a strand of the compute pool, so messages are decoded one at a time
     * and in the order they arrived (an item whose blob travels by id is decoded after the item
     * that carried it). The decoded frame goes to the io thread at once (decoded_()), WORK
     * included: the io thread does the books and starts the evaluation.
     *
     * @param message One complete message from the server
     */
    void dispatch_message_(std::string message) {
        ++n_decoding_;
        auto self = derived().shared_from_this();
        auto guard = boost::asio::make_work_guard(io_context_);
        boost::asio::post(
            decode_strand_,
            [self, message = std::move(message), guard = std::move(guard)]() mutable {
                GCommandContainerT<processable_type> container{GFrameKind::NONE};
                std::string error;
                try {
                    Gem::Courtier::wireDecode(
                        message,
                        container,
                        &self->wire_ctx_,
                        self->wire_ctx_.mode
                    );
                }
                catch(...) {
                    error = Gem::Courtier::describeException(std::current_exception());
                }
                // The pool's handler gives up its hold on the client here: if it kept it until it is
                // destroyed, after the io thread may have finished with the client, the client could be
                // destroyed on a thread of its own pool, which cannot join itself.
                auto &io = self->io_context_;
                boost::asio::post(
                    io,
                    [self = std::move(self),
                     container = std::move(container),
                     error = std::move(error),
                     guard = std::move(guard)]() mutable {
                        self->decoded_(std::move(container), error);
                    }
                );
            }
        );
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Runs on the io thread for every decoded message. WORK and NO_WORK both answer a pull
     * sent earlier, so one pull is resolved; a WORK item is then counted as computing and handed
     * to the compute pool (or held, see "Compute width"), and the concrete client is told
     * (when_work_(), if it has one); anything else goes to the concrete client (when_decoded_()).
     * A message that could not be decoded goes to when_unreadable_(), and so does a WORK frame
     * that carries no work item.
     *
     * @param container The decoded frame
     * @param error Why the message could not be decoded; empty if it could
     */
    void decoded_(GCommandContainerT<processable_type> container, const std::string &error) {
        --n_decoding_;
        this->book_decoded_(std::move(container), error);
        if constexpr(requires(derived_type &d) { d.after_decoded_(); }) {
            derived().after_decoded_();
        }
    }

    //-------------------------------------------------------------------------
    /** @brief The books of decoded_() @param container The decoded frame
     *  @param error Why the message could not be decoded; empty if it could */
    void book_decoded_(GCommandContainerT<processable_type> container, const std::string &error) {
        // A WORK frame that decodes but carries no work item cannot be read either.
        const bool readable =
            error.empty() && (container.kind() != GFrameKind::WORK || container.item() != nullptr);
        if constexpr(requires(derived_type &d) { d.when_answered_(GFrameKind::NONE); }) {
            derived().when_answered_(readable ? container.kind() : GFrameKind::NONE);
        }
        else if(readable && pending_pulls_ > 0) {
            --pending_pulls_;
        }
        if(not error.empty()) {
            derived().when_unreadable_(error);
            return;
        }
        // A WORK frame that decodes but carries no work item cannot be read either: its kind and
        // its payload disagree. It is not counted as WORK.
        if(container.kind() == GFrameKind::WORK && container.item() == nullptr) {
            derived().when_unreadable_("a WORK frame without a work item");
            return;
        }
        if(container.kind() == GFrameKind::WORK) {
            // One pull became one item: the in-flight total is unchanged. It is evaluated at once
            // if the compute width allows, held otherwise -- or dropped, if its result could not
            // be delivered any more (see drop_held_()), or if a halt condition holds: a client
            // that halts starts no evaluation (the server puts the item back when the connection
            // ends).
            if(work_unwanted_ || this->halt()) {
                return;
            }
            if(computing_ < compute_width_) {
                ++computing_;
                this->dispatch_compute_(std::move(container));
                this->busy_changed_();
            }
            else {
                held_.push_back(std::move(container));
            }
            if constexpr(requires(derived_type &d) { d.when_work_(); }) {
                derived().when_work_();
            }
            return;
        }
        derived().when_decoded_(container.kind());
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Hands the given work item to the compute pool for evaluation, keeping the io thread free
     * for transport work (pings / connection exchanges). The compute thread evaluates the item,
     * rewrites the frame into the RETURN the using library wants (makeReturnFrame -- this is where
     * the library, not courtier, decides whether the whole item or a payload of its own travels
     * back) and encodes it; only the bytes travel back to the io thread (finish_compute_()). A work
     * guard pins io_context::run() open from here until finish_compute_() has run, so the posted
     * result is always delivered -- no premature drain and no leftover-handler leak on shutdown.
     * Each evaluation owns its OWN container (moved into the worker lambda), so several items
     * compute concurrently without sharing state.
     *
     * @param container The frame holding the WORK item to evaluate (moved into the worker)
     */
    void dispatch_compute_(GCommandContainerT<processable_type> container) {
        auto self = derived().shared_from_this();
        auto guard = boost::asio::make_work_guard(io_context_);
        boost::asio::post(
            compute_pool_,
            [self, container = std::move(container), guard = std::move(guard)]() mutable {
                // No exception of any type leaves this thread (a thread_pool would end the
                // process): what identifies the item is kept, so that whatever throws below the
                // item can still be returned as failed (returnFailed_()).
                const auto *work = container.item();
                const Gem::Courtier::CORRELATION_ID_TYPE id =
                    work != nullptr ? work->getCorrelationId() : 0;
                const Gem::Courtier::SUBMISSION_UUID_TYPE uuid =
                    work != nullptr ? work->getSubmissionUuid()
                                    : Gem::Courtier::SUBMISSION_UUID_TYPE{};
                std::unique_ptr<processable_type> kept; // the item, if the frame does not ship it
                try {
                    // A failing evaluation leaves the item flagged; it is returned like any result.
                    // The processor is read on the compute-pool threads and never written while the
                    // client runs.
                    Gem::Courtier::processWorkFrame(
                        container,
                        self->client_name_,
                        self->itemProcessor()
                    );
                    kept = Gem::Courtier::makeReturnFrame(container);
                }
                catch(...) {
                    self->returnFailed_(container, id, uuid, std::current_exception());
                }
                container.setKind(self->resultKindHint_());
                // The wire context is only read here: a client never interns a blob
                // (may_intern_blobs is false), so its RETURN carries every blob inline.
                std::string error;
                std::string bytes = self->encodeResult_(container, error);
                // Hop back onto the io thread for the books and the transmission -- connection
                // state must only be touched there.
                // The pool's handler gives up its hold on the client here: if it kept it until it is
                // destroyed, after the io thread may have finished with the client, the client could be
                // destroyed on a thread of its own pool, which cannot join itself.
                auto &io = self->io_context_;
                boost::asio::post(
                    io,
                    [self = std::move(self),
                     bytes = std::move(bytes),
                     error = std::move(error),
                     frame = std::move(container),
                     kept = std::move(kept),
                     guard = std::move(guard)]() mutable {
                        self->finish_compute_(
                            std::move(bytes),
                            error,
                            std::move(frame),
                            std::move(kept)
                        );
                    }
                );
            }
        );
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Turns @p frame into the RETURN of an item that failed on this client in a way that
     * left no result to return (the processing or the library's return seam threw): the outcome
     * alone, ERROR_FLAGGED with the reason, as encodeResult_() returns an item
     * that cannot be encoded. On a compute thread.
     *
     * @param frame The frame, in whatever state the failure left it; becomes the RETURN
     * @param id The item's correlation id, taken before the evaluation
     * @param uuid The item's lineage id, taken before the evaluation
     * @param failure What was thrown
     */
    void returnFailed_(
        GCommandContainerT<processable_type> &frame,
        Gem::Courtier::CORRELATION_ID_TYPE id,
        const Gem::Courtier::SUBMISSION_UUID_TYPE &uuid,
        const std::exception_ptr &failure
    ) {
        const std::string reason = Gem::Courtier::describeException(failure);
        Gem::Courtier::GProcessingOutcome outcome;
        outcome.status = Gem::Courtier::processingStatus::ERROR_FLAGGED;
        outcome.error_descriptions = "The work item could not be processed or returned: " + reason;
        outcome.correlation_id = id;
        outcome.submission_uuid = uuid;
        frame.reset(GFrameKind::RETURN);
        frame.setOutcome(std::move(outcome));
        glogger << "In " << client_name_ << ": a work item could not be processed or returned:"
                << '\n'
                << reason << '\n'
                << "It is returned to the server as failed; the client keeps running." << '\n'
                << GWARNING;
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Encodes a finished result (a RETURN or a RESULT frame). A result whose item cannot be
     * encoded (its serialization throws, whatever it throws) is returned as failed, the way an
     * evaluation that threw is: the frame keeps its kind, its tag and its outcome, which is marked
     * ERROR_FLAGGED with the reason, and loses the item (or library payload); the server ends that
     * slot as failed when it applies the outcome. So an item that cannot be sent back neither ends
     * the client nor travels from client to client. On a compute thread.
     *
     * @param frame The result's frame; stripped of its item if that cannot be encoded
     * @param error Set if not even the stripped frame can be encoded (then nothing is returned)
     * @return The encoded frame
     */
    std::string encodeResult_(GCommandContainerT<processable_type> &frame, std::string &error) {
        std::string reason;
        try {
            return Gem::Courtier::wireEncode(frame, &wire_ctx_, wire_ctx_.mode);
        }
        catch(...) {
            reason = Gem::Courtier::describeException(std::current_exception());
        }
        auto outcome = frame.outcome();
        outcome.status = Gem::Courtier::processingStatus::ERROR_FLAGGED;
        outcome.error_descriptions += "The result could not be encoded: " + reason;
        const GFrameKind kind = frame.kind();
        const std::uint32_t tag = frame.tag();
        frame.reset(kind);
        frame.setTag(tag);
        frame.setOutcome(std::move(outcome));
        glogger << "In " << client_name_ << ": a result could not be encoded:" << '\n'
                << reason << '\n'
                << "It is returned to the server as failed; the client keeps running." << '\n'
                << GWARNING;
        try {
            return Gem::Courtier::wireEncode(frame, &wire_ctx_, wire_ctx_.mode);
        }
        catch(...) {
            error = Gem::Courtier::describeException(std::current_exception());
        }
        return {};
    }

    //-------------------------------------------------------------------------
    /** @brief Runs on the io thread once an evaluation has completed and its result is encoded:
     *  accounts for the finished item, hands a held item to the pool (unless a halt condition
     *  holds: then the held items are dropped), and passes the result on to the concrete client
     *  (sendResult_()), which decides its kind and does the books of the request. The work guard
     *  captured by the posting lambda is released when this returns.
     *
     *  @param bytes The encoded result
     *  @param error Why the result could not be encoded; empty if it could
     *  @param frame The result's frame (its kind says how it was encoded), in case it has to be
     *         encoded once more
     *  @param item The evaluated item if the frame does not carry it; empty otherwise */
    void finish_compute_(
        std::string bytes,
        const std::string &error,
        GCommandContainerT<processable_type> frame,
        std::unique_ptr<processable_type> item
    ) {
        this->incrementProcessingCounter();
        if(computing_ > 0) {
            --computing_;
        }
        // A client that halts starts no evaluation: what it holds is dropped instead.
        if(this->halt()) {
            this->drop_held_();
        }
        if(not held_.empty() && computing_ < compute_width_) {
            ++computing_;
            auto next = std::move(held_.front());
            held_.pop_front();
            this->dispatch_compute_(std::move(next));
        }
        this->busy_changed_();
        derived().sendResult_(std::move(bytes), error, std::move(frame), std::move(item));
    }

    //-------------------------------------------------------------------------
    /** @brief Drops the items that are held and not evaluated yet, and every WORK item decoded
     *  from now on (their results could not be delivered any more): also one that arrived before
     *  and is still being decoded. On the io thread.
     *  @return How many held items were dropped */
    std::size_t drop_held_() {
        work_unwanted_ = true;
        const std::size_t n = held_.size();
        held_.clear();
        return n;
    }

    //-------------------------------------------------------------------------
    /** @brief The work items in flight, as the io thread counts them: requests without an answer,
     *  items held and items being evaluated.
     *  @return Their number */
    [[nodiscard]] std::size_t in_flight_() const {
        return pending_pulls_ + held_.size() + computing_;
    }

    //-------------------------------------------------------------------------
    /** @brief After a NO_WORK reply, waits a short randomized backoff and then tops the pipeline back
     *  up. A single timer suffices: refill_() covers the whole deficit at once. */
    void schedule_refill_() {
        if(pipeline_stopped_) {
            return;
        }
        std::uniform_int_distribution<std::uint32_t> dist(
            Gem::Courtier::GCLIENTNOWORKPAUSELOWERBOUNDARYMSEC,
            Gem::Courtier::GCLIENTNOWORKPAUSEUPPERBOUNDARYMSEC
        );
        nodata_timer_.expires_after(std::chrono::milliseconds(dist(rng_engine_)));
        auto self = derived().shared_from_this();
        nodata_timer_.async_wait([self](boost::system::error_code ec) {
            if(ec || self->pipeline_stopped_) { // cancelled, or stopped while this handler waited
                return;
            }
            if(not self->halt()) {
                self->refill_();
            }
        });
    }

    //-------------------------------------------------------------------------
    /** @brief Arms a periodic timer that polls the base-class halt() condition (max runtime / stop
     *  request / error flag). A prefetching client can sit idle with all items computing and no
     *  exchange active (or with only a read outstanding), so a timer is needed to notice a stop
     *  promptly; on halt the transport-specific haltShutdown_() hook tears the connection down. */
    void start_halt_timer() {
        if(pipeline_stopped_) {
            return;
        }
        halt_timer_.expires_after(Gem::Courtier::GCLIENTHALTPOLLINTERVAL);
        auto self = derived().shared_from_this();
        halt_timer_.async_wait([self](boost::system::error_code ec) { self->on_halt_timer(ec); });
    }

    //-------------------------------------------------------------------------
    /** @brief Timer callback: tears the transport down once a halt condition is reached, otherwise
     *  re-arms the poll.
     *
     *  @param ec The timer error code (non-zero means the timer was cancelled during teardown) */
    void on_halt_timer(boost::system::error_code ec) {
        if(ec || pipeline_stopped_) { // cancelled, or stopped while this handler waited to run
            return;
        }
        if(this->halt()) {
            derived().haltShutdown_();
            return;
        }
        if constexpr(requires(derived_type &d) { d.when_polled_(); }) {
            derived().when_polled_();
        }
        start_halt_timer(); // keep polling (unless when_polled_() has ended the client)
    }

    //-------------------------------------------------------------------------
    /** @brief Stops the halt-poll and the NO_WORK timers for good. A concrete client calls this
     *  when it tears its transport down, so that io_context::run() can return. Cancelling them is
     *  not enough for that: a timer that has already expired has its handler queued, the cancel no
     *  longer reaches it, and the handler would arm the timer again -- the client would then never
     *  terminate. The flag set here makes such a handler return without re-arming. */
    void stopPipelineTimers_() {
        pipeline_stopped_ = true;
        halt_timer_.cancel();
        nodata_timer_.cancel();
    }

    //-------------------------------------------------------------------------
    // Data (all touched on the io thread only, except where noted)

    boost::asio::io_context
        io_context_; ///< The io-service object handling the asynchronous processing

    std::random_device nondet_rng_; ///< Source of non-deterministic random numbers
    std::mt19937 rng_engine_{
        nondet_rng_()
    }; ///< The actual random number engine, seeded by nondet_rng_

    std::string client_name_; ///< The concrete client's name (diagnostics)

    /// Maximum number of work items the client keeps in flight at once (requested-but-not-yet-answered
    /// pulls + currently computing). 1 == serial (one item at a time, the classic behaviour); a larger
    /// depth overlaps network transfer with computation.
    std::size_t prefetch_depth_ = 1;

    /// In-flight bookkeeping, touched on the io thread only (no locking needed): pulls (PULL/RETURN)
    /// sent but not yet answered, and items currently being evaluated on the compute pool. The client
    /// keeps pending_pulls_ + computing_ == prefetch_depth_ whenever work is available.
    std::size_t pending_pulls_ = 0;
    std::size_t computing_ = 0;
    /// Messages handed to the decode strand whose books are not done yet (io thread)
    std::size_t n_decoding_ = 0;

    /// How many items are evaluated at the same time (the width of the compute pool); at most the
    /// prefetch depth
    std::size_t compute_width_ = 1;
    /// WORK items that arrived while compute_width_ items were evaluated, oldest first (io thread)
    std::deque<GCommandContainerT<processable_type>> held_;
    /// Set by drop_held_(): a WORK item decoded from now on is dropped (io thread)
    bool work_unwanted_ = false;

    /// How often a NO_WORK reply was received (reported at shutdown). Written on the io thread; an
    /// atomic, so that it may be read from any thread (noWorkCount()).
    std::atomic<std::uint64_t> n_nodata_{0};

    bool pipeline_stopped_ = false; ///< Set by stopPipelineTimers_(): no timer is armed any more

    /// Per-client cache of received blobs (keyed by content id), and the wire scope installed around
    /// every (de)serialisation so an id-referenced blob resolves locally (blob send-once). The
    /// concrete client configures the scope (and any cache-miss fetch) in its constructor.
    Gem::Courtier::GWireBlobRegistry wire_registry_;
    Gem::Courtier::GWireSerializationContext wire_ctx_;

    boost::asio::steady_timer halt_timer_{
        io_context_
    }; ///< Periodically polls halt() so a stop is noticed even while all items are computing
    boost::asio::steady_timer nodata_timer_{
        io_context_
    }; ///< Backoff timer that retries a PULL top-up after a NO_WORK reply (async, never blocks)

    /// The compute pool's reservation in the process-wide thread budget: Fixed, because the pool
    /// must hold its poolThreads_() workers to keep compute_width_ items computing. Declared
    /// before the pool so it is released only after the pool's threads are joined.
    Gem::Common::Concurrency::GThreadBudget::Reservation compute_budget_{
        Gem::Common::Concurrency::threadBudget().reserve(
            "client:compute",
            static_cast<unsigned int>(this->poolThreads_()),
            Gem::Common::Concurrency::ThreadElasticity::Fixed
        )
    };

    /// A thread pool that runs the (possibly long, unbounded) work-item evaluations OFF the io thread,
    /// so the io thread stays free for transport work while items are computed, and that decodes the
    /// messages (decode_strand_). poolThreads_() threads. Declared after io_context_ and the other
    /// pipeline state, so that it is destroyed (and its threads joined) before them; only the
    /// decode strand follows it, whose implementation the pool's handlers share.
    boost::asio::thread_pool compute_pool_;

    /// Decodes the messages of the server one at a time, in arrival order, on the compute pool
    boost::asio::strand<boost::asio::thread_pool::executor_type> decode_strand_{
        boost::asio::make_strand(compute_pool_)
    };

private:
    //-------------------------------------------------------------------------
    /** @brief How many threads the compute pool has: the compute width, and one more if the width
     *  is below the prefetch depth -- the thread that decodes an item held ahead while compute_width_
     *  items are evaluated. With a width equal to the depth nothing is held ahead, and an item is
     *  decoded only when a thread is free anyway.
     *  @return The number of threads */
    [[nodiscard]] std::size_t poolThreads_() const noexcept {
        return compute_width_ < prefetch_depth_ ? compute_width_ + 1 : compute_width_;
    }

    //-------------------------------------------------------------------------
    /** @brief Tells the concrete client (when_busy_(), if it has one) when computing_ has become,
     *  or stopped being, 0. On the io thread. */
    void busy_changed_() {
        const bool busy = computing_ > 0;
        if(busy == busy_told_) {
            return;
        }
        busy_told_ = busy;
        if constexpr(requires(derived_type &d) { d.when_busy_(true); }) {
            derived().when_busy_(busy);
        }
    }

    bool busy_told_ = false; ///< What busy_changed_() told the concrete client last (io thread)

    //-------------------------------------------------------------------------
    /** @brief The CRTP downcast to the concrete client.
     *  @return A reference to *this as the concrete client type */
    derived_type &derived() {
        return static_cast<derived_type &>(*this);
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
