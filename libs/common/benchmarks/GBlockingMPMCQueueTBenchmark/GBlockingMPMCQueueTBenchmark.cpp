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
 * Load / throughput benchmark for Gem::Common::Concurrency::GBlockingMPMCQueueT.
 *
 * This is deliberately NOT a unit test and is NOT registered with CTest: it runs
 * for a while and is meant to be invoked by hand, e.g.
 *
 *   ./GBlockingMPMCQueueTBenchmark            # full sweep
 *   ./GBlockingMPMCQueueTBenchmark --quick    # short sweep
 *   ./GBlockingMPMCQueueTBenchmark --items 50000000 --producers 8 --consumers 8
 *
 * It measures aggregate throughput (items/s) for several producer/consumer counts
 * over both a bounded and an unbounded queue, plus a latency sketch (p50/p99/p999
 * of pop wait time) and a teardown stress (repeatedly close() with threads parked
 * in blocking calls, to flush out shutdown hangs/races). It also verifies item
 * conservation in every run, so it doubles as a heavy correctness soak test.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/concurrency/GBlockingMPMCQueueT.hpp"

using Gem::Common::Concurrency::GBlockingMPMCQueueT;
using clk = std::chrono::steady_clock;

namespace {

constexpr std::size_t BOUNDED_CAP = 1024;

// ---- one MPMC run over a queue with compile-time capacity Cap ----------------
template <std::size_t Cap>
struct RunResult {
    double seconds = 0.0;
    long long produced = 0;
    long long consumed = 0;
    long long checksum = 0;     // sum of all consumed values
    long long expected = 0;     // expected sum (for conservation check)
};

template <std::size_t Cap>
RunResult<Cap> run_mpmc(int producers, int consumers, long long items_per_producer) {
    GBlockingMPMCQueueT<std::uint64_t, Cap> q;
    std::atomic<long long> consumed{0};
    std::atomic<long long> checksum{0};

    std::vector<std::thread> cons;
    cons.reserve(static_cast<std::size_t>(consumers));
    for(int c = 0; c < consumers; ++c) {
        cons.emplace_back([&]() {
            long long local_n = 0;
            long long local_sum = 0;
            while(auto item = q.pop()) {
                local_sum += static_cast<long long>(*item);
                ++local_n;
            }
            consumed.fetch_add(local_n);
            checksum.fetch_add(local_sum);
        });
    }

    auto t0 = clk::now();
    std::vector<std::thread> prod;
    prod.reserve(static_cast<std::size_t>(producers));
    for(int p = 0; p < producers; ++p) {
        prod.emplace_back([&, p]() {
            for(long long i = 0; i < items_per_producer; ++i) {
                // All producers push the same range 0..items_per_producer-1; the
                // expected sum accounts for that (producers * n*(n-1)/2).
                (void)q.push(static_cast<std::uint64_t>(i));
            }
        });
    }
    for(auto &t : prod) t.join();
    q.close();
    for(auto &t : cons) t.join();
    auto t1 = clk::now();

    RunResult<Cap> r;
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.produced = static_cast<long long>(producers) * items_per_producer;
    r.consumed = consumed.load();
    r.checksum = checksum.load();
    r.expected = static_cast<long long>(producers) *
                 (items_per_producer * (items_per_producer - 1) / 2);
    return r;
}

template <std::size_t Cap>
void report(const char *label, int prod, int cons, long long per_prod) {
    auto r = run_mpmc<Cap>(prod, cons, per_prod);
    double const mips = (r.seconds > 0.0) ? (static_cast<double>(r.consumed) / r.seconds / 1e6) : 0.0;
    bool const ok = (r.consumed == r.produced) && (r.checksum == r.expected);
    std::printf(
        "%-10s P=%-2d C=%-2d items=%-12lld | %8.2f Mitems/s | %6.3f s | conserve=%s\n",
        label, prod, cons, r.produced, mips, r.seconds, ok ? "OK" : "*** FAIL ***"
    );
    if(not ok) {
        std::printf(
            "    consumed=%lld produced=%lld checksum=%lld expected=%lld\n",
            r.consumed, r.produced, r.checksum, r.expected
        );
    }
}

// ---- latency sketch: single producer pacing, measure pop wait latency --------
void latency_sketch(long long n) {
    GBlockingMPMCQueueT<std::uint64_t, BOUNDED_CAP> q;
    std::vector<long long> lat_ns;
    lat_ns.reserve(static_cast<std::size_t>(n));

    std::thread consumer([&]() {
        for(long long i = 0; i < n; ++i) {
            auto t0 = clk::now();
            auto item = q.pop();
            auto t1 = clk::now();
            if(not item) break;
            lat_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    });

    for(long long i = 0; i < n; ++i) {
        (void)q.push(static_cast<std::uint64_t>(i));
        if((i & 0x3FF) == 0) std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    q.close();
    consumer.join();

    std::sort(lat_ns.begin(), lat_ns.end());
    auto pct = [&](double p) -> long long {
        if(lat_ns.empty()) return 0;
        auto idx = static_cast<std::size_t>(p * static_cast<double>(lat_ns.size() - 1));
        return lat_ns[idx];
    };
    std::printf(
        "latency (pop, %lld samples): p50=%lld ns  p99=%lld ns  p999=%lld ns\n",
        static_cast<long long>(lat_ns.size()), pct(0.50), pct(0.99), pct(0.999)
    );
}

// ---- teardown stress: park threads in blocking calls, then close() -----------
void teardown_stress(int rounds) {
    int const hangs = 0;
    for(int r = 0; r < rounds; ++r) {
        auto q = std::make_unique<GBlockingMPMCQueueT<std::uint64_t, 1>>();
        (void)q->try_push(0); // make it full so producers block

        std::vector<std::thread> ts;
        for(int i = 0; i < 4; ++i) ts.emplace_back([&]() { (void)q->push(1); });   // blocked (full)
        for(int i = 0; i < 4; ++i) ts.emplace_back([&]() { (void)q->pop(); });      // may block

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        q->close(); // must wake everyone
        for(auto &t : ts) t.join();
        (void)hangs;
    }
    std::printf("teardown stress: %d rounds completed (no hang)\n", rounds);
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    bool quick = false;
    int producers = 0, consumers = 0;
    long long items = 0;
    for(int i = 1; i < argc; ++i) {
        std::string const a = argv[i];
        if(a == "--quick") quick = true;
        else if(a == "--producers" && i + 1 < argc) producers = std::atoi(argv[++i]);
        else if(a == "--consumers" && i + 1 < argc) consumers = std::atoi(argv[++i]);
        else if(a == "--items" && i + 1 < argc) items = std::atoll(argv[++i]);
    }

    const long long per_prod = (items > 0) ? items : (quick ? 200000 : 5000000);

    std::printf("GBlockingMPMCQueueT benchmark (bounded cap=%zu, unbounded)\n", BOUNDED_CAP);
    std::printf("items/producer = %lld%s\n", per_prod, quick ? " (quick)" : "");

    if(producers > 0 && consumers > 0) {
        report<BOUNDED_CAP>("bounded", producers, consumers, per_prod);
        report<0>("unbounded", producers, consumers, per_prod);
    } else {
        const int configs[][2] = {{1, 1}, {1, 4}, {4, 1}, {4, 4}, {8, 8}};
        const int n = quick ? 3 : 5;
        std::printf("\n-- bounded (capacity=%zu) --\n", BOUNDED_CAP);
        for(int i = 0; i < n; ++i) report<BOUNDED_CAP>("bounded", configs[i][0], configs[i][1], per_prod);
        std::printf("\n-- unbounded --\n");
        for(int i = 0; i < n; ++i) report<0>("unbounded", configs[i][0], configs[i][1], per_prod);
    }

    std::printf("\n");
    latency_sketch(quick ? 100000 : 2000000);
    teardown_stress(quick ? 200 : 2000);

    return 0;
}
