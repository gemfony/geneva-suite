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
 * Conformance tests for the GMPMCQueueT facade run against BOTH backends (the std::deque-backed
 * GBlockingMPMCQueueT and the preallocated-ring GPreallocatedMPMCQueueT). Every test below is a
 * TEMPLATE_TEST_CASE parametrised over a backend tag, so the identical behaviour is asserted for
 * each backend -- this is what guarantees the two are interchangeable. Only bounded scenarios are
 * exercised here because the preallocated backend is bounded by construction; the unbounded (Cap==0,
 * deque-only) behaviour is covered by GBlockingMPMCQueueTTests.
 */

// Standard headers go here
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
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

// A move-only payload that tracks a value and detects moved-from state.
struct MoveOnly {
    explicit MoveOnly(int v) : v_(v) {}
    MoveOnly(MoveOnly &&o) noexcept : v_(o.v_) { o.v_ = -1; }
    MoveOnly &operator=(MoveOnly &&o) noexcept {
        v_ = o.v_;
        o.v_ = -1;
        return *this;
    }
    MoveOnly(MoveOnly const &) = delete;
    MoveOnly &operator=(MoveOnly const &) = delete;
    int v_;
};

// A copyable AND movable payload that records, per object, whether it has ever been copy- or
// move-constructed/assigned, so a round-trip reveals whether the queue copied or moved it.
struct Tracked {
    Tracked() = default;
    explicit Tracked(int v) : v_(v) {}
    Tracked(Tracked const &o) : v_(o.v_), history_(o.history_) { history_.push_back('c'); }
    Tracked(Tracked &&o) noexcept : v_(o.v_), history_(std::move(o.history_)) {
        history_.push_back('m');
        o.v_ = -1;
    }
    Tracked &operator=(Tracked const &o) {
        v_ = o.v_;
        history_ = o.history_;
        history_.push_back('c');
        return *this;
    }
    Tracked &operator=(Tracked &&o) noexcept {
        v_ = o.v_;
        history_ = std::move(o.history_);
        history_.push_back('m');
        o.v_ = -1;
        return *this;
    }
    [[nodiscard]] bool was_copied() const {
        return std::find(history_.begin(), history_.end(), 'c') != history_.end();
    }
    [[nodiscard]] bool was_moved() const {
        return std::find(history_.begin(), history_.end(), 'm') != history_.end();
    }
    int v_ = -1;
    std::vector<char> history_;
};

// A payload whose COPY constructor can be made to throw (its move constructor never throws, so the
// queue can still move it out on pop). Used to check that a failed push leaves capacity intact.
struct Thrower {
    static std::atomic<int> copies;   // number of copy-constructions so far
    static std::atomic<int> throw_on; // throw on the copy whose 1-based index equals this (-1 = never)
    int v_ = 0;
    explicit Thrower(int v) : v_(v) {}
    Thrower(Thrower const &o) : v_(o.v_) {
        if(copies.fetch_add(1) + 1 == throw_on.load()) {
            throw std::runtime_error("Thrower copy boom");
        }
    }
    Thrower(Thrower &&o) noexcept : v_(o.v_) {}
    Thrower &operator=(Thrower const &) = default;
    Thrower &operator=(Thrower &&) noexcept = default;
};
std::atomic<int> Thrower::copies{0};
std::atomic<int> Thrower::throw_on{-1};

// Backend tags: each exposes an alias template selecting one concrete facade specialisation.
struct DequeTag {
    template <typename T, std::size_t Cap>
    using queue = GMPMCQueueT<T, Cap, QueueBackend::Deque>;
};
struct PreallocTag {
    template <typename T, std::size_t Cap>
    using queue = GMPMCQueueT<T, Cap, QueueBackend::Preallocated>;
};

} // namespace

#define MPMC_BACKENDS DequeTag, PreallocTag

/******************************************************************************/
// Compile-time observers + backend selection

TEMPLATE_TEST_CASE("GMPMCQueueT compile-time observers", "[GMPMCQueueT]", MPMC_BACKENDS) {
    using Q = typename TestType::template queue<int, 8>;
    STATIC_REQUIRE(Q::capacity() == 8);
    STATIC_REQUIRE(Q::bounded());
}

/******************************************************************************/
// Basic push/pop and FIFO ordering

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent linear push/pop/FIFO-order assertion sequence, macro-instantiated per backend by TEMPLATE_TEST_CASE; there is no seam within a single scenario to split on
TEMPLATE_TEST_CASE("GMPMCQueueT basic try_push/try_pop and FIFO order", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<int, 8> q;

    REQUIRE(q.empty());
    REQUIRE(q.size() == 0);
    REQUIRE_FALSE(q.try_pop().has_value());

    REQUIRE(q.try_push(10));
    REQUIRE(q.try_push(20));
    REQUIRE(q.try_push(30));
    REQUIRE(q.size() == 3);
    REQUIRE_FALSE(q.empty());

    auto a = q.try_pop();
    auto b = q.try_pop();
    auto c = q.try_pop();
    REQUIRE((a.has_value() && b.has_value() && c.has_value()));
    REQUIRE(*a == 10);
    REQUIRE(*b == 20);
    REQUIRE(*c == 30);
    REQUIRE(q.empty());
    REQUIRE_FALSE(q.try_pop().has_value());
}

/******************************************************************************/
// Bounded fullness behaviour (incl. wrap-around across the ring)

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent linear fill/drain/wrap-around assertion sequence, macro-instantiated per backend by TEMPLATE_TEST_CASE; there is no seam within a single scenario to split on
TEMPLATE_TEST_CASE("GMPMCQueueT bounded fullness and wrap-around", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<int, 3> q;
    REQUIRE(q.remaining_space() == 3);

    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE(q.try_push(3));
    REQUIRE(q.size() == 3);
    REQUIRE(q.remaining_space() == 0);
    REQUIRE_FALSE(q.try_push(4)); // full

    // Drain and refill repeatedly so the ring indices wrap around several times.
    int expected = 1;
    int next = 4;
    for(int round = 0; round < 10; ++round) {
        auto r = q.try_pop();
        REQUIRE(r.has_value());
        REQUIRE(*r == expected++);
        REQUIRE(q.try_push(next++));
        REQUIRE(q.size() == 3);
    }
}

/******************************************************************************/
// Timeout semantics

TEMPLATE_TEST_CASE("GMPMCQueueT push_wait / pop_wait timeouts", "[GMPMCQueueT]", MPMC_BACKENDS) {
    SECTION("push_wait times out when full") {
        typename TestType::template queue<int, 1> q;
        REQUIRE(q.try_push(1));
        auto t0 = std::chrono::steady_clock::now();
        REQUIRE_FALSE(q.push_wait(2, 30ms));
        REQUIRE(std::chrono::steady_clock::now() - t0 >= 25ms);
        REQUIRE(q.size() == 1);
    }
    SECTION("pop_wait times out when empty") {
        typename TestType::template queue<int, 4> q;
        auto t0 = std::chrono::steady_clock::now();
        REQUIRE_FALSE(q.pop_wait(30ms).has_value());
        REQUIRE(std::chrono::steady_clock::now() - t0 >= 25ms);
    }
    SECTION("pop_wait returns an available item promptly") {
        typename TestType::template queue<int, 4> q;
        REQUIRE(q.try_push(42));
        auto r = q.pop_wait(1s);
        REQUIRE(r.has_value());
        REQUIRE(*r == 42);
    }
}

/******************************************************************************/
// Blocking pop wakes when an item is pushed

TEMPLATE_TEST_CASE("GMPMCQueueT blocking pop wakes on push", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<int, 4> q;
    std::optional<int> popped;

    std::thread consumer([&]() { popped = q.pop(); });
    std::this_thread::sleep_for(30ms);
    REQUIRE(q.push(99));
    consumer.join();

    REQUIRE(popped.has_value());
    REQUIRE(*popped == 99);
}

/******************************************************************************/
// Blocking push wakes when space appears

TEMPLATE_TEST_CASE("GMPMCQueueT blocking push wakes on pop", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<int, 1> q;
    REQUIRE(q.try_push(1)); // full

    std::atomic<bool> pushed{false};
    std::thread producer([&]() {
        (void)q.push(2);
        pushed.store(true);
    });

    std::this_thread::sleep_for(30ms);
    REQUIRE_FALSE(pushed.load());

    auto first = q.try_pop();
    REQUIRE(first.has_value());
    REQUIRE(*first == 1);

    producer.join();
    REQUIRE(pushed.load());

    auto second = q.try_pop();
    REQUIRE(second.has_value());
    REQUIRE(*second == 2);
}

/******************************************************************************/
// close(): shutdown semantics

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent linear close/drain assertion sequence, macro-instantiated per backend by TEMPLATE_TEST_CASE; there is no seam within a single scenario to split on
TEMPLATE_TEST_CASE("GMPMCQueueT close rejects pushes and drains then ends", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<int, 8> q;
    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE_FALSE(q.is_closed());

    q.close();
    REQUIRE(q.is_closed());

    REQUIRE_FALSE(q.try_push(3));
    REQUIRE_FALSE(q.push(3));
    REQUIRE_FALSE(q.push_wait(3, 1ms));

    auto a = q.pop();
    auto b = q.pop();
    REQUIRE((a.has_value() && b.has_value()));
    REQUIRE(*a == 1);
    REQUIRE(*b == 2);
    REQUIRE_FALSE(q.pop().has_value());
    REQUIRE_FALSE(q.pop_wait(1ms).has_value());
    REQUIRE_FALSE(q.try_pop().has_value());
}

TEMPLATE_TEST_CASE("GMPMCQueueT close wakes a blocked consumer", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<int, 4> q;
    std::atomic<bool> returnedEmpty{false};

    std::thread consumer([&]() {
        auto r = q.pop();
        returnedEmpty.store(not r.has_value());
    });

    std::this_thread::sleep_for(30ms);
    q.close();
    consumer.join();
    REQUIRE(returnedEmpty.load());
}

TEMPLATE_TEST_CASE("GMPMCQueueT close wakes a blocked producer", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<int, 1> q;
    REQUIRE(q.try_push(1)); // full
    std::atomic<bool> pushFailed{false};

    std::thread producer([&]() {
        bool const ok = q.push(2);
        pushFailed.store(not ok);
    });

    std::this_thread::sleep_for(30ms);
    q.close();
    producer.join();
    REQUIRE(pushFailed.load());
}

TEMPLATE_TEST_CASE("GMPMCQueueT close is idempotent", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<int, 4> q;
    q.close();
    q.close();
    REQUIRE(q.is_closed());
}

/******************************************************************************/
// Move-only payloads (e.g. std::unique_ptr) round-trip

TEMPLATE_TEST_CASE("GMPMCQueueT supports move-only items", "[GMPMCQueueT]", MPMC_BACKENDS) {
    SECTION("custom move-only struct") {
        typename TestType::template queue<MoveOnly, 4> q;
        REQUIRE(q.try_push(MoveOnly{7}));
        auto r = q.try_pop();
        REQUIRE(r.has_value());
        REQUIRE(r->v_ == 7);
    }
    SECTION("std::unique_ptr") {
        typename TestType::template queue<std::unique_ptr<int>, 4> q;
        REQUIRE(q.push(std::make_unique<int>(123)));
        auto r = q.pop();
        REQUIRE(r.has_value());
        REQUIRE(*r != nullptr);
        REQUIRE(**r == 123);
    }
}

/******************************************************************************/
// Copyable payloads (e.g. std::shared_ptr): copy vs. move

TEMPLATE_TEST_CASE("GMPMCQueueT supports copyable items (copy and move)", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<std::shared_ptr<int>, 4> q;
    auto sp = std::make_shared<int>(5);

    REQUIRE(q.try_push(sp)); // lvalue -> copy
    REQUIRE(sp.use_count() == 2);

    auto sp2 = std::make_shared<int>(6);
    REQUIRE(q.try_push(std::move(sp2))); // rvalue -> move
    REQUIRE(sp2 == nullptr);

    auto a = q.try_pop();
    auto b = q.try_pop();
    REQUIRE((a.has_value() && b.has_value()));
    REQUIRE(**a == 5);
    REQUIRE(**b == 6);
}

/******************************************************************************/
// Copy-vs-move semantics: lvalue push copies, rvalue push moves, pop moves out

TEMPLATE_TEST_CASE("GMPMCQueueT copy vs. move semantics", "[GMPMCQueueT]", MPMC_BACKENDS) {
    typename TestType::template queue<Tracked, 8> q;

    SECTION("lvalue push -> copy into the queue; pop -> move out") {
        Tracked const t(5);
        REQUIRE(q.try_push(t));
        REQUIRE(t.v_ == 5);

        auto r = q.try_pop();
        REQUIRE(r.has_value());
        REQUIRE(r->v_ == 5);
        REQUIRE(r->was_copied());
        REQUIRE(r->was_moved());
    }

    SECTION("rvalue push -> move into the queue; source is moved-from") {
        Tracked t(6);
        REQUIRE(q.try_push(std::move(t)));
        REQUIRE(t.v_ == -1);

        auto r = q.try_pop();
        REQUIRE(r.has_value());
        REQUIRE(r->v_ == 6);
        REQUIRE_FALSE(r->was_copied());
        REQUIRE(r->was_moved());
    }
}

/******************************************************************************/
// High-volume single-thread FIFO integrity with ring wrap-around

TEMPLATE_TEST_CASE("GMPMCQueueT high-volume FIFO integrity", "[GMPMCQueueT]", MPMC_BACKENDS) {
    constexpr std::size_t Cap = 64;
    constexpr std::size_t N = 100000; // many wrap-arounds through the Cap-slot ring
    typename TestType::template queue<std::size_t, Cap> q;

    std::size_t next_in = 0;
    std::size_t next_out = 0;
    while(next_out < N) {
        while(next_in < N && q.try_push(next_in)) {
            ++next_in;
        }
        auto r = q.try_pop();
        REQUIRE(r.has_value());
        REQUIRE(*r == next_out); // FIFO preserved across wrap-around
        ++next_out;
    }
    REQUIRE(q.empty());
}

/******************************************************************************/
// A push whose element constructor throws must leave the queue unchanged (strong guarantee):
// in particular it must NOT silently consume capacity.

TEMPLATE_TEST_CASE("GMPMCQueueT a throwing element ctor preserves capacity", "[GMPMCQueueT]", MPMC_BACKENDS) {
    constexpr int Cap = 4;
    typename TestType::template queue<Thrower, Cap> q;

    Thrower::copies.store(0);
    Thrower::throw_on.store(1); // the very next copy throws

    Thrower const src(7);
    bool threw = false;
    try {
        (void)q.try_push(src); // lvalue -> copy -> throws inside the queue's placement-new
    } catch(const std::runtime_error &) {
        threw = true;
    }
    REQUIRE(threw);
    REQUIRE(q.empty()); // nothing was stored

    // The reserved slot must have been handed back: the queue must still accept a full Cap items.
    Thrower::throw_on.store(-1); // no more throwing
    int accepted = 0;
    for(int i = 0; i < Cap; ++i) {
        Thrower const item(i);
        if(q.try_push(item)) { // copies (no throw now)
            ++accepted;
        }
    }
    REQUIRE(accepted == Cap); // without the give-back, only Cap-1 would fit
    REQUIRE_FALSE(q.try_push(src)); // genuinely full now
}

/******************************************************************************/
// Multi-producer / multi-consumer conservation (correctness under concurrency)

TEMPLATE_TEST_CASE("GMPMCQueueT MPMC conservation", "[GMPMCQueueT]", MPMC_BACKENDS) {
    constexpr int n_producers = 4;
    constexpr int n_consumers = 4;
    constexpr int per_producer = 20000;
    constexpr long long expected_count = static_cast<long long>(n_producers) * per_producer;
    const long long expected_sum =
        static_cast<long long>(n_producers) *
        (static_cast<long long>(per_producer) * (per_producer - 1) / 2);

    typename TestType::template queue<int, 256> q; // bounded -> exercises producer backpressure

    std::atomic<long long> total_sum{0};
    std::atomic<long long> total_count{0};

    std::vector<std::thread> consumers;
    for(int c = 0; c < n_consumers; ++c) {
        consumers.emplace_back([&]() {
            long long local_sum = 0;
            long long local_count = 0;
            while(auto item = q.pop()) {
                local_sum += *item;
                ++local_count;
            }
            total_sum.fetch_add(local_sum);
            total_count.fetch_add(local_count);
        });
    }

    // Catch2 assertion macros are NOT thread-safe; workers record into atomics only.
    std::atomic<bool> all_pushes_ok{true};
    std::vector<std::thread> producers;
    for(int p = 0; p < n_producers; ++p) {
        producers.emplace_back([&]() {
            for(int i = 0; i < per_producer; ++i) {
                if(not q.push(i)) {
                    all_pushes_ok.store(false);
                }
            }
        });
    }

    for(auto &t : producers) t.join();
    REQUIRE(all_pushes_ok.load());
    q.close();
    for(auto &t : consumers) t.join();

    REQUIRE(total_count.load() == expected_count);
    REQUIRE(total_sum.load() == expected_sum);
    REQUIRE(q.empty());
}
