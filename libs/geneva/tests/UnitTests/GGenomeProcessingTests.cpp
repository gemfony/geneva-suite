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
 * @brief Characterization tests for the new candidate-solution hierarchy (GGenome /
 * GGenome / GGenomeT). The hierarchy is additive and unwired at this stage; these tests
 * exercise it in isolation: value-channel round-trips, evaluation + external-result acceptance, the
 * multi-format serialization round-trip, clone independence, and -- the watertight part -- the
 * layout-keyed countParameters cache and its invalidation when the genome's layout changes.
 */

#include <algorithm>
#include <cstdint>
#include <functional>
#include <ranges>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>


#include "geneva/genome/GGenome.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE + archive-generic dispatch
#include "geneva/genome/GGenomeT.hpp"
#include "courtier/GWireSerializationContext.hpp" // the worker-side wire scope
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GProblemStoreT.hpp"

using namespace Gem::Geneva;        // activityMode, serializationMode helpers
using namespace Gem::Geneva::Genome;
using Catch::Approx;

namespace Gem::Tests {

/******************************************************************************/
/**
 * A minimal flat individual on the NEW hierarchy: a sphere over its double channel, with optional int32
 * and bool channels so the parameter-count cache can be exercised across channels. Adds no data members,
 * so clone/load/compare come from the CRTP base + GGenome.
 */
class NewSphere : public GGenomeT<NewSphere> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    NewSphere() { buildGenome(3, 2, 4); }
    NewSphere(std::size_t nd, std::size_t ni, std::size_t nb) { buildGenome(nd, ni, nb); }
    NewSphere(const NewSphere &) = default;

    /** @brief Rebuilds this individual's genome with a different structure (re-keys the count cache). */
    void rebuild(std::size_t nd, std::size_t ni, std::size_t nb) { buildGenome(nd, ni, nb); }

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline<double>(v);
        return {std::ranges::fold_left(
            v | std::views::transform([](double x) { return x * x; }), 0., std::plus{})};
    }

private:
    void buildGenome(std::size_t nd, std::size_t ni, std::size_t nb) {
        GGenomeBuilder b;
        b.addDoubleGroup(nd, -10., 10.).init(1.0); // structure only; the adaptor lives on the OA config
        if(ni > 0) {
            b.addInt32Group(ni, -5, 5);
        }
        if(nb > 0) {
            b.addBoolArray(nb);
        }
        this->setGenome(b.build());
    }

    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<NewSphere>>(ar, "GGenomeT", *this);
    }
};

} // namespace Gem::Tests

GEM_REGISTER_ARCHIVABLE(Gem::Tests::NewSphere) // NOLINT

using Gem::Tests::NewSphere;

namespace Gem::Tests {

/******************************************************************************/
/**
 * A single-criterion flat individual carrying the free evaluator: a static evaluate(const SeamSphere&)
 * returning a one-element raw-fitness vector, with evaluate() delegating to it (single-sourced).
 * Used to prove the free-evaluator dispatch path produces byte-identical results to the virtual path.
 */
class SeamSphere : public GGenomeT<SeamSphere> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    SeamSphere() {
        GGenomeBuilder b;
        b.addDoubleGroup(3, -10., 10.).init(1.0); // structure only
        this->setGenome(b.build());
    }
    SeamSphere(const SeamSphere &) = default;

protected:
    /** @brief The evaluation hook: the sum of squares of the genome's external parameters (size-1 vector). */
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
        Gem::Common::archive_named_base<GGenomeT<SeamSphere>>(ar, "GGenomeT", *this);
    }
};

/******************************************************************************/
/**
 * A three-criterion flat individual carrying the E.0 free evaluator in its multi-criterion shape: a static
 * evaluate() returning the full raw result vector (main first). evaluate() delegates to it and
 * writes the secondary results via setResult(), so the virtual and free-evaluator paths are single-sourced.
 * Used to prove the dispatch's secondary-result handling matches the res_vec / virtual paths.
 */
class SeamMulti : public GGenomeT<SeamMulti> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    SeamMulti() {
        GGenomeBuilder b;
        b.addDoubleGroup(3, -10., 10.).init(1.0); // structure only
        this->setGenome(b.build());
        this->setNStoredResults(3); // three evaluation criteria
    }
    SeamMulti(const SeamMulti &) = default;

protected:
    /** @brief The evaluation hook: one distinguishable criterion per parameter (main result first), returned
     *  as the full raw result vector. */
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline<double>(v);
        return std::vector<double>{v.at(0) * v.at(0), 2. * v.at(1) * v.at(1), 3. * v.at(2) * v.at(2)};
    }

private:
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<SeamMulti>>(ar, "GGenomeT", *this);
    }
};

} // namespace Gem::Tests

GEM_REGISTER_ARCHIVABLE(Gem::Tests::SeamSphere) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Tests::SeamMulti) // NOLINT

using Gem::Tests::SeamMulti;
using Gem::Tests::SeamSphere;

/******************************************************************************/
TEST_CASE("GGenome: a fresh flat individual evaluates and reaches PROCESSED", "[candidate]") {
    NewSphere ind(3, 0, 0); // 3 doubles, all initialised to 1.0

    // setGenome() marks the item due for processing.
    REQUIRE(ind.is_due_for_processing());

    const auto first = ind.process();

    CHECK(ind.is_processed());
    // sum of squares of three 1.0 values == 3.0
    CHECK(first.rawFitness() == Approx(3.0));
    CHECK(ind.raw_fitness(0) == Approx(3.0));
    CHECK(ind.transformed_fitness(0) == Approx(3.0)); // USESIMPLEEVALUATION: transformed == raw
}

/******************************************************************************/
TEST_CASE("GGenome: streamline / assignValueVector round-trip", "[candidate]") {
    NewSphere ind(4, 0, 0);

    std::vector<double> before;
    ind.streamline<double>(before);
    REQUIRE(before.size() == 4);

    std::vector<double> written{-3.5, 0.0, 2.25, 9.99};
    ind.assignValueVector<double>(written);

    std::vector<double> after;
    ind.streamline<double>(after);
    REQUIRE(after.size() == written.size());
    for(std::size_t i = 0; i < written.size(); ++i) {
        CHECK(after[i] == Approx(written[i]));
    }
    // Writing parameters marks the item for reprocessing.
    CHECK(ind.is_due_for_processing());
}

/******************************************************************************/
TEST_CASE("GGenome: an external evaluation result is accepted verbatim", "[candidate][external]") {
    NewSphere ind(3, 0, 0);
    REQUIRE(ind.is_due_for_processing());

    // Inject a pre-computed raw result instead of letting evaluate() run.
    const double injected = 42.5;
    ind.process(std::vector<individual_processing_result>{individual_processing_result(injected)});

    CHECK(ind.is_processed());
    CHECK(ind.raw_fitness(0) == Approx(injected));
    CHECK(ind.transformed_fitness(0) == Approx(injected)); // USESIMPLEEVALUATION
}

/******************************************************************************/
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent serialization sweep (TEXT/XML/BINARY) via the shared check_format() helper
TEST_CASE(
    "GGenome: a processed candidate round-trips its structure, results and values in TEXT, XML and BINARY",
    "[candidate][serialize]") {
    using Gem::Common::serializationMode;

    NewSphere ind(5, 2, 3);
    ind.process(); // give it results to carry across the wire

    const auto check_format = [&](serializationMode mode) {
        NewSphere restored;
        restored.fromString(ind.toString(mode), mode);
        // Structure preserved
        CHECK(restored.getLayout()->layoutId() == ind.getLayout()->layoutId());
        CHECK(restored.countParameters<double>() == ind.countParameters<double>());
        CHECK(restored.countParameters<std::int32_t>() == ind.countParameters<std::int32_t>());
        CHECK(restored.countParameters<bool>() == ind.countParameters<bool>());
        // Results preserved
        REQUIRE(restored.is_processed());
        CHECK(restored.raw_fitness(0) == Approx(ind.raw_fitness(0)));
        // Values preserved
        std::vector<double> a;
        std::vector<double> b;
        ind.streamline<double>(a);
        restored.streamline<double>(b);
        REQUIRE(a.size() == b.size());
        for(std::size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i] == Approx(b[i]));
        }
    };

    check_format(serializationMode::GEM_BINARY);
    check_format(serializationMode::GEM_JSON);
}

/******************************************************************************/
TEST_CASE("GGenome: clone is independent of the original", "[candidate]") {
    NewSphere ind(4, 0, 0);
    ind.process();

    auto clone = ind.clone<NewSphere>();
    REQUIRE(clone);

    // Mutating the clone must not touch the original.
    clone->randomInit(activityMode::ALLPARAMETERS);

    std::vector<double> orig_v;
    std::vector<double> clone_v;
    ind.streamline<double>(orig_v);
    clone->streamline<double>(clone_v);

    // The original still holds its evaluated state; the clone has been re-randomized and is dirty.
    CHECK(ind.is_processed());
    CHECK(clone->is_due_for_processing());
}

/******************************************************************************/
TEST_CASE("GGenome: countParameters is cached and re-keyed on layout change", "[candidate][cache]") {
    NewSphere ind(3, 2, 4); // 3 doubles, 2 int32, 4 bool, 0 float

    SECTION("counts are correct and stable across repeated queries (cache hit)") {
        for(int rep = 0; rep < 3; ++rep) {
            CHECK(ind.countParameters<double>() == 3u);
            CHECK(ind.countParameters<float>() == 0u);
            CHECK(ind.countParameters<std::int32_t>() == 2u);
            CHECK(ind.countParameters<bool>() == 4u);
            CHECK(ind.countFPParameters() == 3u);
        }
    }

    SECTION("the cache is invalidated when the genome's layout changes") {
        // Prime the cache for the original structure.
        REQUIRE(ind.countParameters<double>() == 3u);
        REQUIRE(ind.countParameters<std::int32_t>() == 2u);
        REQUIRE(ind.countParameters<bool>() == 4u);

        // Install a structurally different layout: counts must follow.
        ind.rebuild(5, 0, 1);
        CHECK(ind.countParameters<double>() == 5u);
        CHECK(ind.countParameters<std::int32_t>() == 0u);
        CHECK(ind.countParameters<bool>() == 1u);

        // Restore a structure identical to the original: counts must match it again (re-keying works,
        // and a recycled layout address cannot alias because the cache pins the layout it is keyed to).
        ind.rebuild(3, 2, 4);
        CHECK(ind.countParameters<double>() == 3u);
        CHECK(ind.countParameters<std::int32_t>() == 2u);
        CHECK(ind.countParameters<bool>() == 4u);
    }

    SECTION("the cache survives a load_()-based copy") {
        NewSphere const other(1, 1, 1);
        // Prime both caches with their own structures.
        REQUIRE(other.countParameters<double>() == 1u);
        REQUIRE(ind.countParameters<double>() == 3u);

        // load_() (clone path) copies ind's layout into other -- its cache must re-key.
        auto loaded = ind.clone<NewSphere>();
        CHECK(loaded->countParameters<double>() == 3u);
        CHECK(loaded->countParameters<std::int32_t>() == 2u);
        CHECK(loaded->countParameters<bool>() == 4u);
    }
}

/******************************************************************************/
/**
 * Path B: a concrete individual evaluates at the single call site through its virtual evaluate() -- the sole
 * evaluation hook now that evaluate() is gone. Dispatch is by the vtable: no per-instance thunk,
 * no factory install.
 */
TEST_CASE("GGenome: a single-criterion individual evaluates via evaluate()", "[candidate][evaluator]") {
    SeamSphere a;
    a.assignValueVector<double>(std::vector<double>{2.0, -3.0, 4.0}); // sum of squares == 29
    CHECK(a.process().rawFitness() == Approx(29.0));

    NewSphere b(3, 0, 0); // three doubles initialised to 1.0 -> sum of squares == 3
    CHECK(b.process().rawFitness() == Approx(3.0));
}

/******************************************************************************/
/**
 * Path B: evaluate() RETURNS the full raw result vector, so every criterion of a multi-criterion individual
 * is populated from the one return value (runEvaluation_ writes the secondaries).
 */
TEST_CASE("GGenome: member evaluate() populates all criteria (multi-criterion)", "[candidate][evaluator]") {
    SeamMulti ind;
    ind.assignValueVector<double>(std::vector<double>{2.0, 3.0, 4.0}); // criteria: {4, 2*9=18, 3*16=48}
    REQUIRE(ind.getNStoredResults() == 3u);
    ind.process();

    CHECK(ind.raw_fitness(0) == Approx(4.0));
    CHECK(ind.raw_fitness(1) == Approx(18.0));
    CHECK(ind.raw_fitness(2) == Approx(48.0));
}

/******************************************************************************/
/**
 * Path B: dispatch is by vtable, so a clone -- and, by the same mechanism, a Boost-deserialized copy on an
 * MPI worker / from a checkpoint -- evaluates correctly with NO thunk to re-install. The evaluator is
 * intrinsic to the type; this is what lets Path B drop the entire free-evaluator association machinery.
 */
TEST_CASE("GGenome: a cloned individual evaluates via the vtable", "[candidate][evaluator]") {
    SeamSphere parent;
    parent.assignValueVector<double>(std::vector<double>{1.0, 2.0, 2.0}); // 1+4+4 == 9

    auto child = parent.clone<SeamSphere>();
    REQUIRE(child);
    CHECK(child->process().rawFitness() == Approx(9.0));
}

/******************************************************************************/
/**
 * The (c) load-once problem store: fills exactly once, reads immutably, and refuses a read-before-load.
 */
TEST_CASE("GProblemStoreT: loads once and reads immutably", "[candidate][store]") {
    Gem::Geneva::Genome::GProblemStoreT<std::vector<double>> store;

    CHECK_FALSE(store.loaded());
    CHECK_THROWS(store.get()); // read before load is a programming error

    int fillCount = 0;
    const auto fill = [&fillCount]() {
        ++fillCount;
        return std::vector<double>{1.5, 2.5, 3.5};
    };

    store.ensureLoaded(fill);
    REQUIRE(store.loaded());
    REQUIRE(fillCount == 1);
    CHECK(store.get() == std::vector<double>{1.5, 2.5, 3.5});

    // A second ensureLoaded() is a no-op: the payload is immutable after the first fill.
    store.ensureLoaded([]() { return std::vector<double>{9.9}; });
    CHECK(fillCount == 1);
    CHECK(store.get() == std::vector<double>{1.5, 2.5, 3.5});
}

/******************************************************************************/
/**
 * Regression (2026-07-18): an error flagged from INSIDE evaluate() (via GProcessable::force_set_error)
 * must survive as ERROR_FLAGGED and abort process() with a processing exception. On the unfixed code an
 * unconditional terminal `processing_status_ = PROCESSED` inside process() clobbered the flagged error,
 * silently converting a failed evaluation into a successfully processed individual.
 */
namespace Gem::Tests {

class ErrorFlaggingSphere : public GGenomeT<ErrorFlaggingSphere> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    ErrorFlaggingSphere() {
        GGenomeBuilder b;
        b.addDoubleGroup(2, -10., 10.).init(1.0);
        this->setGenome(b.build());
    }
    ErrorFlaggingSphere(const ErrorFlaggingSphere &) = default;

protected:
    /** @brief Flags a user error mid-evaluation, as a real fitness function would on invalid input. */
    std::vector<double> evaluate() override {
        this->force_set_error("ErrorFlaggingSphere: user-flagged evaluation error\n");
        return {0.};
    }

private:
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<ErrorFlaggingSphere>>(ar, "GGenomeT", *this);
    }
};

} // namespace Gem::Tests

GEM_REGISTER_ARCHIVABLE(Gem::Tests::ErrorFlaggingSphere) // NOLINT

TEST_CASE(
    "GGenome: an error flagged inside evaluate() survives as ERROR_FLAGGED",
    "[candidate][status]"
) {
    using Gem::Courtier::processingStatus;

    Gem::Tests::ErrorFlaggingSphere ind;
    REQUIRE(ind.is_due_for_processing());

    // The flagged error must abort processing with a processing exception ...
    CHECK_THROWS_AS(ind.process(), Gem::Courtier::g_processing_exception);

    // ... and must NOT be clobbered into PROCESSED by the terminal status assignment
    CHECK(ind.has_errors());
    CHECK(ind.getProcessingStatus() == processingStatus::ERROR_FLAGGED);
    CHECK_FALSE(ind.is_processed());
}
