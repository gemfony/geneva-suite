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


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "hap/GRandomDistributionsT.hpp"
#include "hap/GRandomT.hpp"
#include "hap/GXoshiro256pp.hpp"

using namespace Gem::Hap;

// Dependency-free statistical quality checks. The proxy-based tests draw
// through Gem::Hap::GRandom and therefore exercise the shipped pipeline,
// including the SIMD bulk-refill on hosts where it is compiled in. Bounds are
// deliberately generous: they must never flag a healthy generator (the factory
// is seeded from std::random_device, so the runs are stochastic) yet still
// catch gross failures such as a broken SIMD lane. Heavier external batteries
// (e.g. TestU01) are out of scope here.

namespace {

constexpr std::uint64_t N = 1'000'000;

} // namespace

TEST_CASE("Hap quality: uniform Kolmogorov-Smirnov", "[hap][quality]") {
    GRandom                                rng;
    std::uniform_real_distribution<double> u(0., 1.);
    std::vector<double>                    v;
    v.reserve(N);
    for (std::uint64_t i = 0; i < N; ++i) v.push_back(u(rng));
    std::sort(v.begin(), v.end());
    double d = 0.;
    for (std::uint64_t i = 0; i < N; ++i) {
        double const hi = static_cast<double>(i + 1) / static_cast<double>(N);
        double const lo = static_cast<double>(i) / static_cast<double>(N);
        d = std::max(d, std::max(hi - v[i], v[i] - lo));
    }
    // asymptotic crit at alpha=1e-3 is ~1.95/sqrt(N) ~= 0.00195; allow headroom
    REQUIRE(d < 0.0040);
}

TEST_CASE("Hap quality: chi-squared frequency (256 buckets)", "[hap][quality]") {
    GRandom                    rng;
    std::vector<std::uint64_t> buckets(256, 0);
    for (std::uint64_t i = 0; i < N; ++i) buckets[static_cast<std::uint64_t>(rng()) >> 56]++;
    const double expected = static_cast<double>(N) / 256.0;
    double       chi2     = 0.;
    for (auto o : buckets) {
        double const diff = static_cast<double>(o) - expected;
        chi2 += diff * diff / expected;
    }
    // chi^2_{0.999, 255} ~= 330.5; generous upper bound guards against flakiness
    REQUIRE(chi2 < 400.0);
}

TEST_CASE("Hap quality: lag-1 serial correlation", "[hap][quality]") {
    GRandom                                rng;
    std::uniform_real_distribution<double> u(0., 1.);
    std::vector<double>                    v;
    v.reserve(N);
    for (std::uint64_t i = 0; i < N; ++i) v.push_back(u(rng));
    double mean = 0.;
    for (double const x : v) mean += x;
    mean /= static_cast<double>(N);
    double num = 0., den = 0.;
    for (std::uint64_t i = 0; i + 1 < N; ++i) num += (v[i] - mean) * (v[i + 1] - mean);
    for (double const x : v) den += (x - mean) * (x - mean);
    double const r = num / den;
    // SE ~= 1/sqrt(N) = 1e-3; 0.005 is ~5 sigma
    REQUIRE(std::abs(r) < 0.005);
}

TEST_CASE("Hap quality: monobit (global bit balance)", "[hap][quality]") {
    GRandom       rng;
    std::uint64_t ones = 0;
    for (std::uint64_t i = 0; i < N; ++i)
        ones += static_cast<std::uint64_t>(__builtin_popcountll(static_cast<unsigned long long>(rng())));
    double const fraction = static_cast<double>(ones) / static_cast<double>(N * 64);
    // 6.4e7 bits, SE ~= 6.25e-5; 5e-4 is ~8 sigma
    REQUIRE(std::abs(fraction - 0.5) < 0.0005);
}

TEST_CASE("Hap quality: STAGED source statistics", "[hap][quality][staged]") {
    // The STAGED proxy claims chunks (STAGED_CHUNK_WORDS) from the shared, bulk-
    // filled staging pool into a per-proxy double buffer. With N == 1e6 and a
    // chunk of a few thousand words, a single run crosses hundreds of double-
    // buffer swaps and several pool refills -- so these checks exercise the chunk
    // boundaries and the refill seam, not just one buffer's worth of numbers.
    SECTION("uniform Kolmogorov-Smirnov") {
        GRandomT<randomSource::STAGED>         rng;
        std::uniform_real_distribution<double> u(0., 1.);
        std::vector<double>                    v;
        v.reserve(N);
        for (std::uint64_t i = 0; i < N; ++i) v.push_back(u(rng));
        std::sort(v.begin(), v.end());
        double d = 0.;
        for (std::uint64_t i = 0; i < N; ++i) {
            double const hi = static_cast<double>(i + 1) / static_cast<double>(N);
            double const lo = static_cast<double>(i) / static_cast<double>(N);
            d = std::max(d, std::max(hi - v[i], v[i] - lo));
        }
        REQUIRE(d < 0.0040);
    }
    SECTION("chi-squared frequency (256 buckets)") {
        GRandomT<randomSource::STAGED> rng;
        std::vector<std::uint64_t>     buckets(256, 0);
        for (std::uint64_t i = 0; i < N; ++i) buckets[static_cast<std::uint64_t>(rng()) >> 56]++;
        const double expected = static_cast<double>(N) / 256.0;
        double       chi2     = 0.;
        for (auto o : buckets) {
            double const diff = static_cast<double>(o) - expected;
            chi2 += diff * diff / expected;
        }
        REQUIRE(chi2 < 400.0);
    }
    SECTION("monobit (global bit balance)") {
        GRandomT<randomSource::STAGED> rng;
        std::uint64_t                  ones = 0;
        for (std::uint64_t i = 0; i < N; ++i)
            ones += static_cast<std::uint64_t>(
                __builtin_popcountll(static_cast<unsigned long long>(rng())));
        double const fraction = static_cast<double>(ones) / static_cast<double>(N * 64);
        REQUIRE(std::abs(fraction - 0.5) < 0.0005);
    }
}

TEST_CASE("Hap quality: QUARANTINE source statistics", "[hap][quality][quarantine]") {
    // The QUARANTINE proxy reads spans in place from N rotating, bulk-filled pools.
    // With N == 1e6 a single run crosses many span claims and pool rotations, so
    // these checks exercise the rotation and the (benign-race) in-place reads, not
    // just one pool's worth of numbers.
    SECTION("uniform Kolmogorov-Smirnov") {
        GRandomT<randomSource::QUARANTINE>     rng;
        std::uniform_real_distribution<double> u(0., 1.);
        std::vector<double>                    v;
        v.reserve(N);
        for (std::uint64_t i = 0; i < N; ++i) v.push_back(u(rng));
        std::sort(v.begin(), v.end());
        double d = 0.;
        for (std::uint64_t i = 0; i < N; ++i) {
            double const hi = static_cast<double>(i + 1) / static_cast<double>(N);
            double const lo = static_cast<double>(i) / static_cast<double>(N);
            d = std::max(d, std::max(hi - v[i], v[i] - lo));
        }
        REQUIRE(d < 0.0040);
    }
    SECTION("chi-squared frequency (256 buckets)") {
        GRandomT<randomSource::QUARANTINE> rng;
        std::vector<std::uint64_t>         buckets(256, 0);
        for (std::uint64_t i = 0; i < N; ++i) buckets[static_cast<std::uint64_t>(rng()) >> 56]++;
        const double expected = static_cast<double>(N) / 256.0;
        double       chi2     = 0.;
        for (auto o : buckets) {
            double const diff = static_cast<double>(o) - expected;
            chi2 += diff * diff / expected;
        }
        REQUIRE(chi2 < 400.0);
    }
    SECTION("monobit (global bit balance)") {
        GRandomT<randomSource::QUARANTINE> rng;
        std::uint64_t                      ones = 0;
        for (std::uint64_t i = 0; i < N; ++i)
            ones += static_cast<std::uint64_t>(
                __builtin_popcountll(static_cast<unsigned long long>(rng())));
        double const fraction = static_cast<double>(ones) / static_cast<double>(N * 64);
        REQUIRE(std::abs(fraction - 0.5) < 0.0005);
    }
}

TEST_CASE("Hap quality: scalar xoshiro256++ engine contract", "[hap][quality]") {
    // Deterministic checks on the header-visible scalar engine (the SIMD engine
    // is library-private and is covered above via the proxy).
    SECTION("reproducible from a fixed seed") {
        xoshiro256pp a(12345ULL), b(12345ULL);
        for (int i = 0; i < 1000; ++i) REQUIRE(a() == b());
    }
    SECTION("different seeds diverge") {
        xoshiro256pp a(1ULL), b(2ULL);
        bool         differ = false;
        for (int i = 0; i < 1000 && !differ; ++i)
            if (a() != b()) differ = true;
        REQUIRE(differ);
    }
    SECTION("full 64-bit range advertised") {
        REQUIRE((xoshiro256pp::min)() == 0ULL);
        REQUIRE((xoshiro256pp::max)() == UINT64_MAX);
    }
}
