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
#include <chrono>
#include <string>
#include <string_view>

// Geneva headers go here
#include "common/GCommonEnums.hpp"                // Gem::Common::serializationMode
#include "common/GGemCommit.hpp"                  // Gem::Common::gemCommit(), programIdentity()
#include "common/GModuleManifest.hpp"             // GEM_VERSION_STRING
#include "courtier/GCommandContainerT.hpp"        // GCommandContainerT, container_to/from_string
#include "courtier/GCourtierEnums.hpp"            // GFrameKind
#include "courtier/GWireSerializationContext.hpp" // GWireSerializationScope / registry / ids

namespace Gem::Courtier {

/******************************************************************************/
/**
 * Transport-agnostic codec for the networked consumer/worker session protocol. The networked
 * transports (websocket, MPI) all (de)serialize a GCommandContainerT. Only the raw byte I/O differs
 * between them; the (de)serialization itself -- and in particular the wire-scope discipline around
 * it -- is identical, so it lives here once instead of being copy-pasted into each transport.
 *
 * The rule this codec encapsulates so no transport has to remember it: a WORK ITEM is
 * (de)serialized inside a GWireSerializationScope bound to the transport's
 * GWireSerializationContext, so the blob send-once machinery engages (wireEncode / wireDecode /
 * wireDecodePayload).
 *
 * Every function is pure with respect to I/O: it turns containers/ids into strings or vice versa.
 * The transport owns the actual send/recv.
 */

/******************************************************************************/
/**
 * @brief Serializes a frame under the given wire context (blob send-once).
 * @tparam processable_type The payload type of the command container
 * @param container The frame to serialize
 * @param ctx The wire context to engage for the duration (may be nullptr for "no context")
 * @param serMode The serialization format to use
 * @return The serialized representation of the frame
 */
template <typename processable_type>
std::string wireEncode(
    const GCommandContainerT<processable_type> &container,
    const GWireSerializationContext *ctx,
    Gem::Common::serializationMode serMode
) {
    GWireSerializationScope const scope(ctx);
    return container_to_string(container, serMode);
}

/******************************************************************************/
/**
 * @brief De-serializes a frame under the given wire context (blob send-once): its head, then its
 * payload.
 * @tparam processable_type The payload type of the command container
 * @param descr The serialized representation to load from
 * @param container The frame to fill (reset before loading)
 * @param ctx The wire context to engage for the duration (may be nullptr for "no context")
 * @param serMode The serialization format the string was produced with
 */
template <typename processable_type>
void wireDecode(
    std::string_view descr,
    GCommandContainerT<processable_type> &container,
    const GWireSerializationContext *ctx,
    Gem::Common::serializationMode serMode
) {
    GWireSerializationScope const scope(ctx);
    container_from_string(descr, container, serMode);
}

/******************************************************************************/
/**
 * @brief De-serializes the payload of a frame whose head has been read (frameHeadFromString()),
 * under the given wire context (blob send-once).
 * @tparam processable_type The payload type of the command container
 * @param payload The payload part of the message
 * @param container The frame whose head has been read
 * @param ctx The wire context to engage for the duration (may be nullptr for "no context")
 * @param serMode The serialization format the message was produced with
 */
template <typename processable_type>
void wireDecodePayload(
    std::string_view payload,
    GCommandContainerT<processable_type> &container,
    const GWireSerializationContext *ctx,
    Gem::Common::serializationMode serMode
) {
    GWireSerializationScope const scope(ctx);
    framePayloadFromString(payload, container, serMode);
}

/******************************************************************************/
/**
 * @brief The HELLO a session sends first on every connection, and the one a client sends before it
 * ends because it does not match: this build (the commit of the Geneva sources,
 * Gem::Common::gemCommit()), the identity of this program (Gem::Common::programIdentity(), empty
 * if it has none), the keep-alive interval of the connection, which the client takes, and the
 * Geneva release (GEM_VERSION_STRING).
 * @tparam processable_type The payload type of the command container
 * @param ser_mode The serialization format to use
 * @param keep_alive The keep-alive interval of the server's ends (GListenerI::keepAlive(); 0: none;
 * a client names none)
 * @return The serialized HELLO
 */
template <typename processable_type>
std::string
buildHello(Gem::Common::serializationMode ser_mode, std::chrono::milliseconds keep_alive) {
    GCommandContainerT<processable_type> hello{GFrameKind::NONE};
    hello.setHello(
        std::string(Gem::Common::gemCommit()),
        std::string(Gem::Common::programIdentity()),
        keep_alive,
        std::string(GEM_VERSION_STRING)
    );
    return container_to_string(hello, ser_mode);
}

/******************************************************************************/
/**
 * @brief Whether a HELLO names the same build as this process -- first the commit of the Geneva
 * sources, then the identity of the program, where both sides have one. Another commit is told
 * with the releases and the commits of both sides.
 * @tparam processable_type The payload type of the command container
 * @param hello A HELLO frame
 * @param other Who sent it: "server" (on a client), or "client" (on the server, the HELLO of a
 * client that does not match)
 * @param own Who this process is
 * @return Empty if they match; otherwise what differs, for the log
 */
template <typename processable_type>
std::string helloMismatch(
    const GCommandContainerT<processable_type> &hello,
    std::string_view other = "server",
    std::string_view own = "client"
) {
    const std::string them{other};
    const std::string us{own};
    const std::string_view own_commit = Gem::Common::gemCommit();
    if(hello.build() != own_commit) {
        const std::string other_release =
            hello.release().empty() ? std::string("(release not named)") : hello.release();
        return "The " + them + " was built from another commit: the " + them + " is Geneva " +
               other_release + " of commit " + hello.build() + ", this " + us + " is Geneva " +
               GEM_VERSION_STRING + " of commit " + std::string(own_commit) +
               ". Server and client must be built from the same commit.";
    }
    const std::string_view own_program = Gem::Common::programIdentity();
    if(own_program.empty() || hello.program().empty() || hello.program() == own_program) {
        return {};
    }
    return "The " + them + " runs another program: the " + them + "'s program is " +
           hello.program() + ", this " + us + "'s is " + std::string(own_program) +
           ". Server and client must run the same build of the same program.";
}

/******************************************************************************/
/**
 * @brief Client side: whether the identity of the program cannot be checked against a HELLO,
 * because one side or both have none (Gem::Common::programIdentity()).
 * @tparam processable_type The payload type of the command container
 * @param hello A HELLO frame
 * @return Empty if both sides have an identity; otherwise which side has none, for the log
 */
template <typename processable_type>
std::string helloProgramUnchecked(const GCommandContainerT<processable_type> &hello) {
    const bool own_none = Gem::Common::programIdentity().empty();
    const bool server_none = hello.program().empty();
    if(not own_none && not server_none) {
        return {};
    }
    std::string which = "the server's program has no identity";
    if(own_none) {
        which = server_none ? "neither the server's program nor this client's has an identity"
                            : "this client's program has no identity";
    }
    return "The program is not checked against the server's: " + which +
           " (Geneva's CMake support stamps a program with GEM_STAMP_PROGRAM; a program may "
           "also call Gem::Common::setProgramIdentity()).";
}

/******************************************************************************/

} /* namespace Gem::Courtier */
