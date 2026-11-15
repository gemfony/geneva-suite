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

/*
 * Tests of the GPU switch and the CPU fall-back of Hap's bulk random-number sources.
 *
 * The switch is process-wide and a GPU source, once left, is not re-entered, so each test case runs in
 * its own process: CMakeLists.txt registers one ctest entry per tag. The cases that need a GPU source
 * to start with are skipped (SKIP, exit code 4) in builds without the CUDA backend or without a device.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <thread>
#include <tuple>

#include <catch2/catch_test_macros.hpp>

#include "hap/GRandomFactory.hpp"

using namespace Gem::Hap;

namespace {

/**
 * Pulls containers from the factory and hands them back, so that the producers keep refilling,
 * until the predicate holds or the time limit is reached.
 *
 * @param done The condition to wait for
 * @return Whether the condition held before the time limit
 */
template <typename Pred>
bool drawUntil(Pred done, std::chrono::seconds limit = std::chrono::seconds(30)) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while(not done()) {
        if(std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        if(auto p = randomFactory()->getNewRandomContainer()) {
            randomFactory()->returnUsedPackage(std::move(p));
        }
    }
    return true;
}

/**
 * Checks that the factory still delivers random words: a fresh container holds varying values.
 */
void requireRandomWords() {
    std::unique_ptr<random_container> p;
    REQUIRE(drawUntil([&p]() { return static_cast<bool>(p = randomFactory()->getNewRandomContainer()); }));
    std::set<std::uint64_t> values;
    while(not p->empty()) {
        values.insert(p->next());
    }
    REQUIRE(values.size() > random_container::size() / 2);
}

/**
 * Waits until at least one source draws from the GPU; skips the test case if none does.
 */
void requireGpuSource() {
    if(not drawUntil([]() { return nGpuRandomSources() > 0; }, std::chrono::seconds(10))) {
        SKIP("No random-number source uses the GPU (no CUDA backend in this build, or no device)");
    }
}

/**
 * Holds every source that starts on the GPU at one stage, and lets them go when it leaves scope -- also
 * when a REQUIRE ends the test case, so that no producer waits on the hold while the process ends.
 */
class GStartHold {
public:
    /** @brief Holds starting GPU sources at the given stage. @param stage The stage */
    explicit GStartHold(GpuSourceStartStage stage)
      : stage_(stage) {
        holdGpuRandomSourceStartsForTests(stage_, true);
    }

    /** @brief Lets the held sources go on. */
    void release() {
        holdGpuRandomSourceStartsForTests(stage_, false);
    }

    /** @brief Lets the held sources go on. */
    ~GStartHold() {
        release();
    }

    GStartHold(GStartHold const &) = delete;
    GStartHold &operator=(GStartHold const &) = delete;

private:
    GpuSourceStartStage stage_;
};

/**
 * Starts a single producer thread and waits until its source reaches the given stage of a GPU start;
 * skips the test case if it does not (no CUDA backend in this build, or no device).
 *
 * @param stage The stage to wait for
 */
void startOneSourceAndWaitFor(GpuSourceStartStage stage) {
    randomFactory()->setNProducerThreads(1);
    std::ignore = randomFactory()->getNewRandomContainer(); // starts the producer; it may time out
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(nGpuRandomSourceStartsForTests(stage) == 0) {
        if(std::chrono::steady_clock::now() > deadline) {
            SKIP("No random-number source starts on the GPU (no CUDA backend or no device)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST_CASE("GPU switched off before the first draw: no source uses the GPU", "[gpu-off-at-start]") {
    setGpuRandomNumbersAllowed(false);
    REQUIRE_FALSE(gpuRandomNumbersAllowed());

    for(std::size_t i = 0; i < 4 * GRandomFactory::getBufferSize(); i++) {
        if(auto p = randomFactory()->getNewRandomContainer()) {
            randomFactory()->returnUsedPackage(std::move(p));
        }
        REQUIRE(nGpuRandomSources() == 0);
    }
    requireRandomWords();
}

TEST_CASE("GPU switched off while sources use it: they move to the CPU", "[gpu-off-later]") {
    requireGpuSource();

    setGpuRandomNumbersAllowed(false);
    REQUIRE(drawUntil([]() { return nGpuRandomSources() == 0; }));
    requireRandomWords();
}

TEST_CASE("A failing GPU source moves to the CPU and keeps delivering", "[gpu-failure]") {
    // One producer, so that the source seen on the GPU is the only one: with more, a producer thread
    // that starts only after the simulated failure has ended may legitimately take the GPU.
    randomFactory()->setNProducerThreads(1);
    requireGpuSource();

    simulateGpuRandomFailureForTests(true);
    REQUIRE(drawUntil([]() { return nGpuRandomSources() == 0; }));
    simulateGpuRandomFailureForTests(false);

    // Failed sources stay on the CPU, even though GPU use is still allowed
    REQUIRE(gpuRandomNumbersAllowed());
    requireRandomWords();
    REQUIRE(nGpuRandomSources() == 0);
}

TEST_CASE(
    "GPU switched off while a source creates its GPU resources: the source is counted",
    "[gpu-off-while-creating]"
) {
    GStartHold hold(GpuSourceStartStage::CONTEXT_CREATION);
    startOneSourceAndWaitFor(GpuSourceStartStage::CONTEXT_CREATION);

    setGpuRandomNumbersAllowed(false);
    // The source is about to use the GPU, so a caller waiting for the count to reach 0 must wait for it
    REQUIRE(nGpuRandomSources() == 1);

    hold.release();
    REQUIRE(drawUntil([]() { return nGpuRandomSources() == 0; }));
    requireRandomWords();
}

TEST_CASE(
    "GPU fails while a source creates its GPU resources: the source is counted",
    "[gpu-failure-while-creating]"
) {
    GStartHold hold(GpuSourceStartStage::CONTEXT_CREATION);
    startOneSourceAndWaitFor(GpuSourceStartStage::CONTEXT_CREATION);

    simulateGpuRandomFailureForTests(true);
    REQUIRE(nGpuRandomSources() == 1);

    hold.release();
    REQUIRE(drawUntil([]() { return nGpuRandomSources() == 0; }));
    simulateGpuRandomFailureForTests(false);
    requireRandomWords();
    REQUIRE(nGpuRandomSources() == 0);
}

TEST_CASE(
    "GPU switched off after a source chose it: the source starts on the CPU",
    "[gpu-off-after-choice]"
) {
    GStartHold hold(GpuSourceStartStage::GPU_CHOSEN);
    startOneSourceAndWaitFor(GpuSourceStartStage::GPU_CHOSEN);

    setGpuRandomNumbersAllowed(false);
    REQUIRE(nGpuRandomSources() == 0);

    hold.release();
    requireRandomWords(); // the only producer has left its start: its words reached the factory
    // Switched off before it counted itself, the source never began to use the GPU
    REQUIRE(nGpuRandomSourceStartsForTests(GpuSourceStartStage::CONTEXT_CREATION) == 0);
    REQUIRE(nGpuRandomSources() == 0);
}
