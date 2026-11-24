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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// std::convertible_to, for the item's own clone() (clone_item_())
#include <concepts>

// Geneva headers (reused from the common + courtier libraries)
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/record/GRecorder.hpp" // the end-of-run report (recordEndOfRun())
#include "courtier/GCourtierEnums.hpp" // processingStatus
#include "courtier/GProcessable.hpp"   // the non-generic work-item lifecycle base
#include "courtier/GSubmissionPolicy.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * Base class for courtier consumers. A consumer is handed a *batch* -- a std::span over the
 * submitter's work items to process -- together with a GSubmissionPolicy, and is SOLELY responsible
 * for turning that span into a fully, successfully processed batch (or for terminating the
 * program when the policy cannot be honoured). There is no "guaranteed full return" assumption
 * anywhere: an evaluation can always fail or never return, so the consumer always reconciles.
 *
 * Concrete consumers implement only dispatch_(): "evaluate these items, blocking until each has
 * returned (PROCESSED) or failed (EXCEPTION_CAUGHT/ERROR_FLAGGED) or -- for networked consumers --
 * timed out (left DO_PROCESS == MISSING)". The shared reconciliation loop in processBatch() then
 * applies the policy (resubmit MISSING, retry transient FAILED, clone, or terminate).
 *
 * @tparam processable_type The concrete work-item type evaluated by this consumer
 */
template <typename processable_type>
class GBaseConsumerT {
public:
    using item_ptr = std::unique_ptr<processable_type>;

    GBaseConsumerT() = default;
    virtual ~GBaseConsumerT() = default;

    GBaseConsumerT(const GBaseConsumerT &) = delete;
    GBaseConsumerT(GBaseConsumerT &&) = delete;
    GBaseConsumerT &operator=(const GBaseConsumerT &) = delete;
    GBaseConsumerT &operator=(GBaseConsumerT &&) = delete;

    /***************************************************************************/
    /** @brief Enables/sizes the consumer's late-return buffer: results that come back AFTER their
     *  batch was already reconciled are retained (instead of dropped) for the submitter to
     *  reap. @p cap == 0 disables it. Default: a no-op -- local (serial / thread-pool) consumers never
     *  produce late returns. Networked consumers override this. The arguments are the maximum number
     *  of late-return items to buffer (cap; 0 disables late-return buffering) and the number of
     *  dispatch rounds a buffered late return is kept before being discarded (ttl_rounds). */
    virtual void enableLateReturns(std::size_t /*cap*/, std::uint64_t /*ttl_rounds*/) {
    }

    /** @brief Hands back (transfers ownership of) the work items the consumer buffered as late returns,
     *  emptying the buffer. Default: none. Networked consumers override this to drain their buffer.
     *  @return The buffered late-return items (ownership transferred); empty if the consumer buffers none */
    virtual std::vector<item_ptr> getLateReturns() {
        return {};
    }

    /***************************************************************************/
    /** @brief How many clients are connected to this consumer now: the one question about its
     *  clients that every consumer answers, so that a caller need not know the transport. A
     *  consumer without clients (a local one) answers 0, the default; a networked consumer that
     *  cannot tell answers std::nullopt (GNetworkedConsumerT); one that can, its count. From any
     *  thread.
     *  @return The number of connected clients, or std::nullopt if this consumer cannot tell */
    [[nodiscard]] virtual std::optional<std::size_t> connectedClients() const {
        return 0;
    }

    /***************************************************************************/
    /** @brief The figures of a consumer at the end of a run, as (name, value) pairs */
    using figures_type = std::vector<std::pair<std::string, std::uint64_t>>;

    /** @brief The counters this consumer keeps, for the end-of-run report (recordEndOfRun()). The
     *  default has none: a local consumer counts nothing worth a report. A consumer that counts
     *  overrides it (the networked ones: GNetworkedConsumerT). Read, not reset.
     *  @return The figures, by name */
    [[nodiscard]] virtual figures_type endOfRunFigures() const {
        return {};
    }

    /** @brief The end-of-run report: appends one row of endOfRunFigures(), after the name of the
     *  consumer, to the stream "courtier" of the recorder (Gem::Common::Record), and closes the
     *  stream, so the file is complete at once. A consumer without figures records nothing (and so
     *  creates no file). courtier's launcher calls it when it releases its consumer, also after an
     *  exception or a stop request.
     *  @param consumer The name of the consumer (its mnemonic) */
    void recordEndOfRun(std::string const &consumer) const {
        figures_type figures = this->endOfRunFigures();
        if(figures.empty()) {
            return;
        }
        std::vector<Gem::Common::Record::GColumn> columns{
            {.name = "consumer", .type = Gem::Common::Record::columnType::TEXT}
        };
        Gem::Common::Record::GRow row{consumer};
        for(auto &[name, value] : figures) {
            columns.push_back({.name = std::move(name), .type = Gem::Common::Record::columnType::UNSIGNED});
            row.emplace_back(value);
        }
        auto stream = Gem::Common::Record::recorder().stream("courtier", std::move(columns));
        stream.append(std::move(row));
        stream.close();
    }

    /***************************************************************************/
    /**
     * Evaluates and reconciles a batch in place. On return, every slot of the span holds a
     * successfully evaluated item (possibly a clone, for tolerant policies, marked as a substitute:
     * GProcessable::isSubstitute()). Terminates the program if the policy cannot be honoured, or if
     * an item its submitter forbids to substitute (GProcessable::setSubstitutable()) stays
     * unresolved.
     *
     * @param items The batch of (uniquely owned) work items to evaluate and reconcile, in place
     * @param policy The submission policy governing resubmit/retry/clone/terminate decisions
     * @param clone_template An optional representative item used as the clone source when refilling
     *        unresolved slots (if null, the first successfully evaluated sibling is used instead)
     */
    // NOLINTNEXTLINE(readability-function-size) -- one coherent reconciliation state machine (dispatch round, per-slot pending/resolved/unresolved classification against the submission policy, then clone/fatal reconciliation) sharing the same per-slot state vectors throughout; splitting would scatter the tightly-coupled loop state
    void processBatch(
        std::span<item_ptr> items,
        const GSubmissionPolicy &policy,
        item_ptr clone_template = nullptr
    ) {
        using Gem::Courtier::processingStatus;

        const std::size_t n = items.size();
        if(n == 0) {
            return;
        }

        enum class slot {
            pending,
            resolved,
            unresolved
        };
        std::vector<slot> state(n, slot::pending);
        std::vector<std::size_t> resubmits(n, 0);
        std::vector<std::size_t> failed_retries(n, 0);
        bool any_success = false;

        // Mark every (non-null) slot due for processing; a substitute of an earlier submission is
        // one no longer. Null slots are treated as resolved.
        for(auto &&[item, item_state] : std::views::zip(items, state)) {
            if(item) {
                item->set_processing_status(processingStatus::DO_PROCESS);
                item->setSubstitute_(false);
            }
            else {
                item_state = slot::resolved;
            }
        }

        // Reconciliation loop. Terminates because every MISSING/FAILED slot eventually exhausts
        // its budget and becomes "unresolved" (so it stops being pending).
        while(true) {
            // Any slot still pending and flagged for processing? (A pending slot is always DO_PROCESS;
            // resolved/unresolved slots are not.) There is no temporary working vector: the consumer
            // evaluates the batch span IN PLACE, processing exactly the slots whose status is DO_PROCESS
            // and writing each (possibly replaced) result straight back into its own slot.
            const bool any_pending =
                std::ranges::any_of(std::views::zip(state, items), [](auto &&slot_and_item) {
                    auto &&[s, item] = slot_and_item;
                    return s == slot::pending && item &&
                           item->getProcessingStatus() == processingStatus::DO_PROCESS;
                });
            if(not any_pending) {
                break; // every slot is resolved or unresolved
            }

            // Consumer-specific evaluation of this round, directly over the batch span. A networked
            // consumer evaluates a copy on a remote client and writes the returned object back into the
            // originating slot; a local consumer mutates each item in place. Either way every DO_PROCESS
            // slot carries its (possibly replaced) result on return, and non-DO_PROCESS slots are skipped.
            this->dispatch_(items);

            for(auto &&[item, item_state, item_resubmits, item_failed_retries] :
                std::views::zip(items, state, resubmits, failed_retries)) {
                if(item_state != slot::pending) {
                    continue;
                }
                const auto st = item->getProcessingStatus();
                if(st == processingStatus::PROCESSED) {
                    item_state = slot::resolved;
                    any_success = true;
                }
                else if(
                    st == processingStatus::EXCEPTION_CAUGHT ||
                    st == processingStatus::ERROR_FLAGGED
                ) {
                    // FAILED: retry only to ride out *transient* crashes; a deterministic crash
                    // will keep failing, so the budget terminates in "unresolved".
                    if(item_failed_retries < policy.max_failed_retries) {
                        ++item_failed_retries;
                        item->set_processing_status(processingStatus::DO_PROCESS);
                    }
                    else {
                        item_state = slot::unresolved; // status already non-DO_PROCESS
                    }
                }
                else {
                    // MISSING (still DO_PROCESS -- a networked timeout). Resubmit within budget;
                    // a MISSING item that persists despite resubmissions is treated as a hidden
                    // FAILED (a poison item) and becomes unresolved rather than looping forever.
                    if(item_resubmits < policy.max_resubmissions) {
                        ++item_resubmits; // stays DO_PROCESS -> re-dispatched next round
                    }
                    else {
                        item_state = slot::unresolved;
                        // Clear the DO_PROCESS flag (DO_PROCESS -> UNPROCESSED is the only valid
                        // transition here) so the next round's in-place dispatch no longer selects this
                        // permanently-unresolved slot. The slot is reconciled below by clone/fatal.
                        item->set_processing_status(processingStatus::UNPROCESSED);
                    }
                }
            }
        }

        // Floor: if nothing at all could be evaluated, no policy can rescue the batch.
        if(policy.fatal_on_none_returned && not any_success) {
            this->fatal_(
                "processBatch(): no work item could be evaluated at all "
                "(every evaluation failed or went missing)."
            );
        }

        // Reconcile the unresolved slots per the policy. An item its submitter forbids to substitute
        // follows the strict rule: its budgets are spent, so the program ends.
        for(auto &&[item, item_state] : std::views::zip(items, state)) {
            if(item_state != slot::unresolved) {
                continue;
            }
            if(not item->isSubstitutable()) {
                this->fatal_(
                    "processBatch(): a work item that may not be substituted could not be "
                    "evaluated (failed or missing) within the budgets of the submission policy."
                );
            }
            else if(policy.unresolved_action == on_unresolved::clone) {
                this->refill_slot_(item, items, clone_template);
            }
            else {
                this->fatal_(
                    "processBatch(): a work item could not be evaluated (failed or missing) "
                    "and the submission policy forbids substitution (full-success-or-fatal)."
                );
            }
        }
    }

protected:
    /***************************************************************************/
    /**
     * Consumer-specific evaluation of one round, directly over the batch span. The consumer processes
     * exactly the slots whose status is DO_PROCESS (skipping null slots and slots in any other state),
     * leaving each evaluated item PROCESSED or -- on a caught processing exception -- EXCEPTION_CAUGHT/
     * ERROR_FLAGGED, or -- for networked consumers that time out -- DO_PROCESS to signal MISSING. Must
     * not let exceptions escape (a failed evaluation is reported via the item's status, not by throwing).
     *
     * A consumer that evaluates a copy elsewhere (e.g. a remote client) writes the resulting object back
     * into its originating span slot in place; a consumer that mutates each item in place leaves the
     * pointers untouched. No working copy of the batch is made -- the span is the submitter's own
     * view of its items.
     *
     * @param items The batch span to evaluate in place; the consumer acts on the DO_PROCESS slots and
     *        writes each (possibly replaced) result straight back into the same slot
     */
    virtual void dispatch_(std::span<item_ptr> items) = 0;

    /***************************************************************************/
    /** @brief Clean, fatal exit when the policy cannot be honoured. This is an expected terminal
     *  condition (e.g. a lost evaluation under a need-all policy), not an internal fault, so it
     *  exits cleanly via LOGEXIT rather than std::terminate()-ing with a core dump.
     *  @param msg The human-readable reason for the fatal exit (logged before exiting) */
    void fatal_(const std::string &msg) const {
        glogger << "In Gem::Courtier consumer:" << '\n'
                << "FATAL: " << msg << '\n'
                << LOGEXIT(EXIT_FAILURE);
    }

    /***************************************************************************/
    /** @brief Deep-clones one (uniquely owned) work item into a fresh owning item. In this order:
     *  the clone function, if one was set (setCloneFunction()); else the item's own clone(), if it
     *  yields the item pointer (so a polymorphic base such as Geneva's GGenome is copied with its
     *  dynamic type, not sliced); else copy construction (correct for leaf/concrete item types).
     *  Used both by the refill path and by the networked consumers when handing a session a copy to
     *  ship.
     *  @param src The (borrowed) uniquely owned work item to clone from
     *  @return A fresh owning copy of @p src; an empty item_ptr if cloning is impossible (after fatal exit) */
    [[nodiscard]] item_ptr clone_item_(const item_ptr &src) const {
        if(clone_fn_) {
            return clone_fn_(src);
        }
        if constexpr(requires(const processable_type &item) {
                         { item.clone() } -> std::convertible_to<item_ptr>;
                     }) {
            return src->clone();
        }
        else if constexpr(std::is_copy_constructible_v<processable_type>) {
            return std::make_unique<processable_type>(*src);
        }
        else {
            this->fatal_(
                "cloning a work item needs a clone function (setCloneFunction), a clone() "
                "member function that yields the item pointer, or a copy-constructible work "
                "item."
            );
        }
        return {};
    }

public:
    /***************************************************************************/
    /** @brief Sets the function that copies a work item, used before anything else (see
     *  clone_item_()). Without one, an item is copied through its own clone() if that yields the
     *  item pointer -- which covers a polymorphic base such as Geneva's GGenome -- and by copy
     *  construction otherwise. A function is needed only for an item type that has no fitting
     *  clone() and must not be copy-constructed, or to copy in a special way (a test double).
     *  @param fn The deep-clone functor taking a borrowed source item and returning a fresh owning copy */
    void setCloneFunction(std::function<item_ptr(const item_ptr &)> fn) {
        clone_fn_ = std::move(fn);
    }

private:
    /***************************************************************************/
    /** @brief Refills an unresolved slot @p dest with a viable sibling under clone-on-partial-return,
     *  preferring an in-place substitution that PRESERVES the slot's heap address.
     *
     *  Source = a caller-supplied @p clone_template (e.g. a representative item) if present, else the
     *  first successfully evaluated sibling in the batch. The substitution copies the source's content INTO
     *  the failed slot without relocating it (@c loadContentFrom) and then mints a fresh lineage id -- a
     *  refill is a NEW item, distinct from the failed original, so a very-late return for that
     *  original never reunites with the substitute. Either way the refill is marked as a substitute
     *  (GProcessable::isSubstitute()), so the submitter sees that this evaluation went wrong. Item types that need no address stability (the
     *  containers of courtier's tests) return false from @c loadContentFrom and fall back to
     *  clone-and-replace, exactly as before. Keeping the address stable lets a submitter's
     *  concurrent per-item prefetch hold a snapshot of the batch's addresses across the submission.
     *
     *  @param dest The unresolved slot to refill (its owning pointer, so a fallback can replace it)
     *  @param items The batch, scanned for a successfully evaluated sibling when no template is given
     *  @param clone_template An explicit clone source; if empty, the first processed item of @p items is used */
    void
    refill_slot_(item_ptr &dest, std::span<item_ptr> items, const item_ptr &clone_template) const {
        // Items are uniquely owned, so the source is only borrowed (a pointer to the chosen owner).
        const item_ptr *src = &clone_template;
        if(not *src) {
            for(auto &it : items) {
                if(it && it->is_processed()) {
                    src = &it;
                    break;
                }
            }
        }
        if(not *src) {
            this->fatal_(
                "clone-on-partial-return: no clone template was supplied and no successfully "
                "evaluated item is available to clone from."
            );
            return;
        }
        // Prefer the pointer-preserving in-place substitution; fall back to clone-and-replace for item
        // types that do not support it (they need no address stability).
        if(dest && dest->loadContentFrom(**src)) {
            dest->setSubmissionUuid(detail::mint_submission_uuid()); // a refill is a new item
        }
        else {
            dest = this->clone_item_(*src);
        }
        dest->setSubstitute_(true);
    }

    /***************************************************************************/
    std::function<item_ptr(const item_ptr &)> clone_fn_; ///< The clone function, if one was set
};

/******************************************************************************/

} /* namespace Gem::Courtier */
