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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

// Catch2 headers go here
#include <catch2/catch_test_macros.hpp>

// Geneva headers go here
#include "common/concurrency/GBlockingMPMCQueueT.hpp"

using Gem::Common::Concurrency::GBlockingMPMCQueueT;
using Gem::Common::DEFAULTBUFFERSIZE;
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

// A copyable AND movable payload that records, per object, whether it has ever
// been copy- or move-constructed/assigned. The history travels with the object
// (like GBoundedBufferTTests' copy_move_struct), so after a round-trip we can tell
// whether the queue copied or moved it.
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

} // namespace

// NOTE on coverage vs. the older GBoundedBufferT tests: GBlockingMPMCQueueT requires its
// element type to be move-constructible (static_assert), so the artificial
// "copyable but move-deleted" type that GBoundedBufferTTests exercised is
// intentionally NOT supported — pop() moves elements out (into std::optional), which
// such a type forbids. Every other aspect the old tests covered (multi-capacity
// construction + observers, high-volume per-element FIFO integrity, copy-vs-move
// semantics, moved-from sources, bounded/unbounded) is covered below.

/******************************************************************************/
// Compile-time observers

TEST_CASE("GBlockingMPMCQueueT compile-time observers", "[GBlockingMPMCQueueT]") {
    SECTION("bounded") {
        using Q = GBlockingMPMCQueueT<int, 8>;
        STATIC_REQUIRE(Q::capacity() == 8);
        STATIC_REQUIRE(Q::bounded());
    }
    SECTION("unbounded") {
        using Q = GBlockingMPMCQueueT<int, 0>;
        STATIC_REQUIRE(Q::capacity() == 0);
        STATIC_REQUIRE_FALSE(Q::bounded());
    }
}

/******************************************************************************/
// Basic push/pop and FIFO ordering

TEST_CASE("GBlockingMPMCQueueT basic try_push/try_pop and FIFO order", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<int, 8> q;

    REQUIRE(q.empty());
    REQUIRE(q.size() == 0);
    REQUIRE_FALSE(q.try_pop().has_value());

    REQUIRE(q.try_push(10));
    REQUIRE(q.try_push(20));
    REQUIRE(q.try_push(30));
    REQUIRE(q.size() == 3);
    REQUIRE_FALSE(q.empty());

    // FIFO: items come out in the order they went in
    auto a = q.try_pop();
    auto b = q.try_pop();
    auto c = q.try_pop();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    REQUIRE(*a == 10);
    REQUIRE(*b == 20);
    REQUIRE(*c == 30);
    REQUIRE(q.empty());
    REQUIRE_FALSE(q.try_pop().has_value());
}

/******************************************************************************/
// Bounded fullness behaviour

TEST_CASE("GBlockingMPMCQueueT bounded fullness", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<int, 3> q;
    REQUIRE(q.remaining_space() == 3);

    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE(q.try_push(3));
    REQUIRE(q.size() == 3);
    REQUIRE(q.remaining_space() == 0);

    // Full: try_push must fail without blocking
    REQUIRE_FALSE(q.try_push(4));
    REQUIRE(q.size() == 3);

    // Make room and push again
    REQUIRE(q.try_pop().has_value());
    REQUIRE(q.remaining_space() == 1);
    REQUIRE(q.try_push(4));
    REQUIRE(q.remaining_space() == 0);
}

/******************************************************************************/
// Unbounded behaviour (the remaining_space underflow fix)

TEST_CASE("GBlockingMPMCQueueT unbounded behaviour", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<int, 0> q;

    // The old GBoundedBufferT underflowed here; it must report SIZE_MAX.
    REQUIRE(q.remaining_space() == (std::numeric_limits<std::size_t>::max)());

    for(int i = 0; i < 1000; ++i) {
        REQUIRE(q.try_push(i)); // never full
    }
    REQUIRE(q.size() == 1000);
    REQUIRE(q.remaining_space() == (std::numeric_limits<std::size_t>::max)());

    // push() never blocks on an unbounded queue
    REQUIRE(q.push(1234));
    REQUIRE(q.push_wait(5678, 1ms));
    REQUIRE(q.size() == 1002);
}

/******************************************************************************/
// Timeout semantics

TEST_CASE("GBlockingMPMCQueueT push_wait / pop_wait timeouts", "[GBlockingMPMCQueueT]") {
    SECTION("push_wait times out when full") {
        GBlockingMPMCQueueT<int, 1> q;
        REQUIRE(q.try_push(1));
        auto t0 = std::chrono::steady_clock::now();
        REQUIRE_FALSE(q.push_wait(2, 30ms));
        REQUIRE(std::chrono::steady_clock::now() - t0 >= 25ms);
        REQUIRE(q.size() == 1);
    }
    SECTION("pop_wait times out when empty") {
        GBlockingMPMCQueueT<int, 4> q;
        auto t0 = std::chrono::steady_clock::now();
        REQUIRE_FALSE(q.pop_wait(30ms).has_value());
        REQUIRE(std::chrono::steady_clock::now() - t0 >= 25ms);
    }
    SECTION("pop_wait returns an available item promptly") {
        GBlockingMPMCQueueT<int, 4> q;
        REQUIRE(q.try_push(42));
        auto r = q.pop_wait(1s);
        REQUIRE(r.has_value());
        REQUIRE(*r == 42);
    }
}

/******************************************************************************/
// Blocking pop wakes when an item is pushed

TEST_CASE("GBlockingMPMCQueueT blocking pop wakes on push", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<int, 4> q;
    std::optional<int> popped;

    std::thread consumer([&]() { popped = q.pop(); });
    std::this_thread::sleep_for(30ms); // let the consumer block
    REQUIRE(q.push(99));
    consumer.join();

    REQUIRE(popped.has_value());
    REQUIRE(*popped == 99);
}

/******************************************************************************/
// Blocking push wakes when space appears

TEST_CASE("GBlockingMPMCQueueT blocking push wakes on pop", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<int, 1> q;
    REQUIRE(q.try_push(1)); // queue now full

    std::atomic<bool> pushed{false};
    std::thread producer([&]() {
        (void)q.push(2); // blocks until the main thread pops
        pushed.store(true);
    });

    std::this_thread::sleep_for(30ms);
    REQUIRE_FALSE(pushed.load()); // still blocked

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

TEST_CASE("GBlockingMPMCQueueT close rejects pushes and drains then ends", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<int, 8> q;
    REQUIRE(q.try_push(1));
    REQUIRE(q.try_push(2));
    REQUIRE_FALSE(q.is_closed());

    q.close();
    REQUIRE(q.is_closed());

    // No new items accepted
    REQUIRE_FALSE(q.try_push(3));
    REQUIRE_FALSE(q.push(3));
    REQUIRE_FALSE(q.push_wait(3, 1ms));

    // Remaining items still drain in FIFO order, then end-of-stream
    auto a = q.pop();
    auto b = q.pop();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(*a == 1);
    REQUIRE(*b == 2);
    REQUIRE_FALSE(q.pop().has_value());        // closed + empty -> nullopt
    REQUIRE_FALSE(q.pop_wait(1ms).has_value());
    REQUIRE_FALSE(q.try_pop().has_value());
}

TEST_CASE("GBlockingMPMCQueueT close wakes a blocked consumer", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<int, 4> q;
    std::atomic<bool> returnedEmpty{false};

    std::thread consumer([&]() {
        auto r = q.pop(); // blocks on the empty queue
        returnedEmpty.store(not r.has_value());
    });

    std::this_thread::sleep_for(30ms);
    q.close();      // must wake the consumer
    consumer.join();
    REQUIRE(returnedEmpty.load());
}

TEST_CASE("GBlockingMPMCQueueT close wakes a blocked producer", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<int, 1> q;
    REQUIRE(q.try_push(1)); // full
    std::atomic<bool> pushFailed{false};

    std::thread producer([&]() {
        bool const ok = q.push(2); // blocks (full); must wake on close and return false
        pushFailed.store(not ok);
    });

    std::this_thread::sleep_for(30ms);
    q.close();
    producer.join();
    REQUIRE(pushFailed.load());
}

TEST_CASE("GBlockingMPMCQueueT close is idempotent", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<int, 4> q;
    q.close();
    q.close(); // must not throw or deadlock
    REQUIRE(q.is_closed());
}

/******************************************************************************/
// Move-only payloads (e.g. std::unique_ptr) round-trip

TEST_CASE("GBlockingMPMCQueueT supports move-only items", "[GBlockingMPMCQueueT]") {
    SECTION("custom move-only struct") {
        GBlockingMPMCQueueT<MoveOnly, 4> q;
        REQUIRE(q.try_push(MoveOnly{7}));
        auto r = q.try_pop();
        REQUIRE(r.has_value());
        REQUIRE(r->v_ == 7);
    }
    SECTION("std::unique_ptr") {
        GBlockingMPMCQueueT<std::unique_ptr<int>, 4> q;
        REQUIRE(q.push(std::make_unique<int>(123)));
        auto r = q.pop();
        REQUIRE(r.has_value());
        REQUIRE(*r != nullptr);
        REQUIRE(**r == 123);
    }
}

/******************************************************************************/
// Copyable payloads (e.g. std::shared_ptr): copy vs. move

TEST_CASE("GBlockingMPMCQueueT supports copyable items (copy and move)", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<std::shared_ptr<int>, 4> q;
    auto sp = std::make_shared<int>(5);

    // lvalue -> copy: the shared_ptr's use_count rises
    REQUIRE(q.try_push(sp));
    REQUIRE(sp.use_count() == 2);

    // rvalue -> move: no extra reference retained on the source
    auto sp2 = std::make_shared<int>(6);
    REQUIRE(q.try_push(std::move(sp2)));
    REQUIRE(sp2 == nullptr);

    auto a = q.try_pop();
    auto b = q.try_pop();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(**a == 5);
    REQUIRE(**b == 6);
}

/******************************************************************************/
// Multi-producer / multi-consumer conservation (correctness under concurrency).
// Kept modest here; the heavy load test lives in the benchmark.

TEST_CASE("GBlockingMPMCQueueT MPMC conservation", "[GBlockingMPMCQueueT]") {
    constexpr int n_producers = 4;
    constexpr int n_consumers = 4;
    constexpr int per_producer = 20000;
    constexpr long long expected_count =
        static_cast<long long>(n_producers) * per_producer;

    GBlockingMPMCQueueT<int, 256> q; // bounded -> exercises producer backpressure

    // Each producer pushes the values 0 .. per_producer-1; sum is the closed form.
    const long long expected_sum =
        static_cast<long long>(n_producers) *
        (static_cast<long long>(per_producer) * (per_producer - 1) / 2);

    std::atomic<long long> total_sum{0};
    std::atomic<long long> total_count{0};

    std::vector<std::thread> consumers;
    for(int c = 0; c < n_consumers; ++c) {
        consumers.emplace_back([&]() {
            long long local_sum = 0;
            long long local_count = 0;
            while(auto item = q.pop()) { // ends when closed + drained
                local_sum += *item;
                ++local_count;
            }
            total_sum.fetch_add(local_sum);
            total_count.fetch_add(local_count);
        });
    }

    // NOTE: Catch2 assertion macros are NOT thread-safe, so worker threads must
    // never call REQUIRE/CHECK directly — they record results in atomics that the
    // main thread asserts on after the join.
    std::atomic<bool> all_pushes_ok{true};
    std::vector<std::thread> producers;
    for(int p = 0; p < n_producers; ++p) {
        producers.emplace_back([&]() {
            for(int i = 0; i < per_producer; ++i) {
                if(not q.push(i)) { // blocks under backpressure; never fails (not closed yet)
                    all_pushes_ok.store(false);
                }
            }
        });
    }

    for(auto &t : producers) t.join();
    REQUIRE(all_pushes_ok.load());
    q.close(); // signal end of stream to the consumers
    for(auto &t : consumers) t.join();

    REQUIRE(total_count.load() == expected_count);
    REQUIRE(total_sum.load() == expected_sum);
    REQUIRE(q.empty());
}

/******************************************************************************/
// Construction at several capacities + observers (mirrors GBoundedBufferTTests)

TEST_CASE("GBlockingMPMCQueueT construction and observers across capacities", "[GBlockingMPMCQueueT]") {
    CHECK_NOTHROW((GBlockingMPMCQueueT<int, 0>{}));
    CHECK_NOTHROW((GBlockingMPMCQueueT<int, 1>{}));
    CHECK_NOTHROW((GBlockingMPMCQueueT<int, 10>{}));
    CHECK_NOTHROW((GBlockingMPMCQueueT<int>{})); // default capacity
    CHECK_NOTHROW((GBlockingMPMCQueueT<std::unique_ptr<int>, 10>{}));
    CHECK_NOTHROW((GBlockingMPMCQueueT<std::shared_ptr<int>, 10>{}));

    STATIC_REQUIRE(GBlockingMPMCQueueT<int>::capacity() == DEFAULTBUFFERSIZE);
    STATIC_REQUIRE(GBlockingMPMCQueueT<int>::bounded());
    STATIC_REQUIRE(GBlockingMPMCQueueT<int, 0>::capacity() == 0);
    STATIC_REQUIRE_FALSE(GBlockingMPMCQueueT<int, 0>::bounded());

    GBlockingMPMCQueueT<int, 10> const q;
    CHECK(q.capacity() == 10);
    CHECK(q.bounded());
    CHECK(q.empty());
    CHECK(q.size() == 0);
    CHECK(q.remaining_space() == 10);
    CHECK_FALSE(q.is_closed());
}

/******************************************************************************/
// High-volume single-thread FIFO integrity (matches the old tests' loop volume)

TEST_CASE("GBlockingMPMCQueueT high-volume FIFO integrity", "[GBlockingMPMCQueueT]") {
    constexpr std::size_t N = 2 * DEFAULTBUFFERSIZE; // same volume as GBoundedBufferTTests

    SECTION("unbounded") {
        GBlockingMPMCQueueT<std::size_t, 0> q;
        for(std::size_t i = 0; i < N; ++i) {
            REQUIRE(q.try_push(i));
            REQUIRE(q.size() == i + 1);
        }
        for(std::size_t i = 0; i < N; ++i) {
            auto r = q.try_pop();
            REQUIRE(r.has_value());
            REQUIRE(*r == i); // FIFO: values come out in push order
            REQUIRE(q.size() == N - i - 1);
        }
        REQUIRE(q.empty());
    }

    SECTION("bounded (capacity == N)") {
        GBlockingMPMCQueueT<std::size_t, N> q;
        for(std::size_t i = 0; i < N; ++i) {
            REQUIRE(q.try_push(i));
        }
        REQUIRE(q.size() == N);
        REQUIRE_FALSE(q.try_push(12345)); // full
        for(std::size_t i = 0; i < N; ++i) {
            auto r = q.try_pop();
            REQUIRE(r.has_value());
            REQUIRE(*r == i);
        }
        REQUIRE(q.empty());
    }
}

/******************************************************************************/
// Copy-vs-move semantics: lvalue push copies, rvalue push moves, pop moves out

TEST_CASE("GBlockingMPMCQueueT copy vs. move semantics", "[GBlockingMPMCQueueT]") {
    GBlockingMPMCQueueT<Tracked, 8> q;

    SECTION("lvalue push -> copy into the queue; pop -> move out") {
        Tracked const t(5);
        REQUIRE(q.try_push(t)); // lvalue: must be copied into the queue
        REQUIRE(t.v_ == 5);     // source untouched by a copy

        auto r = q.try_pop();   // pop moves the stored element out
        REQUIRE(r.has_value());
        REQUIRE(r->v_ == 5);
        REQUIRE(r->was_copied()); // it was copied on the way in
        REQUIRE(r->was_moved());  // and moved on the way out
    }

    SECTION("rvalue push -> move into the queue; source is moved-from") {
        Tracked t(6);
        REQUIRE(q.try_push(std::move(t))); // rvalue: must be moved into the queue
        REQUIRE(t.v_ == -1);               // source moved-from

        auto r = q.try_pop();
        REQUIRE(r.has_value());
        REQUIRE(r->v_ == 6);
        REQUIRE_FALSE(r->was_copied()); // never copied
        REQUIRE(r->was_moved());        // moved in, moved out
    }
}
