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
 * Throughput comparison of the two GMPMCQueueT backends -- the std::deque-backed queue
 * (QueueBackend::Deque) and the experimental preallocated ring (QueueBackend::Preallocated) -- as a
 * function of the number of concurrent readers and writers.
 *
 * For each thread count N from 1 to 16 it runs N producers AND N consumers (readers == writers, so
 * the sweep stays short) over a fixed total amount of work, and reports aggregate throughput
 * (Mitems/s) for each backend side by side, with the ratio. Item conservation is verified in every
 * run, so it doubles as a heavy correctness soak.
 *
 * This is NOT a unit test and is NOT registered with CTest. Run it by hand:
 *
 *   ./GMPMCQueueBackendBenchmark             # full sweep (1..16 readers/writers)
 *   ./GMPMCQueueBackendBenchmark --quick     # shorter sweep
 *   ./GMPMCQueueBackendBenchmark --items 20000000   # set the fixed total work
 *   ./GMPMCQueueBackendBenchmark --max 8     # cap the thread sweep at N=8
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "common/concurrency/GMPMCQueueT.hpp"

using Gem::Common::Concurrency::GMPMCQueueT;
using Gem::Common::Concurrency::QueueBackend;
using clk = std::chrono::steady_clock;

namespace {

// Bounded capacity used for BOTH backends, so the comparison is apples-to-apples (the preallocated
// ring is bounded by construction; the deque queue is given the same bound).
constexpr std::size_t CAP = 1024;

struct RunResult {
    double seconds = 0.0;
    long long consumed = 0;
    long long checksum = 0;
    long long expected = 0;
    bool conserved = false;
};

// One MPMC run: `n` producers and `n` consumers move `total_items` items through the queue.
template <QueueBackend Backend>
// NOLINTNEXTLINE(readability-function-size) -- one coherent thread-orchestration-and-measurement kernel for a single MPMC run; producer/consumer lambdas share atomics and the queue by reference, so splitting would only add parameter-heavy helpers around the same shared mutable state
RunResult run(int n, long long total_items) {
    GMPMCQueueT<std::uint64_t, CAP, Backend> q;

    // Split the fixed total as evenly as possible across the producers.
    const long long base = total_items / n;
    const long long extra = total_items % n;

    std::atomic<long long> consumed{0};
    std::atomic<long long> checksum{0};

    std::vector<std::thread> cons;
    cons.reserve(static_cast<std::size_t>(n));
    for(int c = 0; c < n; ++c) {
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

    const auto t0 = clk::now();
    std::vector<std::thread> prod;
    prod.reserve(static_cast<std::size_t>(n));
    long long expected_sum = 0;
    for(int p = 0; p < n; ++p) {
        const long long count = base + (p < extra ? 1 : 0);
        expected_sum += count * (count - 1) / 2; // each producer pushes 0..count-1
        prod.emplace_back([&q, count]() {
            for(long long i = 0; i < count; ++i) {
                (void)q.push(static_cast<std::uint64_t>(i));
            }
        });
    }
    for(auto &t : prod) t.join();
    q.close();
    for(auto &t : cons) t.join();
    const auto t1 = clk::now();

    RunResult r;
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    r.consumed = consumed.load();
    r.checksum = checksum.load();
    r.expected = expected_sum;
    r.conserved = (r.consumed == total_items) && (r.checksum == r.expected);
    return r;
}

double mitems_per_s(const RunResult &r) {
    return (r.seconds > 0.0) ? (static_cast<double>(r.consumed) / r.seconds / 1e6) : 0.0;
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    bool quick = false;
    long long total_items = 0;
    int max_n = 16;
    for(int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if(a == "--quick") quick = true;
        else if(a == "--items" && i + 1 < argc) total_items = std::atoll(argv[++i]);
        else if(a == "--max" && i + 1 < argc) max_n = std::atoi(argv[++i]);
    }
    if(max_n < 1) max_n = 1;
    if(max_n > 64) max_n = 64;

    // Fixed total work per configuration, so the wall-clock stays bounded as N grows.
    const long long items = (total_items > 0) ? total_items : (quick ? 1'000'000 : 8'000'000);

    std::printf("GMPMCQueueT backend throughput sweep (capacity=%zu, fixed total=%lld items)\n",
                CAP, items);
    std::printf("readers == writers; each row runs N producers + N consumers%s\n",
                quick ? " (quick)" : "");
    std::printf("%-4s | %14s | %14s | %8s | %s\n",
                "N", "Deque Mit/s", "Prealloc Mit/s", "P/D", "conserve");
    std::printf("-----+----------------+----------------+----------+---------\n");

    for(int n = 1; n <= max_n; ++n) {
        const RunResult d = run<QueueBackend::Deque>(n, items);
        const RunResult p = run<QueueBackend::Preallocated>(n, items);
        const double dm = mitems_per_s(d);
        const double pm = mitems_per_s(p);
        const double ratio = (dm > 0.0) ? (pm / dm) : 0.0;
        const bool ok = d.conserved && p.conserved;
        std::printf("%-4d | %14.2f | %14.2f | %8.2f | %s\n",
                    n, dm, pm, ratio, ok ? "OK" : "*** FAIL ***");
    }

    std::printf("\nP/D > 1.0 means the preallocated ring was faster than the deque queue at that "
                "reader/writer count.\n");
    return 0;
}
