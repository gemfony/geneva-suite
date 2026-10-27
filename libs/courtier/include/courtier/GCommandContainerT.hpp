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
#include <memory>
#include <string>
#include <variant>

// Boost headers go here

// Geneva headers go here
#include "common/GArchiveNamed.hpp"      // archive_named (named-member emitter)
#include "weft/GArchivePolymorphic.hpp"  // GArchive codecs for the GEM_BINARY / GEM_JSON wire arm
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GProcessable.hpp"
#include "courtier/GProcessingOutcome.hpp"
#include "courtier/GWireProtocolT.hpp"            // the library seam (tag + payload type)
#include "courtier/GWireSerializationContext.hpp" // GWireBlobId / GWirePeerId / the blob frame payloads

namespace Gem::Courtier {

/******************************************************************************************/
////////////////////////////////////////////////////////////////////////////////////////////
/******************************************************************************************/
/**
 * One message of the networked consumer/worker session protocol: a transport FRAME plus whatever that
 * frame carries.
 *
 * The class is deliberately two-layered, and the layers belong to different owners:
 *
 *  - **Courtier's layer** is the frame kind (GFrameKind), the announcing peer id and -- on a RETURN --
 *    the GProcessingOutcome. These are the things courtier itself acts on: dispatch, check-in, shutdown,
 *    blob fetch, reconciliation.
 *  - **The using library's layer** is an opaque numeric @c tag and, for frames that do not ship a work
 *    item, a library-defined payload (GWireProtocolT<processable_type>::payload_type). Courtier stores
 *    and relays both without ever interpreting them.
 *
 * The payload is a variant rather than the single work-item pointer of the earlier protocol. That single
 * shape was the root cause of a whole family of workarounds: a return that wanted to carry only computed
 * values had to be encoded as a hollowed-out work item. With one alternative per frame purpose --
 * nothing, a work item, a blob request, a blob reply, or the library's own payload type -- each frame
 * carries exactly what it means, and nothing inert rides along on the frames that mean something else.
 *
 * Courtier knows exactly ONE of the alternatives from the inside: the work item, which it must be able
 * to process() and route. Everything else it archives blind.
 *
 * @tparam processable_type The type of the processable work item carried as payload
 */
template <typename processable_type>
class GCommandContainerT {
    ///////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /**
     * @brief (De-)serializes the frame header, the payload variant and -- for a RETURN frame -- the
     * processing outcome.
     *
     * The payload rides Weft's native, index-tagged std::variant support, so the stream stays
     * self-describing and each frame writes only its own alternative. The outcome is written only where
     * it is meaningful; the frame kind is streamed first, so save and load agree on that branch without
     * any version field.
     *
     * @tparam Archive The GArchive codec type
     * @param ar The archive to read from / write to
     * @param version The class version (unused)
     */
    template <class Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        archive_named(ar, "kind_", kind_);
        archive_named(ar, "tag_", tag_);
        archive_named(ar, "peer_id_", peer_id_);
        archive_named(ar, "payload_", payload_);
        if(kind_ == GFrameKind::RETURN) {
            archive_named(ar, "outcome_", outcome_);
        }
    }
    ///////////////////////////////////////////////////////////////

    // The payload must be a processable (status / process() lifecycle).
    static_assert(
        std::is_base_of_v<Gem::Courtier::GProcessable, processable_type>,
        "processable_type must derive from Gem::Courtier::GProcessable"
    );

public:
    /** @brief The library-defined payload alternative, supplied through the GWireProtocolT seam. */
    using library_payload_type = wire_payload_t<processable_type>;

    /** @brief The payload of a frame: one alternative per frame purpose. */
    using payload_variant = std::variant<
        std::monostate,                    // PULL / NO_WORK / SHUTDOWN: nothing to carry
        std::unique_ptr<processable_type>, // WORK, and a RETURN that ships the whole item
        GBlobRequest,                      // BLOB_REQUEST
        GBlobReply,                        // BLOB_REPLY
        library_payload_type>;             // a RETURN (or any library frame) carrying library data

    //-------------------------------------------------------------------------
    /**
	  * @brief Initialization with a frame kind only, in cases where nothing needs to be carried.
	  *
	  * @param kind The frame kind
	  */
    explicit GCommandContainerT(GFrameKind kind)
      : kind_(kind) { /* nothing */
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Initialization with a frame kind and a work item.
	  *
	  * @param kind The frame kind
	  * @param payload_ptr The work item transported by this frame (sole ownership is taken by move)
	  */
    GCommandContainerT(GFrameKind kind, std::unique_ptr<processable_type> payload_ptr)
      : kind_(kind)
      , payload_(std::move(payload_ptr)) { /* nothing */
    }

    //-------------------------------------------------------------------------
    // Defaulted constructors, destructor and move assigment operator

    GCommandContainerT() = default;
    GCommandContainerT(GCommandContainerT &&cp) noexcept = default;
    ~GCommandContainerT() = default;

    GCommandContainerT &operator=(GCommandContainerT &&cp) noexcept = default;

    //-------------------------------------------------------------------------
    // Deleted copy-constructors and assignment operator -- the class is non-copyable

    GCommandContainerT(const GCommandContainerT &) = delete;
    GCommandContainerT &operator=(const GCommandContainerT &) = delete;

    //-------------------------------------------------------------------------
    /**
	  * @brief Resets the frame to a new kind, dropping every payload, tag, peer id and outcome, so a
	  * reused container never carries stale data into the next message.
	  *
	  * @param kind The new frame kind (defaults to NONE, i.e. cleared)
	  * @return A reference to this object, so we can serialize it in one go
	  */
    const GCommandContainerT &reset(GFrameKind kind = GFrameKind::NONE) {
        kind_ = kind;
        tag_ = 0;
        peer_id_ = 0;
        payload_ = payload_variant{};
        outcome_ = GProcessingOutcome{};
        return *this;
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Resets the frame to a new kind carrying a work item (the WORK / full-RETURN case).
	  *
	  * @param kind The new frame kind
	  * @param payload_ptr The work item to carry (sole ownership is taken by move)
	  * @return A reference to this object, so we can serialize it in one go
	  */
    const GCommandContainerT &reset(GFrameKind kind, std::unique_ptr<processable_type> payload_ptr) {
        reset(kind);
        payload_ = std::move(payload_ptr);
        return *this;
    }

    //-------------------------------------------------------------------------
    // The frame header -- courtier's own

    /** @brief Sets the frame kind. @param kind The new frame kind */
    void setKind(GFrameKind kind) noexcept { kind_ = kind; }
    /** @brief @return The kind of this frame */
    [[nodiscard]] GFrameKind kind() const noexcept { return kind_; }

    /** @brief Sets the announcing peer's stable id (used by ASIO, whose one-shot connections have no
     *  persistent per-connection identity, to tell the server which peer a request belongs to).
     *  @param peer The stable peer id of the announcing client. */
    void setPeerId(GWirePeerId peer) noexcept { peer_id_ = peer; }
    /** @brief @return The stable peer id announced on this frame (0 if none). */
    [[nodiscard]] GWirePeerId peerId() const noexcept { return peer_id_; }

    /** @brief Sets the processing outcome reported by a RETURN frame.
     *  @param outcome The worker's lifecycle report for the returned slot */
    void setOutcome(GProcessingOutcome outcome) { outcome_ = std::move(outcome); }
    /** @brief @return The processing outcome carried by a RETURN frame (default-constructed otherwise) */
    [[nodiscard]] const GProcessingOutcome &outcome() const noexcept { return outcome_; }

    //-------------------------------------------------------------------------
    // The library layer -- courtier stores and relays, never interprets

    /** @brief Sets the library-defined tag of this frame. @param tag The library's own message tag */
    void setTag(std::uint32_t tag) noexcept { tag_ = tag; }
    /** @brief @return The library-defined tag of this frame (0 on courtier's own control frames) */
    [[nodiscard]] std::uint32_t tag() const noexcept { return tag_; }

    /** @brief Stores a library-defined payload in this frame. @param payload The library's payload */
    void setLibraryPayload(library_payload_type payload) { payload_ = std::move(payload); }
    /** @brief @return A pointer to the carried library payload, or nullptr if the frame carries
     *  something else */
    [[nodiscard]] const library_payload_type *libraryPayload() const noexcept {
        return std::get_if<library_payload_type>(&payload_);
    }

    //-------------------------------------------------------------------------
    // The work-item alternative -- the one payload courtier knows from the inside

    /** @brief Stores a work item in this frame. @param payload_ptr The work item (ownership taken by move) */
    void setItem(std::unique_ptr<processable_type> payload_ptr) { payload_ = std::move(payload_ptr); }

    /**
	  * @brief Retrieves the carried work item by const reference (a non-destructive borrow). Use
	  * releaseItem() to take ownership of it.
	  *
	  * @return A pointer to the carried work item, or nullptr if this frame carries none
	  */
    [[nodiscard]] const processable_type *item() const noexcept {
        const auto *held = std::get_if<std::unique_ptr<processable_type>>(&payload_);
        return held != nullptr ? held->get() : nullptr;
    }

    /**
	  * @brief Extracts (moves out) the carried work item, transferring sole ownership to the caller. The
	  * frame's payload is empty afterwards. Used by the transports to hand a received item on.
	  *
	  * @return The work item; the frame retains no ownership afterwards (empty if it carried none)
	  */
    std::unique_ptr<processable_type> releaseItem() {
        auto *held = std::get_if<std::unique_ptr<processable_type>>(&payload_);
        if(held == nullptr) {
            return {};
        }
        auto out = std::move(*held);
        payload_ = payload_variant{};
        return out;
    }

    //-------------------------------------------------------------------------
    // The blob-fetch alternatives -- courtier's own cache-miss round trip

    /** @brief Turns this frame into a blob request. @param id The content id of the wanted blob */
    void setBlobRequest(const GWireBlobId &id) { payload_ = GBlobRequest{id}; }
    /** @brief @return The carried blob request, or nullptr if this frame carries something else */
    [[nodiscard]] const GBlobRequest *blobRequest() const noexcept {
        return std::get_if<GBlobRequest>(&payload_);
    }

    /** @brief Turns this frame into a blob reply.
     *  @param id The content id being answered
     *  @param blob The serialized blob (moved in; empty on a registry miss) */
    void setBlobReply(const GWireBlobId &id, std::string blob) {
        payload_ = GBlobReply{id, std::move(blob)};
    }
    /** @brief @return The carried blob reply, or nullptr if this frame carries something else */
    [[nodiscard]] const GBlobReply *blobReply() const noexcept {
        return std::get_if<GBlobReply>(&payload_);
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Processing of the carried work item. Delegates to the item's process() method.
	  *
	  * Errors during processing are handled one layer down: GProcessingContainerT::process()
	  * wraps the user's process_() in a try/catch, records the failure on the item
	  * (processing_status_ = EXCEPTION_CAUGHT plus stored error descriptions) and rethrows a
	  * typed g_processing_exception. This method intentionally lets that exception propagate to
	  * the caller (the worker compute loop), which catches it and returns the item carrying its
	  * error state -- so a faulty work item never crashes the worker.
	  *
	  * @throws geneva_exception if the frame holds no work item
	  * @throws g_processing_exception (from the item) if processing flagged or threw an error
	  */
    void process() {
        auto *held = std::get_if<std::unique_ptr<processable_type>>(&payload_);
        if(held != nullptr && *held) {
            (*held)->process();
        }
        else {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GCommandContainerT<processable_type>::process():" << '\n'
                << "Tried to process a work item while the frame carries none" << '\n'
            );
        }
    }

private:
    //-------------------------------------------------------------------------
    // Data

    GFrameKind kind_{GFrameKind::NONE}; ///< What this frame does to the transport (courtier's own)
    std::uint32_t tag_{0};              ///< What this frame means to the using library (relayed only)
    GWirePeerId peer_id_{0};            ///< Stable announcing-peer id (ASIO); 0 == none
    payload_variant payload_;           ///< What the frame carries (one alternative per purpose)
    GProcessingOutcome outcome_;        ///< The worker's lifecycle report; meaningful on RETURN only

    //-------------------------------------------------------------------------
};

/******************************************************************************/
/**
 * @brief Worker side: turns a frame whose work item has just been processed into the RETURN frame the
 * using library wants sent back.
 *
 * Courtier contributes the parts it owns -- the RETURN frame kind and the GProcessingOutcome snapshot
 * that carries status, errors, timings and the routing correlation id. The library contributes the rest
 * through GWireProtocolT::buildReturn(): the tag, and either "ship the whole item" or a payload of its
 * own. This is the single place where the return decision is made, shared by every transport.
 *
 * @tparam processable_type The payload type of the command container
 * @param container The frame holding the just-processed work item; rewritten in place into a RETURN
 */
template <typename processable_type>
void makeReturnFrame(GCommandContainerT<processable_type> &container) {
    auto item_ptr = container.releaseItem();
    GProcessingOutcome outcome;
    std::uint32_t tag = 0;
    typename GCommandContainerT<processable_type>::library_payload_type payload;
    bool ships_item = true;

    if(item_ptr) {
        outcome = item_ptr->processingOutcome();
        ships_item = GWireProtocolT<processable_type>::buildReturn(*item_ptr, tag, payload);
    }

    container.reset(GFrameKind::RETURN);
    container.setTag(tag);
    container.setOutcome(std::move(outcome));
    if(ships_item) {
        container.setItem(std::move(item_ptr));
    }
    else {
        container.setLibraryPayload(std::move(payload));
    }
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Conversion of a GCommandContainerT to a serialized string.
 *
 * @tparam processable_type The payload type of the command container
 * @param container The command container to be serialized
 * @param serMode The serialization format to use (GArchive binary or JSON)
 * @return The serialized representation of the container (empty string only on the unreachable fall-through)
 * @throws geneva_exception if serialization fails
 */
template <typename processable_type>
std::string container_to_string(
    const GCommandContainerT<processable_type> &container,
    Gem::Common::serializationMode serMode
) {
    try {
        switch(serMode) {
            using enum Gem::Common::serializationMode;
        case GEM_BINARY: {
            // GArchive flat-binary codec. The blob send-once interning is orthogonal: it lives
            // inside the work item's own save() (an ambient GWireSerializationScope, if any, is honoured
            // there, and the interned blob travels as an opaque blob), so the outer codec choice is free.
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("command_container", container);
            return oa.str();
        }

        case GEM_JSON: {
            Gem::Weft::GJsonOArchive oa;
            oa &Gem::Weft::make_nvp("command_container", container);
            return oa.str();
        }
        }
    }
    catch(const std::exception &e) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In container_to_string(GCommandContainerT<>):" << '\n'
            << "Caught std::exception exception with messages:" << '\n'
            << e.what() << '\n'
            << "with serializationMode == " << Gem::Common::serModeToString(serMode) << '\n'
        );
    }
    catch(...) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In container_to_string(GCommandContainerT<>):" << '\n'
            << "Caught unknown exception" << '\n'
            << "with serializationMode == " << Gem::Common::serModeToString(serMode) << '\n'
        );
    }

    // Make the compiler happy
    return {};
}

/******************************************************************************/
/**
 * @brief Loading of a GCommandContainerT from a serialized string. The container is reset before
 * loading.
 *
 * @tparam processable_type The payload type of the command container
 * @param descr The serialized representation to load from
 * @param container The command container to be filled (output parameter; reset before loading)
 * @param serMode The serialization format the string was produced with (GArchive binary or JSON)
 * @throws geneva_exception if de-serialization fails
 */
template <typename processable_type>
void container_from_string(
    const std::string &descr,
    GCommandContainerT<processable_type> &container,
    Gem::Common::serializationMode serMode
) {
    container.reset();

    try {
        switch(serMode) {
            using enum Gem::Common::serializationMode;
        case GEM_BINARY: {
            // descr is a live named parameter, so GBinaryIArchive's string_view over it stays valid.
            Gem::Weft::GBinaryIArchive ia(descr);
            ia &Gem::Weft::make_nvp("command_container", container);
        } break;

        case GEM_JSON: {
            Gem::Weft::GJsonIArchive ia(descr);
            ia &Gem::Weft::make_nvp("command_container", container);
        } break;
        }
    }
    catch(const std::exception &e) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In container_from_string(GCommandContainerT<>):" << '\n'
            << "Caught std::exception exception with messages:" << '\n'
            << e.what() << '\n'
            << "with serializationMode == " << Gem::Common::serModeToString(serMode) << '\n'
        );
    }
    catch(...) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In container_from_string(GCommandContainerT<>):" << '\n'
            << "Caught unknown exception" << '\n'
            << "with serializationMode == " << Gem::Common::serModeToString(serMode) << '\n'
        );
    }
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Courtier */

/******************************************************************************/
