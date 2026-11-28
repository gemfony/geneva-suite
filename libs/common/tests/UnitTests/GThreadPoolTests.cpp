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

// Standard headers go here
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

// Catch2 headers go here
#include <catch2/catch_test_macros.hpp>

// Geneva headers go here
#include "common/GCommonHelperFunctions.hpp"
#include "common/GExceptions.hpp"
#include "common/concurrency/GThreadBudget.hpp"
#include "common/concurrency/GThreadPool.hpp"

using Gem::Common::Concurrency::GThreadPool;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// A seam for one test: this executable replaces the global allocation functions, so that a
// thread can count its allocations and make one of them fail.

namespace {
/// The allocations of this thread so far (counted while counting() is true)
std::size_t &allocationsCounted() {
    thread_local std::size_t n = 0;
    return n;
}
/// Whether this thread counts its allocations
bool &counting() {
    thread_local bool on = false;
    return on;
}
/// The number of the counted allocation that fails; 0: none
std::size_t &failingAllocation() {
    thread_local std::size_t n = 0;
    return n;
}
} // namespace

void *operator new(std::size_t size) {
    if(counting() && ++allocationsCounted() == failingAllocation()) {
        throw std::bad_alloc();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    if(void *p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept {
    std::free(p); // NOLINT(cppcoreguidelines-no-malloc)
}
void operator delete(void *p, [[maybe_unused]] std::size_t size) noexcept {
    std::free(p); // NOLINT(cppcoreguidelines-no-malloc)
}

// NOTE: Catch2 assertion macros are NOT thread-safe, so callables running inside
// the pool never call REQUIRE/CHECK; they return values (checked via futures on
// the main thread) or update std::atomics (checked after wait()).

/******************************************************************************/
// Construction / configuration.

TEST_CASE("GThreadPool construction and getNThreads", "[common][thread-pool]") {
    GThreadPool const pool(4);
    // Workers start eagerly in the constructor, so getNThreads() reports the live
    // count right away (this is the modernised behaviour: the old Boost.Asio-based pool
    // created threads lazily on the first submission and reported 0 until then).
    REQUIRE(pool.getNThreads() == 4);

    GThreadPool const zero(0); // 0: the hardware threads assigned to the process
    REQUIRE(zero.getNThreads() == Gem::Common::getNHardwareThreads());
}

TEST_CASE(
    "GThreadPool asked for 0 threads starts the assigned hardware threads",
    "[common][thread-pool]"
) {
    // Regression: both constructors and setNThreads() started 2 threads for a request of 0
    // (DEFAULTNHARDWARETHREADS), while their comments promised the hardware default.
    GThreadPool const budgeted("test:zero", 0, Gem::Common::Concurrency::ThreadElasticity::Elastic);
    CHECK(budgeted.getNThreads() == Gem::Common::getNHardwareThreads());

    GThreadPool resized(3);
    resized.setNThreads(0);
    CHECK(resized.getNThreads() == Gem::Common::getNHardwareThreads());
}

TEST_CASE("GThreadPool is neither copyable nor movable", "[common][thread-pool]") {
    static_assert(not std::is_copy_constructible_v<GThreadPool>);
    static_assert(not std::is_copy_assignable_v<GThreadPool>);
    static_assert(not std::is_move_constructible_v<GThreadPool>);
    static_assert(not std::is_move_assignable_v<GThreadPool>);
    static_assert(not std::is_default_constructible_v<GThreadPool>);
    SUCCEED("compile-time type traits hold");
}

/******************************************************************************/
// async_schedule: non-void return type -> result carried by the future.

TEST_CASE(
    "GThreadPool non-void tasks return their result via the future",
    "[common][thread-pool]"
) {
    GThreadPool pool(4);
    auto f = pool.async_schedule([](int x) { return x * 2; }, 21);
    REQUIRE(f.get() == 42);

    // Several at once, with bound arguments forwarded.
    std::vector<std::future<int>> fs;
    for(int i = 0; i < 100; ++i) {
        fs.push_back(pool.async_schedule([](int a, int b) { return a + b; }, i, 1000));
    }
    for(int i = 0; i < 100; ++i) {
        REQUIRE(fs[static_cast<std::size_t>(i)].get() == i + 1000);
    }
}

/******************************************************************************/
// async_schedule: void return type -> future is satisfied, side effects run.

TEST_CASE("GThreadPool void tasks complete and the future is satisfied", "[common][thread-pool]") {
    GThreadPool pool(4);
    std::atomic<int> counter{0};
    auto f = pool.async_schedule([&counter]() { counter.fetch_add(1); });
    f.get(); // must not throw / hang (void specialisation sets the value)
    REQUIRE(counter.load() == 1);
}

/******************************************************************************/
// Exception handling: a throwing task surfaces through the returned future.

TEST_CASE("GThreadPool propagates task exceptions through the future", "[common][thread-pool]") {
    GThreadPool pool(4);
    SECTION("void task") {
        auto f = pool.async_schedule([]() { throw std::runtime_error("boom"); });
        REQUIRE_THROWS_AS(f.get(), std::runtime_error);
    }
    SECTION("non-void task") {
        auto f = pool.async_schedule([]() -> int { throw std::logic_error("nope"); });
        REQUIRE_THROWS_AS(f.get(), std::logic_error);
    }
    // The pool survives throwing tasks and keeps working.
    auto ok = pool.async_schedule([]() { return 7; });
    REQUIRE(ok.get() == 7);
}

/******************************************************************************/
// wait(): blocks until all submitted work has completed.

TEST_CASE("GThreadPool wait() drains all submitted work", "[common][thread-pool]") {
    GThreadPool pool(8);
    constexpr int N = 5000;
    std::atomic<int> done{0};
    for(int i = 0; i < N; ++i) {
        (void)pool.async_schedule([&done]() { done.fetch_add(1); });
    }
    pool.wait();
    REQUIRE(done.load() == N); // every task ran before wait() returned
}

TEST_CASE(
    "GThreadPool wait() is safe on an empty / already-drained pool",
    "[common][thread-pool]"
) {
    GThreadPool pool(2);
    pool.wait(); // nothing submitted -> returns immediately
    auto f = pool.async_schedule([]() { return 1; });
    REQUIRE(f.get() == 1);
    pool.wait(); // already drained via .get() -> returns immediately
    pool.wait(); // idempotent
    SUCCEED("repeated wait() calls did not hang");
}

TEST_CASE("GThreadPool genuinely waits for slow in-flight tasks", "[common][thread-pool]") {
    GThreadPool pool(2);
    std::atomic<int> done{0};
    for(int i = 0; i < 8; ++i) {
        (void)pool.async_schedule([&done]() {
            std::this_thread::sleep_for(5ms);
            done.fetch_add(1);
        });
    }
    pool.wait();
    REQUIRE(done.load() == 8); // wait() did not return early
}

/******************************************************************************/
// MPMC conservation: a large number of futures, each carrying a distinct value.

TEST_CASE("GThreadPool MPMC conservation (futures)", "[common][thread-pool]") {
    GThreadPool pool(8);
    constexpr int N = 20000;
    std::vector<std::future<long long>> fs;
    fs.reserve(N);
    for(int i = 0; i < N; ++i) {
        fs.push_back(pool.async_schedule([](long long k) { return k; }, static_cast<long long>(i)));
    }
    long long sum = 0;
    for(auto &f : fs)
        sum += f.get();
    REQUIRE(sum == static_cast<long long>(N) * (N - 1) / 2);
}

/******************************************************************************/
// Multi-producer submission: several threads submit concurrently (exercises the
// shared submission lock and the MPMC queue's producer side under contention).

TEST_CASE(
    "GThreadPool tolerates concurrent submission from many producers",
    "[common][thread-pool]"
) {
    GThreadPool pool(8);
    constexpr int PRODUCERS = 6;
    constexpr int PER_PRODUCER = 2000;
    constexpr int TOTAL = PRODUCERS * PER_PRODUCER;

    // Pre-sized vector with disjoint index ranges per producer: no future is ever
    // touched by more than one thread, so no extra synchronisation is needed.
    std::vector<std::future<long long>> fs(TOTAL);

    std::vector<std::thread> producers;
    producers.reserve(PRODUCERS);
    for(int p = 0; p < PRODUCERS; ++p) {
        producers.emplace_back([&pool, &fs, p]() {
            for(int i = 0; i < PER_PRODUCER; ++i) {
                const int idx = p * PER_PRODUCER + i;
                fs[static_cast<std::size_t>(idx)] =
                    pool.async_schedule([](long long k) { return k; }, static_cast<long long>(idx));
            }
        });
    }
    for(auto &t : producers)
        t.join();

    long long sum = 0;
    for(auto &f : fs)
        sum += f.get();
    REQUIRE(sum == static_cast<long long>(TOTAL) * (TOTAL - 1) / 2);
}

/******************************************************************************/
// setNThreads: grows and shrinks while staying functional. Mirrors the old
// behaviour the manual GThreadPoolTest exercised, but as a fast unit test.

TEST_CASE(
    "GThreadPool setNThreads grows and shrinks and stays functional",
    "[common][thread-pool]"
) {
    GThreadPool pool(2);
    REQUIRE(pool.getNThreads() == 2);

    auto run_batch = [&pool](int n) {
        std::atomic<int> done{0};
        for(int i = 0; i < n; ++i) {
            (void)pool.async_schedule([&done]() { done.fetch_add(1); });
        }
        pool.wait();
        return done.load();
    };

    REQUIRE(run_batch(1000) == 1000);

    pool.setNThreads(6); // grow
    REQUIRE(pool.getNThreads() == 6);
    REQUIRE(run_batch(1000) == 1000);

    pool.setNThreads(1); // shrink (recreates the queue + workers)
    REQUIRE(pool.getNThreads() == 1);
    REQUIRE(run_batch(1000) == 1000);

    pool.setNThreads(1); // no-op
    REQUIRE(pool.getNThreads() == 1);

    pool.setNThreads(0); // 0: the hardware threads assigned to the process
    REQUIRE(pool.getNThreads() == Gem::Common::getNHardwareThreads());
    REQUIRE(run_batch(1000) == 1000);
}

TEST_CASE("GThreadPool survives repeated resize-under-load cycles", "[common][thread-pool]") {
    // Condensed analogue of the manual stress test: submit a batch, resize, wait,
    // and confirm every task ran in every cycle.
    GThreadPool pool(2);
    constexpr int CYCLES = 12;
    constexpr int JOBS = 500;
    const unsigned int sizes[] = {4u, 1u, 8u, 3u};

    for(int c = 0; c < CYCLES; ++c) {
        std::atomic<int> done{0};
        for(int i = 0; i < JOBS; ++i) {
            (void)pool.async_schedule([&done]() { done.fetch_add(1); });
        }
        pool.setNThreads(sizes[static_cast<std::size_t>(c) % 4]); // drains, then resizes
        pool.wait();
        REQUIRE(done.load() == JOBS); // resize+wait flushed the whole batch
    }
}

/******************************************************************************/
// Clean shutdown: the destructor must drain pending work and join without hanging.

TEST_CASE("GThreadPool clean shutdown with pending work (no hang)", "[common][thread-pool]") {
    std::atomic<int> done{0};
    {
        GThreadPool pool(4);
        for(int i = 0; i < 2000; ++i) {
            (void)pool.async_schedule([&done]() { done.fetch_add(1); });
        }
        // No explicit wait(): the destructor must drain the queue (run all tasks)
        // and join cleanly.
    }
    REQUIRE(done.load() == 2000); // destructor drained everything
}

/******************************************************************************/
// post(): fire-and-forget submission (no future, exceptions logged-not-propagated).

TEST_CASE(
    "GThreadPool post() is fire-and-forget and survives throwing tasks",
    "[common][thread-pool]"
) {
    GThreadPool pool(4);
    std::atomic<int> done{0};
    constexpr int N = 3000;
    for(int i = 0; i < N; ++i) {
        pool.post([&done]() { done.fetch_add(1); }); // no future returned
    }
    // Throwing fire-and-forget tasks must be logged-and-ignored, not crash the pool.
    pool.post([]() { throw std::runtime_error("ignored on purpose"); });
    pool.post([]() { throw 42; }); // non-std exception

    pool.wait();               // post() tasks participate in wait()
    REQUIRE(done.load() == N); // all non-throwing tasks ran; throwers did not crash the pool

    // The pool remains usable after a thrown-and-logged task.
    auto f = pool.async_schedule([]() { return 7; });
    REQUIRE(f.get() == 7);
}

// ---------------------------------------------------------------------------
// Bulk fork/join: blocking_for_each

TEST_CASE(
    "GThreadPool::blocking_for_each runs the whole batch and blocks until done",
    "[common][thread-pool]"
) {
    GThreadPool pool(4);
    constexpr std::size_t N = 200;
    std::vector<int> data(N, 0);

    pool.blocking_for_each(std::span<int>(data), [](int &x) { x += 1; });
    // Every element was visited exactly once, and the call blocked until all tasks completed.
    bool all_one = true;
    for(int const v : data) {
        if(v != 1) {
            all_one = false;
        }
    }
    CHECK(all_one);

    // A per-item exception is swallowed: every task still runs, the join does not hang, and the pool
    // stays usable afterwards.
    std::atomic<int> ran{0};
    pool.blocking_for_each(std::span<int>(data), [&ran](int &) {
        ran.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("boom");
    });
    CHECK(ran.load() == static_cast<int>(N));

    auto f = pool.async_schedule([]() { return 5; });
    CHECK(f.get() == 5);

    // An empty span is a no-op.
    std::vector<int> empty;
    pool.blocking_for_each(std::span<int>(empty), [](int &x) { x += 1; });
    CHECK(empty.empty());
}

/******************************************************************************/
/**
 * Regression guard: a budgeted pool's reservation must follow setNThreads().
 *
 * budget_reservation_ is taken once in the budgeted constructor. setNThreads() used to resize
 * the worker set without touching it, so after pool.setNThreads(6) on a pool built with 2 the
 * process-wide ledger still counted 2 while 6 workers ran -- making reserved() and the
 * oversubscription warning derived from it provably wrong for the rest of the pool's life.
 *
 * The budget is a process-global shared by every other pool in the test binary, so this asserts
 * DELTAS against a baseline taken at entry, never absolute totals.
 */
TEST_CASE(
    "GThreadPool: a budgeted pool's reservation tracks setNThreads",
    "[common][concurrency][threadpool][budget]"
) {
    using Gem::Common::Concurrency::threadBudget;
    using Gem::Common::Concurrency::ThreadElasticity;

    const unsigned int baseline = threadBudget().reserved();

    {
        GThreadPool pool("test:resize", 2, ThreadElasticity::Fixed);
        CHECK(pool.getNThreads() == 2);
        CHECK(threadBudget().reserved() == baseline + 2);

        // Grow: the ledger must follow upwards...
        pool.setNThreads(6);
        CHECK(pool.getNThreads() == 6);
        CHECK(threadBudget().reserved() == baseline + 6);

        // ...and the pool must still work after the re-reservation.
        auto f = pool.async_schedule([]() { return 7; });
        CHECK(f.get() == 7);

        // Shrink: and downwards (this path recreates the queue, so it is worth its own check).
        pool.setNThreads(3);
        CHECK(pool.getNThreads() == 3);
        CHECK(threadBudget().reserved() == baseline + 3);

        // A no-op resize must not double-count.
        pool.setNThreads(3);
        CHECK(threadBudget().reserved() == baseline + 3);
    }

    // The handle is returned in full when the pool dies -- no leak, no residue.
    CHECK(threadBudget().reserved() == baseline);
}

/******************************************************************************/
/**
 * The unbudgeted constructor takes no reservation, so resizing such a pool must leave the
 * ledger completely untouched (an empty handle must not be turned into a live one).
 */
TEST_CASE(
    "GThreadPool: an unbudgeted pool never touches the thread budget",
    "[common][concurrency][threadpool][budget]"
) {
    using Gem::Common::Concurrency::threadBudget;

    const unsigned int baseline = threadBudget().reserved();

    {
        GThreadPool pool(2);
        CHECK(threadBudget().reserved() == baseline);

        pool.setNThreads(5);
        CHECK(pool.getNThreads() == 5);
        CHECK(threadBudget().reserved() == baseline);
    }

    CHECK(threadBudget().reserved() == baseline);
}

/******************************************************************************/
// Calls from a task of the same pool, and the pool's own-worker mark.

/**
 * Regression: blocking_for_each() from a task of the same pool waited for tasks that only the
 * pool's own workers can run. With both workers of a pool of 2 inside such a call, no worker was
 * left and both calls hung for good. They are refused now. The futures are waited for with a time
 * limit; on a hang the pool is leaked, since its stuck workers could never be joined.
 */
TEST_CASE(
    "GThreadPool::blocking_for_each from a task of the same pool is refused",
    "[common][thread-pool]"
) {
    auto pool = std::make_unique<GThreadPool>(2);
    std::atomic<int> nested_runs{0};
    auto nested = [&pool, &nested_runs]() -> bool {
        std::vector<int> data(4, 0);
        try {
            pool->blocking_for_each(std::span<int>(data), [&nested_runs](int &x) {
                x = 1;
                nested_runs.fetch_add(1);
            });
            return false;
        }
        catch(Gem::Common::gem_exception const &) {
            return true;
        }
    };
    auto f1 = pool->async_schedule(nested);
    auto f2 = pool->async_schedule(nested);
    const bool done = f1.wait_for(10s) == std::future_status::ready &&
                      f2.wait_for(10s) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        (void)pool.release(); // leaked on purpose: its stuck workers can never be joined
        return;
    }
    CHECK(f1.get());
    CHECK(f2.get());
    CHECK(nested_runs.load() == 0);
}

/**
 * Regression: wait() and setNThreads() from a task of the same pool waited for the task that
 * called them to end. Both are refused now (time limit and leak on a hang as above).
 */
TEST_CASE(
    "GThreadPool wait() and setNThreads() from a task of the same pool are refused",
    "[common][thread-pool]"
) {
    auto pool = std::make_unique<GThreadPool>(2);
    auto calls_wait = [&pool]() -> bool {
        try {
            pool->wait();
            return false;
        }
        catch(Gem::Common::gem_exception const &) {
            return true;
        }
    };
    auto calls_resize = [&pool]() -> bool {
        try {
            pool->setNThreads(3);
            return false;
        }
        catch(Gem::Common::gem_exception const &) {
            return true;
        }
    };
    auto f_wait = pool->async_schedule(calls_wait);
    auto f_resize = pool->async_schedule(calls_resize);
    const bool done = f_wait.wait_for(10s) == std::future_status::ready &&
                      f_resize.wait_for(10s) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        (void)pool.release(); // leaked on purpose: its stuck workers can never be joined
        return;
    }
    CHECK(f_wait.get());
    CHECK(f_resize.get());
    CHECK(pool->getNThreads() == 2);
}

TEST_CASE(
    "GThreadPool::inWorkerThread is true only on a worker running a task",
    "[common][thread-pool]"
) {
    CHECK_FALSE(GThreadPool::inWorkerThread());
    GThreadPool pool(2);
    auto f = pool.async_schedule([]() { return GThreadPool::inWorkerThread(); });
    CHECK(f.get());
    CHECK_FALSE(GThreadPool::inWorkerThread());
}

TEST_CASE(
    "GThreadPool::blocking_for_each from a task of another pool runs",
    "[common][thread-pool]"
) {
    GThreadPool outer(2);
    GThreadPool inner(2);
    auto f = outer.async_schedule([&inner]() {
        std::vector<int> data(8, 0);
        inner.blocking_for_each(std::span<int>(data), [](int &x) { x = 1; });
        int sum = 0;
        for(int const v : data) {
            sum += v;
        }
        return sum;
    });
    REQUIRE(f.wait_for(10s) == std::future_status::ready);
    CHECK(f.get() == 8);
}

/******************************************************************************/
// Submissions to a closing pool.

/**
 * Regression: when the pool was closing, blocking_for_each() dropped the refused tasks without
 * counting its latch down, and then waited for them for good. Now it waits for the queued ones
 * and throws. The pool is destroyed in a second thread while one worker is held in a task, so the
 * queue is closed while the pool still exists; the call runs in a thread of its own with a time
 * limit (detached on a hang, it owns all it uses).
 */
TEST_CASE(
    "GThreadPool submissions to a closing pool are refused without a hang",
    "[common][thread-pool]"
) {
    auto pool = std::make_unique<GThreadPool>(1);
    std::promise<void> gate;
    auto gate_f = gate.get_future().share();
    std::atomic<bool> held{false};
    pool->post([gate_f, &held]() {
        held.store(true);
        gate_f.wait();
    });
    while(not held.load()) {
        std::this_thread::yield();
    }

    GThreadPool *raw = pool.get();
    std::thread destroyer([&pool]() { pool.reset(); }); // closes the queue, then joins

    // Wait until the queue is closed: async_schedule() then reports it through the future
    bool closed = false;
    for(int i = 0; i < 5000 && not closed; ++i) {
        auto probe = raw->async_schedule([]() { return 0; });
        if(probe.wait_for(0s) == std::future_status::ready) {
            try {
                (void)probe.get();
            }
            catch(Gem::Common::gem_exception const &) {
                closed = true;
            }
        }
        if(not closed) {
            std::this_thread::sleep_for(1ms);
        }
    }
    REQUIRE(closed);

    // post() to the closed pool drops the task (with a warning): it never runs
    std::atomic<bool> posted_ran{false};
    raw->post([&posted_ran]() { posted_ran.store(true); });

    auto outcome = std::make_shared<std::promise<bool>>();
    auto outcome_f = outcome->get_future();
    std::thread caller([raw, outcome]() {
        std::vector<int> data(3, 0);
        try {
            raw->blocking_for_each(std::span<int>(data), [](int &x) { x = 1; });
            outcome->set_value(false);
        }
        catch(Gem::Common::gem_exception const &) {
            outcome->set_value(true);
        }
    });
    const bool returned = outcome_f.wait_for(10s) == std::future_status::ready;
    CHECK(returned);
    if(returned) {
        caller.join();
        CHECK(outcome_f.get()); // the refusal is reported to the caller
    }
    else {
        caller.detach(); // owns all it uses; hangs for good on the defect
    }

    gate.set_value();
    destroyer.join();
    CHECK_FALSE(posted_ran.load());
}

/******************************************************************************/
// Resizing while other threads submit, and a task that throws something else.

TEST_CASE(
    "GThreadPool setNThreads while other threads submit loses no task",
    "[common][thread-pool]"
) {
    GThreadPool pool(2);
    std::atomic<int> done{0};
    constexpr int PER_SUBMITTER = 500;
    std::vector<std::thread> submitters;
    for(int s = 0; s < 3; ++s) {
        submitters.emplace_back([&pool, &done]() {
            for(int i = 0; i < PER_SUBMITTER; ++i) {
                pool.post([&done]() { done.fetch_add(1); });
            }
        });
    }
    const unsigned int sizes[] = {4u, 1u, 3u, 2u};
    for(unsigned int const n : sizes) {
        pool.setNThreads(n);
    }
    for(auto &t : submitters) {
        t.join();
    }
    pool.wait();
    CHECK(done.load() == 3 * PER_SUBMITTER);
    CHECK(pool.getNThreads() == 2);
}

/**
 * Regression: getNThreads() read n_threads_ without a lock while setNThreads() wrote it, a data
 * race by the rules of the language (seen by ThreadSanitizer). The atomics of the test are
 * relaxed, so that they order nothing a race detector could rely on.
 */
TEST_CASE("GThreadPool getNThreads may be read while setNThreads runs", "[common][thread-pool]") {
    GThreadPool pool(2);
    std::atomic<bool> stop{false};
    std::atomic<bool> reading{false};
    std::atomic<unsigned int> seen_max{0};
    std::thread reader([&pool, &stop, &seen_max, &reading]() {
        while(not stop.load(std::memory_order_relaxed)) {
            const unsigned int n = pool.getNThreads();
            if(n > seen_max.load(std::memory_order_relaxed)) {
                seen_max.store(n, std::memory_order_relaxed);
            }
            reading.store(true, std::memory_order_relaxed);
        }
    });
    while(not reading.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }
    for(int i = 0; i < 20; ++i) {
        pool.setNThreads(1 + static_cast<unsigned int>(i % 4));
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();
    CHECK(pool.getNThreads() == 4);
    CHECK(seen_max.load() <= 4u);
}

TEST_CASE(
    "GThreadPool async_schedule carries an exception that is no std::exception",
    "[common][thread-pool]"
) {
    GThreadPool pool(2);
    auto f = pool.async_schedule([]() -> int { throw 42; });
    CHECK_THROWS_AS(f.get(), int);
    auto g = pool.async_schedule([]() { return 3; });
    CHECK(g.get() == 3);
}

/**
 * Regression: when a submission of blocking_for_each() threw (out of memory), the exception left
 * the call while the tasks already queued still ran on pointers into the caller's items and on
 * its per_item. Now they are waited for first. One allocation of the calling thread during the
 * submissions fails (the seam above); the number of the first allocation of the second element's
 * submission is taken from a run with one element.
 */
TEST_CASE(
    "GThreadPool::blocking_for_each waits for its queued tasks before a failed submission leaves",
    "[common][thread-pool]"
) {
    GThreadPool pool(2);
    std::atomic<int> started{0};
    std::atomic<int> finished{0};
    auto per_item = [&started, &finished](int &x) {
        started.fetch_add(1);
        std::this_thread::sleep_for(20ms);
        x = 1;
        finished.fetch_add(1);
    };

    std::vector<int> one(1, 0);
    allocationsCounted() = 0;
    counting() = true;
    pool.blocking_for_each(std::span<int>(one), per_item);
    counting() = false;
    const std::size_t for_one = allocationsCounted();

    std::vector<int> data(64, 0);
    allocationsCounted() = 0;
    failingAllocation() = for_one + 1;
    counting() = true;
    bool threw = false;
    int running_at_exit = -1;
    try {
        pool.blocking_for_each(std::span<int>(data), per_item);
    }
    catch(std::bad_alloc const &) {
        threw = true;
        running_at_exit = started.load() - finished.load();
    }
    counting() = false;
    failingAllocation() = 0;
    CHECK(threw);
    CHECK(running_at_exit == 0); // no task of the call ran on after it had left
    pool.wait();
}
