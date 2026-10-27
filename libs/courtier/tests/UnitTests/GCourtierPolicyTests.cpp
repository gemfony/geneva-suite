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
 * Tier-1 unit tests for the courtier submission path: the local (multi-threaded) consumer
 * driving GBaseConsumerT::processBatch() against the various submission policies, using the
 * GFaultyContainer test double to exhibit success / clean error flag / throwing evaluations.
 *
 * Fatal policy paths (full_success_or_fatal hitting an unfixable failure, the zero-usable floor)
 * are intentionally NOT exercised here: they exit the process via LOGEXIT (std::exit), which
 * cannot be caught in-process by Catch2. They are covered by the standalone manual exercisers.
 */

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>
#include <span>

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

using namespace Gem::Courtier;
using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;

/** @brief Wires a local consumer of type @p ConsumerT (built with @p n_threads worker threads,
 *  0 == hardware concurrency) for a test. */
template <typename ConsumerT>
struct LocalFixtureT {
    explicit LocalFixtureT(unsigned int n_threads = 0)
      : consumer(std::make_shared<ConsumerT>(n_threads)) {}

    std::shared_ptr<GBaseConsumerT<GFaultyContainer>> consumer;

    /** @brief Reconciles the batch in place against the policy via the consumer directly. */
    void workOn(std::vector<item_ptr> &items, const GSubmissionPolicy &policy,
                item_ptr clone_template = nullptr) {
        consumer->processBatch(std::span<item_ptr>(items.data(), items.size()), policy,
                               std::move(clone_template));
    }
};

using LocalFixture = LocalFixtureT<GStdThreadConsumerT<GFaultyContainer>>;

/** @brief Builds a batch of @p n items, the ones at @p faulty_indices exhibiting @p fm. */
std::vector<item_ptr> make_batch(
    std::size_t n,
    const std::vector<std::size_t> &faulty_indices = {},
    fault_mode fm = fault_mode::THROW_PROCESSING
) {
    std::vector<item_ptr> v;
    v.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        const bool faulty =
            std::ranges::contains(faulty_indices, i);
        v.push_back(std::make_unique<GFaultyContainer>(i, faulty ? fm : fault_mode::NONE));
    }
    return v;
}

std::size_t count_processed(const std::vector<item_ptr> &v) {
    std::size_t c = 0;
    for(const auto &it : v) {
        if(it && it->is_processed()) {
            ++c;
        }
    }
    return c;
}

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("courtier: a clean batch is fully evaluated", "[courtier][policy]") {
    LocalFixture f;
    auto batch = make_batch(16);

    f.workOn(batch, GSubmissionPolicy::full_success_or_fatal());

    CHECK(batch.size() == 16);
    CHECK(count_processed(batch) == 16);
}

TEST_CASE("courtier: size is preserved across all policies", "[courtier][policy]") {
    LocalFixture f;
    auto batch = make_batch(10, {2, 5, 9}, fault_mode::THROW_PROCESSING);

    f.workOn(batch, GSubmissionPolicy::clone_on_partial_return());

    // The span is fixed-size: clone-on-partial-return refills the failed slots, never shrinks.
    CHECK(batch.size() == 10);
}

TEST_CASE("courtier: clone-on-partial-return refills throwing slots", "[courtier][policy]") {
    LocalFixture f;
    auto batch = make_batch(12, {1, 4, 7, 11}, fault_mode::THROW_PROCESSING);

    f.workOn(batch, GSubmissionPolicy::clone_on_partial_return());

    // Every slot ends up holding a successfully evaluated item (originals or clones of survivors).
    CHECK(count_processed(batch) == 12);
}

TEST_CASE("courtier: clone-on-partial-return handles a clean-error flag", "[courtier][policy]") {
    LocalFixture f;
    auto batch = make_batch(8, {3, 6}, fault_mode::FLAG_ERROR);

    f.workOn(batch, GSubmissionPolicy::clone_on_partial_return());

    CHECK(count_processed(batch) == 8);
}

TEST_CASE("courtier: a single-item clean batch works", "[courtier][policy]") {
    LocalFixture f;
    auto batch = make_batch(1);

    f.workOn(batch, GSubmissionPolicy::fail_on_no_return());

    CHECK(count_processed(batch) == 1);
}

TEST_CASE("courtier: an empty batch is a no-op", "[courtier][policy]") {
    LocalFixture f;
    std::vector<item_ptr> batch;

    f.workOn(batch, GSubmissionPolicy::full_success_or_fatal());

    CHECK(batch.empty());
}

TEST_CASE("courtier: a mostly-faulty batch still recovers via cloning", "[courtier][policy]") {
    LocalFixture f;
    // 9 of 10 throw; the lone survivor seeds the clones.
    auto batch = make_batch(10, {0, 1, 2, 3, 4, 5, 6, 8, 9}, fault_mode::THROW_PROCESSING);

    f.workOn(batch, GSubmissionPolicy::clone_on_partial_return());

    CHECK(count_processed(batch) == 10);
}

/******************************************************************************/
// The reconciliation contract must also hold for the one local consumer in its single-threaded (serial)
// configuration -- one worker thread == serial execution. The multi-threaded case is exercised by the
// LocalFixture tests above.

TEST_CASE(
    "courtier: the local consumer honours the reconciliation contract when single-threaded",
    "[courtier][policy][consumers]"
) {
    LocalFixtureT<GStdThreadConsumerT<GFaultyContainer>> f(1);

    SECTION("clean batch") {
        auto batch = make_batch(8);
        f.workOn(batch, GSubmissionPolicy::full_success_or_fatal());
        CHECK(count_processed(batch) == 8);
    }

    SECTION("clone-on-partial-return refills throwing slots") {
        auto batch = make_batch(8, {2, 5}, fault_mode::THROW_PROCESSING);
        f.workOn(batch, GSubmissionPolicy::clone_on_partial_return());
        CHECK(batch.size() == 8);
        CHECK(count_processed(batch) == 8);
    }

    SECTION("clone-on-partial-return handles a clean-error flag") {
        auto batch = make_batch(8, {1, 6}, fault_mode::FLAG_ERROR);
        f.workOn(batch, GSubmissionPolicy::clone_on_partial_return());
        CHECK(count_processed(batch) == 8);
    }
}

/******************************************************************************/
