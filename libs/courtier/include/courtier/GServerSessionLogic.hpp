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
#include <memory>
#include <string>
#include <utility>

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
 * returned result and serve the next (RETURN), or answer a blob cache-miss fetch (BLOB_REQUEST). The
 * three transports all make the same decisions; only how they pull/sink items (a callable wrapping the
 * consumer's checkout/checkin) and how they drive their I/O differs. The decision pieces live here once.
 *
 * Two helpers, matching the two server styles:
 *
 *  - serveWorkItem(): the pure frame decision -- check a work item out and set the frame to
 *    WORK (with the item) or NO_WORK (queue empty). No serialization, no I/O. Used by every transport
 *    (the synchronous ones via handleServerRequest() below; MPI directly, as its decide and send phases
 *    are split across its asynchronous receive/send).
 *
 *  - handleServerRequest(): the full synchronous request -> response-bytes dispatch used by the
 *    request/response transports (Asio, websocket). It takes an already-deserialized inbound container
 *    (the transport owns the receive + its wire scope) and returns the serialized response. MPI does not
 *    use it (its server is structured as a split deserialize-phase + later send-phase, not a synchronous
 *    call) -- it composes serveWorkItem() + the GWireCodec helpers itself.
 *
 * The item source / sink are passed as callables so this header stays free of any consumer type:
 *   getItem : () -> std::unique_ptr<processable_type>          (checkout; null when the queue is empty)
 *   putItem : (GCommandContainerT<processable_type>&&) -> void (checkin a returned frame)
 */

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
 * @brief Synchronous server dispatch: acts on an already-deserialized inbound frame and returns the
 * serialized response. Used by the request/response transports (Asio, websocket).
 *
 * A PULL serves a work item; a RETURN hands the whole inbound frame to the sink (the consumer applies
 * its outcome and its library payload to the slot) and then serves the next item; both responses are
 * serialized under @p respCtx so a WORK item's blob is sent send-once. A BLOB_REQUEST is answered from
 * the shared registry (under a null scope, via the codec). An unknown frame logs a warning and yields
 * no response.
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
 * @return The serialized response, or an empty string on an unknown/invalid frame
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
    switch(container.kind()) {
        using enum GFrameKind;
    case PULL: {
        serveWorkItem(container, std::forward<GetItemF>(getItem));
        return wireEncode(container, respCtx, serMode);
    }
    case RETURN: {
        // Hand the whole return frame to the sink -- outcome, library tag and payload together -- then
        // serve the next item from the (now cleared) frame.
        GCommandContainerT<processable_type> inbound{std::move(container)};
        container.reset();
        putItem(std::move(inbound));
        serveWorkItem(container, std::forward<GetItemF>(getItem));
        return wireEncode(container, respCtx, serMode);
    }
    case BLOB_REQUEST: {
        // Blob cache-miss fetch: answer with the serialized blob from the shared registry (empty
        // blob -> the worker treats the fetch as failed).
        const auto *request = container.blobRequest();
        return buildBlobReply<processable_type>(
            request != nullptr ? request->id : GWireBlobId{0, 0},
            registry,
            serMode
        );
    }
    default: {
        glogger << "In Gem::Courtier::handleServerRequest():" << '\n'
                << "Got unknown or invalid frame " << fkToStr(container.kind()) << '\n'
                << GWARNING;
        return {};
    }
    }
}

/******************************************************************************/

} /* namespace Gem::Courtier */
