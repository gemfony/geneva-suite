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
 * Tests of the GPU consumer that need no device: the consumer is built around a stand-in backend
 * that computes on the host. They cover what the consumer itself is responsible for -- here, that
 * batches of several concurrent submitters reach the device one at a time and from one thread.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <latch>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/gpu/GGPUConsumer.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GSubmissionPolicy;
using Gem::Courtier::GPU::GGPUConsumerT;
using Gem::Courtier::GPU::GGPUDeviceBackendI;
using Gem::Courtier::GPU::GGPUEvaluableI;
using Gem::Courtier::GPU::KernelSpec;
using namespace std::chrono_literals;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;

/** @brief What the stand-ins observe, shared with the test. */
struct Observations {
    std::atomic<int> inside{0};             ///< Callers currently inside the backend
    std::atomic<int> maxInside{0};          ///< The largest number of callers seen inside at once
    std::atomic<std::size_t> mismatches{0}; ///< Results that did not belong to their item
    std::atomic<std::size_t> foreignThreadCalls{
        0
    }; ///< evaluate() calls not made by the first caller's thread
    std::atomic<std::thread::id> deviceThread; ///< The thread of the first evaluate() call
};

/** @brief A backend that "evaluates" on the host: the fitness of an item is its first parameter. It
 *  stays inside evaluate() for a while, so two unserialized callers are certain to overlap. */
class StandInBackend final : public GGPUDeviceBackendI<double> {
public:
    explicit StandInBackend(std::shared_ptr<Observations> seen)
      : seen_(std::move(seen)) { /* nothing */
    }

    void initialize(const KernelSpec & /*spec*/) override { /* nothing to compile */
    }

    void evaluate(
        const double *params,
        int n_items,
        int dim,
        const std::byte * /*pconst*/,
        std::size_t /*pconst_size*/,
        double *fitness_out,
        int /*threads_per_item*/
    ) override {
        std::thread::id expected{}; // no thread yet
        if(not seen_->deviceThread.compare_exchange_strong(expected, std::this_thread::get_id()) &&
           expected != std::this_thread::get_id()) {
            seen_->foreignThreadCalls.fetch_add(1);
        }
        const int now = seen_->inside.fetch_add(1) + 1;
        int seen_max = seen_->maxInside.load();
        while(now > seen_max && not seen_->maxInside.compare_exchange_weak(seen_max, now)) {
            /* retry */
        }
        std::this_thread::sleep_for(20ms);
        for(int i = 0; i < n_items; ++i) {
            fitness_out[i] = params[static_cast<std::size_t>(i) * static_cast<std::size_t>(dim)];
        }
        seen_->inside.fetch_sub(1);
    }

    [[nodiscard]] std::string name() const override {
        return "stand-in";
    }

private:
    std::shared_ptr<Observations> seen_;
};

/** @brief Marshals a GFaultyContainer as one parameter, its stored number, and checks on the way
 *  back that each result is the one computed for its own item. */
class StandInMarshaller final : public GGPUEvaluableI<GFaultyContainer, double> {
public:
    explicit StandInMarshaller(std::shared_ptr<Observations> seen)
      : seen_(std::move(seen)) { /* nothing */
    }

    [[nodiscard]] std::size_t itemDimension(const item_ptr & /*item*/) const override {
        return 1;
    }

    void flatten(std::span<const item_ptr> items, std::vector<double> &params_out) const override {
        params_out.resize(items.size());
        for(std::size_t i = 0; i < items.size(); ++i) {
            params_out[i] = static_cast<double>(items[i]->get_stored_number());
        }
    }

    void
    scatter(std::span<const item_ptr> items, const std::vector<double> &fitness) const override {
        for(std::size_t i = 0; i < items.size(); ++i) {
            if(fitness[i] != static_cast<double>(items[i]->get_stored_number())) {
                seen_->mismatches.fetch_add(1);
            }
            items[i]->markAsProcessedWith({true});
        }
    }

private:
    std::shared_ptr<Observations> seen_;
};

std::vector<item_ptr> makeBatch(std::size_t n, std::size_t first_number) {
    std::vector<item_ptr> v;
    v.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        v.push_back(std::make_unique<GFaultyContainer>(first_number + i, fault_mode::NONE));
    }
    return v;
}

} /* anonymous namespace */

TEST_CASE(
    "courtier(gpu): batches of concurrent submitters reach the device one at a time, from one "
    "thread",
    "[courtier][gpu]"
) {
    constexpr std::size_t N = 64;
    constexpr int ROUNDS = 5;

    auto seen = std::make_shared<Observations>();
    GGPUConsumerT<GFaultyContainer, double> consumer(
        std::make_unique<StandInBackend>(seen),
        std::make_shared<StandInMarshaller>(seen)
    );
    CHECK(consumer.activeBackendName() == "stand-in");

    auto batch_a = makeBatch(N, 0);
    auto batch_b = makeBatch(N, 1000);

    // Both submitters start together and submit several rounds each, as two algorithms sharing the
    // one process consumer would.
    std::latch start{1};
    auto submit = [&](std::vector<item_ptr> &batch) {
        start.wait();
        for(int r = 0; r < ROUNDS; ++r) {
            consumer.processBatch(
                std::span<item_ptr>(batch.data(), batch.size()),
                GSubmissionPolicy::full_success_or_fatal()
            );
        }
    };
    {
        const std::jthread a(submit, std::ref(batch_a));
        const std::jthread b(submit, std::ref(batch_b));
        start.count_down();
    }

    CHECK(seen->maxInside.load() == 1);          // never two batches on the device at once
    CHECK(seen->foreignThreadCalls.load() == 0); // and always from the same thread
    CHECK(seen->deviceThread.load() != std::this_thread::get_id());
    CHECK(seen->mismatches.load() == 0); // every result reached the item it was computed for
    for(const auto &batch : {std::cref(batch_a), std::cref(batch_b)}) {
        for(const auto &it : batch.get()) {
            CHECK(it->is_processed());
        }
    }
}
