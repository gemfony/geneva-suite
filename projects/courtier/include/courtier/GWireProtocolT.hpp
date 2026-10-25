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

// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named
#include "courtier/GProcessable.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The payload type of a library that defines no payload shape of its own -- i.e. one whose work items
 * always travel back whole. An empty, archivable placeholder so the frame's payload variant has a
 * well-formed (and distinct) alternative in that case.
 */
struct GNoLibraryPayload {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /** @brief (De)serialises nothing -- the type carries no state.
     *  @tparam Archive The GArchive codec type
     *  @param ar The (unused) archive
     *  @param version The (unused) serialization version number */
    template <typename Archive>
    void serialize([[maybe_unused]] Archive &ar, [[maybe_unused]] const unsigned int version) { /* nothing */ }
    ///////////////////////////////////////////////////////////////////////
};

/******************************************************************************/
/**
 * The seam through which a USING LIBRARY states what its messages MEAN, without courtier learning any
 * of it.
 *
 * Courtier owns the frame vocabulary (GFrameKind) and the lifecycle (GProcessingOutcome) -- the things it
 * acts on. Everything else about a message is the library's: an opaque numeric tag and, where the frame
 * does not ship the work item itself, a library-defined payload type. This template is where a library
 * supplies those three answers; courtier only calls them.
 *
 * The seam is keyed on the WORK-ITEM TYPE, because that is the type whose semantics are at stake, and
 * because courtier's transports, sessions and consumers are all templated on it and nothing else. A
 * library specializes this template right next to its work-item class, so no instantiation of the
 * transports can pick up the primary template by accident.
 *
 * The primary template is the degenerate protocol every geneva-free user of courtier (its own demo
 * containers and networked test suites included) gets for free: one tag, no library payload, whole work
 * items in both directions.
 *
 * @tparam processable_type The work-item type exchanged over the transport
 */
//! [courtier-wire-seam]
template <typename processable_type>
struct GWireProtocolT {
    /** @brief The library-defined payload alternative of a frame (see GCommandContainerT). */
    using payload_type = GNoLibraryPayload;

    /** @brief @return The library tag a server puts on a WORK frame ("what should be done with this
     *  item?"). Courtier only relays it. */
    static std::uint32_t workTag() { return 0; }

    /** @brief @return The library tag a server puts on a SHUTDOWN frame. Courtier only relays it. */
    static std::uint32_t shutdownTag() { return 0; }

    /**
     * @brief Worker side: decides how a just-processed work item is returned to the server.
     *
     * @param item The processed work item (never null)
     * @param tag Receives the library tag to put on the RETURN frame
     * @param payload Receives the library payload, iff the return does not ship the item
     * @return true if the work item itself must travel in the frame, false if @p payload replaces it
     */
    static bool buildReturn(
        [[maybe_unused]] const processable_type &item,
        std::uint32_t &tag,
        [[maybe_unused]] payload_type &payload
    ) {
        tag = 0;
        return true; // the default protocol returns the whole item
    }

    /**
     * @brief Server side: writes a return onto the live slot it belongs to.
     *
     * The frame's GProcessingOutcome has already been applied by the consumer; this call adds whatever
     * the library considers the RESULT of an evaluation. The slot is reconciled IN PLACE (its heap
     * address is load-bearing), so an implementation copies into @p slot rather than replacing it.
     *
     * @param slot The live work item the return belongs to (reconciled in place)
     * @param tag The library tag the worker put on the frame
     * @param payload The library payload, or nullptr when the frame carried the work item instead
     * @param item The returned work item, or nullptr when the frame carried a library payload instead
     */
    static void applyReturn(
        processable_type &slot,
        [[maybe_unused]] std::uint32_t tag,
        [[maybe_unused]] const payload_type *payload,
        const processable_type *item
    ) {
        if(item != nullptr) {
            slot.absorbResultsFrom(*item);
        }
    }
};
//! [courtier-wire-seam]

/******************************************************************************/
/** @brief Shorthand for the library payload type of a work-item type. */
template <typename processable_type>
using wire_payload_t = typename GWireProtocolT<processable_type>::payload_type;

/******************************************************************************/

} /* namespace Gem::Courtier */
