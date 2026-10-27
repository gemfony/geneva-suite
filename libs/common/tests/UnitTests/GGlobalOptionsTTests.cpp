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

#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "common/GExceptions.hpp"
#include "common/GGlobalOptionsT.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE("GGlobalOptionsT: rule-of-five — copy/move are deleted, default-construct works",
          "[common][global-options]") {
    static_assert(std::is_default_constructible_v<GGlobalOptionsT<int>>);
    static_assert(not std::is_copy_constructible_v<GGlobalOptionsT<int>>);
    static_assert(not std::is_move_constructible_v<GGlobalOptionsT<int>>);
    static_assert(not std::is_copy_assignable_v<GGlobalOptionsT<int>>);
    static_assert(not std::is_move_assignable_v<GGlobalOptionsT<int>>);
    static_assert(std::is_same_v<GGlobalOptionsT<int>::value_type, int>);
}

// ---------------------------------------------------------------------------
// set / get / size / empty / exists

TEST_CASE("GGlobalOptionsT: set/get round-trip via out-parameter and direct accessor",
          "[common][global-options]") {
    GGlobalOptionsT<int> opts;
    CHECK(opts.empty());
    CHECK(opts.empty());

    opts.set("alpha", 1);
    opts.set("beta",  2);
    CHECK_FALSE(opts.empty());
    CHECK(opts.size() == 2);
    CHECK(opts.exists("alpha"));
    CHECK(opts.exists("beta"));
    CHECK_FALSE(opts.exists("gamma"));

    int v = -1;
    REQUIRE(opts.get("alpha", v));
    CHECK(v == 1);

    CHECK(opts.get("beta") == 2);
}

TEST_CASE("GGlobalOptionsT::get(key) throws on missing key (no silent insert)",
          "[common][global-options]") {
    // The fixed get(key) overload no longer uses map::operator[], so missing
    // keys must throw rather than silently inserting a default-constructed
    // value (the historical surprise we documented).
    GGlobalOptionsT<int> opts;
    opts.set("present", 42);

    CHECK_THROWS_AS(opts.get("absent"), geneva_exception);

    // Crucially — the failed get() must not have grown the map.
    CHECK(opts.size() == 1);
    CHECK_FALSE(opts.exists("absent"));
}

TEST_CASE("GGlobalOptionsT::get(key,&out) returns false on miss without writing the out-param",
          "[common][global-options]") {
    GGlobalOptionsT<int> opts;
    opts.set("known", 9);

    int v = 1234;
    CHECK_FALSE(opts.get("unknown", v));
    CHECK(v == 1234);                  // out-param untouched on miss

    CHECK(opts.get("known", v));
    CHECK(v == 9);
}

// ---------------------------------------------------------------------------
// set vs setOnce

TEST_CASE("GGlobalOptionsT: set() overwrites; setOnce() refuses to overwrite",
          "[common][global-options]") {
    GGlobalOptionsT<int> opts;

    CHECK(opts.setOnce("k", 10));      // first insert succeeds
    CHECK(opts.get("k") == 10);

    CHECK_FALSE(opts.setOnce("k", 99)); // second setOnce: no-op
    CHECK(opts.get("k") == 10);

    opts.set("k", 99);                  // plain set overwrites
    CHECK(opts.get("k") == 99);
}

// ---------------------------------------------------------------------------
// remove

TEST_CASE("GGlobalOptionsT::remove() drops only present keys and reports correctly",
          "[common][global-options]") {
    GGlobalOptionsT<int> opts;
    opts.set("a", 1);
    opts.set("b", 2);

    CHECK(opts.remove("a"));
    CHECK_FALSE(opts.exists("a"));
    CHECK(opts.size() == 1);

    CHECK_FALSE(opts.remove("a"));     // already gone
    CHECK_FALSE(opts.remove("never-was"));
    CHECK(opts.size() == 1);
}

// ---------------------------------------------------------------------------
// getKeyDescription / getKeyVector / getContentVector / getContentSnapshot

TEST_CASE("GGlobalOptionsT: getKeyDescription lists all keys in map order",
          "[common][global-options]") {
    GGlobalOptionsT<int> opts;
    opts.set("zeta",  3);
    opts.set("alpha", 1);
    opts.set("mu",    2);

    // std::map iterates in key order, so the description is deterministic.
    CHECK(opts.getKeyDescription() == "alpha, mu, zeta");
}

TEST_CASE("GGlobalOptionsT::getKeyDescription on an empty map returns an empty string",
          "[common][global-options]") {
    GGlobalOptionsT<int> const opts;
    CHECK(opts.getKeyDescription().empty());
}

TEST_CASE("GGlobalOptionsT::getKeyVector clears caller vector and refills with stored keys",
          "[common][global-options]") {
    GGlobalOptionsT<int> opts;
    opts.set("k1", 10);
    opts.set("k2", 20);

    std::vector<std::string> keys{"stale", "data"};   // pre-existing content
    opts.getKeyVector(keys);

    // map order ⇒ sorted by key
    REQUIRE(keys.size() == 2);
    CHECK(keys[0] == "k1");
    CHECK(keys[1] == "k2");
}

TEST_CASE("GGlobalOptionsT::getContentVector returns values in key order",
          "[common][global-options]") {
    GGlobalOptionsT<int> opts;
    opts.set("z", 100);
    opts.set("a", 1);
    opts.set("m", 50);

    std::vector<int> values{999};      // pre-existing content, must be cleared
    opts.getContentVector(values);
    REQUIRE(values.size() == 3);
    CHECK(values[0] == 1);             // 'a'
    CHECK(values[1] == 50);            // 'm'
    CHECK(values[2] == 100);           // 'z'
}

TEST_CASE("GGlobalOptionsT::getContentSnapshot returns a fresh value vector",
          "[common][global-options]") {
    GGlobalOptionsT<int> opts;
    opts.set("a", 1);
    opts.set("b", 2);
    opts.set("c", 3);

    auto snap = opts.getContentSnapshot();
    REQUIRE(snap.size() == 3);
    CHECK(snap[0] == 1);
    CHECK(snap[1] == 2);
    CHECK(snap[2] == 3);

    // Snapshot is by-value; mutating it must not change the map.
    snap[0] = -99;
    CHECK(opts.get("a") == 1);
}

TEST_CASE("GGlobalOptionsT::getContentSnapshot on an empty map yields an empty vector",
          "[common][global-options]") {
    GGlobalOptionsT<int> const opts;
    auto snap = opts.getContentSnapshot();
    CHECK(snap.empty());
}

// ---------------------------------------------------------------------------
// Non-trivial value type — exercises the std::string value branch.

TEST_CASE("GGlobalOptionsT<std::string>: works with non-trivial value types",
          "[common][global-options]") {
    GGlobalOptionsT<std::string> opts;
    opts.set("greeting", std::string{"hello"});
    opts.set("farewell", std::string{"bye"});

    CHECK(opts.get("greeting") == "hello");
    std::string v;
    REQUIRE(opts.get("farewell", v));
    CHECK(v == "bye");

    auto snap = opts.getContentSnapshot();
    REQUIRE(snap.size() == 2);
    // map iteration: "farewell" < "greeting"
    CHECK(snap[0] == "bye");
    CHECK(snap[1] == "hello");
}

// ---------------------------------------------------------------------------
// Concurrency: many writers + many readers must produce a consistent final
// state — no torn map, no UB, all-or-nothing keys.

// NOLINTNEXTLINE(readability-function-size) -- one coherent concurrency stress-test kernel (spawns writer/reader threads sharing atomics + the store under test, joins, then asserts); splitting would only scatter the tightly-coupled thread lambdas
TEST_CASE("GGlobalOptionsT: concurrent set/get/snapshot remains consistent",
          "[common][global-options][concurrency]") {
    GGlobalOptionsT<int> opts;

    constexpr int kWriters    = 4;
    constexpr int kPerWriter  = 256;
    constexpr int kReaders    = 4;
    constexpr int kReadPasses = 64;
    constexpr int kMaxValue   = kWriters * 1000 + kPerWriter;

    // Workers cannot invoke Catch CHECK/REQUIRE (Catch's output redirection
    // is not thread-safe). Record observed violations in atomics and CHECK
    // on the main thread after join.
    std::atomic<bool> go{false};
    std::atomic<int>  out_of_range{0};
    std::atomic<int>  reader_sum_pass{0};
    std::vector<std::thread> ts;
    ts.reserve(kWriters + kReaders);

    for(int w = 0; w < kWriters; ++w) {
        ts.emplace_back([w, &opts, &go] {
            while(not go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for(int i = 0; i < kPerWriter; ++i) {
                opts.set("w" + std::to_string(w) + "_k" + std::to_string(i), w * 1000 + i);
            }
        });
    }

    for(int r = 0; r < kReaders; ++r) {
        ts.emplace_back([&opts, &go, &out_of_range, &reader_sum_pass] {
            while(not go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for(int i = 0; i < kReadPasses; ++i) {
                auto snap = opts.getContentSnapshot();
                for(int const v : snap) {
                    if(v < 0 || v >= kMaxValue) {
                        ++out_of_range;
                    }
                }
                ++reader_sum_pass;
            }
        });
    }

    go.store(true, std::memory_order_release);
    for(auto &t : ts) {
        t.join();
    }

    CHECK(out_of_range.load() == 0);
    CHECK(opts.size() == static_cast<std::size_t>(kWriters * kPerWriter));
    CHECK(reader_sum_pass.load() == kReaders * kReadPasses);
}
