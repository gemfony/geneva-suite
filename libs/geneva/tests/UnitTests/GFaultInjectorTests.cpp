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
#include <functional>
#include <memory>
#include <ranges>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "courtier/GCourtierEnums.hpp" // processingStatus
#include "geneva/GFaultInjector.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE + archive-generic dispatch
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"

using namespace Gem::Geneva;
using namespace Gem::Geneva::Genome;
using Gem::Courtier::processingStatus;

namespace Gem::Tests {

/******************************************************************************/
/** A minimal flat individual: a sphere over a single double group. Fresh instances are DO_PROCESS. */
class FISphere : public GGenomeT<FISphere> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
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
            v | std::views::transform([](double x) { return x * x; }), 0., std::plus{})};
    }

private:
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<FISphere>>(ar, "GGenomeT", *this);
    }
};

/** @brief An injector that always requests the given fault. */
class ConstantInjector : public GFaultInjector {
public:
    explicit ConstantInjector(Fault fault) : fault_(fault) {}
    Fault evaluate(const GGenome & /*item*/, Gem::Hap::GRandomBase & /*gr*/) override {
        return fault_;
    }

private:
    Fault fault_;
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
    GFaultInjectorRegistry::set(std::make_unique<ConstantInjector>(GFaultInjector::Fault::FLAG_ERROR));

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
