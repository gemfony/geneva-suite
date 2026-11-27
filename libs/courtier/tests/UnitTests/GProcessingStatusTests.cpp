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

#include <sstream>
#include <vector>

#include "common/GSerializableFunctionObjectT.hpp"
#include "common/testing/GStandardTests.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GProcessingContainerT.hpp"
#include "courtier/gpu/GGPUKernelSpec.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp"
#include "courtier/transport/GPeerI.hpp"

using namespace Gem::Courtier;

namespace {

/******************************************************************************/
/**
 * A minimal processing container whose process_() behaviour is selectable:
 * succeed with a fixed result, flag a user error, or throw.
 */
class StatusProbeContainer : public GProcessingContainerT<StatusProbeContainer, double> {
    ///////////////////////////////////////////////////////////////////////

    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GProcessingContainerT<StatusProbeContainer, double>>(
            ar,
            "GProcessingContainerT_StatusProbeContainer",
            *this
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    enum class Mode {
        SUCCEED,
        FLAG_ERROR,
        THROW
    };

    explicit StatusProbeContainer(Mode mode)
      : mode_(mode) {
        this->mark_as_due_for_processing();
    }
    StatusProbeContainer() = default; // deserialization only

private:
    void process_([[maybe_unused]] const std::vector<double> &res_vec) final {
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
    explicit RecordingPostProcessor(bool *hit)
      : hit_(hit) {
    }
    RecordingPostProcessor() = default;

protected:
    bool process_([[maybe_unused]] StatusProbeContainer &p) override {
        if(hit_ != nullptr) {
            *hit_ = true;
        }
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

/******************************************************************************/
// The size check of markAsProcessedWith() was compiled only in Debug builds; it now runs in every
// build, so a result vector of the wrong size can no longer be stored unnoticed.
TEST_CASE(
    "GProcessingContainerT::markAsProcessedWith(): a result vector of the wrong size throws",
    "[proc][status]"
) {
    StatusProbeContainer c(StatusProbeContainer::Mode::SUCCEED);
    CHECK_THROWS_AS(c.markAsProcessedWith({1., 2.}), Gem::Common::gem_exception);
    CHECK(c.is_due_for_processing());
    CHECK_NOTHROW(c.markAsProcessedWith({3.}));
    CHECK(c.is_processed());
}

/******************************************************************************/
// The names of courtier's enums (enum_names_v): every name is pinned, as logs and messages quote
// them, and each name leads back to its value.
TEST_CASE("courtier's enums: every value has its name", "[proc][status][names]") {
    using Gem::Common::Testing::checkEnumNames;
    using Gem::Courtier::Consumers::GPeerClose;
    using Gem::Courtier::Consumers::GPeerEnd;
    using Gem::Courtier::GPU::BackendKind;
    checkEnumNames<processingStatus>(
        {{processingStatus::UNPROCESSED, "UNPROCESSED"},
         {processingStatus::DO_PROCESS, "DO_PROCESS"},
         {processingStatus::PROCESSED, "PROCESSED"},
         {processingStatus::EXCEPTION_CAUGHT, "EXCEPTION_CAUGHT"},
         {processingStatus::ERROR_FLAGGED, "ERROR_FLAGGED"}}
    );
    checkEnumNames<GFrameKind>(
        {{GFrameKind::NONE, "NONE"},
         {GFrameKind::PULL, "PULL"},
         {GFrameKind::WORK, "WORK"},
         {GFrameKind::RESULT, "RESULT"},
         {GFrameKind::HELLO, "HELLO"}}
    );
    checkEnumNames<run_state>(
        {{run_state::INIT, "INIT"}, {run_state::RUN, "RUN"}, {run_state::FINALLY, "FINALLY"}}
    );
    checkEnumNames<processRole>(
        {{processRole::SUBMITTER, "SUBMITTER"},
         {processRole::CLIENT, "CLIENT"},
         {processRole::BYSTANDER, "BYSTANDER"}}
    );
    checkEnumNames<GMPICascadeLayout>(
        {{GMPICascadeLayout::SUBMITTER_AND_GROUPS, "SUBMITTER_AND_GROUPS"},
         {GMPICascadeLayout::GROUPS_ONLY, "GROUPS_ONLY"}}
    );
    checkEnumNames<GPeerEnd>(
        {{GPeerEnd::CLOSED_HERE, "CLOSED_HERE"},
         {GPeerEnd::CLOSED_THERE, "CLOSED_THERE"},
         {GPeerEnd::SILENT, "SILENT"},
         {GPeerEnd::FAILED, "FAILED"}}
    );
    checkEnumNames<GPeerClose>(
        {{GPeerClose::NORMAL, "NORMAL"},
         {GPeerClose::ERROR, "ERROR"},
         {GPeerClose::RUN_ENDED, "RUN_ENDED"}}
    );
    checkEnumNames<BackendKind>({{BackendKind::CUDA, "cuda"}});
    CHECK(Gem::Courtier::GPU::backendKindFromString("cuda") == BackendKind::CUDA);
    CHECK_THROWS_AS(Gem::Courtier::GPU::backendKindFromString("cpu"), Gem::Common::gem_exception);

    std::ostringstream status;
    status << processingStatus::EXCEPTION_CAUGHT;
    CHECK(status.str() == "processingStatus::EXCEPTION_CAUGHT");
    StatusProbeContainer c(StatusProbeContainer::Mode::SUCCEED);
    CHECK(c.getProcessingStatusAsStr() == "DO_PROCESS");
}
