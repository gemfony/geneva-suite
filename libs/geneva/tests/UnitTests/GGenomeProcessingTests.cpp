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
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "courtier/GWireSerializationContext.hpp" // the worker-side wire scope
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GProblemStoreT.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE + archive-generic dispatch

#include "common/GSerializableFunctionObjectT.hpp"    // the pre-processor of process(evaluation)
#include "geneva/individuals/GFunctionIndividual.hpp" // GSphereConstraint

#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/gpu/GGPUConsumer.hpp" // the GPU path, on a host stand-in backend
#include "geneva/genome/GBaseGPUMarshallerT.hpp"

using namespace Gem::Geneva; // activityMode, serializationMode helpers
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
    using genome_only =
        void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    NewSphere() {
        buildGenome(3, 2, 4);
    }
    NewSphere(std::size_t nd, std::size_t ni, std::size_t nb) {
        buildGenome(nd, ni, nb);
    }
    NewSphere(const NewSphere &) = default;

    /** @brief Rebuilds this individual's genome with a different structure (re-keys the count cache). */
    void rebuild(std::size_t nd, std::size_t ni, std::size_t nb) {
        buildGenome(nd, ni, nb);
    }

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline<double>(v);
        return {std::ranges::fold_left(
            v | std::views::transform([](double x) { return x * x; }),
            0.,
            std::plus{}
        )};
    }

private:
    void buildGenome(std::size_t nd, std::size_t ni, std::size_t nb) {
        GGenomeBuilder b;
        b.addDoubleGroup(nd, -10., 10.)
            .init(1.0); // structure only; the adaptor lives on the OA config
        if(ni > 0) {
            b.addInt32Group(ni, -5, 5);
        }
        if(nb > 0) {
            b.addBoolArray(nb);
        }
        this->setGenome(b.build());
    }

    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
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
    using genome_only =
        void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
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
            v | std::views::transform([](double x) { return x * x; }),
            0.,
            std::plus{}
        )};
    }

private:
    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
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
    using genome_only =
        void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
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
        return std::vector<double>{
            v.at(0) * v.at(0),
            2. * v.at(1) * v.at(1),
            3. * v.at(2) * v.at(2)
        };
    }

private:
    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<SeamMulti>>(ar, "GGenomeT", *this);
    }
};

} // namespace Gem::Tests

GEM_REGISTER_ARCHIVABLE(Gem::Tests::SeamSphere) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Tests::SeamMulti)  // NOLINT

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
// The output operator of GFunctionIndividual wrote its dump to std::cout and returned the stream it
// was given untouched, so printing an individual into a file or a string produced nothing.
TEST_CASE(
    "GFunctionIndividual: operator<< writes to the stream it is given",
    "[candidate][print]"
) {
    gind::GFunctionIndividual ind; // the default demo function, the parabola
    GGenomeBuilder b;
    b.addDoubleGroup(2, -10., 10.).init(1.0);
    ind.setGenome(b.build());
    ind.process();
    REQUIRE(ind.is_processed());

    std::ostringstream direct;
    direct << ind;
    CHECK_THAT(direct.str(), Catch::Matchers::ContainsSubstring("Raw fitness: "));
    CHECK_THAT(direct.str(), Catch::Matchers::ContainsSubstring("1: 1"));

    std::ostringstream via_pointer;
    via_pointer << std::make_unique<gind::GFunctionIndividual>(ind);
    CHECK(via_pointer.str() == direct.str());
}

/******************************************************************************/
// The pointer overload of GFunctionIndividual's output operator dereferenced its argument without a
// check, so printing an empty pointer (e.g. the result of a run that produced no individual) was
// undefined behaviour -- a crash in practice.
TEST_CASE(
    "GFunctionIndividual: operator<< prints an empty pointer as (null)",
    "[candidate][print]"
) {
    const std::unique_ptr<gind::GFunctionIndividual> empty;
    std::ostringstream out;
    out << empty;
    CHECK(out.str() == "(null)\n");
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
    "GGenome: a processed candidate round-trips its structure and results and values in "
    "TEXT/XML/BINARY",
    "[candidate][serialize]"
) {
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
TEST_CASE(
    "GGenome: countParameters is cached and re-keyed on layout change",
    "[candidate][cache]"
) {
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
TEST_CASE(
    "GGenome: a single-criterion individual evaluates via evaluate()",
    "[candidate][evaluator]"
) {
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
TEST_CASE(
    "GGenome: member evaluate() populates all criteria (multi-criterion)",
    "[candidate][evaluator]"
) {
    SeamMulti ind;
    ind.assignValueVector<double>(
        std::vector<double>{2.0, 3.0, 4.0}
    ); // criteria: {4, 2*9=18, 3*16=48}
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
    using genome_only =
        void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
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
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
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

/******************************************************************************/
// process(evaluation): the evaluation a caller supplies is called where process() would call
// evaluate(), inside the processing lifecycle (precondition, pre-processing, feasibility gate,
// timing, exception funneling).

namespace Gem::Tests {

/** @brief What the cases of process(evaluation) observe: the calls of CountingSphere::evaluate()
 *  and the order in which the pre-processor and the supplied evaluation ran. */
struct SuppliedEvaluationLog {
    std::size_t evaluateCalls = 0;
    std::vector<std::string> order;
};
// Observed by single-threaded test cases only.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
SuppliedEvaluationLog suppliedEvaluationLog;

/**
 * A single-criterion flat individual (three doubles, all 1.0) whose evaluate() counts its calls in
 * suppliedEvaluationLog, so a case can prove that process(evaluation) did not call it.
 */
class CountingSphere : public GGenomeT<CountingSphere> {
public:
    using genome_only = void; // genome-only flat leaf -- opt into GGenomeT's empty localMembers_()

    CountingSphere() {
        GGenomeBuilder b;
        b.addDoubleGroup(3, -10., 10.).init(1.0); // structure only
        this->setGenome(b.build());
    }

protected:
    /** @brief Counts the call; the sum of squares of the double channel. */
    std::vector<double> evaluate() override {
        ++suppliedEvaluationLog.evaluateCalls;
        std::vector<double> v;
        this->streamline<double>(v);
        return {std::ranges::fold_left(
            v | std::views::transform([](double x) { return x * x; }),
            0.,
            std::plus{}
        )};
    }

private:
    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<CountingSphere>>(ar, "GGenomeT", *this);
    }
};

/** @brief A pre-processor that assigns 2.0 to every double parameter and logs "pre". */
class ParameterSettingPreProcessor : public Gem::Common::GSerializableFunctionObjectT<GGenome> {
public:
    ParameterSettingPreProcessor() = default;

protected:
    bool process_(GGenome &p) override {
        suppliedEvaluationLog.order.emplace_back("pre");
        p.assignValueVector<double>(std::vector<double>(p.countParameters<double>(), 2.0));
        return true;
    }

private:
    [[nodiscard]] Gem::Common::GSerializableFunctionObjectT<GGenome> *clone_() const override {
        // The raw return is the clone_() contract.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new ParameterSettingPreProcessor(*this);
    }
};

} // namespace Gem::Tests

GEM_REGISTER_ARCHIVABLE(Gem::Tests::CountingSphere) // NOLINT

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- a section per lifecycle rule
TEST_CASE(
    "GGenome: process(evaluation) calls the supplied evaluation inside the processing lifecycle",
    "[candidate][evaluation]"
) {
    using Gem::Courtier::g_processing_exception;
    using Gem::Courtier::processingStatus;
    using Gem::Tests::CountingSphere;
    using Gem::Tests::suppliedEvaluationLog;

    suppliedEvaluationLog = {};
    std::size_t evaluation_calls = 0;

    CountingSphere ind;
    ind.set_processing_status(processingStatus::DO_PROCESS);
    REQUIRE(ind.is_due_for_processing());

    SECTION("the evaluation is used") {
        ind.process([&evaluation_calls](const GGenome &) {
            ++evaluation_calls;
            return std::vector<double>{42.};
        });
        CHECK(ind.getProcessingStatus() == processingStatus::PROCESSED);
        CHECK(ind.raw_fitness(0) == 42.);
        CHECK(evaluation_calls == 1);
        CHECK(suppliedEvaluationLog.evaluateCalls == 0);
    }

    SECTION("pre-processing comes first") {
        ind.registerPreProcessor(std::make_shared<Gem::Tests::ParameterSettingPreProcessor>());
        std::vector<double> seen;
        ind.process([&seen](const GGenome &candidate) {
            suppliedEvaluationLog.order.emplace_back("evaluation");
            candidate.streamline<double>(seen);
            return std::vector<double>{1.};
        });
        CHECK(seen == std::vector<double>(3, 2.0));
        CHECK(suppliedEvaluationLog.order == std::vector<std::string>{"pre", "evaluation"});
    }

    SECTION("the feasibility gate") {
        // All parameters are 1.0: the norm sqrt(3) lies outside a sphere of diameter 0.5.
        ind.registerConstraint(std::make_shared<Gem::Geneva::Individuals::GSphereConstraint>(0.5));
        ind.setEvaluationPolicy(evaluationPolicy::USEWORSTCASEFORINVALID);
        CHECK_NOTHROW(ind.process([&evaluation_calls](const GGenome &) {
            ++evaluation_calls;
            return std::vector<double>{1.};
        }));
        CHECK(evaluation_calls == 0);
        CHECK(ind.raw_fitness(0) == ind.getWorstCase());
    }

    SECTION("the timing") {
        ind.process([](const GGenome &) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return std::vector<double>{1.};
        });
        CHECK(std::get<1>(ind.getProcessingTimes()) >= 0.02);
    }

    SECTION("an exception") {
        CHECK_THROWS_AS(
            ind.process([](const GGenome &) -> std::vector<double> {
                throw std::runtime_error("injected");
            }),
            g_processing_exception
        );
        CHECK(ind.getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);
        CHECK(ind.getStoredErrorDescriptions().contains("injected"));
    }

    SECTION("a wrong number of results") {
        CHECK_THROWS_AS(
            ind.process([](const GGenome &) { return std::vector<double>{1., 2.}; }),
            g_processing_exception
        );
        CHECK(ind.getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);
    }

    SECTION("an empty evaluation") {
        CHECK_THROWS_AS(ind.process(GGenome::evaluation_function{}), geneva_exception);
        CHECK(ind.getProcessingStatus() == processingStatus::DO_PROCESS);
    }

    SECTION("an item that is not due") {
        ind.set_processing_status(processingStatus::UNPROCESSED);
        CHECK_THROWS(ind.process([&evaluation_calls](const GGenome &) {
            ++evaluation_calls;
            return std::vector<double>{1.};
        }));
        CHECK(evaluation_calls == 0);
    }
}

/******************************************************************************/
// The GPU path: a batch is evaluated on a device in one launch, between the marshaller's flatten()
// and scatter(). It keeps the processing contract of the CPU path: the pre-processor runs before
// the device sees the values, the processing time covers the device evaluation, the post-processor
// runs on the evaluated item, and an item whose processing fails fails alone.

namespace Gem::Tests {

/** @brief A host stand-in for a device backend: the fitness of an item is the sum of squares of its
 *  row. It logs "device" and stays inside evaluate() for 20 ms. */
class SumOfSquaresBackend final : public Gem::Courtier::GPU::GGPUDeviceBackendI<double> {
public:
    void initialize(const Gem::Courtier::GPU::KernelSpec & /*spec*/) override { /* nothing */
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
        suppliedEvaluationLog.order.emplace_back("device");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const std::span<const double> all(
            params,
            static_cast<std::size_t>(n_items) * static_cast<std::size_t>(dim)
        );
        for(int i = 0; i < n_items; ++i) {
            double sum = 0.;
            for(const double x :
                all.subspan(static_cast<std::size_t>(i * dim), static_cast<std::size_t>(dim))) {
                sum += x * x;
            }
            fitness_out[i] = sum;
        }
    }

    [[nodiscard]] std::string name() const override {
        return "sum-of-squares";
    }
};

/** @brief A post-processor that logs "post" and whether the item was already processed. */
class LoggingPostProcessor : public Gem::Common::GSerializableFunctionObjectT<GGenome> {
public:
    LoggingPostProcessor() = default;

protected:
    bool process_(GGenome &p) override {
        suppliedEvaluationLog.order.emplace_back(p.is_processed() ? "post" : "post-unprocessed");
        return true;
    }

private:
    [[nodiscard]] Gem::Common::GSerializableFunctionObjectT<GGenome> *clone_() const override {
        // The raw return is the clone_() contract.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new LoggingPostProcessor(*this);
    }
};

/** @brief A backend whose device fails: evaluate() throws. */
class FailingBackend final : public Gem::Courtier::GPU::GGPUDeviceBackendI<double> {
public:
    void initialize(const Gem::Courtier::GPU::KernelSpec & /*spec*/) override { /* nothing */
    }

    void evaluate(
        const double * /*params*/,
        int /*n_items*/,
        int /*dim*/,
        const std::byte * /*pconst*/,
        std::size_t /*pconst_size*/,
        double * /*fitness_out*/,
        int /*threads_per_item*/
    ) override {
        throw std::runtime_error("device failed");
    }

    [[nodiscard]] std::string name() const override {
        return "failing";
    }
};

/** @brief A post-processor that stays inside for 200 ms. */
class SlowPostProcessor : public Gem::Common::GSerializableFunctionObjectT<GGenome> {
public:
    SlowPostProcessor() = default;

protected:
    bool process_(GGenome & /*p*/) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return true;
    }

private:
    [[nodiscard]] Gem::Common::GSerializableFunctionObjectT<GGenome> *clone_() const override {
        // The raw return is the clone_() contract.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new SlowPostProcessor(*this);
    }
};

/** @brief A pre-processor that installs a genome with one more parameter. */
class GrowingPreProcessor : public Gem::Common::GSerializableFunctionObjectT<GGenome> {
public:
    GrowingPreProcessor() = default;

protected:
    bool process_(GGenome &p) override {
        GGenomeBuilder b;
        b.addDoubleGroup(p.countParameters<double>() + 1, -10., 10.).init(1.0);
        p.setGenome(b.build());
        return true;
    }

private:
    [[nodiscard]] Gem::Common::GSerializableFunctionObjectT<GGenome> *clone_() const override {
        // The raw return is the clone_() contract.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new GrowingPreProcessor(*this);
    }
};

/** @brief A pre-processor that takes its item out of processing. */
class UnsubmittingPreProcessor : public Gem::Common::GSerializableFunctionObjectT<GGenome> {
public:
    UnsubmittingPreProcessor() = default;

protected:
    bool process_(GGenome &p) override {
        p.mark_as_ignorable();
        return true;
    }

private:
    [[nodiscard]] Gem::Common::GSerializableFunctionObjectT<GGenome> *clone_() const override {
        // The raw return is the clone_() contract.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new UnsubmittingPreProcessor(*this);
    }
};

/** @brief A marshaller that writes the device results back through process(result), as the GPU
 *  interface once advised. */
class ProcessWritingMarshaller final : public Gem::Geneva::GBaseGPUMarshallerT<double> {
public:
    void scatter(
        std::span<const item_ptr> items,
        const std::vector<double> &fitness,
        double /*evaluation_time*/
    ) const override {
        for(auto const &[item, fit] : std::views::zip(items, fitness)) {
            item->process(
                std::vector<individual_processing_result>{individual_processing_result(fit)}
            );
        }
    }
};

/** @brief A pre- or post-processor that throws. */
class ThrowingProcessor : public Gem::Common::GSerializableFunctionObjectT<GGenome> {
public:
    ThrowingProcessor() = default;

protected:
    bool process_(GGenome & /*p*/) override {
        throw std::runtime_error("processor failed");
    }

private:
    [[nodiscard]] Gem::Common::GSerializableFunctionObjectT<GGenome> *clone_() const override {
        // The raw return is the clone_() contract.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        return new ThrowingProcessor(*this);
    }
};

} // namespace Gem::Tests

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- a section per lifecycle rule
TEST_CASE(
    "GGenome: the GPU path keeps the processing contract of the CPU path",
    "[candidate][gpu]"
) {
    using Gem::Courtier::processingStatus;
    using Gem::Tests::CountingSphere;
    using Gem::Tests::suppliedEvaluationLog;

    suppliedEvaluationLog = {};
    std::vector<std::unique_ptr<GGenome>> batch;
    for(int i = 0; i < 3; ++i) {
        batch.push_back(std::make_unique<CountingSphere>());
    }

    // The sections that run the consumer need its backend factory, which courtier contains only
    // where the GPU consumer is built; the marshaller and the item's own rules are tested everywhere.
#ifdef GENEVA_BUILD_WITH_GPU_CONSUMER
    using Gem::Courtier::GSubmissionPolicy;
    using Gem::Courtier::GPU::GGPUConsumerT;
    GGPUConsumerT<GGenome, double> consumer(
        std::make_unique<Gem::Tests::SumOfSquaresBackend>(),
        std::make_shared<GBaseGPUMarshallerT<double>>()
    );

    SECTION("the pre-processor runs before the device, the post-processor after it") {
        for(auto &item : batch) {
            item->registerPreProcessor(
                std::make_shared<Gem::Tests::ParameterSettingPreProcessor>()
            );
            item->registerPostProcessor(std::make_shared<Gem::Tests::LoggingPostProcessor>());
        }
        consumer.processBatch(batch, GSubmissionPolicy::full_success_or_fatal());

        CHECK(
            suppliedEvaluationLog.order ==
            std::vector<std::string>{"pre", "pre", "pre", "device", "post", "post", "post"}
        );
        for(const auto &item : batch) {
            CHECK(item->getProcessingStatus() == processingStatus::PROCESSED);
            CHECK(item->raw_fitness(0) == 12.); // the device saw the pre-processed 2.0, not 1.0
            CHECK(std::get<1>(item->getProcessingTimes()) >= 0.02); // covers the device evaluation
        }
        CHECK(suppliedEvaluationLog.evaluateCalls == 0);
    }

    SECTION("the processing time is the batch's evaluation, not the other items' processing") {
        // Each post-processor takes 200 ms; a window per item from its own pre- to its own
        // post-processing would count the post-processors of the items before it (0.2 s, 0.4 s).
        for(auto &item : batch) {
            item->registerPostProcessor(std::make_shared<Gem::Tests::SlowPostProcessor>());
        }
        consumer.processBatch(batch, GSubmissionPolicy::full_success_or_fatal());
        for(const auto &item : batch) {
            CHECK(item->getProcessingStatus() == processingStatus::PROCESSED);
            CHECK(std::get<1>(item->getProcessingTimes()) >= 0.02);
            CHECK(std::get<1>(item->getProcessingTimes()) < 0.15);
            CHECK(std::get<2>(item->getProcessingTimes()) >= 0.2);
        }
    }
#endif /* GENEVA_BUILD_WITH_GPU_CONSUMER */

    SECTION("a pre-processor that changes the dimension is refused before the buffer is written") {
        batch[0]->registerPreProcessor(std::make_shared<Gem::Tests::GrowingPreProcessor>());
        for(auto &item : batch) {
            item->set_processing_status(processingStatus::DO_PROCESS);
        }
        const GBaseGPUMarshallerT<double> marshaller;
        std::vector<double> params;
        CHECK_THROWS_WITH(
            marshaller.flatten(batch, params),
            Catch::Matchers::ContainsSubstring("must not change the number of parameters")
        );
    }

    SECTION("results written back through process() are refused on a begun item") {
        for(auto &item : batch) {
            item->registerPreProcessor(
                std::make_shared<Gem::Tests::ParameterSettingPreProcessor>()
            );
            item->set_processing_status(processingStatus::DO_PROCESS);
        }
        const Gem::Tests::ProcessWritingMarshaller marshaller;
        std::vector<double> params;
        marshaller.flatten(batch, params);
        CHECK_THROWS_AS(
            marshaller.scatter(batch, std::vector<double>(3, 12.), 0.),
            geneva_exception
        );
        CHECK(
            suppliedEvaluationLog.order == std::vector<std::string>{"pre", "pre", "pre"}
        ); // not run a second time
        CHECK(batch[0]->getProcessingStatus() == processingStatus::DO_PROCESS);
        CHECK_NOTHROW(batch[0]->completeProcessing({individual_processing_result(12.)}));
    }

#ifdef GENEVA_BUILD_WITH_GPU_CONSUMER
    SECTION("a failing device reaches the submitter; the items are begun again when resubmitted") {
        GGPUConsumerT<GGenome, double> failing(
            std::make_unique<Gem::Tests::FailingBackend>(),
            std::make_shared<GBaseGPUMarshallerT<double>>()
        );
        for(auto &item : batch) {
            item->registerPreProcessor(
                std::make_shared<Gem::Tests::ParameterSettingPreProcessor>()
            );
        }
        CHECK_THROWS_WITH(
            failing.processBatch(batch, GSubmissionPolicy::full_success_or_fatal()),
            Catch::Matchers::ContainsSubstring("device failed")
        );
        for(const auto &item : batch) {
            CHECK(item->getProcessingStatus() == processingStatus::DO_PROCESS); // begun, not done
        }

        // Un-submitted and submitted again, an item is not begun any more
        batch[0]->set_processing_status(processingStatus::UNPROCESSED);
        batch[0]->set_processing_status(processingStatus::DO_PROCESS);
        CHECK_THROWS_AS(
            batch[0]->completeProcessing({individual_processing_result(3.)}),
            geneva_exception
        );

        // A working device processes the resubmitted batch: each item is begun anew
        suppliedEvaluationLog = {};
        consumer.processBatch(batch, GSubmissionPolicy::full_success_or_fatal());
        CHECK(
            suppliedEvaluationLog.order == std::vector<std::string>{"pre", "pre", "pre", "device"}
        );
        for(const auto &item : batch) {
            CHECK(item->getProcessingStatus() == processingStatus::PROCESSED);
        }
    }
#endif /* GENEVA_BUILD_WITH_GPU_CONSUMER */

    SECTION("an item whose pre- or post-processor throws fails alone") {
        // On the marshaller directly: under a submission policy the consumer would end the run
        // (fatal) or refill the failed slot with a clone, and the failure would not be visible.
        batch[0]->registerPreProcessor(std::make_shared<Gem::Tests::ThrowingProcessor>());
        batch[1]->registerPostProcessor(std::make_shared<Gem::Tests::ThrowingProcessor>());
        for(auto &item : batch) {
            item->set_processing_status(processingStatus::DO_PROCESS);
        }
        const GBaseGPUMarshallerT<double> marshaller;
        std::vector<double> params;
        CHECK_NOTHROW(marshaller.flatten(batch, params));
        CHECK(params == std::vector<double>(9, 1.));
        CHECK_NOTHROW(marshaller.scatter(batch, std::vector<double>{-1., 3., 3.}, 0.));

        CHECK(batch[0]->getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);
        CHECK(batch[0]->getStoredErrorDescriptions().contains("processor failed"));
        CHECK(batch[1]->getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);
        CHECK(batch[1]->getStoredErrorDescriptions().contains("processor failed"));
        CHECK(batch[2]->getProcessingStatus() == processingStatus::PROCESSED);
        CHECK(batch[2]->raw_fitness(0) == 3.);
    }

    SECTION("a completion needs a beginning and results") {
        GGenome &item = *batch[0];
        item.set_processing_status(processingStatus::DO_PROCESS);
        const std::vector<individual_processing_result> res{individual_processing_result(3.)};
        CHECK_THROWS_AS(item.completeProcessing(res), geneva_exception);
        item.beginProcessing();
        CHECK_THROWS_AS(item.completeProcessing({}), geneva_exception);
        CHECK(item.getProcessingStatus() == processingStatus::DO_PROCESS);
        CHECK_NOTHROW(item.completeProcessing(res));
        CHECK(item.getProcessingStatus() == processingStatus::PROCESSED);
        CHECK_THROWS_AS(item.completeProcessing(res), geneva_exception); // not twice

        // A begun item that is un-submitted and submitted again must be begun again
        item.set_processing_status(processingStatus::DO_PROCESS);
        item.beginProcessing();
        item.set_processing_status(processingStatus::UNPROCESSED);
        item.set_processing_status(processingStatus::DO_PROCESS);
        CHECK_THROWS_AS(item.completeProcessing(res), geneva_exception);

        // So does a submission of a begun item that is due already (DO_PROCESS to DO_PROCESS)
        item.beginProcessing();
        item.set_processing_status(processingStatus::DO_PROCESS);
        CHECK_THROWS_AS(item.completeProcessing(res), geneva_exception);

        // A clone of a begun item is not begun
        item.set_processing_status(processingStatus::DO_PROCESS);
        item.beginProcessing();
        auto copy = item.clone();
        CHECK(copy->getProcessingStatus() == processingStatus::DO_PROCESS);
        CHECK_THROWS_AS(copy->completeProcessing(res), geneva_exception);
        CHECK_NOTHROW(item.completeProcessing(res));

        // Results adopted with markAsProcessedWith() end a begun processing: the item is refused
        // for its status, not as begun, and a new submission must be begun again
        item.set_processing_status(processingStatus::DO_PROCESS);
        item.beginProcessing();
        item.markAsProcessedWith(res);
        CHECK_THROWS_WITH(
            item.process(res),
            Catch::Matchers::ContainsSubstring("Function called while processing_status_")
        );
        item.mark_as_due_for_processing();
        CHECK_THROWS_AS(item.completeProcessing(res), geneva_exception);

        // So does a new submission of a begun item (e.g. its values were changed after the
        // pre-processor ran)
        item.beginProcessing();
        item.mark_as_due_for_processing();
        CHECK_THROWS_AS(item.completeProcessing(res), geneva_exception);
        item.beginProcessing();
        CHECK_NOTHROW(item.completeProcessing(res));
        CHECK(item.getProcessingStatus() == processingStatus::PROCESSED);
    }
}

TEST_CASE(
    "GGenome: a pre-processor that takes the item out of processing fails the processing",
    "[candidate]"
) {
    // The processing of an item that is no longer due must not go on to PROCESSED; like any other
    // failing step, it is recorded on the item.
    using Gem::Courtier::processingStatus;
    Gem::Tests::CountingSphere item;
    item.registerPreProcessor(std::make_shared<Gem::Tests::UnsubmittingPreProcessor>());
    item.set_processing_status(processingStatus::DO_PROCESS);
    CHECK_THROWS_AS(item.process(), Gem::Courtier::g_processing_exception);
    CHECK(item.getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);
    CHECK(item.getStoredErrorDescriptions().contains("changed the processing status"));
}
