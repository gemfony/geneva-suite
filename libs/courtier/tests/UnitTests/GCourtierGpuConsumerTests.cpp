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
 * that computes on the host. They cover what the consumer itself is responsible for: batches of
 * several concurrent submitters reach the device one at a time and from one thread, and only the
 * slots that are due are evaluated.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "GDemoProcessingContainers.hpp"
#include "common/concurrency/GCompletionLatch.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/gpu/GGPUConsumer.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GSubmissionPolicy;
using Gem::Courtier::processingStatus;
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
        double *results_out,
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
            results_out[i] = params[static_cast<std::size_t>(i) * static_cast<std::size_t>(dim)];
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

    void scatter(
        std::span<const item_ptr> items,
        const std::vector<double> &fitness,
        double /*evaluation_time*/
    ) const override {
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

/** @brief A backend that "evaluates" on the host (the fitness of an item is its first parameter)
 *  and counts the items it was asked for. */
class CountingBackend final : public GGPUDeviceBackendI<double> {
public:
    explicit CountingBackend(std::shared_ptr<std::size_t> n_evaluated)
      : n_evaluated_(std::move(n_evaluated)) { /* nothing */
    }

    void initialize(const KernelSpec & /*spec*/) override { /* nothing to compile */
    }

    void evaluate(
        const double *params,
        int n_items,
        int dim,
        const std::byte * /*pconst*/,
        std::size_t /*pconst_size*/,
        double *results_out,
        int /*threads_per_item*/
    ) override {
        *n_evaluated_ += static_cast<std::size_t>(n_items);
        for(int i = 0; i < n_items; ++i) {
            results_out[i] = params[static_cast<std::size_t>(i) * static_cast<std::size_t>(dim)];
        }
    }

    [[nodiscard]] std::string name() const override {
        return "counting";
    }

private:
    std::shared_ptr<std::size_t> n_evaluated_;
};

/** @brief Marshals a GFaultyContainer as its stored number. The item with number @p missing is
 *  left unprocessed the first time it comes back (as if it had gone missing), so the consumer
 *  sees a second round in which the other items are already processed. Each item's stored
 *  result tells whether it was the first result given to that item. Null items are counted, not
 *  touched. */
class SelectiveMarshaller final : public GGPUEvaluableI<GFaultyContainer, double> {
public:
    explicit SelectiveMarshaller(std::size_t missing)
      : missing_(missing) { /* nothing */
    }

    [[nodiscard]] std::size_t itemDimension(const item_ptr & /*item*/) const override {
        return 1;
    }

    void flatten(std::span<const item_ptr> items, std::vector<double> &params_out) const override {
        params_out.resize(items.size());
        for(std::size_t i = 0; i < items.size(); ++i) {
            if(not items[i]) {
                ++nullItemsSeen;
                params_out[i] = 0.;
                continue;
            }
            params_out[i] = static_cast<double>(items[i]->get_stored_number());
        }
    }

    void scatter(
        std::span<const item_ptr> items,
        const std::vector<double> & /*fitness*/,
        double /*evaluation_time*/
    ) const override {
        for(const auto &item : items) {
            if(not item) {
                ++nullItemsSeen;
                continue;
            }
            const std::size_t visits = ++scatterVisits[item->get_stored_number()];
            if(item->get_stored_number() == missing_ && visits == 1) {
                continue; // stays DO_PROCESS: missing, resubmitted by the consumer
            }
            const bool first_result = marked_.insert(item->get_stored_number()).second;
            item->markAsProcessedWith({first_result});
        }
    }

    mutable std::size_t nullItemsSeen = 0; ///< Null slots handed to the marshaller
    mutable std::map<std::size_t, std::size_t> scatterVisits; ///< Scatters per stored number

private:
    std::size_t missing_;
    mutable std::set<std::size_t> marked_; ///< The stored numbers already given a result
};

/** @brief The order in which the lifecycle steps and the device saw a batch. Written on the
 *  device thread, read by the test after processBatch() has returned. */
std::vector<std::string> &lifecycleLog() {
    static std::vector<std::string> log;
    return log;
}

/** @brief A pre-processor of a GFaultyContainer that only logs "pre". */
class LoggingPreProcessor final
  : public Gem::Common::GSerializableFunctionObjectT<GFaultyContainer> {
public:
    LoggingPreProcessor() = default;

protected:
    bool process_(GFaultyContainer & /*p*/) override {
        lifecycleLog().emplace_back("pre");
        return true;
    }

private:
    [[nodiscard]] Gem::Common::GSerializableFunctionObjectT<GFaultyContainer> *
    clone_() const override {
        // The raw return is the clone_() contract.
        return new LoggingPreProcessor(*this);
    }
};

/** @brief A backend that logs "device", stays inside evaluate() for 20 ms and returns the first
 *  parameter of each item. */
class LoggingBackend final : public GGPUDeviceBackendI<double> {
public:
    void initialize(const KernelSpec & /*spec*/) override { /* nothing to compile */
    }

    void evaluate(
        const double *params,
        int n_items,
        int dim,
        const std::byte * /*pconst*/,
        std::size_t /*pconst_size*/,
        double *results_out,
        int /*threads_per_item*/
    ) override {
        lifecycleLog().emplace_back("device");
        std::this_thread::sleep_for(20ms);
        for(int i = 0; i < n_items; ++i) {
            results_out[i] = params[static_cast<std::size_t>(i) * static_cast<std::size_t>(dim)];
        }
    }

    [[nodiscard]] std::string name() const override {
        return "logging";
    }
};

/** @brief A marshaller that keeps the processing rule of GGPUEvaluableI for a
 *  GProcessingContainerT item: flatten() begins each item, scatter() completes it. */
class LifecycleMarshaller final : public GGPUEvaluableI<GFaultyContainer, double> {
public:
    [[nodiscard]] std::size_t itemDimension(const item_ptr & /*item*/) const override {
        return 1;
    }

    void flatten(std::span<const item_ptr> items, std::vector<double> &params_out) const override {
        params_out.resize(items.size());
        for(std::size_t i = 0; i < items.size(); ++i) {
            try {
                items[i]->beginProcessing();
            }
            catch(const Gem::Courtier::g_processing_exception &) {
                // recorded on the item; it fails alone
            }
            params_out[i] = static_cast<double>(items[i]->get_stored_number());
        }
    }

    void scatter(
        std::span<const item_ptr> items,
        const std::vector<double> & /*fitness*/,
        const double evaluation_time
    ) const override {
        for(const auto &item : items) {
            if(not item->is_due_for_processing()) {
                continue;
            }
            try {
                item->completeProcessing({true}, evaluation_time);
            }
            catch(const Gem::Courtier::g_processing_exception &) {
                // recorded on the item; it fails alone
            }
        }
    }
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
    "courtier(gpu): batches of concurrent submitters reach the device one at a time and from one "
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
    Gem::Common::Concurrency::GCompletionLatch start{1};
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

// One linear scenario: the cognitive-complexity score comes from the expansion of the Catch2
// assertion macros in the per-slot loop, not from branching.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE(
    "courtier(gpu): only the slots that are due are evaluated; null slots are skipped",
    "[courtier][gpu]"
) {
    constexpr std::size_t MISSING = 2;

    auto n_evaluated = std::make_shared<std::size_t>(0);
    auto marshaller = std::make_shared<SelectiveMarshaller>(MISSING);
    GGPUConsumerT<GFaultyContainer, double> consumer(
        std::make_unique<CountingBackend>(n_evaluated),
        marshaller
    );

    // A null slot first, then three items. Item MISSING does not come back in the first round, so
    // the second round has a null slot, two processed slots and one due slot.
    std::vector<item_ptr> batch;
    batch.push_back(nullptr);
    batch.push_back(std::make_unique<GFaultyContainer>(1, fault_mode::NONE));
    batch.push_back(std::make_unique<GFaultyContainer>(MISSING, fault_mode::NONE));
    batch.push_back(std::make_unique<GFaultyContainer>(3, fault_mode::NONE));
    const GFaultyContainer *const first_address = batch[1].get();

    consumer.processBatch(
        std::span<item_ptr>(batch.data(), batch.size()),
        GSubmissionPolicy::full_success_or_fatal()
    );

    CHECK(*n_evaluated == 4); // three items in the first round, the missing one in the second
    CHECK(marshaller->nullItemsSeen == 0);
    CHECK(batch[0] == nullptr);
    CHECK(batch[1].get() == first_address); // the items stay in their slots
    for(std::size_t slot = 1; slot < batch.size(); ++slot) {
        REQUIRE(batch[slot]);
        CHECK(batch[slot]->getProcessingStatus() == processingStatus::PROCESSED);
        // Evaluated once only: a processed slot was not evaluated again in the second round.
        CHECK(batch[slot]->getStoredResult() == true);
        CHECK(
            marshaller->scatterVisits[batch[slot]->get_stored_number()] ==
            (slot == MISSING ? 2U : 1U)
        );
    }
}

TEST_CASE(
    "courtier(gpu): a processing container keeps the processing contract on the GPU path",
    "[courtier][gpu]"
) {
    // The processing can be split around a device evaluation for every processable item, not only
    // for geneva's individuals: GProcessable::beginProcessing() and
    // GProcessingContainerT::completeProcessing().
    lifecycleLog().clear();
    GGPUConsumerT<GFaultyContainer, double> consumer(
        std::make_unique<LoggingBackend>(),
        std::make_shared<LifecycleMarshaller>()
    );
    std::vector<item_ptr> batch = makeBatch(2, 1);
    for(auto &item : batch) {
        item->registerPreProcessor(std::make_shared<LoggingPreProcessor>());
    }

    consumer.processBatch(
        std::span<item_ptr>(batch.data(), batch.size()),
        GSubmissionPolicy::full_success_or_fatal()
    );

    CHECK(lifecycleLog() == std::vector<std::string>{"pre", "pre", "device"});
    for(const auto &item : batch) {
        CHECK(item->getProcessingStatus() == processingStatus::PROCESSED);
        CHECK(item->getStoredResult() == true);
        CHECK(std::get<1>(item->getProcessingTimes()) >= 0.02); // the device evaluation
    }

    GFaultyContainer &item = *batch[0];
    item.set_processing_status(processingStatus::DO_PROCESS);
    item.beginProcessing();

    // A copy of a begun item is not begun
    GFaultyContainer copy(item);
    CHECK_THROWS_AS(copy.completeProcessing({true}), Gem::Common::gem_exception);

    // Results given to a begun item belong to completeProcessing(): process() would run the
    // pre-processor a second time.
    CHECK_THROWS_AS(item.process({true}), Gem::Common::gem_exception);
    CHECK(item.getProcessingStatus() == processingStatus::DO_PROCESS);
    CHECK_NOTHROW(item.completeProcessing({true}));
    CHECK(item.getProcessingStatus() == processingStatus::PROCESSED);

    // A new submission of a begun item must be begun again
    item.set_processing_status(processingStatus::DO_PROCESS);
    item.beginProcessing();
    item.mark_as_due_for_processing();
    CHECK_THROWS_AS(item.completeProcessing({true}), Gem::Common::gem_exception);
}
