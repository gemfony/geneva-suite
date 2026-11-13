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

/**
 * Shutdown / teardown stress tests for the GMPMCQueueT backends, with particular attention to the
 * (experimental) preallocated concurrent backend: close() racing many blocked or active threads,
 * the destructor draining leftover elements, and rapid create/use/close/destroy churn. Run against
 * BOTH backends so the deque backend's shutdown is re-validated alongside. These complement the
 * functional contract tests in GMPMCQueueConformanceTests.
 *
 * NOTE: Catch2 assertion macros are not thread-safe, so worker threads only ever record into atomics;
 * all REQUIRE/CHECK happen on the main thread after the joins.
 */

// Standard headers go here
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

// Catch2 headers go here
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>

// Geneva headers go here
#include "common/concurrency/GMPMCQueueT.hpp"

using Gem::Common::Concurrency::GMPMCQueueT;
using Gem::Common::Concurrency::QueueBackend;
using namespace std::chrono_literals;

/******************************************************************************/

namespace {

struct DequeTag {
    template <typename T, std::size_t Cap>
    using queue = GMPMCQueueT<T, Cap, QueueBackend::Deque>;
};
struct PreallocTag {
    template <typename T, std::size_t Cap>
    using queue = GMPMCQueueT<T, Cap, QueueBackend::Preallocated>;
};

// An element type that tracks how many instances are currently alive, so leaks or double-frees in
// the ring's manual element lifetime management are detected by the destructor-drain test.
struct Counted {
    static std::atomic<long long> alive;
    int v_ = 0;
    Counted() { alive.fetch_add(1); }
    explicit Counted(int v) : v_(v) { alive.fetch_add(1); }
    Counted(Counted const &o) : v_(o.v_) { alive.fetch_add(1); }
    Counted(Counted &&o) noexcept : v_(o.v_) { alive.fetch_add(1); }
    Counted &operator=(Counted const &o) { v_ = o.v_; return *this; }
    Counted &operator=(Counted &&o) noexcept { v_ = o.v_; return *this; }
    ~Counted() { alive.fetch_sub(1); }
};
std::atomic<long long> Counted::alive{0};

} // namespace

#define MPMC_BACKENDS DequeTag, PreallocTag

/******************************************************************************/
// close() wakes EVERY blocked producer (queue stays full; all report failure)

TEMPLATE_TEST_CASE("GMPMCQueueT shutdown: close wakes all blocked producers", "[GMPMCQueueT][shutdown]",
                   MPMC_BACKENDS) {
    constexpr int n_producers = 16;
    typename TestType::template queue<int, 4> q;
    for(int i = 0; i < 4; ++i) {
        REQUIRE(q.try_push(i)); // fill to capacity
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> producers;
    for(int p = 0; p < n_producers; ++p) {
        producers.emplace_back([&]() {
            if(not q.push(999)) { // blocks (full); must wake on close and return false
                failures.fetch_add(1);
            }
        });
    }

    std::this_thread::sleep_for(50ms); // let them all park
    q.close();
    for(auto &t : producers) t.join(); // must not hang

    REQUIRE(failures.load() == n_producers); // every blocked producer returned false
    REQUIRE(q.size() == 4);                  // nothing got added
}

/******************************************************************************/
// close() wakes EVERY blocked consumer (queue empty; all see end-of-stream)

TEMPLATE_TEST_CASE("GMPMCQueueT shutdown: close wakes all blocked consumers", "[GMPMCQueueT][shutdown]",
                   MPMC_BACKENDS) {
    constexpr int n_consumers = 16;
    typename TestType::template queue<int, 8> q;

    std::atomic<int> empties{0};
    std::vector<std::thread> consumers;
    for(int c = 0; c < n_consumers; ++c) {
        consumers.emplace_back([&]() {
            if(not q.pop().has_value()) { // blocks (empty); must wake on close with nullopt
                empties.fetch_add(1);
            }
        });
    }

    std::this_thread::sleep_for(50ms);
    q.close();
    for(auto &t : consumers) t.join(); // must not hang

    REQUIRE(empties.load() == n_consumers);
}

/******************************************************************************/
// close() mid-stream: every accepted item is consumed exactly once, nothing else is

TEMPLATE_TEST_CASE("GMPMCQueueT shutdown: mid-stream close conserves accepted items",
                   "[GMPMCQueueT][shutdown]", MPMC_BACKENDS) {
    constexpr int n_producers = 6;
    constexpr int n_consumers = 6;
    constexpr int per_producer = 5000;
    constexpr int total = n_producers * per_producer;

    typename TestType::template queue<int, 64> q; // small -> heavy backpressure

    std::vector<std::atomic<int>> accepted(total); // 1 if push() returned true for value v
    std::vector<std::atomic<int>> consumed(total); // number of times value v was popped
    for(int i = 0; i < total; ++i) {
        accepted[i].store(0);
        consumed[i].store(0);
    }

    std::vector<std::thread> consumers;
    for(int c = 0; c < n_consumers; ++c) {
        consumers.emplace_back([&]() {
            while(auto item = q.pop()) { // ends on closed + drained
                consumed[*item].fetch_add(1);
            }
        });
    }

    std::vector<std::thread> producers;
    for(int p = 0; p < n_producers; ++p) {
        producers.emplace_back([&, p]() {
            for(int i = 0; i < per_producer; ++i) {
                const int v = p * per_producer + i;
                if(q.push(v)) { // may return false once the closer fires
                    accepted[v].store(1);
                }
            }
        });
    }

    // Close while producers are still going, so some pushes are rejected.
    std::thread closer([&]() {
        std::this_thread::sleep_for(5ms);
        q.close();
    });

    for(auto &t : producers) t.join();
    closer.join();
    q.close(); // idempotent; ensure consumers can finish even if the timed close already fired
    for(auto &t : consumers) t.join(); // must not hang

    // Every accepted value was consumed exactly once; nothing un-accepted was consumed.
    long long accepted_count = 0;
    long long consumed_count = 0;
    bool ok = true;
    for(int v = 0; v < total; ++v) {
        const int a = accepted[v].load();
        const int c = consumed[v].load();
        accepted_count += a;
        consumed_count += c;
        if(c > 1) ok = false;       // never consumed twice
        if(c != a) ok = false;      // consumed iff accepted
    }
    REQUIRE(ok);
    REQUIRE(consumed_count == accepted_count);
    REQUIRE(q.empty());
}

/******************************************************************************/
// The destructor destroys leftover elements exactly once (no leak, no double-free)

TEMPLATE_TEST_CASE("GMPMCQueueT shutdown: destructor drains leftover elements", "[GMPMCQueueT][shutdown]",
                   MPMC_BACKENDS) {
    Counted::alive.store(0);
    {
        typename TestType::template queue<Counted, 16> q;
        for(int i = 0; i < 10; ++i) {
            REQUIRE(q.try_push(Counted{i})); // 10 live elements left in the queue
        }
        REQUIRE(Counted::alive.load() == 10);
        // pop a few so head_ != 0 and the leftover live region wraps from a non-zero head
        REQUIRE(q.try_pop().has_value());
        REQUIRE(q.try_pop().has_value());
        REQUIRE(Counted::alive.load() == 8);
        // q goes out of scope here with 8 live elements still inside
    }
    REQUIRE(Counted::alive.load() == 0); // destructor freed exactly the leftovers, no double-free
}

/******************************************************************************/
// Rapid create / use / close / destroy churn must never hang or corrupt

TEMPLATE_TEST_CASE("GMPMCQueueT shutdown: rapid lifecycle churn", "[GMPMCQueueT][shutdown]",
                   MPMC_BACKENDS) {
    Counted::alive.store(0);
    for(int round = 0; round < 200; ++round) {
        auto q = std::make_unique<typename TestType::template queue<Counted, 8>>();

        std::atomic<int> popped{0};
        std::thread consumer([&]() {
            while(auto item = q->pop()) {
                popped.fetch_add(1);
            }
        });

        for(int i = 0; i < 5; ++i) {
            (void)q->push(Counted{i});
        }
        q->close();
        consumer.join();
        q.reset(); // destroy
    }
    REQUIRE(Counted::alive.load() == 0);
}
