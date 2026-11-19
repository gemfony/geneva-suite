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
#include "common/GGenevaCommit.hpp"               // Gem::Common::genevaCommit(), programIdentity()
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
 * @brief Server side: the HELLO a session sends first on every connection: this build (the commit
 * of the Geneva sources, Gem::Common::genevaCommit()), the identity of this program
 * (Gem::Common::programIdentity(), empty if it has none) and the keep-alive interval of the
 * connection, which the client takes.
 * @tparam processable_type The payload type of the command container
 * @param ser_mode The serialization format to use
 * @param keep_alive The keep-alive interval of the server's ends (GListenerI::keepAlive(); 0: none)
 * @return The serialized HELLO
 */
template <typename processable_type>
std::string
buildHello(Gem::Common::serializationMode ser_mode, std::chrono::milliseconds keep_alive) {
    GCommandContainerT<processable_type> hello{GFrameKind::NONE};
    hello.setHello(
        std::string(Gem::Common::genevaCommit()),
        std::string(Gem::Common::programIdentity()),
        keep_alive
    );
    return container_to_string(hello, ser_mode);
}

/******************************************************************************/
/**
 * @brief Client side: whether a HELLO names the same build as this process -- first the commit of
 * the Geneva sources, then the identity of the program, where both sides have one.
 * @tparam processable_type The payload type of the command container
 * @param hello A HELLO frame
 * @return Empty if they match; otherwise what differs, for the log
 */
template <typename processable_type>
std::string helloMismatch(const GCommandContainerT<processable_type> &hello) {
    const std::string_view own = Gem::Common::genevaCommit();
    if(hello.build() != own) {
        return "The server was built from another commit: the server's Geneva commit is " +
               hello.build() + ", this client's (Geneva " + GEM_VERSION_STRING + ") is " +
               std::string(own) + ". Server and client must be built from the same commit.";
    }
    const std::string_view own_program = Gem::Common::programIdentity();
    if(own_program.empty() || hello.program().empty() || hello.program() == own_program) {
        return {};
    }
    return "The server runs another program: the server's program is " + hello.program() +
           ", this client's is " + std::string(own_program) +
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
           " (Geneva's CMake support stamps a program with GENEVA_STAMP_PROGRAM; a program may "
           "also call Gem::Common::setProgramIdentity()).";
}

/******************************************************************************/

} /* namespace Gem::Courtier */
