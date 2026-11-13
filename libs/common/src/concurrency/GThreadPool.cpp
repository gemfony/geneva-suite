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

#include "common/concurrency/GThreadPool.hpp"
#include "common/GCommonEnums.hpp"
#include "common/GLogger.hpp"
#include <mutex>
#include <shared_mutex>
#include <stop_token>
#include <utility>

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * @brief Initialisation with a number of threads.
 *
 * The worker threads are started eagerly (they then block on the empty task queue at ~0 CPU).
 *
 * @param n_threads The desired number of worker threads (0 -> hardware default)
 */
GThreadPool::GThreadPool(unsigned int n_threads)
  : n_threads_(n_threads > 0 ? n_threads : DEFAULTNHARDWARETHREADS) {
    if(0 == n_threads) {
        glogger << "In GThreadPool::GThreadPool(unsigned int n_threads):" << '\n'
                << "User requested n_threads == 0. n_threads was reset to the default "
                << DEFAULTNHARDWARETHREADS << '\n'
                << GWARNING;
    }
    task_queue_.emplace(); // construct the (unbounded) task queue
    start_workers(n_threads_);
}

/******************************************************************************/
/**
 * @brief Budgeted initialisation (see the declaration). Reserves the (normalized) thread count
 * in the process-wide GThreadBudget and starts the granted number of workers. The budget does
 * pure accounting: the grant always equals the request.
 *
 * @param source A short name identifying this pool in the budget (e.g. "oa:tp")
 * @param n_threads The number of worker threads the pool wants (0 picks a hardware-based default)
 * @param elasticity Whether the pool could correctly run with fewer workers than requested
 */
GThreadPool::GThreadPool(
    std::string_view source,
    unsigned int n_threads,
    ThreadElasticity elasticity
)
  : budget_reservation_(threadBudget().reserve(
        source,
        n_threads > 0 ? n_threads : DEFAULTNHARDWARETHREADS,
        elasticity
    ))
  , budget_source_(source)
  , budget_elasticity_(elasticity)
  , n_threads_(budget_reservation_.granted()) {
    task_queue_.emplace(); // construct the (unbounded) task queue
    start_workers(n_threads_);
}

/******************************************************************************/
/**
 * @brief The destructor.
 *
 * Closing the queue makes the workers drain any remaining tasks (running them, so their
 * futures are satisfied) and then exit on the now-empty queue; we then join them. No
 * exception may escape a destructor.
 */
GThreadPool::~GThreadPool() {
    try {
        // Shut the workers down through the cooperative stop_token: join_all() requests stop on
        // each worker jthread, whose stop_callback closes the task queue; the workers then drain
        // any remaining tasks (satisfying their futures) and exit. No exception may escape a
        // destructor.
        worker_group_.join_all();
        worker_group_.clearThreads();
    }
    catch(...) { // NOLINT(bugprone-empty-catch) — a destructor must not throw
    }
}

/******************************************************************************/
/**
 * @brief Starts n worker threads, each draining the current task queue.
 *
 * @param n The number of additional worker threads to create
 */
void GThreadPool::start_workers(unsigned int n) {
    worker_group_.create_threads([this](std::stop_token st) { this->worker_loop(std::move(st)); }, n);
}

/******************************************************************************/
/**
 * @brief Common submission path for async_schedule() and post().
 *
 * Bumps the in-flight counter and enqueues the (already type-erased) task while holding the
 * shared submission lock, so submissions run concurrently with each other but not while
 * wait()/setNThreads() drains the pool. On a closed queue the speculative increment is undone.
 *
 * @param task The type-erased work item to be executed by a worker thread (taken by value/moved)
 * @return true if the task was enqueued; false if the queue was closed (pool shutting down),
 *         in which case the in-flight counter is left unchanged
 */
bool GThreadPool::enqueue(std::move_only_function<void()> task) {
    std::shared_lock<std::shared_mutex> const sub_lck(submission_mutex_);
    {
        std::scoped_lock<std::mutex> const cnt_lck(counter_mutex_);
        ++tasks_in_flight_;
    }
    // task_queue_ is engaged from construction on; setNThreads() re-emplaces it under the
    // exclusive submission lock this function holds shared, so the deref cannot race a gap.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    if(task_queue_->push(std::move(task))) {
        return true;
    }
    // The queue is closed: undo the speculative increment.
    std::scoped_lock<std::mutex> const cnt_lck(counter_mutex_);
    if(0 == --tasks_in_flight_) {
        all_done_.notify_all();
    }
    return false;
}

/******************************************************************************/
/**
 * @brief The worker body (documented below at GThreadPool::worker_loop).
 *
 * pop() blocks at ~0 CPU on an empty queue, drains remaining tasks after close(), and
 * returns std::nullopt once the queue is closed and empty -- which is how a worker leaves
 * the loop and terminates.
 */
namespace {
// True while THIS thread is executing a task inside a GThreadPool worker loop. Thread-local, so it
// reflects only the current thread. Set around each task so nested work (e.g. a sub-optimizer started
// from within a task) can detect that it is already running on a pool worker.
thread_local bool t_in_worker_thread = false;
} // namespace

/**
 * @brief Reports whether the calling thread is currently executing inside a pool worker loop.
 *
 * Lets nested work (e.g. a sub-optimizer launched from within a task) detect that it is
 * already running on a pool worker, so it can avoid re-submitting to the same pool.
 *
 * @return true if the current thread is a GThreadPool worker running a task, false otherwise
 */
bool GThreadPool::inWorkerThread() noexcept {
    return t_in_worker_thread;
}

/**
 * @brief The worker thread body: pulls tasks off the queue and runs them until the queue closes.
 *
 * A stop request (e.g. from GThreadGroup::join_all()) closes the task queue via a stop_callback;
 * the loop then drains the remaining tasks and exits once the queue is closed and empty, so
 * pending futures are still satisfied (a graceful "drain and stop").
 *
 * @param st The cooperative stop token whose stop request closes the task queue
 */
void GThreadPool::worker_loop(const std::stop_token& st) {
    // A stop request (e.g. from GThreadGroup::join_all()) closes the task queue. The drain loop
    // below then finishes the remaining tasks and exits once the queue is closed and empty, so
    // pending tasks' futures are still satisfied -- request_stop() is a graceful "drain and
    // stop", not an abrupt abandon. (A stop_token cannot by itself wake a blocked pop(); closing
    // the queue can, which is why we route the stop through close().)
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- engaged for the workers' whole lifetime
    const std::stop_callback stop_cb(st, [this]() { task_queue_->close(); });

    t_in_worker_thread = true; // this thread spends its life running pool tasks

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- engaged for the workers' whole lifetime
    while(auto task = task_queue_->pop()) {
        // The task wrapper fulfils its own promise and never lets an exception
        // escape, so the in-flight bookkeeping below always runs.
        (*task)();

        std::scoped_lock<std::mutex> const cnt_lck(counter_mutex_);
        if(0 == --tasks_in_flight_) {
            all_done_.notify_all();
        }
    }
}

/******************************************************************************/
/**
 * @brief Blocks (under counter_mutex_) until no tasks are in flight.
 *
 * The caller must already hold submission_mutex_ exclusively, so no new tasks can be added
 * while this drains the pool.
 */
void GThreadPool::drain() {
    std::unique_lock<std::mutex> cnt_lck(counter_mutex_);
    all_done_.wait(cnt_lck, [this]() -> bool { return 0 == tasks_in_flight_; });
}

/******************************************************************************/
/**
 * @brief Blocks until all submitted tasks have been processed.
 *
 * Blocks new submissions for the duration. Must NOT be called from a task running inside
 * the pool (would deadlock).
 */
void GThreadPool::wait() {
    std::unique_lock<std::shared_mutex> const sub_lck(submission_mutex_);
    drain();
}

/******************************************************************************/
/**
 * @brief Retrieves the configured number of worker threads.
 *
 * Since workers start eagerly, this is also the live count of worker threads.
 *
 * @return The current number of worker threads in the pool
 */
unsigned int GThreadPool::getNThreads() const {
    return n_threads_;
}

/******************************************************************************/
/**
 * @brief Sets the number of worker threads, draining and reconfiguring the pool.
 *
 * Blocks new submissions and lets the pool run empty first. Growing simply adds workers to
 * the same queue; shrinking recreates the queue (its close() is terminal) and restarts the
 * worker set. Must NOT be called from a task running inside the pool (would deadlock).
 *
 * @param n_threads The desired number of worker threads (0 -> hardware default)
 */
void GThreadPool::setNThreads(unsigned int n_threads) {
    const unsigned int n = n_threads > 0 ? n_threads : DEFAULTNHARDWARETHREADS;

    std::unique_lock<std::shared_mutex> const sub_lck(submission_mutex_);
    if(n == n_threads_) {
        return;
    }
    drain();

    if(n > n_threads_) {
        // Grow: add workers; they pull from the same (still-open) queue.
        start_workers(n - n_threads_);
    }
    else {
        // Shrink: stop the current workers via the stop_token (join_all -> request_stop ->
        // stop_callback closes the queue -> drain -> exit), then recreate the queue (its close()
        // is terminal) and restart with the new worker count.
        worker_group_.join_all();
        worker_group_.clearThreads();
        task_queue_.emplace(); // fresh, open queue
        start_workers(n);
    }
    n_threads_ = n;

    // Keep the process-wide budget in step with the pool's actual size. A budgeted pool that is
    // resized must re-reserve, or the budget goes on counting the size the pool was CONSTRUCTED
    // with -- making reserved() and oversubscribed() wrong for the rest of the pool's life. The
    // old handle is released first so the two reservations never overlap in the ledger. An
    // unbudgeted pool (empty handle, granted() == 0) has nothing to keep in step.
    if(budget_reservation_.granted() != 0) {
        budget_reservation_.release();
        budget_reservation_ = threadBudget().reserve(budget_source_, n, budget_elasticity_);
    }
}

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
