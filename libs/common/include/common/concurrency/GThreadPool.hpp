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

// Standard header files go here
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <shared_mutex>
#include <span>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>

// Geneva header files go here
#include "common/concurrency/GBlockingMPMCQueueT.hpp"
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "common/concurrency/GThreadBudget.hpp"
#include "common/concurrency/GThreadGroup.hpp"

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * A thread pool built entirely from Geneva's own primitives -- a GThreadGroup of
 * worker threads (std::jthread) draining a shared, blocking, multi-producer/
 * multi-consumer task queue (GBlockingMPMCQueueT). It carries no third-party
 * dependency (in particular no Boost.ASIO io_context / work guard) and uses the
 * queue's terminal close() for clean, deadlock-free shutdown -- workers block at
 * ~0 CPU on an empty queue and wake the instant work is submitted or the pool is
 * shut down.
 *
 * Submit work with async_schedule(), which returns a std::future carrying the
 * task's result and any exception it threw; one if-constexpr template body handles
 * both void and non-void return types. When the outcome need not be observed, the
 * fire-and-forget post() schedules a task without allocating a promise/future.
 * wait() blocks until the pool has run empty. Workers are started eagerly in the
 * constructor, so getNThreads() always reports the live worker count.
 *
 * Note: tasks are type-erased through std::move_only_function, so the submitted
 * callable plus its bound arguments only need to be movable, not copyable. Geneva's
 * usage (closures capturing std::shared_ptr) satisfies this. The single-owner task
 * type expresses that a queued task is never copied, only moved to the worker that
 * runs it.
 *
 * This class is neither copyable nor movable (it owns threads, a queue, mutexes
 * and condition variables). wait() / setNThreads() must NOT be called from inside
 * a task running in the pool (and a task must not submit further work while another
 * thread is inside wait()): both would deadlock.
 */
class GThreadPool final {
public:
    /***************************************************************************/
    /** @brief Deleted default constructor -- a thread count must be supplied */
    GThreadPool() = delete;
    /**
     * @brief Initialisation with a number of threads (0 -> hardware default)
     * @param n_threads The number of worker threads to start (0 picks a hardware-based default)
     */
    explicit GThreadPool(unsigned int n_threads);
    /**
     * @brief Budgeted initialisation: reserves the requested thread count in the process-wide
     * GThreadBudget under the given source name (holding the reservation for the pool's life)
     * and starts the granted number of workers. The plain constructor above stays unbudgeted,
     * so leaf tests/benchmarks are unaffected.
     *
     * @param source A short name identifying this pool in the budget (e.g. "oa:tp")
     * @param n_threads The number of worker threads the pool wants (0 picks a hardware-based default)
     * @param elasticity Whether the pool could correctly run with fewer workers than requested
     */
    GThreadPool(std::string_view source, unsigned int n_threads, ThreadElasticity elasticity);
    /** @brief The destructor drains the queue and joins all workers */
    ~GThreadPool();

    // Neither copyable nor movable (owns threads, a queue, mutexes and CVs).
    GThreadPool(GThreadPool const &) = delete;
    GThreadPool &operator=(GThreadPool const &) = delete;
    GThreadPool(GThreadPool &&) = delete;
    GThreadPool &operator=(GThreadPool &&) = delete;

    /**
     * @brief Sets the number of worker threads (lets the pool run empty first)
     * @param n_threads The new number of worker threads (0 picks a hardware-based default)
     */
    void setNThreads(unsigned int n_threads);
    /**
     * @brief Retrieves the current number of worker threads
     * @return The live worker-thread count
     */
    [[nodiscard]] unsigned int getNThreads() const;

    /** @brief Blocks until all submitted tasks have been processed */
    void wait();

    /***************************************************************************/
    /**
     * @brief True iff the calling thread is currently executing a task on SOME GThreadPool worker.
     *
     * Lets nested work avoid spawning a second pool on a thread that is already a pool worker (which
     * multiplies threads without adding parallelism -- the outer pool already parallelises across the
     * outer work items). The flag is thread-local, so it reflects only the current thread's state.
     */
    [[nodiscard]] static bool inWorkerThread() noexcept;

    /***************************************************************************/
    /**
     * Submits a task to the pool and returns immediately, before the task runs.
     * One overload (selected via if constexpr) handles both void and non-void
     * return types. The returned std::future carries the task's result and any
     * exception it threw.
     *
     * @tparam F The callable type to be invoked on a worker thread
     * @tparam Args The argument types forwarded to and bound into the task
     * @param f The callable to be executed by a worker thread
     * @param args Arguments forwarded to and bound into the task
     * @return A std::future holding the result (and/or exception) of f(args...)
     */
    template <typename F, typename... Args>
        requires std::invocable<F, Args...>
    auto async_schedule(F &&f, Args &&...args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using result_type = std::invoke_result_t<F, Args...>;

        auto promise_ptr = std::make_shared<std::promise<result_type>>();
        std::future<result_type> result = promise_ptr->get_future();

        // The task: bind f + its arguments by value, fulfil the promise on
        // completion (or store the exception). One body for both void and
        // non-void via if constexpr.
        auto task = [promise_ptr,
                     f = std::forward<F>(f),
                     ... args = std::forward<Args>(args)]() mutable {
            try {
                if constexpr(std::is_void_v<result_type>) {
                    f(std::move(args)...);
                    promise_ptr->set_value();
                }
                else {
                    promise_ptr->set_value(f(std::move(args)...));
                }
            }
            catch(...) {
                try {
                    promise_ptr->set_exception(std::current_exception());
                }
                catch(...) { // set_exception() itself may throw -- nothing safe remains
                    glogger << "In GThreadPool::async_schedule(): promise.set_exception() threw."
                            << '\n'
                            << "We cannot continue" << '\n'
                            << GTERMINATION;
                }
            }
        };

        // Submitters take a SHARED lock; wait()/setNThreads() take it exclusively,
        // so submissions run concurrently except while the pool is being drained.
        if(not enqueue(std::move_only_function<void()>(std::move(task)))) {
            // The queue is closed (pool shutting down): surface the failure through
            // the future rather than losing it silently.
            promise_ptr->set_exception(std::make_exception_ptr(geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GThreadPool::async_schedule(): submission after the pool was closed" << '\n'
            )));
        }
        return result;
    }

    /***************************************************************************/
    /**
     * Fire-and-forget submission: schedules the task but returns no future and
     * allocates no promise. As there is no result channel, an exception thrown by
     * the task is caught and logged -- never propagated, which would terminate the
     * worker thread. Use this for tasks whose result and success the caller does
     * not need to observe; it still participates in wait().
     *
     * @tparam F The callable type to be invoked on a worker thread
     * @tparam Args The argument types forwarded to and bound into the task
     * @param f The callable to be executed by a worker thread
     * @param args Arguments forwarded to and bound into the task
     */
    template <typename F, typename... Args>
        requires std::invocable<F, Args...>
    void post(F &&f, Args &&...args) {
        auto task = [f = std::forward<F>(f),
                     ... args = std::forward<Args>(args)]() mutable {
            try {
                (void) f(std::move(args)...); // any return value is discarded
            }
            catch(std::exception const &e) {
                glogger << "In GThreadPool::post(): a fire-and-forget task threw:" << '\n'
                        << e.what() << '\n'
                        << "No future to carry it -- logged and ignored." << '\n'
                        << GWARNING;
            }
            catch(...) {
                glogger << "In GThreadPool::post(): a fire-and-forget task threw a"
                        << " non-std exception." << '\n'
                        << "No future to carry it -- logged and ignored." << '\n'
                        << GWARNING;
            }
        };
        if(not enqueue(std::move_only_function<void()>(std::move(task)))) {
            glogger << "In GThreadPool::post(): submission after the pool was closed;"
                    << " task dropped." << '\n'
                    << GWARNING;
        }
    }

    /***************************************************************************/
    /**
     * @brief Runs @p per_item over every element of @p items on the pool and blocks until all have
     * finished (a bulk fork/join).
     *
     * One task per element is submitted (fire-and-forget), and a private @c GCompletionLatchT sized to the
     * batch is used to wait for exactly this batch -- not the whole pool -- so it is safe when the pool is
     * shared (e.g. several algorithms submitting concurrently). @p per_item is invoked CONCURRENTLY, once
     * per element, so it must be safe to run on distinct elements in parallel (evaluating distinct work
     * items is the intended use). An exception thrown by @p per_item is swallowed (the element is expected
     * to record its own failure state, as a work item's processing status does); the latch is still
     * counted down so the join never hangs. A single-thread pool degenerates to sequential execution in
     * submission order.
     *
     * @tparam T The element type of the span (e.g. a work-item unique_ptr)
     * @tparam Fn A callable invoked as @c per_item(T&) for each element
     * @param items The batch to run; its backing storage must outlive the call (this blocks until done)
     * @param per_item The work to run on each element (invoked concurrently)
     */
    template <typename T, typename Fn>
    void blocking_for_each(std::span<T> items, const Fn &per_item) {
        if(items.empty()) {
            return;
        }
        auto latch = std::make_shared<GCompletionLatchT>(items.size());
        for(std::size_t i = 0; i < items.size(); ++i) {
            T *elem = &items[i]; // stable pointer into the caller's span (which outlives this blocking call)
            this->post([elem, &per_item, latch]() {
                try {
                    per_item(*elem);
                }
                // NOLINTNEXTLINE(bugprone-empty-catch) -- deliberate: the element records its own failure
                catch(...) { /* swallow so the join never hangs */
                }
                latch->count_down();
            });
        }
        latch->wait();
    }

private:
    /***************************************************************************/
    /**
     * @brief Common submission path for async_schedule()/post(). Increments the
     * in-flight counter and enqueues the (already type-erased) task under a shared
     * submission lock. Returns false if the queue was closed (pool shutting down),
     * having left the counter unchanged.
     *
     * @param task The type-erased task to enqueue (moved into the queue)
     * @return true if the task was enqueued, false if the queue was already closed
     */
    bool enqueue(std::move_only_function<void()> task);

    /***************************************************************************/
    /** @brief Worker body: drains the queue until it is closed and empty. Observes the
     *  jthread's stop_token: a stop request closes the task queue, so the worker still drains
     *  the remaining tasks (their futures complete) and then exits -- request_stop() is thus a
     *  graceful "drain and stop", never an abrupt abandon.
     *  @param st The jthread stop token; a stop request closes the task queue so the worker drains
     *   the remaining tasks and then exits */
    void worker_loop(const std::stop_token& st);
    /**
     * @brief Starts n worker threads draining the (current) queue
     * @param n The number of worker threads to start
     */
    void start_workers(unsigned int n);
    /** @brief Blocks (under counter_mutex_) until no tasks are in flight */
    void drain();

    /***************************************************************************/
    /// The pool's reservation in the process-wide thread budget (empty for the unbudgeted
    /// constructor). Declared FIRST so it is destroyed LAST -- the reservation must only be
    /// returned to the budget after the workers below have been joined.
    GThreadBudget::Reservation budget_reservation_;

    /// The reservation's source name and elasticity, retained from the budgeted constructor so
    /// setNThreads() can re-reserve at the new worker count. Without them the budget would keep
    /// counting the pool's ORIGINAL size for the rest of its life, and the oversubscription
    /// warning would be computed from a stale total.
    std::string budget_source_;
    ThreadElasticity budget_elasticity_ = ThreadElasticity::Fixed;

    // The task queue is held in an optional so setNThreads() can replace it (the
    // queue's close() is terminal). It is unbounded (capacity 0): submission never
    // blocks on fullness. Always engaged after construction.
    std::optional<GBlockingMPMCQueueT<std::move_only_function<void()>, 0>> task_queue_;

    GThreadGroup worker_group_; ///< Holds the worker threads (std::jthread)

    mutable std::mutex counter_mutex_;       ///< Guards tasks_in_flight_
    std::condition_variable all_done_;       ///< Signalled when tasks_in_flight_ hits 0
    std::size_t tasks_in_flight_ = 0;        ///< Submitted-but-not-yet-completed tasks

    /// Submitters hold this shared; wait()/setNThreads() hold it exclusively, so
    /// the pool can be quiesced (no new submissions) while it drains.
    mutable std::shared_mutex submission_mutex_;

    unsigned int n_threads_; ///< The configured / live number of worker threads
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
