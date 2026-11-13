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
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

// Geneva headers go here
#include "common/GCommonEnums.hpp"   // for DEFAULTBUFFERSIZE

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * A thread-safe, **multi-producer / multi-consumer (MPMC)** FIFO blocking queue.
 *
 * The "MPMC" in the name is deliberate: any number of threads may push and any
 * number may pop concurrently. Restricted concurrency patterns (SPSC, SPMC, MPSC)
 * permit cheaper, partly lock-free designs; this class makes no such assumption and
 * is safe for the fully general case. Use it whenever the producer/consumer
 * multiplicity is not provably restricted to a single thread on one side.
 *
 * This is a modernised successor of the older GBoundedBufferT. It keeps the proven
 * concurrency core (a mutex plus two condition variables, predicate-based waits,
 * notification outside the lock) but cleans up the API and fixes several issues:
 *
 *  - A single, perfect-forwarding `push` family covers both copy and move (one
 *    function instead of a copy/move pair), constrained with a concept so it only
 *    accepts items convertible to `T`. This makes the queue usable both for
 *    copyable items (e.g. std::shared_ptr) and move-only items (e.g.
 *    std::unique_ptr) without dead overloads.
 *  - `pop` returns `std::optional<T>` (works for move-only `T`; no out-parameter,
 *    no default-construct-then-assign).
 *  - Bounded vs. unbounded is handled with `if constexpr`, not a duplicated set of
 *    `requires`-constrained overloads on a dummy template parameter.
 *  - `remaining_space()` no longer underflows for the unbounded case (it returns
 *    SIZE_MAX) — the old `t_capacity - size()` wrapped around when `t_capacity==0`.
 *  - A `close()` operation wakes every blocked waiter and gives clean shutdown
 *    semantics: after `close()` no new items are accepted, blocked producers return
 *    `false`, and consumers drain the remaining items before `pop()` reports the
 *    end of stream via `std::nullopt`. The old class had no way to unblock a
 *    consumer parked in a no-timeout blocking pop, so teardown could hang.
 *  - All observers are `const`; all value-returning functions are `[[nodiscard]]`.
 *
 * Setting the template argument `t_capacity` to 0 yields an unbounded queue (in
 * which case the producing functions never block on fullness). `std::deque` is used
 * as the backing store deliberately: it serves both the bounded and the unbounded
 * configuration with one type. `T` must be at least move-constructible.
 *
 * Items are pushed at the front and popped from the back, giving FIFO order.
 *
 * @tparam T The element type stored in the queue (must be move-constructible)
 * @tparam t_capacity The maximum number of items; 0 selects an unbounded queue
 */
template <typename T, std::size_t t_capacity = DEFAULTBUFFERSIZE>
class GBlockingMPMCQueueT final {
    static_assert(
        std::is_move_constructible_v<T>,
        "GBlockingMPMCQueueT<T>: T must be move-constructible"
    );

public:
    using value_type = T;
    using container_type = std::deque<T>;

    /*************************************************************************/
    GBlockingMPMCQueueT() = default;

    // The queue owns a mutex and condition variables and is therefore neither
    // copyable nor movable.
    GBlockingMPMCQueueT(GBlockingMPMCQueueT const &) = delete;
    GBlockingMPMCQueueT &operator=(GBlockingMPMCQueueT const &) = delete;
    GBlockingMPMCQueueT(GBlockingMPMCQueueT &&) = delete;
    GBlockingMPMCQueueT &operator=(GBlockingMPMCQueueT &&) = delete;

    /*************************************************************************/
    /**
     * The destructor closes the queue (waking any stragglers) and clears the
     * backing store. A destructor must not throw, so any exception escaping the
     * element destructors or the lock is swallowed.
     */
    ~GBlockingMPMCQueueT() {
        try {
            close();
            std::scoped_lock const lock(mutex_);
            container_.clear();
        } catch(...) { // NOLINT(bugprone-empty-catch) — a destructor must not throw
        }
    }

    /*************************************************************************/
    /**
     * Tries to add an item without blocking. Returns false immediately if the
     * queue is full (bounded case) or closed. Covers both copy and move via
     * perfect forwarding.
     *
     * @tparam U The forwarded argument type (must be usable to construct a T)
     * @param item The item to add (forwarded into the queue)
     * @return true if the item was added, false otherwise
     */
    template <typename U>
        requires std::constructible_from<T, U &&>
    [[nodiscard]] bool try_push(U &&item) {
        {
            std::scoped_lock const lock(mutex_);
            if(closed_ || full_unlocked()) {
                return false;
            }
            container_.emplace_front(std::forward<U>(item));
        } // release the lock before notifying
        not_empty_.notify_one();
        return true;
    }

    /*************************************************************************/
    /**
     * Adds an item, blocking until space is available. Returns false (without
     * adding) if the queue is closed while waiting or already closed. For an
     * unbounded queue this never blocks.
     *
     * @tparam U The forwarded argument type (must be usable to construct a T)
     * @param item The item to add (forwarded into the queue)
     * @return true if the item was added, false if the queue was closed
     */
    template <typename U>
        requires std::constructible_from<T, U &&>
    [[nodiscard]] bool push(U &&item) {
        {
            std::unique_lock lock(mutex_);
            // The predicate is re-checked under the lock, which both handles
            // spurious wake-ups and makes the post-unlock notify safe.
            not_full_.wait(lock, [this]() -> bool { return closed_ || not full_unlocked(); });
            if(closed_) {
                return false;
            }
            container_.emplace_front(std::forward<U>(item));
        }
        not_empty_.notify_one();
        return true;
    }

    /*************************************************************************/
    /**
     * Adds an item, blocking until space is available or the timeout elapses.
     * Returns false if it timed out or the queue was closed. For an unbounded
     * queue this never times out on fullness.
     *
     * @tparam U The forwarded argument type (must be usable to construct a T)
     * @tparam Rep The std::chrono::duration tick representation of the timeout
     * @tparam Period The std::chrono::duration period of the timeout
     * @param item The item to add (forwarded into the queue)
     * @param timeout Maximum time to wait for space
     * @return true if the item was added, false on timeout or close
     */
    template <typename U, typename Rep, typename Period>
        requires std::constructible_from<T, U &&>
    [[nodiscard]] bool push_wait(U &&item, std::chrono::duration<Rep, Period> const &timeout) {
        {
            std::unique_lock lock(mutex_);
            if(not not_full_.wait_for(
                   lock, timeout, [this]() -> bool { return closed_ || not full_unlocked(); }
               )) {
                return false; // timed out with no space
            }
            if(closed_) {
                return false;
            }
            container_.emplace_front(std::forward<U>(item));
        }
        not_empty_.notify_one();
        return true;
    }

    /*************************************************************************/
    /**
     * Tries to remove an item without blocking. Returns std::nullopt if the queue
     * is empty.
     *
     * @return The item, or std::nullopt if the queue was empty
     */
    [[nodiscard]] std::optional<T> try_pop() {
        std::optional<T> result;
        {
            std::scoped_lock const lock(mutex_);
            if(not container_.empty()) {
                result.emplace(std::move(container_.back()));
                container_.pop_back();
            }
        }
        if(result.has_value()) {
            not_full_.notify_one();
        }
        return result;
    }

    /*************************************************************************/
    /**
     * Removes an item, blocking until one is available. If the queue is closed
     * and drained, returns std::nullopt to signal the end of the stream — so the
     * canonical consumer loop is `while (auto item = q.pop()) { ... }`.
     *
     * @return The item, or std::nullopt if the queue is closed and empty
     */
    [[nodiscard]] std::optional<T> pop() {
        std::optional<T> result;
        {
            std::unique_lock lock(mutex_);
            not_empty_.wait(lock, [this]() -> bool { return closed_ || not container_.empty(); });
            // Drain remaining items even after close; only report end-of-stream
            // once the queue is both closed and empty.
            if(not container_.empty()) {
                result.emplace(std::move(container_.back()));
                container_.pop_back();
            }
        }
        if(result.has_value()) {
            not_full_.notify_one();
        }
        return result;
    }

    /*************************************************************************/
    /**
     * Removes an item, blocking until one is available or the timeout elapses.
     * Returns std::nullopt on timeout, or if the queue is closed and empty.
     *
     * @tparam Rep The std::chrono::duration tick representation of the timeout
     * @tparam Period The std::chrono::duration period of the timeout
     * @param timeout Maximum time to wait for an item
     * @return The item, or std::nullopt on timeout / closed-and-empty
     */
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> pop_wait(std::chrono::duration<Rep, Period> const &timeout) {
        std::optional<T> result;
        {
            std::unique_lock lock(mutex_);
            if(not not_empty_.wait_for(
                   lock, timeout, [this]() -> bool { return closed_ || not container_.empty(); }
               )) {
                return std::nullopt; // timed out with no item
            }
            if(not container_.empty()) {
                result.emplace(std::move(container_.back()));
                container_.pop_back();
            }
        }
        if(result.has_value()) {
            not_full_.notify_one();
        }
        return result;
    }

    /*************************************************************************/
    /**
     * Closes the queue. Wakes every blocked producer and consumer: subsequent and
     * blocked producers return false, consumers drain the remaining items and then
     * see std::nullopt. Closing is terminal and idempotent.
     */
    void close() {
        {
            std::scoped_lock const lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /*************************************************************************/
    /** @brief Returns whether the queue has been closed. */
    [[nodiscard]] bool is_closed() const {
        std::scoped_lock const lock(mutex_);
        return closed_;
    }

    /*************************************************************************/
    /** @brief The maximum number of items (0 means unbounded). Compile-time. */
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return t_capacity;
    }

    /*************************************************************************/
    /** @brief Whether this is a bounded queue. Compile-time. */
    [[nodiscard]] static constexpr bool bounded() noexcept {
        return t_capacity > 0;
    }

    /*************************************************************************/
    /**
     * The current number of items. Only an indication in concurrent use, as the
     * size may change immediately after this returns.
     */
    [[nodiscard]] std::size_t size() const {
        std::scoped_lock const lock(mutex_);
        return container_.size();
    }

    /*************************************************************************/
    /**
     * The remaining space. Returns SIZE_MAX for an unbounded queue (the old
     * GBoundedBufferT underflowed here). Only an indication in concurrent use.
     */
    [[nodiscard]] std::size_t remaining_space() const {
        if constexpr(t_capacity == 0) {
            return (std::numeric_limits<std::size_t>::max)();
        } else {
            std::scoped_lock const lock(mutex_);
            return t_capacity - container_.size();
        }
    }

    /*************************************************************************/
    /** @brief Whether the queue is currently empty. Only an indication. */
    [[nodiscard]] bool empty() const {
        std::scoped_lock const lock(mutex_);
        return container_.empty();
    }

private:
    /*************************************************************************/
    /** @brief Whether the container is at capacity. Must be called under the lock. */
    [[nodiscard]] bool full_unlocked() const noexcept {
        if constexpr(t_capacity == 0) {
            return false; // unbounded: never full
        } else {
            return container_.size() >= t_capacity;
        }
    }

    /*************************************************************************/
    mutable std::mutex mutex_;            ///< Guards container_ and closed_
    std::condition_variable not_empty_;   ///< Signalled when an item becomes available
    std::condition_variable not_full_;    ///< Signalled when space becomes available
    container_type container_;            ///< The actual data store (FIFO: push front, pop back)
    bool closed_ = false;                 ///< Set by close(); terminal
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
