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

#include <cstddef>

namespace Gem::Courtier {

/******************************************************************************/
/**
 * What a consumer should do with a population slot that it ultimately cannot turn into a
 * successfully evaluated item -- i.e. an item that FAILED (its evaluation threw/crashed) or one
 * that stayed MISSING (never returned) beyond the resubmission budget / was reclassified as a
 * hidden FAILED.
 */
//! [courtier-submission-policy#1]
enum class on_unresolved : Gem::Common::ENUMBASETYPE {
    fatal = 0, ///< Cannot continue -- terminate the program (e.g. gradient methods need every value)
    clone = 1  ///< Refill the slot with a clone of a successfully evaluated item (population-based, tolerant)
};
//! [courtier-submission-policy#1]

/******************************************************************************/
/**
 * A submission policy tells the consumer how to reconcile a batch (a std::span over the
 * population slice) when evaluations are missing or fail. Submission can NEVER be assumed to
 * succeed for every item (an evaluate() can throw, a client can die), so the consumer
 * always reconciles the span against this policy and guarantees a valid, full-size population on
 * return -- or terminates fatally when the policy cannot be honoured.
 *
 * Two failure kinds are distinguished per slot:
 *  - MISSING  : the item never came back (network loss, timeout, dead client).
 *  - FAILED   : the item came back flagged (its evaluation threw/crashed).
 * A MISSING item that never returns despite repeated resubmissions (max_resubmissions) is likely a
 * *poison* individual that crashes the client; once its resubmission budget is exhausted it becomes
 * unresolved rather than being resubmitted forever (see GBaseConsumerT::processBatch).
 */
//! [courtier-submission-policy#2]
struct GSubmissionPolicy {
    /** @brief What to do with a slot that cannot be successfully evaluated */
    on_unresolved unresolved_action = on_unresolved::fatal;

    /** @brief Maximum number of times a MISSING item is resubmitted before it counts as unresolved */
    std::size_t max_resubmissions = 5;

    /** @brief Maximum number of times a FAILED item is re-evaluated (to ride out *transient* crashes;
     *  a deterministic crash will keep failing). 0 == a FAILED item is immediately unresolved. */
    std::size_t max_failed_retries = 0;

    /** @brief Floor under every policy: if NOTHING usable came back at all, terminate fatally */
    bool fatal_on_none_returned = true;
//! [courtier-submission-policy#2]

    /***************************************************************************/
    /** @brief GD/CGD and the parameter scan: every slot must be successfully evaluated; resubmit
     *  MISSING, optionally retry transient FAILED, and terminate if any slot stays unresolved.
     *
     *  @param max_resub Maximum number of times a MISSING item is resubmitted
     *  @param max_failed_retries Maximum number of times a FAILED item is re-evaluated (rides out transient crashes)
     *  @return A fatal-on-unresolved submission policy with the given resubmission/retry budgets */
    static GSubmissionPolicy full_success_or_fatal(
        std::size_t max_resub = 5,
        std::size_t max_failed_retries = 0
    ) {
        GSubmissionPolicy p;
        p.unresolved_action = on_unresolved::fatal;
        p.max_resubmissions = max_resub;
        p.max_failed_retries = max_failed_retries;
        p.fatal_on_none_returned = true;
        return p;
    }

    /***************************************************************************/
    /** @brief EA and other population-based algorithms: refill missing/failed slots with clones of
     *  successful items (never fatal except the zero-usable floor).
     *
     *  Resubmission defaults to 0 here ON PURPOSE. An evaluate() is unbounded in time, so
     *  resubmitting a MISSING item means waiting out *another* full (possibly very long) evaluation;
     *  a tolerant algorithm gains nothing by that wait, because a clone of an already-evaluated
     *  sibling is immediately available and is an acceptable population member. So for these
     *  algorithms a single clone beats a resubmission: a slot that times out is cloned at once rather
     *  than re-dispatched. (Need-all algorithms have no such option -- see full_success_or_fatal,
     *  where resubmission is mandatory because only a real evaluation will do.) Pass a non-zero
     *  @p max_resub only if a particular problem genuinely prefers re-evaluation over substitution.
     *
     *  @param max_resub Maximum number of MISSING resubmissions before falling back to cloning (default 0)
     *  @return A clone-on-unresolved submission policy (never fatal except the zero-usable floor) */
    static GSubmissionPolicy clone_on_partial_return(std::size_t max_resub = 0) {
        GSubmissionPolicy p;
        p.unresolved_action = on_unresolved::clone;
        p.max_resubmissions = max_resub;
        p.max_failed_retries = 0;
        p.fatal_on_none_returned = true; // even a tolerant algorithm cannot continue from nothing
        return p;
    }

    /***************************************************************************/
    /** @brief Strictest floor: no resubmission, any unresolved slot is fatal.
     *  @return A submission policy with zero resubmissions/retries and fatal-on-unresolved */
    static GSubmissionPolicy fail_on_no_return() {
        GSubmissionPolicy p;
        p.unresolved_action = on_unresolved::fatal;
        p.max_resubmissions = 0;
        p.max_failed_retries = 0;
        p.fatal_on_none_returned = true;
        return p;
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier */
