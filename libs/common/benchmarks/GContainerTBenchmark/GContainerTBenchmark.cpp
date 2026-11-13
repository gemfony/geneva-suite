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
 * @file GContainerTBenchmark.cpp
 *
 * @brief Micro-benchmark comparing GContainerT (GPodContainerT / GPtrContainerT)
 *        against raw std::vector and std::deque for push_back, random access,
 *        sort, erase, and clone-based operations.
 *
 * Usage:
 *   GContainerTBenchmark [--mode pod|ptr|fuzz|all]
 *                        [--container vector|deque|all]
 *                        [--size <n>]
 *                        [--duration <secs>]
 *                        [--seed <n>]
 *                        [--quick]
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/GContainerT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"

/******************************************************************************/
// Anonymous-namespace helpers

namespace {

/******************************************************************************/
// Minimal cloneable type for SharedPtrStorage benchmarks

struct BenchObj : Gem::Common::gemfony_common_interface_indicator {
    double data = 0.0;

    BenchObj() = default;
    explicit BenchObj(double v)
      : data(v) {}
    virtual ~BenchObj() = default;

    template <typename TargetType = BenchObj>
    [[nodiscard]] std::shared_ptr<TargetType> clone() const {
        return std::make_shared<TargetType>(*this);
    }

    void load(const std::shared_ptr<BenchObj>& cp) {
        data = cp->data;
    }

    bool operator==(const BenchObj &o) const {
        return data == o.data;
    }

    // Required by Gem::Common::compare_base when SharedPtrStorage is used
    void compare(
        const BenchObj &cp,
        Gem::Common::expectation e,
        double limit
    ) const {
        using namespace Gem::Common;
        if(e == expectation::EQUALITY || e == expectation::FP_SIMILARITY) {
            double const diff = (data > cp.data) ? (data - cp.data) : (cp.data - data);
            if(diff > limit) {
                throw g_expectation_violation("BenchObj::compare: values differ");
            }
        }
        else if(e == expectation::INEQUALITY) {
            double const diff = (data > cp.data) ? (data - cp.data) : (cp.data - data);
            if(diff <= limit) {
                throw g_expectation_violation("BenchObj::compare: values are equal");
            }
        }
    }
};

/******************************************************************************/
// Concrete GContainerT subclasses

class PodVecBench : public Gem::Common::GPodContainerT<double> {
public:
    PodVecBench() = default;
    ~PodVecBench() override = default;
};

class PodDeqBench
    : public Gem::Common::GContainerT<double, Gem::Common::PodStorage<double, std::deque<double>>> {
public:
    PodDeqBench() = default;
    ~PodDeqBench() override = default;
};

class PtrVecBench : public Gem::Common::GPtrContainerT<BenchObj> {
public:
    PtrVecBench() = default;
    ~PtrVecBench() override = default;
};

/******************************************************************************/
// Timing helpers

using Clock = std::chrono::high_resolution_clock;
using Ns = std::chrono::nanoseconds;

struct BenchResult {
    std::string mode;
    std::string containerLabel;
    std::string backendLabel;
    double opsPerSec = 0.0;
    double nsPerOp = 0.0;
};

/******************************************************************************/
// Table printer

void printHeader() {
    std::cout << std::left << std::setw(6) << "Mode" << " | " << std::setw(16) << "Container"
              << " | " << std::setw(12) << "Backend" << " | " << std::right << std::setw(12)
              << "Ops/s" << " | " << std::setw(9) << "ns/Op" << '\n';
    std::cout << std::string(70, '-') << '\n';
}

void printRow(const BenchResult &r) {
    std::cout << std::left << std::setw(6) << r.mode << " | " << std::setw(16) << r.containerLabel
              << " | " << std::setw(12) << r.backendLabel << " | " << std::right << std::setw(12)
              << static_cast<long long>(r.opsPerSec) << " | " << std::setw(9)
              << static_cast<long long>(r.nsPerOp) << '\n';
}

/******************************************************************************/
// POD benchmark — vector backend

BenchResult benchPodVector(std::size_t n) {
    // Warm up
    PodVecBench warm;
    warm.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        warm.push_back(static_cast<double>(i));
    }

    auto t0 = Clock::now();
    std::size_t iterations = 0;

    PodVecBench c;
    c.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        c.push_back(static_cast<double>(i));
    }
    iterations += n;

    // Random access sum
    volatile double sum = 0.0;
    for(std::size_t i = 0; i < n; ++i) {
        sum += c[i];
    }
    iterations += n;

    // Erase half, push again
    if(!c.empty()) {
        c.erase(c.begin() + static_cast<std::ptrdiff_t>(n / 2), c.end());
    }
    iterations += n / 2;
    for(std::size_t i = 0; i < n / 2; ++i) {
        c.push_back(static_cast<double>(i));
    }
    iterations += n / 2;

    auto t1 = Clock::now();
    double const totalNs = static_cast<double>(std::chrono::duration_cast<Ns>(t1 - t0).count());
    double const nsPerOp = totalNs / static_cast<double>(iterations);

    BenchResult r;
    r.mode = "pod";
    r.containerLabel = "GContainerT";
    r.backendLabel = "std::vector";
    r.nsPerOp = nsPerOp;
    r.opsPerSec = (nsPerOp > 0.0) ? (1.0e9 / nsPerOp) : 0.0;
    return r;
}

/******************************************************************************/
// POD benchmark — raw std::vector (baseline)

BenchResult benchPodVectorBaseline(std::size_t n) {
    auto t0 = Clock::now();
    std::size_t iterations = 0;

    std::vector<double> c;
    c.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        c.push_back(static_cast<double>(i));
    }
    iterations += n;

    volatile double sum = 0.0;
    for(std::size_t i = 0; i < n; ++i) {
        sum += c[i];
    }
    iterations += n;

    if(!c.empty()) {
        c.erase(c.begin() + static_cast<std::ptrdiff_t>(n / 2), c.end());
    }
    iterations += n / 2;
    for(std::size_t i = 0; i < n / 2; ++i) {
        c.push_back(static_cast<double>(i));
    }
    iterations += n / 2;

    auto t1 = Clock::now();
    double const totalNs = static_cast<double>(std::chrono::duration_cast<Ns>(t1 - t0).count());
    double const nsPerOp = totalNs / static_cast<double>(iterations);

    BenchResult r;
    r.mode = "pod";
    r.containerLabel = "std::vector";
    r.backendLabel = "std::vector";
    r.nsPerOp = nsPerOp;
    r.opsPerSec = (nsPerOp > 0.0) ? (1.0e9 / nsPerOp) : 0.0;
    return r;
}

/******************************************************************************/
// POD benchmark — deque backend

BenchResult benchPodDeque(std::size_t n) {
    auto t0 = Clock::now();
    std::size_t iterations = 0;

    PodDeqBench c;
    for(std::size_t i = 0; i < n; ++i) {
        c.push_back(static_cast<double>(i));
    }
    iterations += n;

    // push_front (available for deque)
    for(std::size_t i = 0; i < n / 10; ++i) {
        c.push_front(static_cast<double>(i));
    }
    iterations += n / 10;

    volatile double sum = 0.0;
    // NOLINTNEXTLINE(modernize-loop-convert) -- benchmark deliberately measures operator[] indexed access
    for(std::size_t i = 0; i < c.size(); ++i) {
        sum += c[i];
    }
    iterations += c.size();

    auto t1 = Clock::now();
    double const totalNs = static_cast<double>(std::chrono::duration_cast<Ns>(t1 - t0).count());
    double const nsPerOp = totalNs / static_cast<double>(iterations);

    BenchResult r;
    r.mode = "pod";
    r.containerLabel = "GContainerT";
    r.backendLabel = "std::deque";
    r.nsPerOp = nsPerOp;
    r.opsPerSec = (nsPerOp > 0.0) ? (1.0e9 / nsPerOp) : 0.0;
    return r;
}

/******************************************************************************/
// POD benchmark — raw std::deque (baseline)

BenchResult benchPodDequeBaseline(std::size_t n) {
    auto t0 = Clock::now();
    std::size_t iterations = 0;

    std::deque<double> c;
    for(std::size_t i = 0; i < n; ++i) {
        c.push_back(static_cast<double>(i));
    }
    iterations += n;

    for(std::size_t i = 0; i < n / 10; ++i) {
        c.push_front(static_cast<double>(i));
    }
    iterations += n / 10;

    volatile double sum = 0.0;
    // NOLINTNEXTLINE(modernize-loop-convert) -- benchmark deliberately measures operator[] indexed access
    for(std::size_t i = 0; i < c.size(); ++i) {
        sum += c[i];
    }
    iterations += c.size();

    auto t1 = Clock::now();
    double const totalNs = static_cast<double>(std::chrono::duration_cast<Ns>(t1 - t0).count());
    double const nsPerOp = totalNs / static_cast<double>(iterations);

    BenchResult r;
    r.mode = "pod";
    r.containerLabel = "std::deque";
    r.backendLabel = "std::deque";
    r.nsPerOp = nsPerOp;
    r.opsPerSec = (nsPerOp > 0.0) ? (1.0e9 / nsPerOp) : 0.0;
    return r;
}

/******************************************************************************/
// Smart-pointer benchmark — vector backend

BenchResult benchPtrVector(std::size_t n) {
    auto t0 = Clock::now();
    std::size_t iterations = 0;

    PtrVecBench c;
    c.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        c.push_back_clone(std::make_shared<BenchObj>(static_cast<double>(i)));
    }
    iterations += n;

    // sum via access
    volatile double sum = 0.0;
    // NOLINTNEXTLINE(modernize-loop-convert) -- benchmark deliberately measures operator[] indexed access
    for(std::size_t i = 0; i < c.size(); ++i) {
        sum += c[i]->data;
    }
    iterations += n;

    // crossOver at midpoint
    PtrVecBench c2;
    c2.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        c2.push_back_clone(std::make_shared<BenchObj>(static_cast<double>(n - i)));
    }
    c.crossOver(c2, n / 2);
    iterations += n;

    auto t1 = Clock::now();
    double const totalNs = static_cast<double>(std::chrono::duration_cast<Ns>(t1 - t0).count());
    double const nsPerOp = totalNs / static_cast<double>(iterations);

    BenchResult r;
    r.mode = "ptr";
    r.containerLabel = "GContainerT";
    r.backendLabel = "std::vector";
    r.nsPerOp = nsPerOp;
    r.opsPerSec = (nsPerOp > 0.0) ? (1.0e9 / nsPerOp) : 0.0;
    return r;
}

/******************************************************************************/
// Smart-pointer benchmark — raw std::vector baseline

BenchResult benchPtrVectorBaseline(std::size_t n) {
    auto t0 = Clock::now();
    std::size_t iterations = 0;

    std::vector<std::shared_ptr<BenchObj>> c;
    c.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        auto src = std::make_shared<BenchObj>(static_cast<double>(i));
        c.push_back(std::make_shared<BenchObj>(*src)); // explicit clone
    }
    iterations += n;

    volatile double sum = 0.0;
    // NOLINTNEXTLINE(modernize-loop-convert) -- benchmark deliberately measures operator[] indexed access
    for(std::size_t i = 0; i < c.size(); ++i) {
        sum += c[i]->data;
    }
    iterations += n;

    // simulate crossover
    std::vector<std::shared_ptr<BenchObj>> c2;
    c2.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        c2.push_back(std::make_shared<BenchObj>(static_cast<double>(n - i)));
    }
    std::size_t const mid = n / 2;
    for(std::size_t i = mid; i < n; ++i) {
        std::swap(c[i], c2[i]);
    }
    iterations += n;

    auto t1 = Clock::now();
    double const totalNs = static_cast<double>(std::chrono::duration_cast<Ns>(t1 - t0).count());
    double const nsPerOp = totalNs / static_cast<double>(iterations);

    BenchResult r;
    r.mode = "ptr";
    r.containerLabel = "std::vector";
    r.backendLabel = "std::vector";
    r.nsPerOp = nsPerOp;
    r.opsPerSec = (nsPerOp > 0.0) ? (1.0e9 / nsPerOp) : 0.0;
    return r;
}

/******************************************************************************/
// Fuzz / stress-test mode

// NOLINTNEXTLINE(readability-function-size) -- one switch-driven random-operation dispatcher (push/pop/access/erase on both containers) driving the fuzz loop; splitting would scatter the tightly-coupled op-selection logic
void runFuzz(std::size_t n, int durationSecs, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> opDist(0, 5);
    std::uniform_real_distribution<double> valDist(0.0, 1.0);

    PodVecBench pod;
    pod.reserve(n);
    PtrVecBench ptr;
    ptr.reserve(n / 10);

    // Seed with initial data
    for(std::size_t i = 0; i < n; ++i) {
        pod.push_back(valDist(rng));
    }
    for(std::size_t i = 0; i < n / 100; ++i) {
        ptr.push_back_clone(std::make_shared<BenchObj>(valDist(rng)));
    }

    auto deadline = Clock::now() + std::chrono::seconds(durationSecs);
    std::size_t opCount = 0;
    double dummy = 0.0; // Prevent optimiser from eliding accesses

    while(Clock::now() < deadline) {
        int const op = opDist(rng);
        switch(op) {
        case 0: // push_back
            pod.push_back(valDist(rng));
            ++opCount;
            break;
        case 1: // pop_back
            if(!pod.empty()) {
                pod.pop_back();
            }
            ++opCount;
            break;
        case 2: // random access
            if(!pod.empty()) {
                std::uniform_int_distribution<std::size_t> idxDist(0, pod.size() - 1);
                dummy += pod[idxDist(rng)];
            }
            ++opCount;
            break;
        case 3: // erase middle
            if(pod.size() > 2) {
                auto mid = pod.begin() + static_cast<std::ptrdiff_t>(pod.size() / 2);
                pod.erase(mid);
            }
            ++opCount;
            break;
        case 4: // ptr push_back_clone
            ptr.push_back_clone(std::make_shared<BenchObj>(valDist(rng)));
            ++opCount;
            break;
        case 5: // ptr pop
            if(!ptr.empty()) {
                ptr.pop_back();
            }
            ++opCount;
            break;
        }
    }

    (void)dummy;
    std::cout << "Fuzz: completed " << opCount << " random operations over " << durationSecs
              << "s.\n";
}

/******************************************************************************/
// CLI parsing

struct Config {
    std::string mode = "all";
    std::string container = "all";
    std::size_t size = 100000;
    int duration = 60;
    std::uint64_t seed = std::random_device{}();
    bool quick = false;
};

Config parseArgs(int argc, char *argv[]) {
    Config cfg;
    for(int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        if(arg == "--mode" && i + 1 < argc) {
            cfg.mode = argv[++i];
        }
        else if(arg == "--container" && i + 1 < argc) {
            cfg.container = argv[++i];
        }
        else if(arg == "--size" && i + 1 < argc) {
            cfg.size = static_cast<std::size_t>(std::stoul(argv[++i]));
        }
        else if(arg == "--duration" && i + 1 < argc) {
            cfg.duration = std::stoi(argv[++i]);
        }
        else if(arg == "--seed" && i + 1 < argc) {
            cfg.seed = static_cast<std::uint64_t>(std::stoull(argv[++i]));
        }
        else if(arg == "--quick") {
            cfg.quick = true;
        }
        else if(arg == "--help" || arg == "-h") {
            std::cout << "Usage: GContainerTBenchmark [options]\n"
                      << "  --mode pod|ptr|fuzz|all   What to benchmark\n"
                      << "  --container vector|deque|all  Backend container\n"
                      << "  --size <n>                Element count (default 100000)\n"
                      << "  --duration <s>            Fuzz duration in seconds (default 60)\n"
                      << "  --seed <n>                RNG seed for fuzz mode\n"
                      << "  --quick                   Set size=1000, duration=5\n";
            std::exit(0);
        }
    }
    if(cfg.quick) {
        cfg.size = 1000;
        cfg.duration = 5;
    }
    return cfg;
}

/******************************************************************************/

} // namespace

/******************************************************************************/
// main

int main(int argc, char *argv[]) {
    Config const cfg = parseArgs(argc, argv);

    std::cout << "GContainerT Benchmark\n";
    std::cout << "  mode=" << cfg.mode << "  container=" << cfg.container
              << "  size=" << cfg.size << "  duration=" << cfg.duration << "s"
              << "  seed=" << cfg.seed << "\n\n";

    bool const doPod = (cfg.mode == "pod" || cfg.mode == "all");
    bool const doPtr = (cfg.mode == "ptr" || cfg.mode == "all");
    bool const doFuzz = (cfg.mode == "fuzz" || cfg.mode == "all");

    bool const doVec = (cfg.container == "vector" || cfg.container == "all");
    bool const doDeq = (cfg.container == "deque" || cfg.container == "all");

    if(doPod || doPtr) {
        printHeader();

        if(doPod && doVec) {
            printRow(benchPodVector(cfg.size));
            printRow(benchPodVectorBaseline(cfg.size));
        }
        if(doPod && doDeq) {
            printRow(benchPodDeque(cfg.size));
            printRow(benchPodDequeBaseline(cfg.size));
        }
        if(doPtr && doVec) {
            printRow(benchPtrVector(cfg.size));
            printRow(benchPtrVectorBaseline(cfg.size));
        }
    }

    if(doFuzz) {
        std::cout << "\n--- Fuzz mode ---\n";
        runFuzz(cfg.size, cfg.duration, cfg.seed);
    }

    return 0;
}
