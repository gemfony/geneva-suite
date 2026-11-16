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
#include <utility>
#include <variant>
#include <vector>

#include "common/GCommonEnums.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GServerSessionLogic.hpp"
#include "courtier/GWireCodec.hpp"

using namespace Gem::Courtier;

namespace {

using frame_type = GCommandContainerT<GSimpleContainer>;

constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

/** @brief A frame of @p kind that carries a processed item, as a RETURN or a RESULT does */
frame_type returnedFrame(GFrameKind kind) {
    frame_type frame{kind, std::make_unique<GSimpleContainer>(5)};
    GProcessingOutcome outcome;
    outcome.status = processingStatus::PROCESSED;
    outcome.correlation_id = 17;
    frame.setOutcome(outcome);
    return frame;
}

/** @brief What the server's sink was handed: the kind and the routing token of every frame */
struct CheckedIn {
    std::vector<GFrameKind> kinds;
    std::vector<CORRELATION_ID_TYPE> correlationIds;

    /** @brief The sink handed to absorbServerRequest()
     *  @return A callable that records each frame it is given */
    auto sink() {
        return [this](frame_type &&frame) {
            const frame_type taken{std::move(frame)};
            kinds.push_back(taken.kind());
            correlationIds.push_back(taken.outcome().correlation_id);
        };
    }
};

} /* anonymous namespace */

/******************************************************************************/
/**
 * absorbServerRequest() says what the sender is owed: a PULL is owed a work item; a RETURN is
 * checked in and owed a work item; a RESULT is checked in and owed nothing. A frame it does not act
 * on is answered at once.
 */
TEST_CASE(
    "absorbServerRequest: PULL and RETURN are owed a work item but RESULT nothing",
    "[courtier][session-logic]"
) {
    CheckedIn checked_in;

    SECTION("PULL") {
        frame_type frame{GFrameKind::PULL};
        const auto absorbed = absorbServerRequest(frame, checked_in.sink(), nullptr, BIN);
        CHECK(std::holds_alternative<GPullDue>(absorbed));
        CHECK(checked_in.kinds.empty());
    }

    SECTION("RETURN") {
        auto frame = returnedFrame(GFrameKind::RETURN);
        const auto absorbed = absorbServerRequest(frame, checked_in.sink(), nullptr, BIN);
        CHECK(std::holds_alternative<GPullDue>(absorbed));
        REQUIRE(checked_in.kinds.size() == 1);
        CHECK(checked_in.kinds.front() == GFrameKind::RETURN);
        CHECK(checked_in.correlationIds.front() == 17);
        CHECK(frame.kind() == GFrameKind::NONE); // cleared, free for the answer
    }

    SECTION("RESULT") {
        auto frame = returnedFrame(GFrameKind::RESULT);
        const auto absorbed = absorbServerRequest(frame, checked_in.sink(), nullptr, BIN);
        CHECK(std::holds_alternative<GNothingDue>(absorbed));
        REQUIRE(checked_in.kinds.size() == 1);
        CHECK(checked_in.kinds.front() == GFrameKind::RESULT);
        CHECK(checked_in.correlationIds.front() == 17);
        CHECK(frame.kind() == GFrameKind::NONE);
    }

    SECTION("a frame a client never sends is refused with NO_WORK") {
        frame_type frame{GFrameKind::WORK, std::make_unique<GSimpleContainer>(1)};
        const auto absorbed = absorbServerRequest(frame, checked_in.sink(), nullptr, BIN);
        const auto *answer = std::get_if<std::string>(&absorbed);
        REQUIRE(answer != nullptr);
        frame_type reply{GFrameKind::NONE};
        wireDecode(*answer, reply, nullptr, BIN);
        CHECK(reply.kind() == GFrameKind::NO_WORK);
        CHECK(checked_in.kinds.empty());
    }
}
