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

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "common/GCommonEnums.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "GDemoProcessingContainers.hpp"

using namespace Gem::Courtier;

namespace {

using test_container_type = GCommandContainerT<GSimpleContainer>;

/** @brief The two GArchive codecs a frame must survive unchanged */
const auto ALL_MODES = {
    Gem::Common::serializationMode::GEM_BINARY, // GArchive flat-binary wire codec
    Gem::Common::serializationMode::GEM_JSON    // GArchive JSON wire codec
};

/** @brief Builds a WORK frame carrying a work item, so the archive has real content */
test_container_type makeWorkFrame() {
    test_container_type frame{GFrameKind::WORK, std::make_unique<GSimpleContainer>(42)};
    frame.setTag(7);
    return frame;
}

} // anonymous namespace

/******************************************************************************/
/**
 * container_to_string() must return a non-empty document in every (GArchive) mode.
 */
TEST_CASE("container_to_string emits a non-empty document in every mode", "[courtier][serialization]") {
    const auto container = makeWorkFrame();

    REQUIRE_FALSE(container_to_string(container, Gem::Common::serializationMode::GEM_BINARY).empty());
    REQUIRE_FALSE(container_to_string(container, Gem::Common::serializationMode::GEM_JSON).empty());
}

/******************************************************************************/
/**
 * The companion round-trip: every serialization mode must survive a write/read cycle with the frame
 * kind, the library tag, the work item and the item's stable lineage id intact.
 */
TEST_CASE("A WORK frame survives a round-trip in every serialization mode", "[courtier][serialization]") {
    for(const auto mode : ALL_MODES) {
        INFO("serialization mode: " << Gem::Common::serModeToString(mode));

        const auto original = makeWorkFrame();
        REQUIRE(original.item() != nullptr);
        const auto original_uuid = original.item()->getSubmissionUuid();

        const std::string archive = container_to_string(original, mode);

        test_container_type restored{GFrameKind::NONE};
        REQUIRE_NOTHROW(container_from_string(archive, restored, mode));

        CHECK(restored.kind() == GFrameKind::WORK);
        CHECK(restored.tag() == 7);
        REQUIRE(restored.item() != nullptr);
        CHECK(restored.item()->getSubmissionUuid() == original_uuid);
    }
}

/******************************************************************************/
/**
 * The protocol contract this suite exists for (it replaces the old one-payload-shape round-trip):
 * EVERY frame kind, with EVERY payload alternative it may carry, round-trips in BOTH codecs -- and a
 * frame carries ONLY its own alternative, so nothing inert rides along.
 */
TEST_CASE("Every frame kind round-trips with exactly its own payload", "[courtier][serialization]") {
    for(const auto mode : ALL_MODES) {
        INFO("serialization mode: " << Gem::Common::serModeToString(mode));

        // --- the payload-free PULL ----------------------------------------------------------------
        {
            const test_container_type sent{GFrameKind::PULL};
            test_container_type got{GFrameKind::NONE};
            REQUIRE_NOTHROW(container_from_string(container_to_string(sent, mode), got, mode));
            CHECK(got.kind() == GFrameKind::PULL);
            CHECK(got.payloadEmpty());
        }

        // --- the HELLO: a build, a program and the keep-alive interval in the head -----------
        {
            test_container_type sent{GFrameKind::NONE};
            sent.setHello("1.2.3-test", "a program", std::chrono::milliseconds(2500));
            test_container_type got{GFrameKind::NONE};
            REQUIRE_NOTHROW(container_from_string(container_to_string(sent, mode), got, mode));
            CHECK(got.kind() == GFrameKind::HELLO);
            CHECK(got.build() == "1.2.3-test");
            CHECK(got.program() == "a program");
            CHECK(got.keepAlive() == std::chrono::milliseconds(2500));
            CHECK(got.payloadEmpty());
        }

        // --- the blob cache-miss fetch pair ----------------------------------------------------
        {
            const GWireBlobId id{0xDEADBEEFULL, 0xFEEDFACEULL};
            test_container_type sent{GFrameKind::BLOB_REQUEST};
            sent.setBlobRequest(id);
            test_container_type got{GFrameKind::NONE};
            REQUIRE_NOTHROW(container_from_string(container_to_string(sent, mode), got, mode));
            CHECK(got.kind() == GFrameKind::BLOB_REQUEST);
            REQUIRE(got.blobRequest() != nullptr);
            CHECK(got.blobRequest()->id == id);
            CHECK(got.item() == nullptr);
        }
        {
            const GWireBlobId id{1, 2};
            test_container_type sent{GFrameKind::BLOB_REPLY};
            sent.setBlobReply(id, "the-serialized-blob");
            test_container_type got{GFrameKind::NONE};
            REQUIRE_NOTHROW(container_from_string(container_to_string(sent, mode), got, mode));
            CHECK(got.kind() == GFrameKind::BLOB_REPLY);
            REQUIRE(got.blobReply() != nullptr);
            CHECK(got.blobReply()->id == id);
            CHECK(got.blobReply()->blob == "the-serialized-blob");
        }

        // --- a RESULT frame: the processing outcome travels with it ----------------------------
        {
            test_container_type sent{GFrameKind::RESULT};
            GProcessingOutcome outcome;
            outcome.status = processingStatus::ERROR_FLAGGED;
            outcome.error_descriptions = "the worker complained";
            outcome.processing_time = 1.5;
            outcome.correlation_id = 0xABCDEF;
            sent.setOutcome(outcome);
            sent.setItem(std::make_unique<GSimpleContainer>(3));

            test_container_type got{GFrameKind::NONE};
            REQUIRE_NOTHROW(container_from_string(container_to_string(sent, mode), got, mode));
            CHECK(got.kind() == GFrameKind::RESULT);
            CHECK(got.outcome().status == processingStatus::ERROR_FLAGGED);
            CHECK(got.outcome().error_descriptions == "the worker complained");
            CHECK(got.outcome().processing_time == 1.5);
            CHECK(got.outcome().correlation_id == 0xABCDEF);
            CHECK(got.item() != nullptr);
        }

        // --- a RESULT's outcome carries the lineage id too: the server's check-in routes the result
        // by the correlation id and checks it by the lineage id ----------------------------------
        {
            test_container_type sent{GFrameKind::RESULT};
            GProcessingOutcome outcome;
            outcome.status = processingStatus::PROCESSED;
            outcome.processing_time = 0.25;
            outcome.correlation_id = 0x5150;
            outcome.submission_uuid = {0x1111'2222'3333'4444ULL, 77};
            sent.setOutcome(outcome);
            sent.setItem(std::make_unique<GSimpleContainer>(4));

            test_container_type got{GFrameKind::NONE};
            REQUIRE_NOTHROW(container_from_string(container_to_string(sent, mode), got, mode));
            CHECK(got.kind() == GFrameKind::RESULT);
            CHECK(got.outcome().status == processingStatus::PROCESSED);
            CHECK(got.outcome().processing_time == 0.25);
            CHECK(got.outcome().correlation_id == 0x5150);
            CHECK(got.outcome().submission_uuid == outcome.submission_uuid);
            CHECK(got.item() != nullptr);
        }
    }
}

/******************************************************************************/
/**
 * A message from the network may be anything. One whose kind is not a GFrameKind is not a frame,
 * and decoding it fails, however the rest of it would decode: a server refuses it as a message it
 * cannot read, instead of taking it for a frame of an unknown kind.
 */
TEST_CASE("A message whose kind is not a GFrameKind does not decode", "[courtier][serialization]") {
    for(const auto mode : ALL_MODES) {
        INFO("serialization mode: " << Gem::Common::serModeToString(mode));

        test_container_type sent{GFrameKind::PULL};
        sent.setKind(static_cast<GFrameKind>(42));
        const std::string message = container_to_string(sent, mode);

        test_container_type got{GFrameKind::NONE};
        CHECK_THROWS(container_from_string(message, got, mode));

        // A value below the highest kind that GFrameKind does not have is no frame either
        test_container_type unused{GFrameKind::PULL};
        unused.setKind(static_cast<GFrameKind>(5));
        CHECK_THROWS(container_from_string(container_to_string(unused, mode), got, mode));

        // The highest kind there is still decodes.
        test_container_type last{GFrameKind::NONE};
        last.setHello("build", "", std::chrono::milliseconds::zero());
        REQUIRE_NOTHROW(container_from_string(container_to_string(last, mode), got, mode));
        CHECK(got.kind() == GFrameKind::HELLO);
    }
}

/******************************************************************************/

/******************************************************************************/
/**
 * The head of a frame is read first and by itself: a WORK whose payload cannot be read still names
 * its item (correlation id and lineage id) and its kind, so that its reader can fail exactly that
 * item. A message whose head cannot be read is refused as a whole.
 */
TEST_CASE("The head of a frame is read by itself", "[courtier][serialization]") {
    for(const auto mode : ALL_MODES) {
        INFO("serialization mode: " << Gem::Common::serModeToString(mode));

        auto sent = makeWorkFrame();
        GProcessingOutcome named;
        named.correlation_id = 0x7777;
        named.submission_uuid = {5, 6};
        sent.setOutcome(named);
        const std::string message = container_to_string(sent, mode);

        test_container_type got{GFrameKind::NONE};
        const std::string cut = message.substr(0, message.size() - 8);
        std::string_view payload;
        REQUIRE_NOTHROW(payload = frameHeadFromString(std::string_view{cut}, got, mode));
        CHECK(got.kind() == GFrameKind::WORK);
        CHECK(got.tag() == 7);
        CHECK(got.outcome().correlation_id == 0x7777);
        CHECK(got.outcome().submission_uuid == named.submission_uuid);
        CHECK_THROWS(framePayloadFromString(payload, got, mode)); // the payload is cut

        CHECK_THROWS(frameHeadFromString(std::string_view{"xy"}, got, mode));
        CHECK_THROWS(frameHeadFromString(std::string_view{message.substr(0, 6)}, got, mode));
    }
}
