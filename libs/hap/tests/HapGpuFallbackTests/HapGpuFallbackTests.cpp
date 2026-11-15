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

#include "common/concurrency/GCompletionLatchT.hpp"
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
    /** @brief Holds starting GPU sources at the given stage, after the first n_pass.
     *  @param stage The stage @param n_pass How many sources pass before the others wait */
    explicit GStartHold(GpuSourceStartStage stage, std::size_t n_pass = 0)
      : stage_(stage) {
        holdGpuRandomSourceStartsForTests(stage_, true, n_pass);
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
 * skips the test case if it does not (no CUDA backend in this build, or no device). The first draw,
 * which starts the producer and returns only when its start has finished, runs on a thread of its
 * own, so that the test case can act while the source is held.
 */
class GHeldStart {
public:
    /**
     * @brief Starts the producer and waits for its source at the held stage.
     *
     * @param hold The hold the source will wait at
     * @param stage The stage to wait for
     */
    GHeldStart(GStartHold &hold, GpuSourceStartStage stage)
      : hold_(hold) {
        randomFactory()->setNProducerThreads(1);
        first_draw_ = std::thread([]() { std::ignore = randomFactory()->getNewRandomContainer(); });
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while(nGpuRandomSourceStartsForTests(stage) == 0) {
            if(std::chrono::steady_clock::now() > deadline) {
                first_draw_.join(); // the destructor does not run when the constructor skips
                SKIP("No random-number source starts on the GPU (no CUDA backend or no device)");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /** @brief Lets the held source go and waits for the first draw. */
    ~GHeldStart() {
        hold_.release();
        first_draw_.join();
    }

    GHeldStart(GHeldStart const &) = delete;
    GHeldStart &operator=(GHeldStart const &) = delete;

private:
    GStartHold &hold_;
    std::thread first_draw_;
};

/**
 * Lets the sources held at a stage go once the test case has made its check -- or, when the call under
 * test waits for the held sources, once that has become plain: the sources that were not held have
 * finished their start and the check has not come within 200 ms.
 */
class GReleaseAfterCheck {
public:
    /**
     * @brief Starts the helper thread.
     *
     * @param hold The hold to release
     * @param n_free How many producers finish their start without being held
     */
    GReleaseAfterCheck(GStartHold &hold, std::size_t n_free)
      : thread_([this, &hold, n_free]() {
          while(not checked_.wait_for(std::chrono::milliseconds(200))) {
              if(nGpuRandomSourceStartsForTests(GpuSourceStartStage::START_FINISHED) >= n_free) {
                  if(not checked_.wait_for(std::chrono::milliseconds(200))) {
                      break;
                  }
              }
          }
          hold.release();
      }) { /* nothing */
    }

    /** @brief Tells the helper thread that the check has been made. */
    void checked() {
        if(checked_.remaining() > 0) {
            std::ignore = checked_.count_down();
        }
    }

    /** @brief Releases the hold at once and joins the helper thread. */
    ~GReleaseAfterCheck() {
        checked();
        thread_.join();
    }

    GReleaseAfterCheck(GReleaseAfterCheck const &) = delete;
    GReleaseAfterCheck &operator=(GReleaseAfterCheck const &) = delete;

private:
    Gem::Common::Concurrency::GCompletionLatchT checked_{1};
    std::thread thread_; ///< declared last: it uses checked_
};

/**
 * Takes one container from the factory, trying until one arrives.
 */
void takeOneContainer() {
    REQUIRE(drawUntil([]() {
        return static_cast<bool>(randomFactory()->getNewRandomContainer());
    }));
}

/**
 * Skips the test case if no source has chosen the GPU (no CUDA backend in this build, or no device).
 */
void requireGpuChoice() {
    if(nGpuRandomSourceStartsForTests(GpuSourceStartStage::GPU_CHOSEN) == 0) {
        SKIP("No random-number source starts on the GPU (no CUDA backend or no device)");
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
    GHeldStart start(hold, GpuSourceStartStage::CONTEXT_CREATION);

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
    GHeldStart start(hold, GpuSourceStartStage::CONTEXT_CREATION);

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
    GHeldStart start(hold, GpuSourceStartStage::GPU_CHOSEN);

    setGpuRandomNumbersAllowed(false);
    REQUIRE(nGpuRandomSources() == 0);

    hold.release();
    requireRandomWords(); // the only producer has left its start: its words reached the factory
    // Switched off before it counted itself, the source never began to use the GPU
    REQUIRE(nGpuRandomSourceStartsForTests(GpuSourceStartStage::CONTEXT_CREATION) == 0);
    REQUIRE(nGpuRandomSources() == 0);
}

TEST_CASE(
    "The first draw returns when every producer has finished its start",
    "[start-awaited-first-draw]"
) {
    randomFactory()->setNProducerThreads(2);
    GStartHold hold(GpuSourceStartStage::CONTEXT_CREATION, 1); // the second source waits
    GReleaseAfterCheck release(hold, 1);

    takeOneContainer();
    const std::size_t n_finished =
        nGpuRandomSourceStartsForTests(GpuSourceStartStage::START_FINISHED);
    release.checked();
    requireGpuChoice();
    REQUIRE(n_finished == 2);
}

TEST_CASE(
    "Adding producers returns when they have finished their start",
    "[start-awaited-more-producers]"
) {
    randomFactory()->setNProducerThreads(1);
    takeOneContainer();
    requireGpuChoice();
    REQUIRE(nGpuRandomSourceStartsForTests(GpuSourceStartStage::START_FINISHED) == 1);

    GStartHold hold(GpuSourceStartStage::CONTEXT_CREATION);
    GReleaseAfterCheck release(hold, 1);
    randomFactory()->setNProducerThreads(2);
    const std::size_t n_finished =
        nGpuRandomSourceStartsForTests(GpuSourceStartStage::START_FINISHED);
    release.checked();
    REQUIRE(n_finished == 2);
}

TEST_CASE("The program ends cleanly right after its first draw", "[exit-after-first-draw]") {
    // ctest gives this case 10 seconds. A producer still inside the creation of the process's first
    // CUDA resources when the program ends makes the exit hang or abort. The single source is held just
    // before that creation and let go once the first draw has returned -- with or without a number.
    randomFactory()->setNProducerThreads(1);
    GStartHold hold(GpuSourceStartStage::CONTEXT_CREATION);
    GReleaseAfterCheck release(hold, 0);
    std::ignore = randomFactory()->getNewRandomContainer();
    // Leaving the case lets the source go; the process ends right after
}
