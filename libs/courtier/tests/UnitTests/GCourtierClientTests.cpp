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
 * Tests of what a client does with a work item that need no server: processWorkFrame() (how an
 * item is processed, and how a failure of its processing comes back) and the RESULT of an item
 * that failed with nothing to return (makeFailedResult()).
 */

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "GCapturedLog.hpp"
#include "GDemoProcessingContainers.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GProcessingContainerT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

using Gem::Courtier::GCommandContainerT;
using Gem::Courtier::GFaultyContainer;

/******************************************************************************/
// An item whose processing throws something other than a g_processing_exception -- here the
// gem_exception the processing lifecycle throws for an item that is not due -- used to leave
// processWorkFrame() and end the worker. It is returned flagged instead; only a WORK frame without
// an item is still an error.

TEST_CASE(
    "courtier(client): processWorkFrame returns an item that cannot be processed flagged",
    "[courtier][client]"
) {
    using Gem::Courtier::fault_mode;
    using Gem::Courtier::GFrameKind;
    using Gem::Courtier::processingStatus;

    SECTION("an item that was already processed comes back with an error") {
        auto item = std::make_unique<GFaultyContainer>(0, fault_mode::NONE);
        item->set_processing_status(processingStatus::DO_PROCESS);
        item->process();
        REQUIRE(item->is_processed());

        GCommandContainerT<GFaultyContainer> frame{GFrameKind::WORK, std::move(item)};
        CHECK_NOTHROW(Gem::Courtier::processWorkFrame(frame, "test worker"));
        REQUIRE(frame.item() != nullptr);
        CHECK(frame.item()->getProcessingStatus() == processingStatus::ERROR_FLAGGED);
        CHECK(
            frame.item()->getStoredErrorDescriptions().find("could not be processed") !=
            std::string::npos
        );
    }

    SECTION("an item that already reports an error keeps it") {
        auto item = std::make_unique<GFaultyContainer>(0, fault_mode::THROW_PROCESSING);
        item->set_processing_status(processingStatus::DO_PROCESS);
        CHECK_THROWS(item->process()); // the user's code fails: EXCEPTION_CAUGHT
        REQUIRE(item->getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);

        GCommandContainerT<GFaultyContainer> frame{GFrameKind::WORK, std::move(item)};
        CHECK_NOTHROW(Gem::Courtier::processWorkFrame(frame, "test worker"));
        REQUIRE(frame.item() != nullptr);
        CHECK(frame.item()->getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);
    }

    SECTION("a WORK frame without an item is a protocol error") {
        GCommandContainerT<GFaultyContainer> frame{GFrameKind::WORK};
        CHECK_THROWS(Gem::Courtier::processWorkFrame(frame, "test worker"));
    }
}

/******************************************************************************/
// A client can be given a processor that takes the place of a due item's process()
// (GBaseClientT::setItemProcessor()). processWorkFrame() applies it; an item that is not due and a
// frame without an item never reach it, and whatever it throws leaves the item flagged.

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one case, a section per rule
TEST_CASE(
    "courtier(client): processWorkFrame applies an item processor to a due item",
    "[courtier][client]"
) {
    using Gem::Courtier::fault_mode;
    using Gem::Courtier::GFrameKind;
    using Gem::Courtier::GItemProcessorT;
    using Gem::Courtier::processingStatus;

    // GFaultyContainer's own process_() stores the result true; the marking processor stores false.
    int calls = 0;
    GItemProcessorT<GFaultyContainer> marking{[&calls](GFaultyContainer &item) {
        ++calls;
        item.markAsProcessedWith({false});
    }};

    const auto due_frame = [] {
        auto item = std::make_unique<GFaultyContainer>(0, fault_mode::NONE);
        item->set_processing_status(processingStatus::DO_PROCESS);
        return GCommandContainerT<GFaultyContainer>{GFrameKind::WORK, std::move(item)};
    };

    SECTION("no processor") {
        auto frame = due_frame();
        CHECK_NOTHROW(Gem::Courtier::processWorkFrame(frame, "w", nullptr));
        REQUIRE(frame.item() != nullptr);
        REQUIRE(frame.item()->is_processed());
        CHECK(frame.item()->getStoredResult());
    }

    SECTION("a processor that marks") {
        auto frame = due_frame();
        CHECK_NOTHROW(Gem::Courtier::processWorkFrame(frame, "w", &marking));
        REQUIRE(frame.item() != nullptr);
        REQUIRE(frame.item()->is_processed());
        CHECK_FALSE(frame.item()->getStoredResult());
        CHECK(calls == 1);
    }

    SECTION("a processor that throws a std::exception") {
        GItemProcessorT<GFaultyContainer> throwing{[](GFaultyContainer & /*item*/) {
            throw std::runtime_error("injected");
        }};
        auto frame = due_frame();
        CHECK_NOTHROW(Gem::Courtier::processWorkFrame(frame, "w", &throwing));
        REQUIRE(frame.item() != nullptr);
        CHECK(frame.item()->getProcessingStatus() == processingStatus::ERROR_FLAGGED);
        CHECK(frame.item()->getStoredErrorDescriptions().contains("injected"));
    }

    SECTION("a processor that throws something that is not a std::exception") {
        GItemProcessorT<GFaultyContainer> throwing{[](GFaultyContainer & /*item*/) {
            throw 42; // NOLINT(bugprone-std-exception-baseclass): the case under test
        }};
        auto frame = due_frame();
        CHECK_NOTHROW(Gem::Courtier::processWorkFrame(frame, "w", &throwing));
        REQUIRE(frame.item() != nullptr);
        CHECK(frame.item()->getProcessingStatus() == processingStatus::ERROR_FLAGGED);
        CHECK(
            frame.item()->getStoredErrorDescriptions().contains("not derived from std::exception")
        );
    }

    SECTION("an item that is not due does not reach the processor") {
        auto item = std::make_unique<GFaultyContainer>(0, fault_mode::NONE);
        item->set_processing_status(processingStatus::DO_PROCESS);
        item->process();
        REQUIRE(item->is_processed());

        GCommandContainerT<GFaultyContainer> frame{GFrameKind::WORK, std::move(item)};
        CHECK_NOTHROW(Gem::Courtier::processWorkFrame(frame, "w", &marking));
        CHECK(calls == 0);
        REQUIRE(frame.item() != nullptr);
        CHECK(frame.item()->getProcessingStatus() == processingStatus::ERROR_FLAGGED);
    }

    SECTION("a WORK frame without an item does not reach the processor") {
        GCommandContainerT<GFaultyContainer> frame{GFrameKind::WORK};
        CHECK_THROWS(Gem::Courtier::processWorkFrame(frame, "w", &marking));
        CHECK(calls == 0);
    }
}

/******************************************************************************/
// An item that failed with nothing to return (its payload could not be read, its processing or its
// return threw, its result could not be encoded) goes back as a RESULT of the outcome alone: the ids
// from the head of its WORK name it, it is flagged, and the frame carries no result.

TEST_CASE(
    "courtier(client): a failed item is returned as a RESULT of its ids without a result",
    "[courtier][client]"
) {
    using Gem::Courtier::GFrameKind;
    using Gem::Courtier::processingStatus;
    constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

    Gem::Courtier::GProcessingOutcome named;
    named.correlation_id = 4711;
    named.submission_uuid = {17, 18};
    GCommandContainerT<GFaultyContainer> frame{
        GFrameKind::WORK,
        std::make_unique<GFaultyContainer>(0)
    };
    frame.setTag(3);
    frame.setOutcome(named);
    Gem::Courtier::makeFailedResult(frame, frame.outcome(), "it could not be read");

    GCommandContainerT<GFaultyContainer> got{GFrameKind::NONE};
    Gem::Courtier::container_from_string(Gem::Courtier::container_to_string(frame, BIN), got, BIN);
    CHECK(got.kind() == GFrameKind::RESULT);
    CHECK(got.tag() == 3);
    CHECK(got.payloadEmpty());
    CHECK(got.outcome().correlation_id == 4711);
    CHECK(got.outcome().submission_uuid == named.submission_uuid);
    CHECK(got.outcome().status == processingStatus::ERROR_FLAGGED);
    CHECK(got.outcome().error_descriptions.contains("it could not be read"));
}

/******************************************************************************/
// A work item whose processing fails is reported once, where its task ends: by the worker that
// returns it flagged, or by the thread pool of a local consumer -- in the log and in
// GEM-EXCEPTION.log. The throw and the rethrow on the way (the item's processing lifecycle turns the
// failure into a g_processing_exception) write nothing.

namespace {

/** @brief Runs a test in a working directory of its own, where GEM-EXCEPTION.log is written */
class ScratchDirectory {
public:
    explicit ScratchDirectory(std::string const &tag)
      : previous_(std::filesystem::current_path())
      , path_(
            std::filesystem::temp_directory_path() /
            ("gem_courtier_error_report_" + tag + "_" + std::to_string(::getpid()))
        ) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
        std::filesystem::current_path(path_);
    }
    ScratchDirectory(ScratchDirectory const &) = delete;
    ScratchDirectory(ScratchDirectory &&) = delete;
    ScratchDirectory &operator=(ScratchDirectory const &) = delete;
    ScratchDirectory &operator=(ScratchDirectory &&) = delete;
    ~ScratchDirectory() {
        std::error_code ec;
        std::filesystem::current_path(previous_, ec);
        std::filesystem::remove_all(path_, ec);
    }

private:
    std::filesystem::path previous_;
    std::filesystem::path path_;
};

/** @brief How often the injected failure of GFaultyContainer is in GEM-EXCEPTION.log */
std::size_t recordedFailures() {
    std::ifstream in("GEM-EXCEPTION.log");
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    constexpr std::string_view FAILURE = "injected g_processing_exception";
    std::size_t n = 0;
    for(std::size_t pos = text.find(FAILURE); pos != std::string::npos;
        pos = text.find(FAILURE, pos + FAILURE.size())) {
        ++n;
    }
    return n;
}

} // namespace

TEST_CASE(
    "courtier(client): a work item whose processing fails is reported once by the worker",
    "[courtier][client][error-report]"
) {
    using Gem::Courtier::fault_mode;
    using Gem::Courtier::GFrameKind;
    using Gem::Courtier::processingStatus;

    const ScratchDirectory directory("worker");
    const auto log = std::make_shared<Gem::Courtier::Tests::CapturedLog>();
    GEM_LOGGER.addLogTarget(log);

    auto item = std::make_unique<GFaultyContainer>(0, fault_mode::THROW_PROCESSING);
    item->set_processing_status(processingStatus::DO_PROCESS);
    GCommandContainerT<GFaultyContainer> frame{GFrameKind::WORK, std::move(item)};
    CHECK_NOTHROW(Gem::Courtier::processWorkFrame(frame, "test worker"));
    GEM_LOGGER.resetLogTargets();

    CHECK(frame.item()->getProcessingStatus() == processingStatus::EXCEPTION_CAUGHT);
    CHECK(log->countLinesWith({"injected g_processing_exception", "ERROR"}) == 1);
    CHECK(recordedFailures() == 1);
}

TEST_CASE(
    "courtier(client): a work item that fails in a local consumer is reported once per attempt",
    "[courtier][client][error-report]"
) {
    using Gem::Courtier::fault_mode;
    using Gem::Courtier::GStdThreadConsumerConfig;
    using Gem::Courtier::GStdThreadConsumerT;
    using Gem::Courtier::GSubmissionPolicy;

    const ScratchDirectory directory("consumer");
    const auto log = std::make_shared<Gem::Courtier::Tests::CapturedLog>();
    GEM_LOGGER.addLogTarget(log);

    std::vector<std::unique_ptr<GFaultyContainer>> batch;
    for(std::size_t i = 0; i < 4; ++i) {
        batch.push_back(
            std::make_unique<GFaultyContainer>(
                i,
                i == 2 ? fault_mode::THROW_PROCESSING : fault_mode::NONE
            )
        );
    }
    GStdThreadConsumerT<GFaultyContainer> consumer(GStdThreadConsumerConfig{.nThreads = 2});
    // One failed attempt, then the slot is refilled with a clone of a survivor
    consumer.processBatch(
        std::span<std::unique_ptr<GFaultyContainer>>(batch.data(), batch.size()),
        GSubmissionPolicy::clone_on_partial_return()
    );
    GEM_LOGGER.resetLogTargets();

    CHECK(log->countLinesWith({"injected g_processing_exception", "ERROR"}) == 1);
    CHECK(recordedFailures() == 1);
}
