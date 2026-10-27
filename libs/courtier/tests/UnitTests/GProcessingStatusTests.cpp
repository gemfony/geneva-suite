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
 * @brief Regression tests for the GProcessingContainerT<>::process() status contract.
 *
 * Pins the error-status lifecycle: an error flagged from inside the user's process_()
 * implementation (via GProcessable::force_set_error) must survive as ERROR_FLAGGED and
 * cause process() to raise a g_processing_exception -- it must NOT be clobbered by the
 * terminal PROCESSED assignment (a defect fixed 2026-07-18: an unconditional
 * `processing_status_ = PROCESSED` after the timing block silently converted
 * error-flagged items into successfully processed ones). Post-processing must be
 * skipped for an error-flagged item (a post-processor refines an already-evaluated,
 * CLEAN item).
 */

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "common/GSerializableFunctionObjectT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GProcessingContainerT.hpp"

using namespace Gem::Courtier;

namespace {

/******************************************************************************/
/**
 * A minimal processing container whose process_() behaviour is selectable:
 * succeed with a fixed result, flag a user error, or throw.
 */
class StatusProbeContainer
  : public GProcessingContainerT<StatusProbeContainer, double> {
    ///////////////////////////////////////////////////////////////////////

    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GProcessingContainerT<StatusProbeContainer, double>>(
            ar, "GProcessingContainerT_StatusProbeContainer", *this);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    enum class Mode { SUCCEED, FLAG_ERROR, THROW };

    explicit StatusProbeContainer(Mode mode) : mode_(mode) {
        this->mark_as_due_for_processing();
    }
    StatusProbeContainer() = default; // deserialization only

private:
    void process_(
        [[maybe_unused]] const std::vector<double> &res_vec = std::vector<double>()
    ) final {
        switch(mode_) {
            case Mode::SUCCEED:
                this->registerResult(0, 42.);
                break;
            case Mode::FLAG_ERROR:
                this->force_set_error("StatusProbeContainer: user-flagged error\n");
                break;
            case Mode::THROW:
                throw std::runtime_error("StatusProbeContainer: thrown from process_()");
        }
    }

    Mode mode_ = Mode::SUCCEED;
};

/******************************************************************************/
/** A post-processor that records whether it was invoked (observation only, not serialized). */
class RecordingPostProcessor
  : public Gem::Common::GSerializableFunctionObjectT<StatusProbeContainer> {
public:
    explicit RecordingPostProcessor(bool *hit) : hit_(hit) {}
    RecordingPostProcessor() = default;

protected:
    bool process_([[maybe_unused]] StatusProbeContainer &p) override {
        if(hit_ != nullptr) { *hit_ = true; }
        return true;
    }

private:
    [[nodiscard]] RecordingPostProcessor *clone_() const override {
        return new RecordingPostProcessor(*this);
    }

    bool *hit_ = nullptr;
};

} // namespace

/******************************************************************************/
TEST_CASE("GProcessingContainerT::process(): clean run ends PROCESSED", "[proc][status]") {
    StatusProbeContainer c(StatusProbeContainer::Mode::SUCCEED);
    REQUIRE(c.is_due_for_processing());

    CHECK_NOTHROW(c.process());
    CHECK(c.is_processed());
    CHECK_FALSE(c.has_errors());
    CHECK(c.getStoredResult() == 42.);
}

/******************************************************************************/
TEST_CASE(
    "GProcessingContainerT::process(): an error flagged inside process_ survives as ERROR_FLAGGED",
    "[proc][status]"
) {
    StatusProbeContainer c(StatusProbeContainer::Mode::FLAG_ERROR);
    REQUIRE(c.is_due_for_processing());

    // The user-flagged error must abort processing with a processing exception ...
    CHECK_THROWS_AS(c.process(), g_processing_exception);

    // ... and must NOT have been clobbered into PROCESSED by the terminal status assignment
    CHECK(c.has_errors());
    CHECK(c.getProcessingStatus() == processingStatus::ERROR_FLAGGED);
    CHECK_FALSE(c.is_processed());

    // An error-flagged item exposes no results
    CHECK_THROWS(c.getStoredResult());
}

/******************************************************************************/
TEST_CASE(
    "GProcessingContainerT::process(): an exception from process_ surfaces as EXCEPTION_CAUGHT",
    "[proc][status]"
) {
    StatusProbeContainer c(StatusProbeContainer::Mode::THROW);
    REQUIRE(c.is_due_for_processing());

    CHECK_THROWS_AS(c.process(), g_processing_exception);
    CHECK(c.has_errors());
    CHECK(c.getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);
}

/******************************************************************************/
TEST_CASE(
    "GProcessingContainerT::process(): post-processing runs on clean items only",
    "[proc][status]"
) {
    // Clean item: the registered post-processor is invoked
    bool hit_on_success = false;
    StatusProbeContainer ok(StatusProbeContainer::Mode::SUCCEED);
    ok.registerPostProcessor(std::make_shared<RecordingPostProcessor>(&hit_on_success));
    CHECK_NOTHROW(ok.process());
    CHECK(hit_on_success);

    // Error-flagged item: post-processing is skipped (it refines an already-evaluated, clean item)
    bool hit_on_error = false;
    StatusProbeContainer bad(StatusProbeContainer::Mode::FLAG_ERROR);
    bad.registerPostProcessor(std::make_shared<RecordingPostProcessor>(&hit_on_error));
    CHECK_THROWS_AS(bad.process(), g_processing_exception);
    CHECK_FALSE(hit_on_error);
    CHECK(bad.getProcessingStatus() == processingStatus::ERROR_FLAGGED);
}
