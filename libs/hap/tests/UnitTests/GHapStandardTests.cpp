/**
 * @file GHapStandardTests.cpp
 */

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

#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "GHap_tests.hpp"
#include "hap/GRandomDefines.hpp"
#include "hap/GRandomFactory.hpp"

TEST_CASE_METHOD(GHap_tests, "GHap no_failure_expected", "[hap][standard]") {
    no_failure_expected();
}

TEST_CASE_METHOD(GHap_tests, "GHap failures_expected", "[hap][standard][failures-expected]") {
    failures_expected();
}

// PRE-2: a request of 0 producer threads must auto-size to the hardware (not fall back to a fixed 2).
TEST_CASE("GHap autoProducerThreadCount is hardware-derived and clamped", "[hap][standard]") {
    const std::uint16_t n = Gem::Hap::autoProducerThreadCount();

    // Always within the documented clamp, and never zero (so producers actually start).
    REQUIRE(n >= Gem::Hap::DEFAULT01PRODUCERTHREADS);
    REQUIRE(n <= Gem::Hap::MAXAUTOPRODUCERTHREADS);

    const unsigned int hw = std::thread::hardware_concurrency();
    if(hw == 0) {
        // Core count undeterminable -> the fixed fallback.
        REQUIRE(n == Gem::Hap::DEFAULT01PRODUCERTHREADS);
    }
    else {
        // Exactly the clamp(hw / divisor, floor, cap) the helper documents.
        const unsigned int scaled = std::max<unsigned int>(
            Gem::Hap::DEFAULT01PRODUCERTHREADS, hw / Gem::Hap::PRODUCERTHREADS_HW_DIVISOR);
        const std::uint16_t expected =
            static_cast<std::uint16_t>(std::min<unsigned int>(scaled, Gem::Hap::MAXAUTOPRODUCERTHREADS));
        REQUIRE(n == expected);
    }
}

// PRE-1: the factory exposes supply-health counters so RNG production/starvation is observable.
TEST_CASE("GHap factory exposes supply-health counters", "[hap][standard]") {
    auto factory = Gem::Hap::randomFactory();

    // Draw a few containers; a healthy supply hands them out (producers auto-start on first access).
    bool got = false;
    for(int i = 0; i < 100 && not got; ++i) {
        if(auto p = factory->getNewRandomContainer()) { got = true; }
    }
    REQUIRE(got);

    // The production counter must reflect that packages were produced to satisfy those draws.
    REQUIRE(factory->getNPackagesProduced() > 0);

    // The starvation counter is readable and monotonic; we do not assert > 0 (a healthy supply may
    // never time out), only that it is queryable without side effects.
    const std::uint64_t t1 = factory->getNGetTimeouts();
    const std::uint64_t t2 = factory->getNGetTimeouts();
    REQUIRE(t2 >= t1);
}

// Regression (2026-07-18): setNProducerThreads() must record the new count even when the producer
// threads are already running. On the unfixed code the started-threads branch launched the missing
// threads but never stored the new count, so the getter kept the stale value and every subsequent
// grow request re-computed its delta from that stale count, silently spawning duplicate producers.
TEST_CASE("GHap setNProducerThreads keeps its count in step with the running pool", "[hap][standard]") {
    auto factory = Gem::Hap::randomFactory();

    // Ensure the producer threads are running (they start lazily on the first draw).
    bool got = false;
    for(int i = 0; i < 100 && not got; ++i) {
        if(auto p = factory->getNewRandomContainer()) { got = true; }
    }
    REQUIRE(got);

    const std::uint16_t before = factory->getNProducerThreads();
    REQUIRE(before > 0);

    // Grow the running pool: the stored count must follow the request.
    const auto target = static_cast<std::uint16_t>(before + 2);
    factory->setNProducerThreads(target);
    REQUIRE(factory->getNProducerThreads() == target);

    // Repeating the same request is a no-op (the delta is computed from the CURRENT pool size).
    factory->setNProducerThreads(target);
    REQUIRE(factory->getNProducerThreads() == target);

    // A decrease while threads run is refused (warning) and leaves the count unchanged.
    factory->setNProducerThreads(1);
    REQUIRE(factory->getNProducerThreads() == target);
}
