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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <semaphore>
#include <type_traits>
#include <utility>

// Geneva headers go here
#include "common/concurrency/GQueueCommon.hpp" // for the MPMCQueue concept (documentation/checking)

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * EXPERIMENTAL: a thread-safe, multi-producer / multi-consumer (MPMC) FIFO blocking queue backed by
 * a **preallocated ring buffer** with a genuinely **concurrent push/pop** core. It is
 * interface-identical to GBlockingMPMCQueueT (it satisfies the same MPMCQueue concept) and is meant
 * as an optionally selectable alternative via the GMPMCQueueT facade. It is OFF by default in
 * production (the facade's default backend is the deque-backed GBlockingMPMCQueueT); this one is
 * allowed to mature over time.
 *
 * Storage. The ring holds `Cap` raw slots allocated ONCE on the heap (not an inline std::array, so a
 * large `Cap` does not bloat the queue object or the stack). Each slot manages the lifetime of its
 * `T` by hand (placement-new on push, explicit destruction on pop / teardown), so the element type
 * needs no default constructor and move-only types such as std::unique_ptr are fully supported.
 *
 * Concurrency. Producers and consumers use SEPARATE locks -- a tail mutex for push, a head mutex for
 * pop -- so a push and a pop can proceed at the same time (unlike the single-mutex deque queue).
 * Counting is done with two semaphores rather than condition variables: `free_` counts empty slots
 * (a producer acquires one before writing) and `items_` counts filled slots (a consumer acquires one
 * before reading). Semaphores carry a count, so a release can never be "lost" the way a
 * condition-variable notification can if it races the waiter's unlock-and-block window -- which is
 * the bug that a naive two-lock-with-condition-variables design is prone to. By construction the
 * number of `items_` permits always equals the number of live items, so a successful `items_` acquire
 * guarantees there is an item to take (no separate emptiness re-check is needed on the hot path).
 *
 * Shutdown. close() simply sets a flag. A semaphore has no broadcast, so blocked producers/consumers
 * are woken by a short poll backstop: the blocking waits use try_acquire_for() and re-check the flag
 * on timeout. This keeps close() trivially correct and avoids injecting "fake" permits (which would
 * desynchronise the permit==item invariant). Work arrival still wakes a waiter IMMEDIATELY via the
 * normal semaphore release; the poll only fires while a thread would otherwise be blocked anyway
 * (queue genuinely full/empty AND no close yet), so its only cost is a few wake-ups per second on an
 * idle, blocked thread, and a close() is noticed within one poll interval. A wake-free shutdown
 * (waiter-count broadcast) is a possible future refinement; the public interface would not change.
 *
 * `Cap` is the fixed capacity and must be > 0 (this queue is bounded by construction; use the deque
 * backend for the unbounded case). Items are pushed at the tail and popped from the head (FIFO).
 *
 * @tparam T The element type stored in the queue (must be move-constructible)
 * @tparam Cap The fixed ring-buffer capacity in slots (must be > 0)
 */
template <typename T, std::size_t Cap>
class GPreallocatedMPMCQueueT final {
    static_assert(Cap > 0, "GPreallocatedMPMCQueueT<T, Cap>: Cap must be > 0 (this queue is bounded)");
    static_assert(
        std::is_move_constructible_v<T>,
        "GPreallocatedMPMCQueueT<T>: T must be move-constructible"
    );

    /*************************************************************************/
    /** @brief One ring slot. The contained T's lifetime is managed by hand (the union member is left
     *  uninitialised by the slot's constructor and only ever constructed/destroyed explicitly). */
    union Slot {
        T value_;
        Slot() noexcept {}  // leave the storage uninitialised
        ~Slot() noexcept {} // the owning queue destroys live elements explicitly
    };

    /// The semaphores never exceed Cap permits (free_ starts at Cap; items_ tops out at Cap), so Cap
    /// is a sufficient -- and tight -- LeastMaxValue.
    static constexpr std::ptrdiff_t sem_max_ = static_cast<std::ptrdiff_t>(Cap);

    /// How long a blocked producer/consumer sleeps before waking to re-check close(). Work arrival
    /// wakes it sooner (via a semaphore release); this only bounds how long a *full/empty* wait runs
    /// before noticing a shutdown.
    static constexpr std::chrono::milliseconds close_poll_{50};

public:
    using value_type = T;

    /*************************************************************************/
    /** @brief Default constructor; allocates the Cap-slot ring buffer on the heap. */
    GPreallocatedMPMCQueueT()
        : ring_(std::make_unique<Slot[]>(Cap))
    { /* nothing */ }

    // Owns mutexes, semaphores and the ring: neither copyable nor movable.
    GPreallocatedMPMCQueueT(GPreallocatedMPMCQueueT const &) = delete;
    GPreallocatedMPMCQueueT &operator=(GPreallocatedMPMCQueueT const &) = delete;
    GPreallocatedMPMCQueueT(GPreallocatedMPMCQueueT &&) = delete;
    GPreallocatedMPMCQueueT &operator=(GPreallocatedMPMCQueueT &&) = delete;

    /*************************************************************************/
    /**
     * The destructor closes the queue (waking any stragglers) and destroys every element still in the
     * ring. It assumes -- like every queue here -- that no other thread is still using the queue (the
     * canonical teardown is: close(), join the workers, then destroy). A destructor must not throw, so
     * any escaping exception is swallowed.
     */
    ~GPreallocatedMPMCQueueT() {
        try {
            close();
            std::scoped_lock const lock(head_mtx_, tail_mtx_);
            std::size_t idx = head_;
            const std::size_t n = count_.load(std::memory_order_relaxed);
            for(std::size_t k = 0; k < n; ++k) {
                ring_[idx].value_.~T();
                idx = next_index(idx);
            }
            count_.store(0, std::memory_order_relaxed);
        } catch(...) { // NOLINT(bugprone-empty-catch) — a destructor must not throw
        }
    }

    /*************************************************************************/
    /**
     * Tries to add an item without blocking. Returns false immediately if the queue is full or
     * closed. Covers both copy and move via perfect forwarding.
     *
     * @tparam U The forwarded argument type (must be usable to construct a T)
     * @param item The item to add (forwarded into a ring slot)
     * @return true if the item was added, false if the queue was full or closed
     */
    template <typename U>
        requires std::constructible_from<T, U &&>
    [[nodiscard]] bool try_push(U &&item) {
        if(closed_.load(std::memory_order_acquire)) {
            return false;
        }
        if(not free_.try_acquire()) {
            return false; // full
        }
        if(closed_.load(std::memory_order_acquire)) {
            free_.release(); // closed meanwhile -> hand the slot back and reject
            return false;
        }
        store_at_tail_(std::forward<U>(item));
        return true;
    }

    /*************************************************************************/
    /**
     * Adds an item, blocking until space is available. Returns false (without adding) if the queue is
     * closed while waiting or already closed.
     *
     * @tparam U The forwarded argument type (must be usable to construct a T)
     * @param item The item to add (forwarded into a ring slot)
     * @return true if the item was added, false if the queue was closed
     */
    template <typename U>
        requires std::constructible_from<T, U &&>
    [[nodiscard]] bool push(U &&item) {
        for(;;) {
            if(closed_.load(std::memory_order_acquire)) {
                return false;
            }
            if(free_.try_acquire_for(close_poll_)) {
                break; // got a free slot
            }
        }
        if(closed_.load(std::memory_order_acquire)) {
            free_.release();
            return false;
        }
        store_at_tail_(std::forward<U>(item));
        return true;
    }

    /*************************************************************************/
    /**
     * Adds an item, blocking until space is available or the timeout elapses. Returns false if it
     * timed out or the queue was closed.
     *
     * @tparam U The forwarded argument type (must be usable to construct a T)
     * @tparam Rep The std::chrono::duration tick representation of the timeout
     * @tparam Period The std::chrono::duration period of the timeout
     * @param item The item to add (forwarded into a ring slot)
     * @param timeout Maximum time to wait for a free slot
     * @return true if the item was added, false on timeout or close
     */
    template <typename U, typename Rep, typename Period>
        requires std::constructible_from<T, U &&>
    [[nodiscard]] bool push_wait(U &&item, std::chrono::duration<Rep, Period> const &timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for(;;) {
            if(closed_.load(std::memory_order_acquire)) {
                return false;
            }
            const auto now = std::chrono::steady_clock::now();
            if(now >= deadline) {
                return false; // timed out with no space
            }
            if(free_.try_acquire_for(poll_slice_(deadline - now))) {
                break;
            }
        }
        if(closed_.load(std::memory_order_acquire)) {
            free_.release();
            return false;
        }
        store_at_tail_(std::forward<U>(item));
        return true;
    }

    /*************************************************************************/
    /**
     * @brief Tries to remove an item without blocking.
     * @return The head item, or std::nullopt if the queue was empty
     */
    [[nodiscard]] std::optional<T> try_pop() {
        if(items_.try_acquire()) {
            return take_after_permit_();
        }
        return std::nullopt;
    }

    /*************************************************************************/
    /**
     * Removes an item, blocking until one is available. If the queue is closed and drained, returns
     * std::nullopt -- so the canonical consumer loop is `while (auto item = q.pop()) { ... }`.
     *
     * @return The head item, or std::nullopt if the queue is closed and empty
     */
    [[nodiscard]] std::optional<T> pop() {
        for(;;) {
            if(items_.try_acquire_for(close_poll_)) {
                return take_after_permit_();
            }
            if(closed_.load(std::memory_order_acquire)) {
                // Closed: drain anything that arrived right at close, else end-of-stream.
                if(items_.try_acquire()) {
                    return take_after_permit_();
                }
                return std::nullopt;
            }
        }
    }

    /*************************************************************************/
    /**
     * Removes an item, blocking until one is available or the timeout elapses. Returns std::nullopt on
     * timeout, or if the queue is closed and empty.
     *
     * @tparam Rep The std::chrono::duration tick representation of the timeout
     * @tparam Period The std::chrono::duration period of the timeout
     * @param timeout Maximum time to wait for an item
     * @return The head item, or std::nullopt on timeout or closed-and-empty
     */
    template <typename Rep, typename Period>
    [[nodiscard]] std::optional<T> pop_wait(std::chrono::duration<Rep, Period> const &timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for(;;) {
            if(closed_.load(std::memory_order_acquire)) {
                if(items_.try_acquire()) {
                    return take_after_permit_();
                }
                return std::nullopt;
            }
            const auto now = std::chrono::steady_clock::now();
            if(now >= deadline) {
                return std::nullopt; // timed out with no item
            }
            if(items_.try_acquire_for(poll_slice_(deadline - now))) {
                return take_after_permit_();
            }
        }
    }

    /*************************************************************************/
    /**
     * Closes the queue. Subsequent and blocked producers return false; consumers drain the remaining
     * items (each still has its items_ permit) and then see std::nullopt. Blocked waiters notice this
     * within one poll interval. Terminal and idempotent.
     */
    void close() {
        closed_.store(true, std::memory_order_release);
    }

    /*************************************************************************/
    /** @brief Returns whether the queue has been closed. */
    [[nodiscard]] bool is_closed() const {
        return closed_.load(std::memory_order_acquire);
    }

    /*************************************************************************/
    /** @brief The maximum number of items. Compile-time. */
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Cap;
    }

    /*************************************************************************/
    /** @brief Whether this is a bounded queue. Always true for the preallocated backend. */
    [[nodiscard]] static constexpr bool bounded() noexcept {
        return true;
    }

    /*************************************************************************/
    /** @brief The current number of items. Only an indication in concurrent use. */
    [[nodiscard]] std::size_t size() const {
        return count_.load(std::memory_order_relaxed);
    }

    /*************************************************************************/
    /** @brief The remaining space. Only an indication in concurrent use. */
    [[nodiscard]] std::size_t remaining_space() const {
        const std::size_t c = count_.load(std::memory_order_relaxed);
        return c >= Cap ? 0 : Cap - c;
    }

    /*************************************************************************/
    /** @brief Whether the queue is currently empty. Only an indication. */
    [[nodiscard]] bool empty() const {
        return count_.load(std::memory_order_relaxed) == 0;
    }

private:
    /*************************************************************************/
    /**
     * @brief The next ring index after @p i (wraps around at Cap).
     * @param i The current ring index
     * @return The successor index modulo Cap
     */
    [[nodiscard]] static constexpr std::size_t next_index(std::size_t i) noexcept {
        return (i + 1) % Cap;
    }

    /*************************************************************************/
    /**
     * @brief Clamps a remaining-time slice to the poll interval, so a blocking wait still wakes to
     *  re-check close() at least every close_poll_.
     * @tparam Duration The std::chrono::duration type of the remaining-time argument
     * @param remaining The time left until the caller's deadline
     * @return The smaller of @p remaining and close_poll_, expressed in nanoseconds
     */
    template <typename Duration>
    [[nodiscard]] static std::chrono::nanoseconds poll_slice_(Duration const &remaining) {
        const auto r = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining);
        const auto p = std::chrono::duration_cast<std::chrono::nanoseconds>(close_poll_);
        return (std::min)(r, p);
    }

    /*************************************************************************/
    /**
     * @brief Constructs an item into the tail slot (caller already holds a free_ permit), then
     *  publishes it by releasing an items_ permit.
     * @tparam U The forwarded argument type (used to construct a T)
     * @param item The item to construct into the tail slot (forwarded)
     */
    template <typename U>
    void store_at_tail_(U &&item) {
        {
            std::scoped_lock const lock(tail_mtx_);
            try {
                ::new(static_cast<void *>(std::addressof(ring_[tail_].value_)))
                    T(std::forward<U>(item));
            } catch(...) {
                // The element constructor threw, so the slot stays empty and nothing is published.
                // Give the reserved free_ permit back -- otherwise capacity would shrink by one for
                // the queue's lifetime -- and propagate, leaving the queue unchanged (the strong
                // guarantee the deque backend also offers). The items_ == live-items invariant holds:
                // no items_ permit was released.
                free_.release();
                throw;
            }
            tail_ = next_index(tail_);
        }
        count_.fetch_add(1, std::memory_order_relaxed); // observers only
        items_.release();                               // publish: synchronises with the consumer's acquire
    }

    /*************************************************************************/
    /**
     * @brief Moves out and destroys the head item. Precondition: the caller has already acquired one
     *  items_ permit, which guarantees a live item exists at the head (permits == live items).
     * @return The moved-out head item (always engaged given the precondition)
     */
    [[nodiscard]] std::optional<T> take_after_permit_() {
        std::optional<T> result;
        {
            std::scoped_lock const lock(head_mtx_);
            T &slot = ring_[head_].value_;
            result.emplace(std::move(slot));
            slot.~T();
            head_ = next_index(head_);
        }
        count_.fetch_sub(1, std::memory_order_relaxed); // observers only
        free_.release();                                // a slot is now free for a producer
        return result;
    }

    /*************************************************************************/
    std::unique_ptr<Slot[]> ring_; ///< The preallocated ring storage (Cap slots)

    std::mutex tail_mtx_;          ///< Serialises producers (guards tail_)
    std::mutex head_mtx_;          ///< Serialises consumers (guards head_)
    std::size_t tail_ = 0;        ///< Index of the next free slot to push into (under tail_mtx_)
    std::size_t head_ = 0;        ///< Index of the next item to pop (under head_mtx_)

    std::counting_semaphore<sem_max_> free_{static_cast<std::ptrdiff_t>(Cap)}; ///< Empty-slot permits
    std::counting_semaphore<sem_max_> items_{0};                              ///< Filled-slot permits

    std::atomic<std::size_t> count_{0}; ///< Live item count, for observers only (not correctness)
    std::atomic<bool> closed_{false};   ///< Set by close(); terminal
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
