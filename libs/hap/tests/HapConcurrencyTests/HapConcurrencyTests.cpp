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


#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "hap/GRandomDistributionsT.hpp"
#include "hap/GRandomT.hpp"

using namespace Gem::Hap;

// Concurrency stress for the factory/proxy machinery: many proxies drawing at
// once. As a normal ctest this catches crashes, hangs and out-of-range values;
// built under -fsanitize=thread / -fsanitize=address it is the data-race / memory
// gate for the shared factory (producer threads, bounded buffers, singleton).

namespace {
constexpr unsigned      kThreads = 16;
constexpr std::uint64_t kRawPer  = 500'000;
constexpr std::uint64_t kDistPer = 200'000;
} // namespace

TEST_CASE("Hap concurrency: 16 proxies draw raw values", "[hap][concurrency]") {
    std::atomic<std::uint64_t> checksum{0};
    std::vector<std::thread>   threads;
    threads.reserve(kThreads);
    for (unsigned t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            GRandom       rng; // each thread owns its proxy
            std::uint64_t local = 0;
            for (std::uint64_t i = 0; i < kRawPer; ++i) local += static_cast<std::uint64_t>(rng());
            checksum.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto &th : threads) th.join();
    REQUIRE(checksum.load() != 0); // sanity: the run actually produced numbers
}

TEST_CASE("Hap concurrency: 16 proxies draw distributions", "[hap][concurrency]") {
    std::atomic<std::uint64_t> outOfRange{0};
    std::vector<std::thread>   threads;
    threads.reserve(kThreads);
    for (unsigned t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            GRandom                                rng;
            std::uniform_real_distribution<double> u(0., 1.);
            std::normal_distribution<double>       n;
            bi_normal_distribution<double>         bn(1., 2., 1., 3.);
            for (std::uint64_t i = 0; i < kDistPer; ++i) {
                double const a = u(rng);
                if (a < 0. || a >= 1.) outOfRange.fetch_add(1, std::memory_order_relaxed);
                volatile double const b = n(rng);
                volatile double const c = bn(rng);
                (void)b;
                (void)c;
            }
        });
    }
    for (auto &th : threads) th.join();
    REQUIRE(outOfRange.load() == 0); // uniform_real must stay in [0,1)
}

TEST_CASE("Hap concurrency: 16 STAGED proxies draw raw values", "[hap][concurrency][staged]") {
    // The STAGED source funnels every proxy through one shared, lock-free
    // rotating pool (GRotatingPool). Under -fsanitize=thread this is the race gate for that pool
    // (each proxy copies its chunk out word-by-word and then serves from a private buffer).
    std::atomic<std::uint64_t> checksum{0};
    std::vector<std::thread>   threads;
    threads.reserve(kThreads);
    for (unsigned t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            GRandomT<randomSource::STAGED> rng; // each thread owns its proxy
            std::uint64_t                  local = 0;
            for (std::uint64_t i = 0; i < kRawPer; ++i) local += static_cast<std::uint64_t>(rng());
            checksum.fetch_add(local, std::memory_order_relaxed);
        });
    }
    for (auto &th : threads) th.join();
    REQUIRE(checksum.load() != 0); // sanity: the run actually produced numbers
}

TEST_CASE("Hap concurrency: STAGED dormant proxy keeps a stable private stream",
          "[hap][concurrency][staged]") {
    // A STAGED proxy serves from a private copy of its chunk, so it pins no pool
    // memory. This stresses that: one proxy draws a little, then goes dormant
    // while many others churn the pool through countless refills (overwriting the
    // shared buffer again and again), then resumes. Its stream must continue
    // intact (no crash, values still in range) and -- under TSan -- show no race
    // between its dormant private buffer and the others' refills.
    GRandomT<randomSource::STAGED>         dormant;
    std::uniform_real_distribution<double> u(0., 1.);
    for (int i = 0; i < 10; ++i) {
        double const a = u(dormant);
        REQUIRE((a >= 0. && a < 1.));
    }

    std::atomic<std::uint64_t> outOfRange{0};
    std::vector<std::thread>   threads;
    threads.reserve(kThreads);
    for (unsigned t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            GRandomT<randomSource::STAGED>         rng;
            std::uniform_real_distribution<double> v(0., 1.);
            for (std::uint64_t i = 0; i < kRawPer; ++i) {
                double const a = v(rng);
                if (a < 0. || a >= 1.) outOfRange.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &th : threads) th.join();

    // The dormant proxy resumes after the storm; its private stream is unharmed.
    for (int i = 0; i < 100'000; ++i) {
        double const a = u(dormant);
        REQUIRE((a >= 0. && a < 1.));
    }
    REQUIRE(outOfRange.load() == 0);
}

TEST_CASE("Hap concurrency: 16 QUARANTINE proxies draw raw values",
          "[hap][concurrency][quarantine]") {
    // QUARANTINE proxies read spans in place from the shared rotating pools while a
    // background producer refills them. Under -fsanitize=thread the producer's
    // refill races a stale reader by design (benign, aligned-64-bit) -- the pool
    // storage is marked with AnnotateBenignRaceSized in GRotatingPool.hpp, so a clean TSan run here
    // confirms no OTHER, unintended race crept in. As a normal ctest it catches
    // crashes/hangs and out-of-range values.
    std::atomic<std::uint64_t> outOfRange{0};
    std::vector<std::thread>   threads;
    threads.reserve(kThreads);
    for (unsigned t = 0; t < kThreads; ++t) {
        threads.emplace_back([&]() {
            GRandomT<randomSource::QUARANTINE>     rng;
            std::uniform_real_distribution<double> u(0., 1.);
            for (std::uint64_t i = 0; i < kRawPer; ++i) {
                double const a = u(rng);
                if (a < 0. || a >= 1.) outOfRange.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto &th : threads) th.join();
    REQUIRE(outOfRange.load() == 0); // uniform_real must stay in [0,1)
}
