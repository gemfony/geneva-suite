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

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <type_traits>
#include <vector>

#include "common/GArchiveNamed.hpp"     // archive_named
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE

#include "common/GCommonEnums.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GFixedSizePriorityQueueT.hpp"
#include "common/GReflectiveInterfaceT.hpp"

using namespace Gem::Common;

// ===========================================================================
// The fixtures. One item type carrying the Gemfony common interface, and one
// concrete priority queue per storage policy: by value, by shared handle and
// solely owned. Every behavioural case below is then written ONCE per holder,
// so a semantic that silently differs between the three shows up as a red.
// ===========================================================================

namespace {

/** @brief A minimal Geneva-style work item: one double, which is also its priority. */
class TestItem : public GCommonInterfaceT<TestItem> {
public:
    TestItem() = default;
    explicit TestItem(double v)
      : value_(v) {
    }

    [[nodiscard]] double value() const {
        return value_;
    }
    void setValue(double v) {
        value_ = v;
    }

protected:
    void load_(TestItem const *cp) override {
        if(cp != nullptr) {
            value_ = cp->value_;
        }
    }

    void compare_(
        TestItem const &cp,
        expectation const &e,
        [[maybe_unused]] double const &limit
    ) const override {
        GToken token("TestItem", e);
        compare_base_t<GCommonInterfaceT<TestItem>>(*this, cp, token);
        compare_t(Gem::Common::getIdentity(value_, cp.value_, "value_", "cp.value_"), token);
        token.evaluate();
    }

private:
    [[nodiscard]] TestItem *clone_() const override {
        return new TestItem(*this);
    }

    friend struct Gem::Weft::access;
    template <class Archive>
    void serialize(Archive &ar, [[maybe_unused]] unsigned int version) {
        Gem::Common::archive_named(ar, "value_", value_);
    }

    double value_{0.0};
};

/******************************************************************************/
/**
 * @brief The concrete queues under test.
 *
 * Each is spelled exactly the way a production subclass is (GReflectiveInterfaceT
 * over the corresponding queue alias), so the generated clone_/load_/compare_/
 * serialize path is what the tests exercise -- not a hand-written stand-in.
 */
#define GFSPQ_TEST_QUEUE(NAME, BASE, PARAM)                                                       \
    class NAME : public Gem::Common::GReflectiveInterfaceT<NAME, BASE> {                           \
        friend struct Gem::Common::GReflectiveInterfaceAccess;                                     \
        template <typename Self>                                                                   \
        auto localMembers_(this Self &) {                                                          \
            return std::make_tuple();                                                              \
        }                                                                                          \
                                                                                                   \
    public:                                                                                        \
        static constexpr std::string_view class_name = #NAME;                                      \
        NAME() = default;                                                                          \
        explicit NAME(std::size_t max_size)                                                        \
          : Gem::Common::GReflectiveInterfaceT<NAME, BASE>(max_size) {                             \
        }                                                                                          \
        NAME(std::size_t max_size, sortOrder so)                                                   \
          : Gem::Common::GReflectiveInterfaceT<NAME, BASE>(max_size, so) {                         \
        }                                                                                          \
        NAME(NAME const &cp) = default;                                                            \
        NAME(NAME &&cp) noexcept = default;                                                        \
        ~NAME() override = default;                                                                \
                                                                                                   \
    protected:                                                                                     \
        [[nodiscard]] double evaluation(PARAM item) const override {                               \
            return evaluationOf(item);                                                             \
        }                                                                                          \
    };

/** @brief Priority of a value-held item: the value is its own priority. */
inline double evaluationOf(double v) {
    return v;
}
/** @brief Priority of a pointer-held item. */
inline double evaluationOf(std::shared_ptr<TestItem> const &p) {
    return p->value();
}
/** @brief Priority of a solely-owned item. */
inline double evaluationOf(std::unique_ptr<TestItem> const &p) {
    return p->value();
}

GFSPQ_TEST_QUEUE(PodPQ, Gem::Common::GPodFixedSizePriorityQueueT<double>, double const &)
GFSPQ_TEST_QUEUE(SharedPQ, Gem::Common::GPtrFixedSizePriorityQueueT<TestItem>, std::shared_ptr<TestItem> const &)
GFSPQ_TEST_QUEUE(UniquePQ, Gem::Common::GUniquePtrFixedSizePriorityQueueT<TestItem>, std::unique_ptr<TestItem> const &)

#undef GFSPQ_TEST_QUEUE

/******************************************************************************/
// Holder-generic helpers, so one case body can be written for all three queues.

/** @brief Builds a value-held item. */
inline double makeStored(double v, double /*tag*/) {
    return v;
}
/** @brief Builds a shared-handle item. */
inline std::shared_ptr<TestItem> makeStored(double v, std::shared_ptr<TestItem> const & /*tag*/) {
    return std::make_shared<TestItem>(v);
}
/** @brief Builds a solely-owned item. */
inline std::unique_ptr<TestItem> makeStored(double v, std::unique_ptr<TestItem> const & /*tag*/) {
    return std::make_unique<TestItem>(v);
}

/** @brief The numeric value behind a stored handle, whatever the holder. */
template <typename Stored>
double valueOf(Stored const &s) {
    if constexpr(std::is_arithmetic_v<Stored>) {
        return s;
    }
    else {
        return s->value();
    }
}

/** @brief Builds one item of the queue's own storage type. */
template <typename Queue>
typename Queue::StoredType item(double v) {
    typename Queue::StoredType const tag{};
    return makeStored(v, tag);
}

} // namespace

GEM_REGISTER_ARCHIVABLE(TestItem)  // NOLINT
GEM_REGISTER_ARCHIVABLE(PodPQ)     // NOLINT
GEM_REGISTER_ARCHIVABLE(SharedPQ)  // NOLINT
GEM_REGISTER_ARCHIVABLE(UniquePQ)  // NOLINT

// ===========================================================================
// Behaviour that must be identical for all three holders. Each TEMPLATE_TEST_CASE
// body runs three times, once per storage policy.
// ===========================================================================

#define GFSPQ_ALL_HOLDERS PodPQ, SharedPQ, UniquePQ

// ---------------------------------------------------------------------------
// Degenerate case: the empty queue

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: an empty queue reports empty and throws on every accessor",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(5);
    CHECK(pq.empty());
    CHECK(pq.size() == 0);
    CHECK(pq.cloneToVector().empty());
    CHECK(pq.begin() == pq.end());
    CHECK_THROWS_AS(pq.best(), geneva_exception);
    CHECK_THROWS_AS(pq.worst(), geneva_exception);
    CHECK_THROWS_AS(pq.at(0), geneva_exception);
    CHECK_THROWS_AS(pq.pop(), geneva_exception);
}

// ---------------------------------------------------------------------------
// Rank-indexed access: the general form of best() / worst()

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: at() reads the ordered sequence by rank",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(10); // LOWERISBETTER
    pq.add(item<TestType>(5.0));
    pq.add(item<TestType>(1.0));
    pq.add(item<TestType>(3.0));

    REQUIRE(pq.size() == 3);
    CHECK(valueOf(pq.at(0)) == 1.0);
    CHECK(valueOf(pq.at(1)) == 3.0);
    CHECK(valueOf(pq.at(2)) == 5.0);

    // The two ends agree with best() / worst(), and iteration agrees with at().
    CHECK(&pq.at(0) == &pq.best());
    CHECK(&pq.at(pq.size() - 1) == &pq.worst());
    std::size_t rank = 0;
    for(auto const &stored : pq) {
        CHECK(valueOf(stored) == valueOf(pq.at(rank)));
        ++rank;
    }
    CHECK(rank == pq.size());

    // The sort order decides what rank 0 means.
    pq.setSortOrder(sortOrder::HIGHERISBETTER);
    pq.add(item<TestType>(4.0)); // re-sorts under the new order
    CHECK(valueOf(pq.at(0)) == 5.0);

    // Out of range throws rather than reading past the end.
    CHECK_THROWS_AS(pq.at(pq.size()), geneva_exception);
    CHECK_THROWS_AS(pq.at(1000), geneva_exception);
}

// ---------------------------------------------------------------------------
// Ordering / comparator

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: LOWERISBETTER puts the smallest evaluation first",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(10); // LOWERISBETTER is the default
    pq.add(item<TestType>(5.0));
    pq.add(item<TestType>(1.0));
    pq.add(item<TestType>(3.0));

    REQUIRE(pq.size() == 3);
    CHECK(valueOf(pq.best()) == 1.0);
    CHECK(valueOf(pq.worst()) == 5.0);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: HIGHERISBETTER inverts best and worst",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(10, sortOrder::HIGHERISBETTER);
    pq.add(item<TestType>(5.0));
    pq.add(item<TestType>(1.0));
    pq.add(item<TestType>(3.0));

    REQUIRE(pq.size() == 3);
    CHECK(valueOf(pq.best()) == 5.0);
    CHECK(valueOf(pq.worst()) == 1.0);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: setSortOrder round-trips",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(5);
    CHECK(pq.getSortOrder() == sortOrder::LOWERISBETTER);
    pq.setSortOrder(sortOrder::HIGHERISBETTER);
    CHECK(pq.getSortOrder() == sortOrder::HIGHERISBETTER);
}

// ---------------------------------------------------------------------------
// Insertion and eviction at capacity

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: a full queue evicts the worst rather than growing",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(3); // LOWERISBETTER
    pq.add(item<TestType>(1.0));
    pq.add(item<TestType>(2.0));
    pq.add(item<TestType>(3.0));
    REQUIRE(pq.size() == 3);

    pq.add(item<TestType>(5.0)); // worse than the worst -> not kept
    REQUIRE(pq.size() == 3);
    CHECK(valueOf(pq.worst()) == 3.0);

    pq.add(item<TestType>(0.5)); // better than the best -> kept, 3.0 is evicted
    REQUIRE(pq.size() == 3);
    CHECK(valueOf(pq.best()) == 0.5);
    CHECK(valueOf(pq.worst()) == 2.0);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: capacity 1 keeps exactly the single best item",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(1);
    pq.add(item<TestType>(4.0));
    CHECK(pq.size() == 1);

    pq.add(item<TestType>(9.0)); // worse -> ignored
    REQUIRE(pq.size() == 1);
    CHECK(valueOf(pq.best()) == 4.0);

    pq.add(item<TestType>(2.0)); // better -> replaces the incumbent
    REQUIRE(pq.size() == 1);
    CHECK(valueOf(pq.best()) == 2.0);
    CHECK(valueOf(pq.worst()) == 2.0);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: a maximum size of 0 means unlimited",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(0);
    for(int i = 0; i < 50; ++i) {
        pq.add(item<TestType>(static_cast<double>(i)));
    }
    CHECK(pq.size() == 50);
    CHECK(valueOf(pq.best()) == 0.0);
    CHECK(valueOf(pq.worst()) == 49.0);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: setMaxSize trims to the new bound, keeping the best",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(0);
    for(int i = 0; i < 10; ++i) {
        pq.add(item<TestType>(static_cast<double>(i)));
    }
    REQUIRE(pq.size() == 10);

    pq.setMaxSize(4);
    CHECK(pq.size() == 4);
    CHECK(pq.getMaxSize() == 4);
    CHECK(valueOf(pq.best()) == 0.0);
    CHECK(valueOf(pq.worst()) == 3.0);
}

// ---------------------------------------------------------------------------
// Degenerate case: several items of identical priority

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: items of equal priority are all distinct entries",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(4);
    for(int i = 0; i < 4; ++i) {
        pq.add(item<TestType>(7.0));
    }
    // Four separate items that happen to share a priority: none of them is a
    // duplicate of another (duplicate removal is about object identity, not value).
    CHECK(pq.size() == 4);
    CHECK(valueOf(pq.best()) == 7.0);
    CHECK(valueOf(pq.worst()) == 7.0);

    // An equal evaluation is not "better", so it cannot displace an incumbent.
    pq.add(item<TestType>(7.0));
    CHECK(pq.size() == 4);
}

// ---------------------------------------------------------------------------
// pop / cloneToVector / clear

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: pop hands out the best item and shrinks the queue",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(5);
    pq.add(item<TestType>(7.0));
    pq.add(item<TestType>(3.0));
    pq.add(item<TestType>(5.0));

    auto top = pq.pop();
    CHECK(valueOf(top) == 3.0);
    CHECK(pq.size() == 2);
    CHECK(valueOf(pq.best()) == 5.0);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: cloneToVector yields independent copies in priority order",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(5);
    pq.add(item<TestType>(7.0));
    pq.add(item<TestType>(1.0));
    pq.add(item<TestType>(3.0));

    auto v = pq.cloneToVector();
    REQUIRE(v.size() == 3);
    CHECK(valueOf(v[0]) == 1.0);
    CHECK(valueOf(v[1]) == 3.0);
    CHECK(valueOf(v[2]) == 7.0);

    // Copies, not the archive's own entries: for the pointer holders the addresses differ.
    if constexpr(not TestType::holds_values) {
        CHECK(v[0].get() != pq.best().get());
    }
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: clear drops every item",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(3);
    pq.add(item<TestType>(1.0));
    pq.add(item<TestType>(2.0));
    pq.clear();
    CHECK(pq.empty());
}

// ---------------------------------------------------------------------------
// Bulk insertion: replace vs merge

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: a bulk add with replace=true starts from an empty queue",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(5);
    pq.add(item<TestType>(99.0)); // pre-existing content

    typename TestType::ContainerType items;
    items.push_back(item<TestType>(4.0));
    items.push_back(item<TestType>(1.0));
    items.push_back(item<TestType>(7.0));
    pq.addClone(items, /*replace=*/true);

    REQUIRE(pq.size() == 3);
    CHECK(valueOf(pq.best()) == 1.0);
    CHECK(valueOf(pq.worst()) == 7.0);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: a bulk add with replace=false merges into the queue",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(5);
    pq.add(item<TestType>(10.0));

    typename TestType::ContainerType items;
    items.push_back(item<TestType>(4.0));
    items.push_back(item<TestType>(20.0));
    pq.addClone(items, /*replace=*/false);

    // 10, 4, 20 -> best 4, worst 20 under LOWERISBETTER
    REQUIRE(pq.size() == 3);
    CHECK(valueOf(pq.best()) == 4.0);
    CHECK(valueOf(pq.worst()) == 20.0);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: a bulk add of a sub-range adds only that range",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(10);
    typename TestType::ContainerType items;
    for(int i = 0; i < 6; ++i) {
        items.push_back(item<TestType>(static_cast<double>(i)));
    }
    pq.addClone(items.begin(), items.begin() + 3, /*replace=*/false);

    REQUIRE(pq.size() == 3);
    CHECK(valueOf(pq.best()) == 0.0);
    CHECK(valueOf(pq.worst()) == 2.0);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: a bulk add respects the capacity bound",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType pq(3);
    typename TestType::ContainerType items;
    for(int i = 9; i >= 0; --i) { // 9, 8, ..., 0 -- deliberately worst-first
        items.push_back(item<TestType>(static_cast<double>(i)));
    }
    pq.addClone(items, /*replace=*/true);

    REQUIRE(pq.size() == 3);
    CHECK(valueOf(pq.best()) == 0.0);
    CHECK(valueOf(pq.worst()) == 2.0);
}

// ---------------------------------------------------------------------------
// Copy semantics: a copy of a queue never shares its entries, whatever the holder

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: the copy constructor produces an independent queue",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType src(5);
    src.add(item<TestType>(2.0));
    src.add(item<TestType>(8.0));

    TestType cp(src);
    REQUIRE(cp.size() == 2);
    CHECK(valueOf(cp.best()) == 2.0);
    CHECK(valueOf(cp.worst()) == 8.0);
    if constexpr(not TestType::holds_values) {
        CHECK(cp.best().get() != src.best().get()); // deep-copied, not co-owned
    }

    cp.clear();
    CHECK(cp.empty());
    CHECK(src.size() == 2);
}

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: the move constructor resets the source to defaults",
    "[common][priority-queue]",
    GFSPQ_ALL_HOLDERS
) {
    TestType src(7, sortOrder::HIGHERISBETTER);
    src.add(item<TestType>(1.0));
    src.add(item<TestType>(9.0));

    TestType const dst(std::move(src));
    CHECK(dst.size() == 2);
    CHECK(dst.getMaxSize() == 7);
    CHECK(dst.getSortOrder() == sortOrder::HIGHERISBETTER);

    CHECK(src.empty()); // NOLINT(bugprone-use-after-move) -- the reset state is the contract
    CHECK(src.getMaxSize() == GFSPQ_DEF_MAX_SIZE);
    CHECK(src.getSortOrder() == GFSPQ_DEF_SORT_ORDER);
}

// ---------------------------------------------------------------------------
// Serialization round-trip, both codecs, every holder

TEMPLATE_TEST_CASE(
    "GFixedSizePriorityQueueT: round-trips through both GArchive codecs",
    "[common][priority-queue][serialize]",
    GFSPQ_ALL_HOLDERS
) {
    auto const mode =
        GENERATE(serializationMode::GEM_BINARY, serializationMode::GEM_JSON);

    TestType src(4, sortOrder::HIGHERISBETTER);
    src.add(item<TestType>(3.0));
    src.add(item<TestType>(1.0));
    src.add(item<TestType>(9.0));

    std::string const s = src.toString(mode);
    REQUIRE_FALSE(s.empty());

    TestType dst(1); // deliberately different capacity and sort order
    dst.fromString(s, mode);

    CHECK(dst.getMaxSize() == 4);
    CHECK(dst.getSortOrder() == sortOrder::HIGHERISBETTER);
    REQUIRE(dst.size() == 3);
    CHECK(valueOf(dst.best()) == 9.0);
    CHECK(valueOf(dst.worst()) == 1.0);

    // The reconstructed queue is a peer of the original, not an alias of it.
    CHECK_NOTHROW(src.compare(dst, expectation::EQUALITY, 0.));
    if constexpr(not TestType::holds_values) {
        CHECK(dst.best().get() != src.best().get());
    }
}

// ===========================================================================
// Per-holder semantics: this is exactly where the three storage policies are
// allowed to differ, so each is pinned on its own.
// ===========================================================================

// ---------------------------------------------------------------------------
// Value storage

TEST_CASE("GFixedSizePriorityQueueT<POD>: values are copied in, never aliased",
          "[common][priority-queue][pod]") {
    PodPQ pq(5);
    double v = 2.5;
    pq.addClone(v);
    v = 100.0; // mutating the source must not touch the stored entry

    REQUIRE(pq.size() == 1);
    CHECK(pq.best() == 2.5);
}

TEST_CASE("GFixedSizePriorityQueueT<POD>: equal values are never deduplicated",
          "[common][priority-queue][pod]") {
    PodPQ pq(5);
    double const v = 4.0;
    pq.addClone(v);
    pq.addClone(v);
    // Two insertions of the same VALUE are two entries -- a value has no identity,
    // so there is nothing here that could be "the same object twice".
    CHECK(pq.size() == 2);
}

// ---------------------------------------------------------------------------
// Shared storage

TEST_CASE("GFixedSizePriorityQueueT<shared_ptr>: addClone stores a copy, add co-owns the original",
          "[common][priority-queue][shared]") {
    SharedPQ pq(5);

    SECTION("addClone leaves the caller's object untouched") {
        auto src = std::make_shared<TestItem>(2.5);
        pq.addClone(src);

        REQUIRE(pq.size() == 1);
        CHECK(pq.best().get() != src.get()); // a distinct object
        CHECK(pq.best()->value() == src->value());
        CHECK(src.use_count() == 1);         // the queue did not become an owner

        src->setValue(42.0);
        CHECK(pq.best()->value() == 2.5);    // the stored copy is independent
    }

    SECTION("add(handle) makes the queue a second owner of the same object") {
        auto src = std::make_shared<TestItem>(2.5);
        pq.add(std::shared_ptr<TestItem>{src}); // an explicit second owner

        REQUIRE(pq.size() == 1);
        CHECK(pq.best().get() == src.get());
        CHECK(src.use_count() == 2);
    }
}

TEST_CASE("GFixedSizePriorityQueueT<shared_ptr>: the same object handed over twice is one entry",
          "[common][priority-queue][shared]") {
    SharedPQ pq(5);
    auto src = std::make_shared<TestItem>(4.0);
    pq.add(std::shared_ptr<TestItem>{src});
    pq.add(std::shared_ptr<TestItem>{src});
    // Object identity, not value: the second insertion refers to the very same object.
    CHECK(pq.size() == 1);
}

TEST_CASE("GFixedSizePriorityQueueT<shared_ptr>: an empty handle is never admitted",
          "[common][priority-queue][shared]") {
    SharedPQ pq(5);

    // Single item: silently skipped -- and, crucially, never dereferenced. The
    // pre-generalization code evaluated an item before checking it, so a null
    // handle offered to a full queue dereferenced a nullptr.
    pq.addClone(std::shared_ptr<TestItem>{});
    CHECK(pq.empty());

    pq.add(item<SharedPQ>(1.0));
    pq.add(item<SharedPQ>(2.0));
    REQUIRE(pq.size() == 2);
    pq.setMaxSize(2); // now full, so the admission gate is reached with a full queue
    CHECK_NOTHROW(pq.addClone(std::shared_ptr<TestItem>{}));
    CHECK(pq.size() == 2);

    // Bulk: the empty handles are skipped, the rest are kept.
    std::vector<std::shared_ptr<TestItem>> items;
    items.push_back(std::make_shared<TestItem>(3.0));
    items.emplace_back();
    items.push_back(std::make_shared<TestItem>(1.0));
    pq.addClone(items, /*replace=*/true);
    CHECK(pq.size() == 2);
}

// ---------------------------------------------------------------------------
// Unique storage

TEST_CASE("GFixedSizePriorityQueueT<unique_ptr>: add takes ownership, addClone does not",
          "[common][priority-queue][unique]") {
    UniquePQ pq(5);

    SECTION("add(std::move(handle)) transfers sole ownership into the queue") {
        auto src = std::make_unique<TestItem>(2.5);
        auto const *raw = src.get();
        pq.add(std::move(src));

        REQUIRE(pq.size() == 1);
        CHECK(src == nullptr);                 // NOLINT(bugprone-use-after-move)
        CHECK(pq.best().get() == raw);         // the very object, not a copy
    }

    SECTION("addClone leaves the caller owning its object") {
        auto src = std::make_unique<TestItem>(2.5);
        pq.addClone(src);

        REQUIRE(pq.size() == 1);
        REQUIRE(src != nullptr);
        CHECK(pq.best().get() != src.get());
        CHECK(pq.best()->value() == 2.5);

        src->setValue(42.0);
        CHECK(pq.best()->value() == 2.5);
    }
}

TEST_CASE("GFixedSizePriorityQueueT<unique_ptr>: pop hands sole ownership back out",
          "[common][priority-queue][unique]") {
    UniquePQ pq(5);
    auto src = std::make_unique<TestItem>(1.0);
    auto const *raw = src.get();
    pq.add(std::move(src));

    std::unique_ptr<TestItem> const out = pq.pop();
    REQUIRE(out != nullptr);
    CHECK(out.get() == raw);
    CHECK(pq.empty());
}

TEST_CASE("GFixedSizePriorityQueueT<unique_ptr>: a bulk take-over empties the source handles",
          "[common][priority-queue][unique]") {
    UniquePQ pq(10);
    std::vector<std::unique_ptr<TestItem>> items;
    for(int i = 0; i < 4; ++i) {
        items.push_back(std::make_unique<TestItem>(static_cast<double>(i)));
    }
    std::vector<TestItem const *> const raws{
        items[0].get(), items[1].get(), items[2].get(), items[3].get()
    };

    pq.add(std::move(items), /*replace=*/false);
    REQUIRE(pq.size() == 4);

    // Every stored entry IS one of the handed-over objects -- nothing was cloned.
    for(auto const &stored : pq) {
        CHECK(std::ranges::find(raws, stored.get()) != raws.end());
    }
}

TEST_CASE("GFixedSizePriorityQueueT<unique_ptr>: a bulk clone leaves the source population intact",
          "[common][priority-queue][unique]") {
    UniquePQ pq(10);
    std::vector<std::unique_ptr<TestItem>> items;
    for(int i = 0; i < 4; ++i) {
        items.push_back(std::make_unique<TestItem>(static_cast<double>(i)));
    }

    pq.addClone(items, /*replace=*/false);
    REQUIRE(pq.size() == 4);

    for(auto const &src : items) {
        REQUIRE(src != nullptr); // the caller still owns its population
        for(auto const &stored : pq) {
            CHECK(stored.get() != src.get()); // and the archive holds copies of it
        }
    }
}

// ---------------------------------------------------------------------------
// The admission gate is a single, overridable rule

namespace {

/** @brief A queue that admits only items with a non-negative value. */
class GatedPQ : public Gem::Common::GReflectiveInterfaceT<
                    GatedPQ,
                    Gem::Common::GUniquePtrFixedSizePriorityQueueT<TestItem>> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }

public:
    static constexpr std::string_view class_name = "GatedPQ";
    GatedPQ() = default;
    explicit GatedPQ(std::size_t max_size)
      : Gem::Common::GReflectiveInterfaceT<
            GatedPQ,
            Gem::Common::GUniquePtrFixedSizePriorityQueueT<TestItem>>(max_size) {
    }
    GatedPQ(GatedPQ const &cp) = default;
    ~GatedPQ() override = default;

protected:
    [[nodiscard]] bool isValid(std::unique_ptr<TestItem> const &item_ptr) const override {
        return Gem::Common::GUniquePtrFixedSizePriorityQueueT<TestItem>::isValid(item_ptr) &&
               item_ptr->value() >= 0.0;
    }
    [[nodiscard]] double evaluation(std::unique_ptr<TestItem> const &item_ptr) const override {
        return item_ptr->value();
    }
};

} // namespace

GEM_REGISTER_ARCHIVABLE(GatedPQ) // NOLINT

TEST_CASE("GFixedSizePriorityQueueT: a derived admission rule governs every insertion path",
          "[common][priority-queue]") {
    GatedPQ pq(10);

    // single, clone
    auto rejected = std::make_unique<TestItem>(-1.0);
    pq.addClone(rejected);
    CHECK(pq.empty());

    // single, take-over
    pq.add(std::make_unique<TestItem>(-2.0));
    CHECK(pq.empty());

    // bulk: only the admissible items enter
    std::vector<std::unique_ptr<TestItem>> items;
    items.push_back(std::make_unique<TestItem>(-3.0));
    items.push_back(std::make_unique<TestItem>(5.0));
    items.push_back(std::make_unique<TestItem>(-4.0));
    items.push_back(std::make_unique<TestItem>(2.0));
    pq.addClone(items, /*replace=*/false);

    REQUIRE(pq.size() == 2);
    CHECK(pq.best()->value() == 2.0);
    CHECK(pq.worst()->value() == 5.0);
}
