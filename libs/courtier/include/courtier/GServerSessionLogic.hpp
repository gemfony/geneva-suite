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

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard headers go here
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>

// Geneva headers go here
#include "common/GCommonEnums.hpp" // Gem::Common::serializationMode
#include "common/GErrorStreamer.hpp"
#include "common/GLogger.hpp"
#include "courtier/GCommandContainerT.hpp"        // GCommandContainerT
#include "courtier/GCourtierEnums.hpp"            // GFrameKind
#include "courtier/GCourtierHelperFunctions.hpp"  // fkToStr
#include "courtier/GWireCodec.hpp"                // wireEncode / buildBlobReply
#include "courtier/GWireProtocolT.hpp"            // the library seam (the WORK tag)
#include "courtier/GWireSerializationContext.hpp" // GWireSerializationContext / registry

namespace Gem::Courtier {

/******************************************************************************/
/**
 * Transport-agnostic server-side session logic for the networked consumer protocol. A server session,
 * on receiving a request from a worker, decides what to do with it: serve a work item (PULL), sink a
 * returned result and serve the next (RETURN), sink one and owe nothing (RESULT), or answer a blob
 * cache-miss fetch (BLOB_REQUEST). The transports all make the same decisions; only how they
 * pull/sink items (a callable wrapping the consumer's checkout/checkin) and how they drive their I/O
 * differs. The decision pieces live here once.
 *
 *  - serveWorkItem(): the pure frame decision -- check a work item out and set the frame to
 *    WORK (with the item) or NO_WORK (queue empty). No serialization, no I/O. Used by every transport.
 *
 *  - absorbServerRequest(): everything a request asks for except the work item: a returned result
 *    is checked in, a blob request or an unknown frame is answered. It tells the caller what it
 *    owes now: that answer, a work item, or nothing (GAbsorbResult). Used by a transport that may
 *    let the pull wait (GPeerSessionT, the session of the websocket consumer, which parks instead
 *    of answering NO_WORK).
 *
 * The item source / sink are passed as callables so this header stays free of any consumer type:
 *   getItem : () -> std::unique_ptr<processable_type>          (checkout; null when the queue is empty)
 *   putItem : (GCommandContainerT<processable_type>&&) -> void (checkin a returned frame)
 */

/******************************************************************************/
/**
 * @brief How a parked caller is told that work may be available (see
 * GNetworkedConsumerT::checkoutOrPark()). It is called once, from whichever thread made the work
 * available, so it must not block, must not throw and must not call into the consumer: it hands
 * the news to the caller's own executor. It refers to its caller weakly, so that the consumer's
 * list of parked callers never keeps one alive.
 *
 * It returns whether it reached its caller. The consumer wakes as many callers as items became
 * available, and one that is gone must not use up a wake-up that another caller needs.
 */
using GWakeFn = std::move_only_function<bool()>;

/**
 * @brief The outcome of GNetworkedConsumerT::checkoutOrPark(): an item, or the promise of one
 * wake-up, or neither (the server is stopping).
 *
 * @tparam processable_type The work-item type
 */
template <typename processable_type>
struct GCheckoutResult {
    std::unique_ptr<processable_type> item; ///< Set: an item was checked out
    bool parked = false; ///< True: nothing was pending; the caller will be woken once
    /// With an item: which checkout of its slot this is. A put-back of the item names it, so that it
    /// acts only while this is still the slot's current checkout (GNetworkedConsumerT::requeue()).
    std::uint64_t checkout = 0;
};

/******************************************************************************/
/** @brief absorbServerRequest(): the sender is owed a work item (it sent a PULL or a RETURN). */
struct GPullDue {};

/** @brief absorbServerRequest(): the sender is owed nothing (it sent a RESULT, which has been
 *  checked in and asks for nothing). */
struct GNothingDue {};

/**
 * @brief What absorbServerRequest() leaves its caller owing the sender: the serialized answer to
 * send, a work item (to be served now or when one is there), or nothing.
 */
using GAbsorbResult = std::variant<std::string, GPullDue, GNothingDue>;

/******************************************************************************/
/**
 * @brief Checks a work item out and stores it in the frame as WORK, or stores NO_WORK when the
 * queue is empty. Pure frame mutation -- no serialization, no I/O.
 *
 * @tparam processable_type The payload type of the command container
 * @tparam get_item_fn_type A callable () -> std::unique_ptr<processable_type> (the checkout)
 * @param container The frame to fill (reset to WORK+item or NO_WORK)
 * @param getItem The checkout callable
 */
template <typename processable_type, typename get_item_fn_type>
void serveWorkItem(GCommandContainerT<processable_type> &container, get_item_fn_type &&getItem) {
    auto payload_ptr = getItem();
    if(payload_ptr) {
        container.reset(GFrameKind::WORK, std::move(payload_ptr));
        container.setTag(GWireProtocolT<processable_type>::workTag());
    }
    else {
        container.reset(GFrameKind::NO_WORK);
    }
}

/******************************************************************************/
/**
 * @brief The serialized answer to a request the server cannot act on -- a frame it could not
 * decode, could not handle, or does not know: NO_WORK.
 *
 * A worker treats NO_WORK as "ask again later", so a refused request costs it one pause. An empty
 * or missing reply would instead look like a broken server and end the worker.
 *
 * @tparam processable_type The payload type of the command container
 * @param ser_mode The serialization format to use
 * @return The serialized NO_WORK frame
 */
template <typename processable_type>
std::string refusalReply(Gem::Common::serializationMode ser_mode) {
    const GCommandContainerT<processable_type> refusal{GFrameKind::NO_WORK};
    return wireEncode(refusal, nullptr, ser_mode);
}

/******************************************************************************/
/**
 * @brief Acts on everything an already-deserialized inbound frame asks for, except the work item
 * its sender may be due.
 *
 * A RETURN hands the whole inbound frame to the sink (the consumer applies its outcome and its
 * library payload to the slot); like a PULL it leaves a work item owed. A RESULT is checked in the
 * same way and leaves nothing owed: its sender has a request with the server already. A
 * BLOB_REQUEST is answered from the shared registry (under a null scope, via the codec). An unknown
 * frame logs a warning and is answered with NO_WORK (see refusalReply()).
 *
 * @tparam processable_type The payload type of the command container
 * @tparam put_item_fn_type A callable (GCommandContainerT<processable_type>&&) -> void (the checkin)
 * @param container The already-deserialized inbound frame (cleared after a RETURN or a RESULT)
 * @param put_item The checkin callable
 * @param registry The shared blob registry (for BLOB_REQUEST; may be nullptr)
 * @param ser_mode The serialization format to use
 * @return The answer if the request is answered by it (a string); GPullDue if a work item is now
 *         owed; GNothingDue if nothing is
 */
template <typename processable_type, typename put_item_fn_type>
GAbsorbResult absorbServerRequest(
    GCommandContainerT<processable_type> &container,
    put_item_fn_type &&put_item,
    GWireBlobRegistry *registry,
    Gem::Common::serializationMode ser_mode
) {
    switch(container.kind()) {
        using enum GFrameKind;
    case PULL: {
        return GPullDue{};
    }
    case RETURN:
    case RESULT: {
        // Hand the whole frame to the sink -- outcome, library tag and payload together. The (now
        // cleared) frame is free for the answer to the pull a RETURN carries.
        const bool pull_due = container.kind() == RETURN;
        GCommandContainerT<processable_type> inbound{std::move(container)};
        container.reset();
        std::forward<put_item_fn_type>(put_item)(std::move(inbound));
        if(pull_due) {
            return GPullDue{};
        }
        return GNothingDue{};
    }
    case BLOB_REQUEST: {
        // Blob cache-miss fetch: answer with the serialized blob from the shared registry (empty
        // blob -> the worker treats the fetch as failed).
        const auto *request = container.blobRequest();
        return buildBlobReply<processable_type>(
            request != nullptr ? request->id : GWireBlobId{0, 0},
            registry,
            ser_mode
        );
    }
    default: {
        glogger << "In Gem::Courtier::absorbServerRequest():" << '\n'
                << "Got unknown or invalid frame " << fkToStr(container.kind()) << '\n'
                << "The request is refused and answered with NO_WORK." << '\n'
                << GWARNING;
        return refusalReply<processable_type>(ser_mode);
    }
    }
}

/******************************************************************************/

} /* namespace Gem::Courtier */
