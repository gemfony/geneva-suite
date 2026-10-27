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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard headers go here
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

// Boost headers go here

// Geneva headers go here

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * A one-shot "wait for N completions" latch: one party arms it with a count, N worker parties each call
 * @c count_down() exactly once when their unit of work finishes, and one waiter blocks in @c wait() until
 * the count reaches zero. It replaces the hand-rolled per-batch @c atomic + @c mutex + @c condition_variable
 * triple (e.g. a thread-pool consumer waiting for one batch's items while the pool is shared across batches).
 *
 * @par Lifetime (important)
 * The waiter's predicate (@c remaining_ == 0) is satisfied by the atomic decrement in @c count_down(), which
 * happens BEFORE that last worker takes the internal mutex to notify. So @c wait() can return -- and its
 * caller may destroy this object -- while the last worker is still about to lock the mutex, which would lock
 * freed memory (a use-after-scope). Therefore a latch shared between workers and a waiter must be kept alive
 * by ALL of them, e.g. held via a @c std::shared_ptr captured by every worker task and by the waiter, so it
 * outlives the last @c count_down(). Do not stack-allocate it across that boundary.
 *
 * The latch is re-armable (@c arm()) for reuse once a previous round has fully completed.
 */
class GCompletionLatchT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /** @brief Constructs a latch armed for @p n completions (0 == already complete). */
    explicit GCompletionLatchT(std::size_t n = 0) : remaining_(n) {}

    GCompletionLatchT(const GCompletionLatchT &) = delete;
    GCompletionLatchT &operator=(const GCompletionLatchT &) = delete;

    /** @brief (Re-)arms the latch to wait for @p n completions. Call only when no count_down/wait of a
     *  previous round is still in flight. @param n The number of completions to await. */
    void arm(std::size_t n) {
        std::scoped_lock const lk(m_);
        remaining_.store(n, std::memory_order_relaxed);
    }

    /** @brief Records one completion. Must be called exactly @c n times after @c arm(n) (or construction
     *  with n). @return true iff this call brought the count to zero (i.e. it was the last completion). */
    bool count_down() {
        if(remaining_.fetch_sub(1) == 1) { // this was the last outstanding completion
            std::scoped_lock const lk(m_);
            cv_.notify_all();
            return true;
        }
        return false;
    }

    /** @brief Blocks until the armed count has been fully counted down. */
    void wait() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return remaining_.load() == 0; });
    }

    /** @brief Blocks until completion or @p d elapses.
     *  @param d The maximum time to wait. @return true if the latch completed, false on timeout. */
    template <typename Rep, typename Period>
    bool wait_for(const std::chrono::duration<Rep, Period> &d) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, d, [this] { return remaining_.load() == 0; });
    }

    /** @brief @return The number of completions still outstanding (a racy snapshot under churn). */
    std::size_t remaining() const {
        return remaining_.load();
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::atomic<std::size_t> remaining_;
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
