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

#include <memory>
#include <string>

#include "common/GCommonEnums.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GDemoProcessingContainers.hpp"

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
    frame.setPeerId(0x1234'5678'9ABC'DEF0ULL);
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
 * kind, the library tag, the peer id, the work item and the item's stable lineage id intact.
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
        CHECK(restored.peerId() == 0x1234'5678'9ABC'DEF0ULL);
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

        // --- payload-free control frames -------------------------------------------------------
        for(const auto kind : {GFrameKind::PULL, GFrameKind::NO_WORK, GFrameKind::SHUTDOWN}) {
            test_container_type sent{kind};
            sent.setPeerId(99);
            test_container_type got{GFrameKind::NONE};
            REQUIRE_NOTHROW(container_from_string(container_to_string(sent, mode), got, mode));
            CHECK(got.kind() == kind);
            CHECK(got.peerId() == 99);
            CHECK(got.item() == nullptr);
            CHECK(got.blobRequest() == nullptr);
            CHECK(got.blobReply() == nullptr);
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

        // --- a RETURN frame: the processing outcome travels with it ----------------------------
        {
            test_container_type sent{GFrameKind::RETURN};
            GProcessingOutcome outcome;
            outcome.status = processingStatus::ERROR_FLAGGED;
            outcome.error_descriptions = "the worker complained";
            outcome.processing_time = 1.5;
            outcome.correlation_id = 0xABCDEF;
            sent.setOutcome(outcome);
            sent.setItem(std::make_unique<GSimpleContainer>(3));

            test_container_type got{GFrameKind::NONE};
            REQUIRE_NOTHROW(container_from_string(container_to_string(sent, mode), got, mode));
            CHECK(got.kind() == GFrameKind::RETURN);
            CHECK(got.outcome().status == processingStatus::ERROR_FLAGGED);
            CHECK(got.outcome().error_descriptions == "the worker complained");
            CHECK(got.outcome().processing_time == 1.5);
            CHECK(got.outcome().correlation_id == 0xABCDEF);
            CHECK(got.item() != nullptr);
        }
    }
}

/******************************************************************************/
