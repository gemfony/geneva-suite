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
 * @brief Tests for the pluggable evaluation fault injector (GFaultInjector / GFaultInjectorRegistry).
 * A registered injector is consulted inside GGenome::process(): a THROW fault must surface as
 * EXCEPTION_CAUGHT, a FLAG_ERROR fault as ERROR_FLAGGED, and with no injector (the default) evaluation
 * must proceed normally to PROCESSED. Both faults abort the evaluation and cause process() to raise a
 * processing exception, exactly like a genuine evaluation failure.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ranges>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "courtier/GCourtierEnums.hpp" // processingStatus
#include "geneva/GFaultInjector.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/oa/GSepCmaEvolutionStrategy.hpp"
#include "geneva/oa/GSwarmAlgorithm.hpp"
#include "geneva/oa/GSwarmAlgorithm_PersonalityTraits.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE + archive-generic dispatch

using namespace Gem::Geneva;
using namespace Gem::Geneva::Genome;
using Gem::Courtier::processingStatus;

namespace Gem::Tests {

/******************************************************************************/
/** A minimal flat individual: a sphere over a single double group. Fresh instances are DO_PROCESS. */
class FISphere : public GGenomeT<FISphere> {
public:
    using genome_only =
        void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    FISphere() {
        GGenomeBuilder b;
        b.addDoubleGroup(2, -10., 10.).init(1.0);
        this->setGenome(b.build());
    }
    FISphere(const FISphere &) = default;

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
    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<FISphere>>(ar, "GGenomeT", *this);
    }
};

/** @brief An injector that always requests the given fault. */
class ConstantInjector : public GFaultInjector {
public:
    explicit ConstantInjector(Fault fault)
      : fault_(fault) {
    }
    Fault evaluate(const GGenome & /*item*/, Gem::Hap::GRandomBase & /*gr*/) override {
        return fault_;
    }

private:
    Fault fault_;
};

/** @brief An injector that fails every n-th evaluation (the n-th, 2n-th, ...). */
class EveryNthInjector : public GFaultInjector {
public:
    explicit EveryNthInjector(std::size_t n)
      : n_(n) {
    }
    Fault evaluate(const GGenome & /*item*/, Gem::Hap::GRandomBase & /*gr*/) override {
        return (calls_.fetch_add(1) + 1) % n_ == 0 ? Fault::FLAG_ERROR : Fault::NONE;
    }

private:
    std::size_t n_;
    std::atomic<std::size_t> calls_{0};
};

/** @brief An injector that fails every evaluation of one swarm neighborhood in one iteration. */
class NeighborhoodInjector : public GFaultInjector {
public:
    NeighborhoodInjector(std::size_t neighborhood, std::uint32_t iteration)
      : neighborhood_(neighborhood)
      , iteration_(iteration) {
    }
    Fault evaluate(const GGenome &item, Gem::Hap::GRandomBase & /*gr*/) override {
        const auto traits = std::dynamic_pointer_cast<
            Gem::Geneva::OptimizationAlgorithms::GSwarmAlgorithm_PersonalityTraits
        >(item.scratch().personalityRef());
        if(traits && traits->getNeighborhood() == neighborhood_ &&
           item.getAssignedIteration() == iteration_) {
            n_failed_.fetch_add(1);
            return Fault::FLAG_ERROR;
        }
        return Fault::NONE;
    }
    [[nodiscard]] std::size_t nFailed() const {
        return n_failed_.load();
    }

private:
    std::size_t neighborhood_;
    std::uint32_t iteration_;
    std::atomic<std::size_t> n_failed_{0};
};

} // namespace Gem::Tests
GEM_REGISTER_ARCHIVABLE(Gem::Tests::FISphere) // NOLINT

using namespace Gem::Tests;

/******************************************************************************/
TEST_CASE("fault injector: default (none registered) leaves evaluation untouched", "[faultinj]") {
    GFaultInjectorRegistry::clear();
    CHECK(GFaultInjectorRegistry::get() == nullptr);

    FISphere ind;
    REQUIRE(ind.is_due_for_processing());
    CHECK_NOTHROW(ind.process());
    CHECK(ind.is_processed());
    CHECK_FALSE(ind.has_errors());
}

/******************************************************************************/
TEST_CASE("fault injector: a THROW fault surfaces as EXCEPTION_CAUGHT", "[faultinj]") {
    GFaultInjectorRegistry::set(std::make_unique<ConstantInjector>(GFaultInjector::Fault::THROW));

    FISphere ind;
    REQUIRE(ind.is_due_for_processing());
    CHECK_THROWS(ind.process()); // process() rethrows a processing exception on failure
    CHECK(ind.has_errors());
    CHECK(ind.getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);

    GFaultInjectorRegistry::clear();
}

/******************************************************************************/
TEST_CASE("fault injector: a FLAG_ERROR fault surfaces as ERROR_FLAGGED", "[faultinj]") {
    GFaultInjectorRegistry::set(
        std::make_unique<ConstantInjector>(GFaultInjector::Fault::FLAG_ERROR)
    );

    FISphere ind;
    REQUIRE(ind.is_due_for_processing());
    CHECK_THROWS(ind.process());
    CHECK(ind.has_errors());
    CHECK(ind.getProcessingStatus() == processingStatus::ERROR_FLAGGED);

    GFaultInjectorRegistry::clear();
}

/******************************************************************************/
TEST_CASE("fault injector: an explicit NONE fault is a pass-through", "[faultinj]") {
    GFaultInjectorRegistry::set(std::make_unique<ConstantInjector>(GFaultInjector::Fault::NONE));

    FISphere ind;
    CHECK_NOTHROW(ind.process());
    CHECK(ind.is_processed());
    CHECK_FALSE(ind.has_errors());

    GFaultInjectorRegistry::clear();
}

/******************************************************************************/
TEST_CASE("fault injector: clearing restores the zero-cost default", "[faultinj]") {
    GFaultInjectorRegistry::set(std::make_unique<ConstantInjector>(GFaultInjector::Fault::THROW));
    REQUIRE(GFaultInjectorRegistry::get() != nullptr);

    GFaultInjectorRegistry::clear();
    CHECK(GFaultInjectorRegistry::get() == nullptr);

    FISphere ind;
    CHECK_NOTHROW(ind.process());
    CHECK(ind.is_processed());
}

/******************************************************************************/
// Algorithms under failing evaluations

TEST_CASE("sep-CMA-ES keeps its lambda offspring when evaluations fail", "[faultinj][oa]") {
    // The consumer replaces a failed offspring by a clone of a successful one (the submission policy
    // of the tolerant algorithms), so the population keeps its lambda members and the run its
    // progress although a third of all evaluations fail.
    namespace oa = Gem::Geneva::OptimizationAlgorithms;
    GFaultInjectorRegistry::set(std::make_unique<EveryNthInjector>(3));

    auto pop = std::make_shared<oa::GSepCmaEvolutionStrategy>();
    pop->setLambda(12);
    pop->setMaxIteration(15);
    pop->setMaxStallIteration(0);
    pop->setReportIteration(100000);
    pop->setEmitTerminationReason(false);
    pop->push_back(FISphere().clone());
    CHECK_NOTHROW(pop->optimize());
    GFaultInjectorRegistry::clear();

    CHECK(pop->size() == 12);
    auto best = pop->getBestGlobalIndividual<FISphere>();
    REQUIRE(best);
    CHECK(best->is_processed());
    CHECK(std::isfinite(best->raw_fitness(0)));
    CHECK(best->raw_fitness(0) < 2.); // not worse than the start (1, 1)
}

TEST_CASE("the swarm survives the failure of a whole neighborhood", "[faultinj][oa]") {
    // Regression: with every member of a neighborhood gone after an evaluation, the search for
    // the neighborhood's best read the element at its (empty) start: for the last neighborhood the
    // one behind the population.
    namespace oa = Gem::Geneva::OptimizationAlgorithms;

    SECTION("in a later iteration the neighborhood is refilled and the run goes on") {
        auto injector = std::make_unique<NeighborhoodInjector>(2, 2);
        const NeighborhoodInjector *const watch = injector.get();
        GFaultInjectorRegistry::set(std::move(injector));

        auto pop = std::make_shared<oa::GSwarmAlgorithm>();
        pop->setSwarmSizes(3, 4); // 3 neighborhoods x 4 members
        pop->setMaxIteration(6);
        pop->setMaxStallIteration(0);
        pop->setReportIteration(100000);
        pop->setEmitTerminationReason(false);
        pop->push_back(FISphere().clone());
        CHECK_NOTHROW(pop->optimize());
        CHECK(watch->nFailed() == 4); // the case was really met
        GFaultInjectorRegistry::clear();

        CHECK(pop->size() == 12);
        auto best = pop->getBestGlobalIndividual<FISphere>();
        REQUIRE(best);
        CHECK(std::isfinite(best->raw_fitness(0)));
    }

    SECTION("in the first iteration there is nothing to seed its best with: an error says so") {
        GFaultInjectorRegistry::set(std::make_unique<NeighborhoodInjector>(2, 0));

        auto pop = std::make_shared<oa::GSwarmAlgorithm>();
        pop->setSwarmSizes(3, 4);
        pop->setMaxIteration(3);
        pop->setReportIteration(100000);
        pop->setEmitTerminationReason(false);
        pop->push_back(FISphere().clone());
        CHECK_THROWS_WITH(
            pop->optimize(),
            Catch::Matchers::ContainsSubstring("cannot seed its best")
        );
        GFaultInjectorRegistry::clear();
    }
}
