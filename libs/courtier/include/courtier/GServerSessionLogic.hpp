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
 * cache-miss fetch (BLOB_REQUEST). The three transports all make the same decisions; only how
 * they pull/sink items (a callable wrapping the consumer's checkout/checkin) and how they drive
 * their I/O differs. The decision pieces live here once.
 *
 * Three helpers, matching the server styles:
 *
 *  - serveWorkItem(): the pure frame decision -- check a work item out and set the frame to
 *    WORK (with the item) or NO_WORK (queue empty). No serialization, no I/O. Used by every transport
 *    (the synchronous ones via handleServerRequest() below; MPI directly, as its decide and send phases
 *    are split across its asynchronous receive/send).
 *
 *  - absorbServerRequest(): everything a request asks for except the work item: a returned result
 *    is checked in, a blob request or an unknown frame is answered. It tells the caller what it
 *    owes now: that answer, a work item, or nothing (GAbsorbResult). Used directly by a transport
 *    that may let the pull wait (the websocket session, which parks instead of answering
 *    NO_WORK).
 *
 *  - handleServerRequest(): the full synchronous request -> response-bytes dispatch used by the
 *    request/response transport (Asio): absorbServerRequest(), then serveWorkItem(). It takes an
 *    already-deserialized inbound container (the transport owns the receive + its wire scope) and
 *    returns the serialized response. MPI does not use it (its server is structured as a split
 *    deserialize-phase + later send-phase, not a synchronous call) -- it composes serveWorkItem()
 *    + the GWireCodec helpers itself.
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
 * @tparam ProcessableT The work-item type
 */
template <typename ProcessableT>
struct GCheckoutResult {
    std::unique_ptr<ProcessableT> item; ///< Set: an item was checked out
    bool parked = false; ///< True: nothing was pending; the caller will be woken once
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
 * @tparam GetItemF A callable () -> std::unique_ptr<processable_type> (the checkout)
 * @param container The frame to fill (reset to WORK+item or NO_WORK)
 * @param getItem The checkout callable
 */
template <typename processable_type, typename GetItemF>
void serveWorkItem(
    GCommandContainerT<processable_type> &container,
    GetItemF &&getItem
) {
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
 * @tparam ProcessableT The payload type of the command container
 * @tparam PutItemF A callable (GCommandContainerT<ProcessableT>&&) -> void (the checkin)
 * @param container The already-deserialized inbound frame (cleared after a RETURN or a RESULT)
 * @param put_item The checkin callable
 * @param registry The shared blob registry (for BLOB_REQUEST; may be nullptr)
 * @param ser_mode The serialization format to use
 * @return The answer if the request is answered by it (a string); GPullDue if a work item is now
 *         owed; GNothingDue if nothing is
 */
template <typename ProcessableT, typename PutItemF>
GAbsorbResult absorbServerRequest(
    GCommandContainerT<ProcessableT> &container,
    PutItemF &&put_item,
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
        GCommandContainerT<ProcessableT> inbound{std::move(container)};
        container.reset();
        std::forward<PutItemF>(put_item)(std::move(inbound));
        if(pull_due) {
            return GPullDue{};
        }
        return GNothingDue{};
    }
    case BLOB_REQUEST: {
        // Blob cache-miss fetch: answer with the serialized blob from the shared registry (empty
        // blob -> the worker treats the fetch as failed).
        const auto *request = container.blobRequest();
        return buildBlobReply<ProcessableT>(
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
        return refusalReply<ProcessableT>(ser_mode);
    }
    }
}

/******************************************************************************/
/**
 * @brief Synchronous server dispatch: acts on an already-deserialized inbound frame and returns the
 * serialized response. Used by the request/response transport (Asio).
 *
 * absorbServerRequest() deals with the frame; if a pull is due afterwards (a PULL or a RETURN), a
 * work item is served, serialized under @p respCtx so its blob is sent send-once. A RESULT asks for
 * nothing, but a request/response exchange must be answered: it is checked in and answered with
 * NO_WORK, as a refused request is (the asio client never sends one).
 *
 * @tparam processable_type The payload type of the command container
 * @tparam GetItemF A callable () -> std::unique_ptr<processable_type> (the checkout)
 * @tparam PutItemF A callable (GCommandContainerT<processable_type>&&) -> void (the checkin)
 * @param container The already-deserialized inbound frame (reused to build the response)
 * @param getItem The checkout callable
 * @param putItem The checkin callable
 * @param registry The shared blob registry (for BLOB_REQUEST; may be nullptr)
 * @param respCtx The wire context to serialize a WORK/NO_WORK response under (may be nullptr)
 * @param serMode The serialization format to use
 * @return The serialized response (a NO_WORK frame for an unknown/invalid frame)
 */
template <typename processable_type, typename GetItemF, typename PutItemF>
std::string handleServerRequest(
    GCommandContainerT<processable_type> &container,
    GetItemF &&getItem,
    PutItemF &&putItem,
    GWireBlobRegistry *registry,
    const GWireSerializationContext *respCtx,
    Gem::Common::serializationMode serMode
) {
    auto absorbed =
        absorbServerRequest(container, std::forward<PutItemF>(putItem), registry, serMode);
    if(auto *answer = std::get_if<std::string>(&absorbed)) {
        return std::move(*answer);
    }
    if(std::holds_alternative<GNothingDue>(absorbed)) {
        // Nothing is owed, but this exchange needs an answer: the one a refused request gets.
        return refusalReply<processable_type>(serMode);
    }
    serveWorkItem(container, std::forward<GetItemF>(getItem));
    return wireEncode(container, respCtx, serMode);
}

/******************************************************************************/

} /* namespace Gem::Courtier */
