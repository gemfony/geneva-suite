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
 * @brief Tests for the universal late-return integration gate
 * (GOptimizationAlgorithmBase::retainIntegrableLateReturns), the single point at which EVERY
 * optimization algorithm filters the late networked returns it reaps. Two correctness properties are
 * exercised in isolation (no consumer / network needed): the VALIDITY filter (only clean successes are
 * integrated) and the LINEAGE de-duplication keyed by the stable per-individual submission UUID (a
 * lineage already represented in the live population, or recurring within the drained batch, is dropped
 * exactly once). The consumer-side buffering (cap / TTL / eviction) is covered separately by the
 * courtier GCourtierTimeoutTests, and the UUID copy/assign/serialize semantics by the courtier
 * GProcessableIdentityTests; together with this file they cover the mechanism end to end.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <ranges>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "courtier/GCourtierEnums.hpp" // SUBMISSION_UUID_TYPE
#include "geneva/genome/GGenome.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE + archive-generic dispatch
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"

using namespace Gem::Geneva;
using namespace Gem::Geneva::Genome;
using Gem::Geneva::OptimizationAlgorithms::GOptimizationAlgorithmBase;
using Gem::Courtier::SUBMISSION_UUID_TYPE;

namespace Gem::Tests {

/******************************************************************************/
/** A minimal flat individual: a sphere over a single double channel. */
class LRSphere : public GGenomeT<LRSphere> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    LRSphere() {
        GGenomeBuilder b;
        b.addDoubleGroup(2, -10., 10.).init(1.0);
        this->setGenome(b.build());
    }
    LRSphere(const LRSphere &) = default;

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline<double>(v);
        return {std::ranges::fold_left(
            v | std::views::transform([](double x) { return x * x; }), 0., std::plus{})};
    }

private:
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<LRSphere>>(ar, "GGenomeT", *this);
    }
};

/******************************************************************************/
/** A flat individual whose evaluation always throws, used to manufacture an errored work item. */
class LRThrower : public GGenomeT<LRThrower> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    LRThrower() {
        GGenomeBuilder b;
        b.addDoubleGroup(2, -10., 10.).init(1.0);
        this->setGenome(b.build());
    }
    LRThrower(const LRThrower &) = default;

protected:
    std::vector<double> evaluate() override {
        throw std::runtime_error("LRThrower always fails");
    }

private:
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<LRThrower>>(ar, "GGenomeT", *this);
    }
};

/******************************************************************************/
/** @brief A freshly-evaluated (PROCESSED) sphere with an explicitly-set submission UUID. */
std::unique_ptr<gen::GGenome> make_processed(SUBMISSION_UUID_TYPE uuid) {
    auto p = std::make_unique<LRSphere>();
    p->process(); // DO_PROCESS -> PROCESSED
    p->setSubmissionUuid(uuid);
    return p;
}

/** @brief An errored (EXCEPTION_CAUGHT) work item with an explicitly-set submission UUID. */
std::unique_ptr<gen::GGenome> make_errored(SUBMISSION_UUID_TYPE uuid) {
    auto p = std::make_unique<LRThrower>();
    try {
        p->process(); // throws; process() leaves the item in EXCEPTION_CAUGHT and rethrows
    }
    catch(...) { /* expected */
    }
    p->setSubmissionUuid(uuid);
    return p;
}

/** @brief An unprocessed (DO_PROCESS) work item with an explicitly-set submission UUID. */
std::unique_ptr<gen::GGenome> make_unprocessed(SUBMISSION_UUID_TYPE uuid) {
    auto p = std::make_unique<LRSphere>(); // setGenome() leaves it DO_PROCESS
    p->setSubmissionUuid(uuid);
    return p;
}

constexpr SUBMISSION_UUID_TYPE U(std::uint64_t a, std::uint64_t b) { return SUBMISSION_UUID_TYPE{{a, b}}; }

} // namespace Gem::Tests
GEM_REGISTER_ARCHIVABLE(Gem::Tests::LRThrower) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Tests::LRSphere) // NOLINT

using namespace Gem::Tests;

/******************************************************************************/
TEST_CASE("late-return gate: a fresh test individual carries a non-zero, unique lineage UUID", "[lateret][id]") {
    LRSphere const a;
    LRSphere const b;
    CHECK(a.getSubmissionUuid() != SUBMISSION_UUID_TYPE{{0, 0}});
    CHECK(a.getSubmissionUuid() != b.getSubmissionUuid());
}

/******************************************************************************/
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent test of a single retainIntegrableLateReturns() call, checked from several angles (survivors, order, processed/error state, seen-set membership)
TEST_CASE("late-return gate: validity filter keeps only clean successes", "[lateret][validity]") {
    std::vector<std::unique_ptr<gen::GGenome>> items;
    items.push_back(make_processed(U(1, 1)));    // keep
    items.push_back(make_errored(U(2, 2)));      // drop -- errored
    items.push_back(make_unprocessed(U(3, 3)));  // drop -- not processed
    items.push_back(make_processed(U(4, 4)));    // keep

    std::set<SUBMISSION_UUID_TYPE> seen;
    GOptimizationAlgorithmBase::retainIntegrableLateReturns(items, seen);

    REQUIRE(items.size() == 2);
    CHECK(items[0]->getSubmissionUuid() == U(1, 1));
    CHECK(items[1]->getSubmissionUuid() == U(4, 4));
    for(const auto &p : items) {
        CHECK(p->is_processed());
        CHECK(not p->has_errors());
    }
    // The survivors' UUIDs were registered in the seen set.
    CHECK(seen.count(U(1, 1)) == 1);
    CHECK(seen.count(U(4, 4)) == 1);
    // A dropped (invalid) item's UUID must NOT have been registered.
    CHECK(seen.count(U(2, 2)) == 0);
    CHECK(seen.count(U(3, 3)) == 0);
}

/******************************************************************************/
TEST_CASE("late-return gate: a lineage already live is rejected", "[lateret][dedup]") {
    std::vector<std::unique_ptr<gen::GGenome>> items;
    items.push_back(make_processed(U(7, 7)));  // already represented in the live population -> drop
    items.push_back(make_processed(U(8, 8)));  // fresh -> keep

    // Pre-load the seen set with the UUID of an individual still in the live population.
    std::set<SUBMISSION_UUID_TYPE> seen{U(7, 7)};
    GOptimizationAlgorithmBase::retainIntegrableLateReturns(items, seen);

    REQUIRE(items.size() == 1);
    CHECK(items[0]->getSubmissionUuid() == U(8, 8));
}

/******************************************************************************/
TEST_CASE("late-return gate: a lineage duplicated within the batch is kept exactly once", "[lateret][dedup]") {
    std::vector<std::unique_ptr<gen::GGenome>> items;
    items.push_back(make_processed(U(5, 5)));  // first occurrence -> keep
    items.push_back(make_processed(U(5, 5)));  // duplicate lineage -> drop
    items.push_back(make_processed(U(6, 6)));  // distinct -> keep

    std::set<SUBMISSION_UUID_TYPE> seen;
    GOptimizationAlgorithmBase::retainIntegrableLateReturns(items, seen);

    REQUIRE(items.size() == 2);
    CHECK(items[0]->getSubmissionUuid() == U(5, 5));
    CHECK(items[1]->getSubmissionUuid() == U(6, 6));
}

/******************************************************************************/
TEST_CASE("late-return gate: distinct, clean lineages all survive", "[lateret][dedup]") {
    std::vector<std::unique_ptr<gen::GGenome>> items;
    for(std::uint64_t i = 0; i < 5; ++i) {
        items.push_back(make_processed(U(100 + i, 0)));
    }

    std::set<SUBMISSION_UUID_TYPE> seen;
    GOptimizationAlgorithmBase::retainIntegrableLateReturns(items, seen);

    CHECK(items.size() == 5);
}

/******************************************************************************/
TEST_CASE("late-return gate: validity and dedup compose (invalid duplicates never shadow a clean one)",
          "[lateret][validity][dedup]") {
    // An errored item shares a lineage with a later clean item: the errored one is removed by the
    // validity filter BEFORE dedup, so the clean item must still be admitted (its UUID is not yet seen).
    std::vector<std::unique_ptr<gen::GGenome>> items;
    items.push_back(make_errored(U(9, 9)));    // drop (errored) -- must not reserve U(9,9)
    items.push_back(make_processed(U(9, 9)));  // keep -- the only clean carrier of this lineage

    std::set<SUBMISSION_UUID_TYPE> seen;
    GOptimizationAlgorithmBase::retainIntegrableLateReturns(items, seen);

    REQUIRE(items.size() == 1);
    CHECK(items[0]->getSubmissionUuid() == U(9, 9));
    CHECK(items[0]->is_processed());
}
