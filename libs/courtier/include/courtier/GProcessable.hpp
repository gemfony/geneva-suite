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
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

// Boost headers

// Geneva headers
#include "common/GArchiveNamed.hpp"          // archive_named (named-member emitter)
#include "common/GErrorStreamer.hpp"         // g_error_streamer
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "courtier/GCourtierEnums.hpp"           // processingStatus, dispatchState, the id typedefs
#include "courtier/GCourtierHelperFunctions.hpp" // psToStr
#include "courtier/GProcessingOutcome.hpp"       // the transportable lifecycle record

namespace Gem::Courtier {

/******************************************************************************/
namespace detail {

/** @brief The random salt of this process: the first half of every lineage id minted in it.
 *  One instance per process (defined in the courtier library, not in the header, so that a
 *  loaded module and its host agree on it). */
[[nodiscard]] std::uint64_t lineageSalt() noexcept;

/**
 * @brief Mints a process-unique 128-bit work-item lineage id: a per-process random salt combined with a
 * monotonic atomic counter. The salt makes ids minted in different runs (or after a checkpoint resume,
 * where the counter restarts at 0) non-colliding with serialized ids from an earlier run; the counter
 * makes them unique within the run. Minting is cheap (no per-call RNG) so it is affordable at every
 * work-item construction.
 * @return A fresh, process-unique 128-bit lineage id
 */
inline SUBMISSION_UUID_TYPE mint_submission_uuid() {
    static std::atomic<std::uint64_t> counter{0};
    return SUBMISSION_UUID_TYPE{lineageSalt(), counter.fetch_add(1, std::memory_order_relaxed)};
}

/**
 * @brief A self-minting lineage id whose value-semantics encode the D11 identity contract in ONE place, so
 * the enclosing GProcessable can keep its special members defaulted:
 *  - default construction / move  -> a fresh (or transferred) id, as usual;
 *  - COPY construction            -> a FRESH id (a copy of a work item is a NEW item, e.g. a refill);
 *  - COPY assignment              -> KEEPS the target's own id (load(other) must not steal the source's lineage);
 *  - (de)serialization            -> the raw value is written/read, so a wire or checkpoint round-trip PRESERVES
 *                                    the id (a deserialized object is default-constructed -> fresh -> then the
 *                                    archived value overwrites it).
 * The asymmetry is intentional and is pinned by the [proc][id] unit tests.
 */
struct LineageId {
    SUBMISSION_UUID_TYPE value = mint_submission_uuid();

    LineageId() = default;
    LineageId(const LineageId & /* cp */)
      : value(mint_submission_uuid()) { /* a copy is a new item */
    }
    LineageId(LineageId &&) noexcept = default;
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp) -- deliberate: assignment keeps our own lineage (see block comment above)
    LineageId &operator=(const LineageId & /* cp */) {
        return *this; /* keep our own lineage on load */
    }
    LineageId &operator=(LineageId &&) noexcept = default;
    ~LineageId() = default;
};

/**
 * @brief The mark that a work item's processing was begun (GProcessable::beginProcessing()) and not
 * yet completed. It belongs to the object it was set on, and its value-semantics say so in ONE
 * place, so the enclosing GProcessable can keep its special members defaulted:
 *  - COPY construction / assignment -> the target is NOT begun (a clone is a new item, a load
 *                                      replaces the content; neither may be completed without a
 *                                      beginning of its own);
 *  - move                           -> transferred, as usual;
 *  - (de)serialization              -> not serialized (transient, like the dispatch state).
 */
struct ProcessingBegunMark {
    bool begun = false; ///< Whether the processing was begun and not yet completed

    ProcessingBegunMark() = default;
    ProcessingBegunMark(const ProcessingBegunMark & /* cp */) noexcept { /* a copy is not begun */
    }
    ProcessingBegunMark(ProcessingBegunMark &&) noexcept = default;
    ProcessingBegunMark &operator=(const ProcessingBegunMark &cp) noexcept {
        if(this != &cp) {
            begun = false; // a loaded item is not begun
        }
        return *this;
    }
    ProcessingBegunMark &operator=(ProcessingBegunMark &&) noexcept = default;
    ~ProcessingBegunMark() = default;
};

/**
 * @brief The two marks a work item carries for a failed evaluation (GSubmissionPolicy): that a consumer
 * put the item into its slot in place of one whose evaluation failed or went missing (a substitute),
 * and that the submitter forbids such a substitution for it. They belong to the object they were set
 * on -- to the slot of a batch, not to its content -- and their value-semantics say so in ONE place,
 * so the enclosing GProcessable can keep its special members defaulted:
 *  - COPY construction -> the target carries neither mark (a clone is a new item);
 *  - COPY assignment   -> the target KEEPS its own marks (a load or an absorbed return replaces the
 *                         content of a slot, not its part in the submission);
 *  - move              -> transferred, as usual;
 *  - (de)serialization -> not serialized (transient, like the dispatch state).
 */
struct SubmissionMarks {
    /// A consumer refilled the slot with a copy of an evaluated item
    bool substitute = false;
    /// The submitter forbids a substitute for this item
    bool not_substitutable = false;

    SubmissionMarks() = default;
    SubmissionMarks(const SubmissionMarks & /* cp */) noexcept { /* a copy carries no marks */
    }
    SubmissionMarks(SubmissionMarks &&) noexcept = default;
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp) -- deliberate: assignment keeps our own marks (see block comment above)
    SubmissionMarks &operator=(const SubmissionMarks & /* cp */) noexcept {
        return *this; /* keep the marks of this slot */
    }
    SubmissionMarks &operator=(SubmissionMarks &&) noexcept = default;
    ~SubmissionMarks() = default;
};

} // namespace detail

template <typename processable_type>
class GBaseConsumerT;

/******************************************************************************/
// An exception to be thrown if an exception was thrown during a work item's processing. It lives on the
// non-generic lifecycle base so both the courtier transports (which catch it around a process() call) and
// any processable (GProcessingContainerT or a self-contained work item such as the geneva individual)
// throw and catch the SAME type.
class g_processing_exception : public gem_exception {
    using gem_exception::gem_exception;
};

/******************************************************************************/
/**
 * @brief The non-generic processing lifecycle of a courtier work item.
 *
 * GProcessable holds everything about a work item that is INDEPENDENT of what it computes: its
 * processing status (the UNPROCESSED / DO_PROCESS / PROCESSED / EXCEPTION_CAUGHT / ERROR_FLAGGED
 * state machine), its accumulated error descriptions, the transport routing ids (the per-dispatch
 * correlation id and the stable lineage id), the transient per-batch dispatch-scheduling state, and
 * the processing timing. It carries NO result store and is NOT
 * templated on a result type, so the courtier transport and consumer machinery can reason about a work
 * item's lifecycle without knowing what it evaluates to.
 *
 * The result store, the actual process() orchestration and the (typed) pre-/post-processors live one
 * layer down, on whichever class turns a GProcessable into an actual submittable work item. There are two
 * such implementations: the generic, geneva-free GProcessingContainerT<processable_type,
 * processing_result_type> (used by the work items of courtier's tests, libs/courtier/tests/support),
 * and geneva's specialized Gem::Geneva::Genome::GGenome (which derives this class DIRECTLY and supplies its
 * own result store + process() with optimization-specific orchestration). Either way the only coupling
 * between the status machine and the result store -- clearing stored results when the status is reset --
 * is bridged by the virtual clearStoredResults_() hook, which the result-bearing derived class overrides.
 */
class GProcessable {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;
    /// The consumer's reconciliation (GBaseConsumerT::processBatch()) sets and clears the mark of a
    /// substitute; nothing else may.
    template <typename processable_type>
    friend class GBaseConsumerT;

    /**
     * @brief Serialises the non-generic lifecycle state (status, errors, routing ids, timing).
     * The transient dispatch-scheduling state (dispatch_state_), the transient mark of a begun
     * processing (processing_begun_) and the transient marks of a failed evaluation
     * (submission_marks_) are deliberately NOT serialised.
     *
     * @tparam archive_type The GArchive codec type
     * @param ar The archive to (de)serialise the lifecycle state with
     * @param version The (unused) serialization version number
     */
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        // The stable lineage id travels by value (its two 64-bit halves) so a wire / checkpoint round-trip
        // PRESERVES it -- see detail::LineageId for why this preserves while clone() mints fresh.
        archive_named(ar, "submission_uuid_hi", submission_uuid_.value[0]);
        archive_named(ar, "submission_uuid_lo", submission_uuid_.value[1]);
        archive_named(ar, "correlation_id_", correlation_id_);
        archive_named(ar, "pre_processing_time_", pre_processing_time_);
        archive_named(ar, "processing_time_", processing_time_);
        archive_named(ar, "post_processing_time_", post_processing_time_);
        archive_named(ar, "stored_error_descriptions_", stored_error_descriptions_);
        archive_named(ar, "processing_status_", processing_status_);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    GProcessable() = default;
    GProcessable(const GProcessable &) = default;
    GProcessable(GProcessable &&) noexcept = default;
    GProcessable &operator=(const GProcessable &) = default;
    GProcessable &operator=(GProcessable &&) noexcept = default;
    virtual ~GProcessable() = default;

    /***************************************************************************/
    // Processing status

    /** @brief @return The current processing status of this work item */
    [[nodiscard]] processingStatus getProcessingStatus() const noexcept {
        return processing_status_;
    }

    /** @brief @return A string representation of the current processing status (debugging) */
    [[nodiscard]] std::string getProcessingStatusAsStr() const noexcept {
        return psToStr(processing_status_);
    }

    /** @brief @return true if the item carries the PROCESSED flag */
    [[nodiscard]] bool is_processed() const noexcept {
        return (processingStatus::PROCESSED == this->getProcessingStatus());
    }

    /** @brief @return true if the item is currently UNPROCESSED */
    [[nodiscard]] bool is_unprocessed() const noexcept {
        return (processingStatus::UNPROCESSED == this->getProcessingStatus());
    }

    /** @brief @return true if the item is due for processing (DO_PROCESS) */
    [[nodiscard]] bool is_due_for_processing() const noexcept {
        return (processingStatus::DO_PROCESS == this->getProcessingStatus());
    }

    /** @brief @return true if processing produced an error (caught exception or user-flagged) */
    [[nodiscard]] bool has_errors() const noexcept {
        return (processingStatus::EXCEPTION_CAUGHT == processing_status_) ||
               (processingStatus::ERROR_FLAGGED == processing_status_);
    }

    /** @brief @return true if an error was explicitly flagged by the user (ERROR_FLAGGED) */
    [[nodiscard]] bool error_flagged_by_user() const noexcept {
        return (processingStatus::ERROR_FLAGGED == processing_status_);
    }

    /**
     * @brief Sets a given new processing state. Which new states are accepted depends on the current
     * state (see the body). Setting a new state of PROCESSED via this function is not allowed (it is
     * reached only by a successful process()/markAsProcessedWith()) and throws unless already set.
     * Every accepted transition erases existing information (past error messages, any stored
     * results -- the latter via the clearStoredResults_() hook -- and the mark of a begun
     * processing, so a re-submitted item is begun again before it is completed). Setting the state
     * the item has already changes nothing but that mark, which it erases as well.
     *
     * @param target_ps The desired new processing status
     */
    void set_processing_status(processingStatus target_ps = processingStatus::UNPROCESSED) {
        // The state it has already: nothing else changes, but a begun processing is forgotten all
        // the same (a re-submission with DO_PROCESS must be begun again before it is completed)
        if(target_ps == processing_status_) {
            this->assignStatus_(target_ps);
            return;
        }

        // We do not accept setting a target state of PROCESSED via this function
        if(target_ps == processingStatus::PROCESSED) {
            throw gem_exception(
                g_error_streamer()
                << "An attempt was made to set the processing state to PROCESSED" << '\n'
                << "which is not allowed through this function." << '\n'
            );
        }

        // The transition table, stated once: from UNPROCESSED only DO_PROCESS is reachable, from
        // DO_PROCESS only UNPROCESSED (an un-submit), and from any terminal state (PROCESSED /
        // EXCEPTION_CAUGHT / ERROR_FLAGGED) the item may be recycled to UNPROCESSED or DO_PROCESS.
        const bool allowed = [&] {
            using enum Gem::Courtier::processingStatus;
            switch(processing_status_) {
            case UNPROCESSED:
                return target_ps == DO_PROCESS;
            case DO_PROCESS:
                return target_ps == UNPROCESSED;
            case PROCESSED:
            case EXCEPTION_CAUGHT:
            case ERROR_FLAGGED:
                return target_ps == UNPROCESSED || target_ps == DO_PROCESS;
            }
            return false;
        }();

        if(not allowed) {
            throw gem_exception(
                g_error_streamer()
                << "Got invalid target processing status " << psToStr(target_ps) << '\n'
                << "for the current state of " << psToStr(processing_status_) << '\n'
            );
        }

        // Every accepted transition performs the same reset: adopt the state (which forgets a
        // begun processing), forget past errors and drop any stored results (via the
        // clearStoredResults_() hook of the result-bearing class).
        this->assignStatus_(target_ps);
        stored_error_descriptions_.clear();
        this->clearStoredResults_();
    }

    /** @brief Marks this item as being due for processing (DO_PROCESS; a begun processing is
     *  forgotten, so the item is begun again before it is completed). */
    void mark_as_due_for_processing() {
        this->assignStatus_(processingStatus::DO_PROCESS);
    }

    /** @brief Sets the UNPROCESSED flag so the item will not be processed (a begun processing is
     *  forgotten). */
    void mark_as_ignorable() {
        this->assignStatus_(processingStatus::UNPROCESSED);
    }

    /***************************************************************************/
    // Processing in two halves (an execution environment that evaluates a whole batch at once)

    /**
     * @brief Begins the processing of this item for an execution environment that evaluates a
     * whole batch at once (a device): the part of the processing before the evaluation. Only an
     * item with the DO_PROCESS status is accepted; the item's pre-processor runs, so the batch is
     * built from the pre-processed values. The result-bearing class's completeProcessing()
     * finishes it with the results computed for the item (GProcessingContainerT, and geneva's
     * GGenome; the completion lives there because it takes that class's typed results).
     * @throws gem_exception if the item is not due for processing (the item is left untouched)
     * @throws g_processing_exception if the pre-processing fails; the failure is recorded on the
     *         item (EXCEPTION_CAUGHT), as a full processing records it
     */
    void beginProcessing() {
        this->beginProcessingLifecycle_("GProcessable::beginProcessing()");
    }

    /***************************************************************************/
    // Transport routing

    /**
     * @brief Sets the transport correlation id -- the token used to route/match a work item through the
     * transport layer (a (batch_id, slot) token in the courtier networked consumers).
     * @param id The transport correlation id to store on this work item
     */
    void setCorrelationId(const CORRELATION_ID_TYPE &id) noexcept {
        correlation_id_ = id;
    }
    /** @brief @return The transport correlation id stored on this work item (see setCorrelationId()) */
    [[nodiscard]] CORRELATION_ID_TYPE getCorrelationId() const noexcept {
        return correlation_id_;
    }

    /**
     * @brief The stable, per-item lineage id (D11). Minted once at construction, preserved across
     * every (re-)dispatch and serialized round-trip, fresh only on clone(). Unlike the per-dispatch
     * correlation id (which routes a single return to its slot), this identifies the ITEM, so a late
     * return can be reunited with -- and de-duplicated against -- the live item it belongs to even
     * after it has been resubmitted under a new correlation id.
     * @return This work item's 128-bit lineage id
     */
    [[nodiscard]] SUBMISSION_UUID_TYPE getSubmissionUuid() const noexcept {
        return submission_uuid_.value;
    }
    /**
     * @brief Overwrites the lineage id. Used only to RESTORE a source's identity onto a stand-in copy:
     * a plain clone mints a fresh id, but a copy that stands in for an existing item (a consumer's
     * retained original, a checkpoint restore) must keep the lineage of the item it represents. Not for
     * general use -- the id is otherwise immutable after construction. In particular, the clone a
     * consumer hands out must keep the id it got when it was made: its first half names the process
     * that handed it out, and check-in drops a result whose id names another process.
     * @param uuid The lineage id to stamp onto this work item
     */
    void setSubmissionUuid(const SUBMISSION_UUID_TYPE &uuid) noexcept {
        submission_uuid_.value = uuid;
    }

    /**
     * @brief Sets the courtier per-batch scheduling state. This is transient, server-side-only
     * bookkeeping (NOT serialized): it lets a networked consumer track, on the item itself, whether
     * the slot is awaiting a client / in flight / done within one dispatch round.
     * @param s The new per-batch dispatch/scheduling state for this work item
     */
    void setDispatchState(dispatchState s) noexcept {
        dispatch_state_ = s;
    }
    /** @brief @return The current per-batch dispatch/scheduling state (see setDispatchState()) */
    [[nodiscard]] dispatchState getDispatchState() const noexcept {
        return dispatch_state_;
    }

    /***************************************************************************/
    // Substitutes for failed evaluations (see GSubmissionPolicy)

    /**
     * @brief Whether a consumer put this item into its slot in place of one whose evaluation failed or
     * went missing: a copy of a successfully evaluated item of the same submission (on_unresolved::clone).
     * The consumer sets it; it is cleared when the item is submitted again. A copy of the item does not
     * carry it, and it is not serialized.
     * @return true if this item is a substitute from its last submission
     */
    [[nodiscard]] bool isSubstitute() const noexcept {
        return submission_marks_.substitute;
    }

    /**
     * @brief Says whether a consumer may replace this item by a substitute when its evaluation fails or
     * goes missing. An item that may not be replaced follows the strict rule whatever the policy: it is
     * evaluated again within the policy's budgets (max_failed_retries, max_resubmissions), and the
     * program ends if it still has no result. Set by the submitter before the submission (an
     * evolutionary algorithm forbids it for its parents); kept until it is set again. A copy of the
     * item does not carry it, and it is not serialized.
     * @param substitutable false if a failed evaluation of this item may not be replaced by a substitute
     */
    void setSubstitutable(bool substitutable) noexcept {
        submission_marks_.not_substitutable = not substitutable;
    }

    /** @brief @return Whether a consumer may replace this item by a substitute (see setSubstitutable()) */
    [[nodiscard]] bool isSubstitutable() const noexcept {
        return not submission_marks_.not_substitutable;
    }

    /***************************************************************************/
    // Timing

    /** @brief @return A tuple of (pre-processing, processing, post-processing) times in seconds */
    [[nodiscard]] std::tuple<double, double, double> getProcessingTimes() const {
        return std::make_tuple(pre_processing_time_, processing_time_, post_processing_time_);
    }

    /***************************************************************************/
    // Error handling

    /**
     * @brief Retrieves and clears the stored exceptions, resetting the processing status.
     * @param ps The desired new processing status to set after extracting the stored exceptions
     * @return The stored error descriptions that were present before clearing
     */
    std::string get_and_clear_exceptions(processingStatus ps = processingStatus::UNPROCESSED) {
        std::string stored_exceptions = stored_error_descriptions_;
        this->set_processing_status(ps);
        return stored_exceptions;
    }

    /** @brief @return The accumulated error descriptions stored during processing */
    [[nodiscard]] std::string getStoredErrorDescriptions() const {
        return stored_error_descriptions_;
    }

    /***************************************************************************/
    // Retained-state transfer (server-side return reconciliation)

    /**
     * @brief Grafts the retained state of @p original back onto this work item.
     *
     * A work item may hold state that deliberately does not travel on the wire: it stays with the
     * submitter and is written only to a checkpoint or file (see ser_wire_omitted_ptr). A work item
     * that has made a networked round-trip therefore comes back WITHOUT it. When a networked
     * consumer reconciles such a return against the originally-submitted item it still holds, it
     * calls this to take that state back from the original, so the submitter's item keeps it across
     * the round-trip. The base retains no such state, so this is a no-op unless a derived class
     * overrides graftRetainedStateFrom_() (Geneva's GGenome does, for its algorithm scratch).
     * @param original The originally-submitted item supplying the retained state
     */
    void graftRetainedStateFrom(const GProcessable &original) {
        this->graftRetainedStateFrom_(original);
    }

    /***************************************************************************/
    // The processing lifecycle as a transportable record (see GProcessingOutcome).

    /**
     * @brief Snapshots this item's processing lifecycle into a transportable outcome record.
     *
     * Taken on the worker once an evaluation has finished, so the RESULT frame carries the status,
     * error text, timings and routing ids even when the frame ships a small library payload instead of
     * the whole work item.
     * @return The lifecycle snapshot of this work item
     */
    [[nodiscard]] GProcessingOutcome processingOutcome() const {
        return GProcessingOutcome{
            processing_status_,
            stored_error_descriptions_,
            pre_processing_time_,
            processing_time_,
            post_processing_time_,
            correlation_id_,
            submission_uuid_.value
        };
    }

    /**
     * @brief Writes a returned outcome onto this (server-side) work item.
     *
     * The counterpart of processingOutcome(), applied by the consumer to the live slot the return belongs
     * to. The status is assigned DIRECTLY rather than through set_processing_status(), because this is a
     * remote report of an already-completed evaluation, not a local state transition: the validating
     * setter would reject e.g. PROCESSED -> PROCESSED and would clear the very results the caller is about
     * to write. The item KEEPS its own lineage id (the consumer checks the outcome's uuid at
     * check-in, but a slot's identity is not redefined by a return); the transient dispatch state
     * is likewise untouched. A begun processing is forgotten: the outcome ends it.
     * @param outcome The lifecycle record reported by the worker
     */
    void applyProcessingOutcome(const GProcessingOutcome &outcome) {
        this->assignStatus_(outcome.status);
        stored_error_descriptions_ = outcome.error_descriptions;
        pre_processing_time_ = outcome.pre_processing_time;
        processing_time_ = outcome.processing_time;
        post_processing_time_ = outcome.post_processing_time;
        correlation_id_ = outcome.correlation_id;
    }

    /***************************************************************************/
    // In-place, pointer-preserving return reconciliation (server-side).

    /**
     * @brief Absorbs the computed results + processing lifecycle of a returned item @p src INTO this
     * work item, keeping this item's own heap address and lineage id.
     *
     * This is the pointer-preserving counterpart of "the returned object becomes the slot": rather
     * than overwriting a batch slot's owning pointer with the deserialized return (which would
     * free the original object and change its address), a networked consumer keeps the
     * originally-submitted object and absorbs only what the worker computed. Address stability lets
     * other parts of the system (e.g. a submitter's per-item prefetch overlapping the processing)
     * hold a snapshot of the batch's addresses across a submission. The lifecycle (status / errors /
     * timing / correlation id) is copied while THIS item keeps its own stable lineage id; a
     * result-bearing derived class additionally copies its result store, and deliberately keeps its
     * own input data and the state that does not travel on the wire (Geneva's GGenome documents
     * what it keeps).
     *
     * @param src The returned, evaluated item whose results + lifecycle are absorbed into this one
     */
    void absorbResultsFrom(const GProcessable &src) {
        this->absorbResultsFrom_(src);
    }

    /**
     * @brief Replaces this work item's content in place with a deep copy of @p src, keeping this item's
     * heap address (a fresh lineage id is the caller's responsibility -- a refill is a new item).
     *
     * Used by the clone-on-partial-return refill to substitute a viable sibling into a failed slot without
     * relocating it (so a concurrent address snapshot stays valid). The base has no content and returns
     * false ("not supported"); a result-bearing derived class overrides it to perform the in-place copy
     * and returns true. A consumer that gets false falls back to clone-and-replace (correct for the
     * item types of courtier's tests, which need no address stability).
     *
     * @param src The source item to deep-copy into this one
     * @return true if the in-place content copy was performed; false if unsupported (caller should replace)
     */
    [[nodiscard]] bool loadContentFrom(const GProcessable &src) {
        return this->loadContentFrom_(src);
    }

protected:
    /***************************************************************************/
    /**
     * @brief The shared processing lifecycle, stated ONCE for every processable item. This is the
     * template method the typed process() entry points (GProcessingContainerT::process and geneva's
     * GGenome::process) wrap: DO_PROCESS precondition, error/result reset, the timed
     * pre -> core -> (guarded) post sequence, exception funneling into EXCEPTION_CAUGHT, and the
     * error epilogue that zeroes the timings, clears the results and raises g_processing_exception.
     * The pre-processing step is the preProcess_() hook (beginProcessing() on this class needs it);
     * the other two class-specific steps arrive as callables: @p core the actual
     * evaluation/processing, @p post the post-processing step. The item is marked PROCESSED BEFORE
     * post-processing runs -- a post-processor refines an ALREADY-EVALUATED item (e.g. by a short
     * sub-optimization) and rejects a dirty one -- and an item that flagged an error
     * (ERROR_FLAGGED) keeps its error status: PROCESSED is never assigned and post-processing is
     * skipped. (This orchestration used to exist as two hand-synced copies in courtier and geneva;
     * the double-PROCESSED status bug lived in exactly that duplication.)
     *
     * The lifecycle is the two halves beginProcessingLifecycle_() and
     * completeProcessingLifecycle_() run back to back. An execution environment that evaluates a
     * whole batch at once (a device) runs them apart, through beginProcessing() and the
     * result-bearing class's completeProcessing(): the first half for every item before the batch
     * is built, so the batch holds the pre-processed values, and the second half for every item
     * once the batch's results are in. Results given to an item whose processing was begun so are
     * refused here: they were computed from the values after its pre-processor, which this
     * lifecycle would run again before adopting them.
     *
     * @param context The calling function's identity, used in the error texts (e.g. "GGenome::process()")
     * @param results_given Whether the caller hands in results computed elsewhere (a non-empty
     *        result vector) rather than evaluating the item in @p core
     * @param core A callable running the class-specific evaluation/processing
     * @param post A callable running the class-specific post-processing step
     * @throws gem_exception if @p results_given and the item's processing was begun with
     *         beginProcessing() (the item is left untouched)
     */
    template <typename core_fn_type, typename post_fn_type>
    void runProcessingLifecycle_(
        std::string_view context,
        bool results_given,
        core_fn_type &&core,
        post_fn_type &&post
    ) {
        if(results_given && processing_begun_.begun) {
            throw gem_exception(
                g_error_streamer()
                << "In " << context << ": Results were given to an item whose processing was"
                << " begun with beginProcessing()." << '\n'
                << "Complete it with completeProcessing() instead; process() would run the"
                << " pre-processor again before adopting results computed after the first run."
                << '\n'
            );
        }
        this->beginProcessingLifecycle_(context);
        this->completeProcessingLifecycle_(
            context,
            0.,
            std::forward<core_fn_type>(core),
            std::forward<post_fn_type>(post)
        );
    }

    /***************************************************************************/
    /**
     * @brief The first half of the processing lifecycle: the DO_PROCESS precondition, the
     * error/result reset and the timed pre-processing step (the preProcess_() hook). On success
     * the item stays DO_PROCESS, is marked as begun and is ready for
     * completeProcessingLifecycle_(); on an error the epilogue of funnelProcessingStep_() applies.
     * A pre-processing step that changes the processing status (e.g. through mark_as_ignorable())
     * fails the processing like any other failing step: the item is not due any more, so neither
     * the evaluation nor PROCESSED may follow.
     *
     * @param context The calling function's identity, used in the error texts
     */
    void beginProcessingLifecycle_(std::string_view context) {
        // We only accept items that are due for processing
        if(processingStatus::DO_PROCESS != processing_status_) {
            throw gem_exception(
                g_error_streamer()
                << "In " << context << ": Function called while processing_status_ was set to "
                << processing_status_ << '\n'
                << "Expected " << processingStatus::DO_PROCESS << '\n'
            );
        }

        // Clear the error descriptions and nullify the result list
        stored_error_descriptions_.clear();
        this->clearStoredResults_();
        processing_begun_.begun = false;

        this->funnelProcessingStep_(context, [this] {
            const auto start_time = std::chrono::steady_clock::now();
            this->preProcess_();
            pre_processing_time_ =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time)
                    .count();

            // An error flagged during pre-processing is the funnel's to handle. Any other change
            // of the status means the pre-processing step took the item out of processing.
            if(processingStatus::DO_PROCESS != processing_status_ && not this->has_errors()) {
                throw gem_exception(
                    g_error_streamer()
                    << "The pre-processing step changed the processing status to "
                    << processing_status_ << "; expected " << processingStatus::DO_PROCESS << '\n'
                );
            }
        });
        processing_begun_.begun = true;
    }

    /***************************************************************************/
    /**
     * @brief The second half of the processing lifecycle: the timed core step, the PROCESSED
     * status, the guarded post-processing step. Only an item whose first half has run (and that
     * is still DO_PROCESS) is accepted. The processing time is @p evaluation_time plus the time of
     * the core step: on a batch path the results were computed elsewhere, in one evaluation of the
     * whole batch, whose time each item of the batch reports.
     *
     * @param context The calling function's identity, used in the error texts
     * @param evaluation_time The time in seconds it took to compute the results elsewhere (0 if
     *        @p core computes them)
     * @param core A callable running the class-specific evaluation/processing
     * @param post A callable running the class-specific post-processing step
     */
    template <typename core_fn_type, typename post_fn_type>
    void completeProcessingLifecycle_(
        std::string_view context,
        double evaluation_time,
        core_fn_type &&core,
        post_fn_type &&post
    ) {
        if(processingStatus::DO_PROCESS != processing_status_ || not processing_begun_.begun) {
            throw gem_exception(
                g_error_streamer()
                << "In " << context << ": Processing was completed without having been begun"
                << " (processing_status_ is " << processing_status_ << ")" << '\n'
            );
        }
        processing_begun_.begun = false;

        this->funnelProcessingStep_(context, [this, &core, &post, evaluation_time] {
            const auto start_time = std::chrono::steady_clock::now();
            core();

            // The results have now been computed, so the work item is processed. Mark it PROCESSED
            // BEFORE post-processing: a post-processor refines an ALREADY-EVALUATED item and rejects
            // a dirty one. If processing flagged an error (ERROR_FLAGGED), the error status is left
            // intact and post-processing is skipped.
            const auto after_processing = std::chrono::steady_clock::now();
            if(not this->has_errors()) {
                processing_status_ = processingStatus::PROCESSED;
                post();
            }
            const auto after_post_processing = std::chrono::steady_clock::now();

            // Make a note of the time needed for each step
            processing_time_ = evaluation_time +
                               std::chrono::duration<double>(after_processing - start_time).count();
            post_processing_time_ =
                std::chrono::duration<double>(after_post_processing - after_processing).count();
        });
    }

    /***************************************************************************/
    /**
     * @brief The precondition of a result-bearing class's completeProcessing(): results were
     * given (a completion adopts results; it never evaluates the item itself).
     * @param context The calling function's identity, used in the error text
     * @param results_given Whether a non-empty result vector was given
     * @throws gem_exception if @p results_given is false (the item is left untouched)
     */
    static void requireResults_(std::string_view context, bool results_given) {
        if(not results_given) {
            throw gem_exception(
                g_error_streamer() << "In " << context << ": Error!" << '\n'
                                   << "No results were given" << '\n'
            );
        }
    }

    /***************************************************************************/
    /**
     * @brief Runs one step of the processing lifecycle with its exception funneling and error
     * epilogue: an exception out of @p step sets EXCEPTION_CAUGHT; if the item then has an error
     * (a caught exception or one the user flagged), the timings are zeroed, the results cleared,
     * the error descriptions stored and g_processing_exception raised.
     *
     * @param context The calling function's identity, used in the error texts
     * @param step The step to run
     */
    template <typename step_fn_type>
    void funnelProcessingStep_(std::string_view context, step_fn_type &&step) {
        std::ostringstream error_description_stream;

        try {
            step();
        }
        catch(std::exception &e) {
            // Let the audience know we had an error
            processing_status_ = processingStatus::EXCEPTION_CAUGHT;
            error_description_stream << "In " << context << ":" << '\n'
                                     << "Processing has thrown an exception with message" << '\n'
                                     << e.what() << '\n'
                                     << "We will rethrow this exception" << '\n';
        }
        catch(...) {
            // Let the audience know we had an error
            processing_status_ = processingStatus::EXCEPTION_CAUGHT;
            error_description_stream << "In " << context << ":" << '\n'
                                     << "Processing has thrown an unknown exception." << '\n';
        }

        if(this->has_errors()) { // Either an exception was caught or the user flagged an error
            // Do some cleanup
            pre_processing_time_ = 0.;
            processing_time_ = 0.;
            post_processing_time_ = 0.;
            processing_begun_.begun = false;

            // Nullify the result list
            this->clearStoredResults_();

            // Store the exceptions for later reference
            if(processingStatus::EXCEPTION_CAUGHT == processing_status_) {
                // Error information added by the user might already be stored in this variable; hence +=
                stored_error_descriptions_ += error_description_stream.str();
            }

            throw g_processing_exception( // A specific exception type flagging errors during processing
                g_error_streamer() << stored_error_descriptions_
            );
        }
    }

    /***************************************************************************/
    /**
     * @brief Assigns a processing status set from outside the processing lifecycle, and with it
     * forgets a begun processing. This is the rule, stated once: an item begun with
     * beginProcessing() is completed with completeProcessing() only while nothing else has set its
     * status in between. Whoever sets it -- an un-submit or new submission, results adopted with
     * markAsProcessedWith(), a returned outcome, a flagged error -- ends the begun processing, so
     * the next completion needs a beginning of its own (it would otherwise skip the pre-processor
     * of the values it completes). The lifecycle's own steps assign the status directly.
     * @param ps The new processing status
     */
    void assignStatus_(processingStatus ps) noexcept {
        processing_status_ = ps;
        processing_begun_.begun = false;
    }

    /**
     * @brief Hook: clears the (result-bearing derived class's) stored results when the processing
     * status is reset. The base has no result store, so the default is a no-op; GProcessingContainerT
     * overrides it to clear its result vector.
     */
    virtual void clearStoredResults_() { /* no result store in the base */
    }

    /**
     * @brief Hook: the pre-processing step of the processing lifecycle -- runs the item's
     * pre-processor, if it has one and pre-processing is allowed. The base has no pre-processor,
     * so the default is a no-op; the result-bearing classes (GProcessingContainerT, geneva's
     * GGenome) override it.
     */
    virtual void preProcess_() { /* no pre-processor in the base */
    }

    /**
     * @brief Hook: grafts the retained state of @p original back (see graftRetainedStateFrom()). The
     * base retains no such state, so the default is a no-op; Geneva's GGenome overrides it to
     * deep-copy its algorithm scratch back.
     * @param original The originally-submitted item supplying the retained state
     */
    virtual void graftRetainedStateFrom_([[maybe_unused]] const GProcessable &original) {
        /* no retained state in the base */
    }

    /**
     * @brief Hook behind absorbResultsFrom(): copies the non-generic processing lifecycle (status,
     * errors, timing, correlation id) from @p src into this item while keeping this
     * item's own lineage id (LineageId's copy-assignment keeps the target's value). A result-bearing
     * derived class overrides this to additionally copy its result store (and to keep its input data
     * and the state that does not travel on the wire).
     * @param src The returned item whose lifecycle state is absorbed
     */
    virtual void absorbResultsFrom_(const GProcessable &src) {
        GProcessable::operator=(src);
    }

    /**
     * @brief Hook behind loadContentFrom(): performs an in-place deep copy of @p src into this item.
     * The base carries no content and cannot, so it returns false; a result-bearing derived class
     * overrides it to copy its full content and returns true.
     * @param src The source item to copy in place
     * @return true if the in-place copy was performed; false in the base (unsupported)
     */
    virtual bool loadContentFrom_([[maybe_unused]] const GProcessable &src) {
        return false;
    }

    /**
     * @brief Lets derived classes flag a custom error condition. Sets the ERROR_FLAGGED status and
     * appends the (non-empty) message to the stored error descriptions.
     * @param error_info An error description (must not be empty; appended to any existing descriptions)
     */
    void force_set_error(const std::string &error_info) {
        if(error_info.empty()) {
            throw gem_exception(g_error_streamer() << "Error info is empty" << '\n');
        }
        // There may already be information stored in this variable. Hence we attach via +=
        stored_error_descriptions_ += error_info;
        this->assignStatus_(processingStatus::ERROR_FLAGGED);
    }

    /***************************************************************************/
    // Data -- protected so the result-bearing derived class's process()/etc. can read/write it.

    /// Stable, per-item lineage id (D11). A self-minting value type whose copy/assign/serialize
    /// semantics implement "fresh on clone, kept on load, preserved on the wire" -- see detail::LineageId.
    detail::LineageId submission_uuid_;

    CORRELATION_ID_TYPE correlation_id_ = CORRELATION_ID_TYPE();

    /// transient: server-side-only per-batch scheduling state of the networked consumers; the wire
    /// and a clone never need it (see dispatchState).
    dispatchState dispatch_state_ = dispatchState::NONE;

    double pre_processing_time_ = 0.;  ///< Time needed for pre-processing (seconds)
    double processing_time_ = 0.;      ///< Time needed for the actual processing step (seconds)
    double post_processing_time_ = 0.; ///< Time needed for post-processing (seconds)

    /// transient: belongs to one processing in this process -- set when the first half of the
    /// processing lifecycle has run, until the second half starts or the status is set from outside
    /// the lifecycle (assignStatus_()); never copied (see detail::ProcessingBegunMark).
    detail::ProcessingBegunMark processing_begun_;

    std::string
        stored_error_descriptions_; ///< Stores exceptions that may have occurred during processing
    processingStatus processing_status_ =
        processingStatus::UNPROCESSED; ///< By default no processing is initiated

private:
    /** @brief Marks or unmarks this item as a substitute (see isSubstitute()); the consumer's
     *  reconciliation is the only caller.
     *  @param substitute Whether the item stands in for one whose evaluation failed or went missing */
    void setSubstitute_(bool substitute) noexcept {
        submission_marks_.substitute = substitute;
    }

    /// transient: the marks of a failed evaluation belong to the slot of one submission; never
    /// copied to a clone (see detail::SubmissionMarks).
    detail::SubmissionMarks submission_marks_;
};

/******************************************************************************/
/**
 * A function that processes one work item in place of the item's own process(). A client can be
 * given one (GBaseClientT::setItemProcessor()); processWorkFrame() calls it for an item that is
 * due. The function must leave the item processed or flagged, as process() does.
 *
 * @tparam processable_type The type of the work item
 */
template <typename processable_type>
using GItemProcessorT = std::move_only_function<void(processable_type &) const>;

/******************************************************************************/
/**
 * @brief What a consumer requires of the items it distributes, and all it knows of them: an item
 * is a GProcessable (its status and lifecycle), processes itself (process()), and can be copied --
 * by a clone() that yields an owning pointer, or by its copy constructor (a refill, a copy shipped
 * to a client; see GBaseConsumerT::clone_item_()). That it travels over the wire is said by its
 * serialization and its registration with Weft (GEM_REGISTER_ARCHIVABLE), which a C++ concept
 * cannot see. The consumer providers and the launcher (courtier/GLauncherT.hpp) are constrained
 * by it.
 *
 * @tparam processable_type The item type
 */
template <typename processable_type>
concept ProcessableItem =
    std::derived_from<processable_type, GProcessable> && requires(processable_type &item) {
        item.process();
    } && (std::copy_constructible<processable_type> || requires(const processable_type &item) {
        { item.clone() } -> std::convertible_to<std::unique_ptr<processable_type>>;
    });

/******************************************************************************/

} /* namespace Gem::Courtier */
