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
#include <span>
#include <vector>

#include "GDemoProcessingContainers.hpp"
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

// For the polymorphic work items of the clone case
#include <stdexcept>
#include <string>

#include "courtier/GProcessingContainerT.hpp"

using namespace Gem::Courtier;
using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;

/** @brief Wires a local consumer of type @p consumer_type (built with @p n_threads worker threads,
 *  0 == hardware concurrency) for a test. */
template <typename consumer_type>
struct LocalFixtureT {
    explicit LocalFixtureT(unsigned int n_threads = 0)
      : consumer(
            std::make_shared<consumer_type>(
                typename consumer_type::config_type{.nThreads = n_threads}
            )
        ) {
    }

    std::shared_ptr<GBaseConsumerT<GFaultyContainer>> consumer;

    /** @brief Reconciles the batch in place against the policy via the consumer directly. */
    void workOn(
        std::vector<item_ptr> &items,
        const GSubmissionPolicy &policy,
        item_ptr clone_template = nullptr
    ) {
        consumer->processBatch(
            std::span<item_ptr>(items.data(), items.size()),
            policy,
            std::move(clone_template)
        );
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
        const bool faulty = std::ranges::contains(faulty_indices, i);
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

/******************************************************************************/
// A work item whose type is a polymorphic base: clone_item_() must copy it through its own clone(),
// as a copy construction of the base would slice a derived item.
namespace {

/** @brief A concrete, copy-constructible base item with a virtual clone() and a kind() that names
 *  the dynamic type. Built to fail, its process_() throws. */
class ShapeItem : public GProcessingContainerT<ShapeItem, bool> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GProcessingContainerT<ShapeItem, bool>>(
            ar,
            "GProcessingContainerT_ShapeItem",
            *this
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief An item due for processing
     *  @param fail Whether process_() is to throw */
    explicit ShapeItem(bool fail)
      : fail_(fail) {
        this->mark_as_due_for_processing();
    }
    /** @brief For deserialization only */
    ShapeItem() = default;
    ShapeItem(const ShapeItem &) = default;
    ShapeItem &operator=(const ShapeItem &) = default;
    ShapeItem(ShapeItem &&) noexcept = default;
    ShapeItem &operator=(ShapeItem &&) noexcept = default;
    ~ShapeItem() override = default;

    /** @brief A deep copy of this item, of its dynamic type @return The copy */
    [[nodiscard]] virtual std::unique_ptr<ShapeItem> clone() const {
        return std::make_unique<ShapeItem>(*this);
    }
    /** @brief The dynamic type of this item @return "base" */
    [[nodiscard]] virtual std::string kind() const {
        return "base";
    }

private:
    /** @brief Throws if the item was built to fail, registers a result otherwise */
    void process_([[maybe_unused]] const std::vector<bool> &res_vec = std::vector<bool>()) final {
        if(fail_) {
            throw std::runtime_error("ShapeItem: built to fail");
        }
        this->registerResult(0, true);
    }

    bool fail_ = false; ///< Whether process_() throws
};

/** @brief A derived item: its clone() keeps the dynamic type. */
class RoundItem final : public ShapeItem {
public:
    using ShapeItem::ShapeItem;

    /** @brief A deep copy of this item, of its dynamic type @return The copy */
    [[nodiscard]] std::unique_ptr<ShapeItem> clone() const override {
        return std::make_unique<RoundItem>(*this);
    }
    /** @brief The dynamic type of this item @return "round" */
    [[nodiscard]] std::string kind() const override {
        return "round";
    }
};

using shape_ptr = std::unique_ptr<ShapeItem>;

/** @brief Four round items, the last built to fail @return The batch */
std::vector<shape_ptr> roundBatch() {
    std::vector<shape_ptr> batch;
    batch.reserve(4);
    for(std::size_t i = 0; i < 4; ++i) {
        batch.push_back(std::make_unique<RoundItem>(/*fail=*/i == 3));
    }
    return batch;
}

} // anonymous namespace

// Two short sections; the complexity counted is that of Catch2's macros.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE(
    "courtier: a work item with clone() is cloned through it and not copy-constructed",
    "[courtier][policy][clone]"
) {
    auto consumer = std::make_shared<GStdThreadConsumerT<ShapeItem>>();
    auto batch = roundBatch();

    SECTION("the refill has the dynamic type of its source") {
        consumer->processBatch(
            std::span<shape_ptr>(batch.data(), batch.size()),
            GSubmissionPolicy::clone_on_partial_return()
        );
        REQUIRE(batch[3]);
        CHECK(batch[3]->is_processed());
        CHECK(batch[3]->kind() == "round");
        CHECK(dynamic_cast<const RoundItem *>(batch[3].get()) != nullptr);
    }

    SECTION("a function that was set comes first") {
        consumer->setCloneFunction([](const shape_ptr &src) -> shape_ptr {
            return std::make_unique<ShapeItem>(*src);
        });
        consumer->processBatch(
            std::span<shape_ptr>(batch.data(), batch.size()),
            GSubmissionPolicy::clone_on_partial_return()
        );
        REQUIRE(batch[3]);
        CHECK(batch[3]->is_processed());
        CHECK(batch[3]->kind() == "base");
    }
}

/******************************************************************************/
// The marks a work item carries for a failed evaluation: a refill is marked as a substitute, and an
// item its submitter forbids to substitute is evaluated again instead.

TEST_CASE(
    "courtier: a refilled slot is marked as a substitute until it is submitted again",
    "[courtier][policy][substitute]"
) {
    LocalFixture f;
    auto batch = make_batch(6, {2, 4}, fault_mode::THROW_PROCESSING);

    f.workOn(batch, GSubmissionPolicy::clone_on_partial_return());

    REQUIRE(count_processed(batch) == 6);
    for(std::size_t i = 0; i < batch.size(); ++i) {
        INFO("slot " << i);
        CHECK(batch[i]->isSubstitute() == (i == 2 || i == 4));
    }

    // The refills are copies of items that did not fail: submitted again, every slot is evaluated
    // and none is a substitute any more.
    f.workOn(batch, GSubmissionPolicy::clone_on_partial_return());

    CHECK(count_processed(batch) == 6);
    CHECK(std::ranges::none_of(batch, [](const item_ptr &p) { return p->isSubstitute(); }));
}

namespace {

/** @brief An item that fails its first @p failures evaluations and succeeds afterwards. A local
 *  consumer evaluates it in place, so the count survives a failed evaluation. */
class FlakyItem : public GProcessingContainerT<FlakyItem, bool> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GProcessingContainerT<FlakyItem, bool>>(
            ar,
            "GProcessingContainerT_FlakyItem",
            *this
        );
        Gem::Common::archive_named(ar, "id_", id_);
        Gem::Common::archive_named(ar, "failures_left_", failures_left_);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief An item due for processing
     *  @param id Identifies the item
     *  @param failures How many of its evaluations are to fail */
    FlakyItem(std::size_t id, std::size_t failures)
      : id_(id)
      , failures_left_(failures) {
        this->mark_as_due_for_processing();
    }
    /** @brief For deserialization only */
    FlakyItem() = default;

    /** @brief @return The id given at construction */
    [[nodiscard]] std::size_t id() const {
        return id_;
    }

private:
    /** @brief Throws while failures are left, registers a result otherwise */
    void process_([[maybe_unused]] const std::vector<bool> &res_vec = std::vector<bool>()) final {
        if(failures_left_ > 0) {
            --failures_left_;
            throw std::runtime_error("FlakyItem: built to fail");
        }
        this->registerResult(0, true);
    }

    std::size_t id_ = 0;            ///< Identifies the item
    std::size_t failures_left_ = 0; ///< How many evaluations are still to fail
};

using flaky_ptr = std::unique_ptr<FlakyItem>;

} // anonymous namespace

// Two items checked slot by slot; the complexity counted is that of Catch2's macros.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE(
    "courtier: an item that may not be substituted is evaluated again and not substituted",
    "[courtier][policy][substitute]"
) {
    auto consumer = std::make_shared<GStdThreadConsumerT<FlakyItem>>();
    std::vector<flaky_ptr> batch;
    batch.push_back(std::make_unique<FlakyItem>(0, 0));
    batch.push_back(std::make_unique<FlakyItem>(1, 1));   // fails once, may not be substituted
    batch.push_back(std::make_unique<FlakyItem>(2, 100)); // keeps failing, may be substituted
    batch.push_back(std::make_unique<FlakyItem>(3, 0));
    batch[1]->setSubstitutable(false);
    const FlakyItem *const kept = batch[1].get();

    auto policy = GSubmissionPolicy::clone_on_partial_return();
    policy.max_failed_retries = 1;
    consumer->processBatch(std::span<flaky_ptr>(batch.data(), batch.size()), policy);

    // The item that may not be substituted was evaluated again: the same object, its own result.
    REQUIRE(batch[1].get() == kept);
    CHECK(batch[1]->is_processed());
    CHECK(batch[1]->id() == 1);
    CHECK_FALSE(batch[1]->isSubstitute());
    CHECK_FALSE(batch[1]->isSubstitutable()); // the submitter's mark stays until it is set again

    // The one that may be substituted failed beyond its budget and was refilled.
    CHECK(batch[2]->is_processed());
    CHECK(batch[2]->isSubstitute());
    CHECK(batch[2]->id() != 2);
}
