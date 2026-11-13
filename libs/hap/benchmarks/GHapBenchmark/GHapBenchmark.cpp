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
 * @file
 * Throughput / latency benchmark for the Hap RNG library (xoshiro256++ engine,
 * factory + proxy model). This is deliberately NOT a CTest test: it runs for a
 * while and is meant to be invoked by hand, e.g.
 *
 *   ./GHapBenchmark                 # full run
 *   ./GHapBenchmark --quick         # short run
 *   ./GHapBenchmark --proxies 32 --total 2000000000 --producers 8
 *
 * It measures sustained aggregate throughput for the way the library is actually
 * used -- the proxy as a UniformRandomBitGenerator feeding std distributions (and
 * Hap's own bi_normal) -- across several proxy counts, plus a burst latency sketch
 * (p50/p99/p999 of a single draw under a synchronised start). Output is TSV.
 */

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hap/GRandomT.hpp"
#include "hap/GRandomDistributionsT.hpp"
#include "hap/GRandomFactory.hpp"

using clk = std::chrono::steady_clock;
using Gem::Hap::GRandom;

namespace {

enum class Mode { Raw, Uniform, Normal, BiNormal };

const char *mode_name(Mode m) {
    switch(m) {
        case Mode::Raw: return "raw";
        case Mode::Uniform: return "uniform_real";
        case Mode::Normal: return "normal";
        case Mode::BiNormal: return "bi_normal";
    }
    std::unreachable();
}

// Sustained throughput: nProxies threads, each with its own proxy, draw
// total/nProxies values via the chosen mode. Returns numbers/second.
double sustained(unsigned nProxies, std::uint64_t total, Mode mode, double &wall_ms) {
    const std::uint64_t per = total / nProxies;
    std::vector<std::thread> ts;
    ts.reserve(nProxies);

    auto t0 = clk::now();
    for(unsigned i = 0; i < nProxies; ++i) {
        ts.emplace_back([per, mode]() {
            GRandom gr;
            std::uniform_real_distribution<double> ud(0., 1.);
            std::normal_distribution<double> nd;
            Gem::Hap::bi_normal_distribution<double> bd;
            volatile std::uint64_t isink = 0;
            volatile double dsink = 0.;
            for(std::uint64_t k = 0; k < per; ++k) {
                switch(mode) {
                    case Mode::Raw: isink ^= gr(); break;
                    case Mode::Uniform: dsink += ud(gr); break;
                    case Mode::Normal: dsink += nd(gr); break;
                    case Mode::BiNormal: dsink += bd(gr); break;
                }
            }
            (void)isink;
            (void)dsink;
        });
    }
    for(auto &t : ts) t.join();
    double const secs = std::chrono::duration<double>(clk::now() - t0).count();
    wall_ms = secs * 1000.0;
    return static_cast<double>(per * nProxies) / secs;
}

struct LatResult {
    long long p50 = 0, p99 = 0, p999 = 0;
    double throughput = 0.;
};

// Burst latency: nProxies threads released simultaneously by a barrier; each
// records the latency of every raw draw. Returns p50/p99/p999 over all draws.
LatResult burst_latency(unsigned nProxies, std::uint64_t perProxy) {
    std::vector<std::vector<long long>> lat(nProxies);
    std::barrier gate(static_cast<std::ptrdiff_t>(nProxies) + 1);
    std::vector<std::thread> ts;
    ts.reserve(nProxies);

    for(unsigned p = 0; p < nProxies; ++p) {
        ts.emplace_back([&, p]() {
            GRandom gr;
            auto &v = lat[p];
            v.reserve(static_cast<std::size_t>(perProxy));
            gate.arrive_and_wait();
            for(std::uint64_t k = 0; k < perProxy; ++k) {
                auto a = clk::now();
                volatile std::uint64_t const x = gr();
                auto b = clk::now();
                (void)x;
                v.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
            }
        });
    }
    gate.arrive_and_wait();
    auto t0 = clk::now();
    for(auto &t : ts) t.join();
    double const secs = std::chrono::duration<double>(clk::now() - t0).count();

    std::vector<long long> all;
    all.reserve(static_cast<std::size_t>(nProxies) * perProxy);
    for(auto &v : lat) all.insert(all.end(), v.begin(), v.end());
    std::sort(all.begin(), all.end());
    auto pct = [&](double q) -> long long {
        if(all.empty()) return 0;
        return all[static_cast<std::size_t>(q * static_cast<double>(all.size() - 1))];
    };
    LatResult r;
    r.p50 = pct(0.50);
    r.p99 = pct(0.99);
    r.p999 = pct(0.999);
    r.throughput = static_cast<double>(nProxies * perProxy) / secs;
    return r;
}

} // namespace

// NOLINTNEXTLINE(readability-function-size) -- main() of a benchmark program: CLI parsing followed by the sustained-throughput sweep and the burst-latency measurement, printed as one TSV report
int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    bool quick = false;
    unsigned proxies_override = 0, producers = 0;
    std::uint64_t total_override = 0;
    for(int i = 1; i < argc; ++i) {
        std::string const a = argv[i];
        if(a == "--quick") quick = true;
        else if(a == "--proxies" && i + 1 < argc) proxies_override = static_cast<unsigned>(std::stoul(argv[++i]));
        else if(a == "--producers" && i + 1 < argc) producers = static_cast<unsigned>(std::stoul(argv[++i]));
        else if(a == "--total" && i + 1 < argc) total_override = std::stoull(argv[++i]);
    }

    if(producers > 0) {
        Gem::Hap::randomFactory()->setNProducerThreads(static_cast<std::uint16_t>(producers));
    }

    const std::uint64_t total = total_override ? total_override : (quick ? 50'000'000ULL : 500'000'000ULL);

    std::printf("Hap benchmark -- xoshiro256++ engine (factory + proxy)%s\n", quick ? "  [quick]" : "");
    std::printf("total draws per scenario = %llu\n\n", static_cast<unsigned long long>(total));

    // ---- sustained throughput ----
    std::printf("case\tmode\t\tproxies\tthroughput_Mnum_s\twall_ms\n");
    const Mode modes[] = {Mode::Raw, Mode::Uniform, Mode::Normal, Mode::BiNormal};
    const unsigned counts_full[] = {1, 4, 16};
    const unsigned counts_quick[] = {1, 4};
    const unsigned *counts = quick ? counts_quick : counts_full;
    const int n_counts = quick ? 2 : 3;

    for(Mode const m : modes) {
        // Distributions cost several raw draws each; scale the work down so every
        // scenario takes a comparable wall time.
        std::uint64_t mode_total = total;
        if(m == Mode::Uniform) mode_total = total / 4;
        else if(m == Mode::Normal || m == Mode::BiNormal) mode_total = total / 8;
        for(int c = 0; c < n_counts; ++c) {
            unsigned const np = proxies_override ? proxies_override : counts[c];
            double wall = 0.;
            double const tput = sustained(np, mode_total, m, wall);
            std::printf(
                "S\t%-12s\t%u\t%16.2f\t%8.1f\n", mode_name(m), np, tput / 1e6, wall
            );
            if(proxies_override) break; // honour the explicit proxy count once
        }
    }

    // ---- burst latency (raw draws, synchronised start) ----
    std::printf("\n");
    const unsigned burst_proxies = proxies_override ? proxies_override : (quick ? 16 : 64);
    const std::uint64_t per = quick ? 50'000ULL : 200'000ULL;
    LatResult const lr = burst_latency(burst_proxies, per);
    std::printf(
        "burst: %u proxies x %llu raw draws | %.2f Mnum/s | p50=%lld ns  p99=%lld ns  p999=%lld ns\n",
        burst_proxies, static_cast<unsigned long long>(per), lr.throughput / 1e6, lr.p50, lr.p99, lr.p999
    );

    return 0;
}
