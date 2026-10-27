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

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "common/concurrency/GThreadGroup.hpp"

using namespace Gem::Common;
using namespace Gem::Common::Concurrency;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Empty group

TEST_CASE("GThreadGroup: empty group reports size 0 and joins cleanly",
          "[common][thread-group]") {
    GThreadGroup g;
    CHECK(g.size() == 0);
    CHECK_NOTHROW(g.join_all());
}

// ---------------------------------------------------------------------------
// create_thread / create_threads / join_all

TEST_CASE("GThreadGroup::create_thread runs the supplied callable",
          "[common][thread-group]") {
    GThreadGroup g;
    std::atomic<int> counter{0};

    auto t = g.create_thread([&counter] { ++counter; });
    REQUIRE(t);
    CHECK(g.size() == 1);

    g.join_all();
    CHECK(counter.load() == 1);
}

TEST_CASE("GThreadGroup::create_threads spawns N threads with the same callable",
          "[common][thread-group]") {
    GThreadGroup g;
    std::atomic<int> counter{0};
    constexpr std::size_t N = 8;

    g.create_threads([&counter] { ++counter; }, N);
    CHECK(g.size() == N);

    g.join_all();
    CHECK(counter.load() == static_cast<int>(N));
}

// ---------------------------------------------------------------------------
// create_thread returns a joinable handle whose side effect is observable.

TEST_CASE("GThreadGroup::create_thread returns a joinable handle",
          "[common][thread-group]") {
    GThreadGroup g;
    std::atomic<int> counter{0};

    auto t = g.create_thread([&counter] { ++counter; });
    REQUIRE(t);
    // The handle is a live jthread; until we join it is joinable.
    CHECK(t->joinable());

    g.join_all();
    CHECK_FALSE(t->joinable());
    CHECK(counter.load() == 1);
}

// ---------------------------------------------------------------------------
// add_thread: takes an externally constructed std::jthread

TEST_CASE("GThreadGroup::add_thread accepts a pre-built thread",
          "[common][thread-group]") {
    GThreadGroup g;
    std::atomic<int> counter{0};
    auto t = std::make_shared<std::jthread>([&counter] { ++counter; });

    g.add_thread(t);
    CHECK(g.size() == 1);

    g.join_all();
    CHECK(counter.load() == 1);
}

TEST_CASE("GThreadGroup::add_thread(nullptr) is ignored",
          "[common][thread-group]") {
    GThreadGroup g;

    g.add_thread(nullptr);
    CHECK(g.size() == 0);

    std::atomic<int> counter{0};
    auto t = std::make_shared<std::jthread>([&counter] { ++counter; });
    g.add_thread(t);
    CHECK(g.size() == 1);

    g.join_all();
    CHECK(counter.load() == 1);
}

// ---------------------------------------------------------------------------
// join_all leaves all handles non-joinable and the group can be re-used.

TEST_CASE("GThreadGroup::join_all leaves no joinable threads and the group "
          "can be re-used",
          "[common][thread-group]") {
    GThreadGroup g;
    std::atomic<int> counter{0};

    auto a = g.create_thread([&counter] { ++counter; });
    auto b = g.create_thread([&counter] { ++counter; });
    CHECK(g.size() == 2);

    g.join_all();
    CHECK_FALSE(a->joinable());
    CHECK_FALSE(b->joinable());
    CHECK(counter.load() == 2);

    // Re-use: the group keeps the (now joined) handles but accepts more work.
    auto c = g.create_thread([&counter] { ++counter; });
    CHECK(g.size() == 3);

    g.join_all();
    CHECK_FALSE(c->joinable());
    CHECK(counter.load() == 3);
}

// ---------------------------------------------------------------------------
// size() under sequential creation.

TEST_CASE("GThreadGroup::size reflects sequential creation",
          "[common][thread-group]") {
    GThreadGroup g;
    std::atomic<int> counter{0};
    constexpr std::size_t N = 5;

    for(std::size_t i = 0; i < N; ++i) {
        g.create_thread([&counter] { ++counter; });
        CHECK(g.size() == i + 1);
    }

    g.join_all();
    CHECK(counter.load() == static_cast<int>(N));
}

// ---------------------------------------------------------------------------
// Concurrency: many threads call create_thread on one group concurrently.
// TSan-friendly: workers do not touch Catch2 macros; we assert on the main
// thread once every driver has joined.

TEST_CASE("GThreadGroup::create_thread is safe under concurrent creation",
          "[common][thread-group]") {
    GThreadGroup g;
    std::atomic<int> counter{0};
    constexpr std::size_t kDrivers = 8;
    constexpr std::size_t kPerDriver = 8;

    // A gate so the drivers start hammering create_thread at roughly the
    // same time, maximising contention without using sleeps.
    std::atomic<bool> go{false};

    std::vector<std::jthread> drivers;
    drivers.reserve(kDrivers);
    for(std::size_t d = 0; d < kDrivers; ++d) {
        drivers.emplace_back([&] {
            while(!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for(std::size_t i = 0; i < kPerDriver; ++i) {
                g.create_thread([&counter] { ++counter; });
            }
        });
    }

    go.store(true, std::memory_order_release);
    for(auto &d : drivers) {
        d.join();
    }

    // All creations are done; size must be exact.
    CHECK(g.size() == kDrivers * kPerDriver);

    g.join_all();
    CHECK(counter.load() == static_cast<int>(kDrivers * kPerDriver));
}

// ---------------------------------------------------------------------------
// jthread auto-join safety: a group destroyed WITHOUT an explicit join_all()
// must not crash. With std::thread the shared handle would std::terminate on
// destruction; std::jthread joins automatically. The workers are short and
// self-terminating, so destruction is fast.

TEST_CASE("GThreadGroup destroyed without join_all auto-joins cleanly",
          "[common][thread-group]") {
    std::atomic<int> counter{0};

    CHECK_NOTHROW([&] {
        GThreadGroup g;
        g.create_threads([&counter] { ++counter; }, 4);
        // Intentionally NO join_all(): leaving this scope destroys the group
        // and the contained std::jthreads auto-join.
    }());

    // Every worker ran to completion before its jthread destructor returned.
    CHECK(counter.load() == 4);
}

// ---------------------------------------------------------------------------
// join_all waits for in-flight work.

TEST_CASE("GThreadGroup::join_all blocks until in-flight work completes",
          "[common][thread-group]") {
    GThreadGroup g;
    std::atomic<bool> done{false};

    g.create_thread([&done] {
        std::this_thread::sleep_for(20ms);
        done.store(true);
    });

    // Pre-join, the worker should still be executing (small but real delay).
    g.join_all();
    CHECK(done.load());
}
