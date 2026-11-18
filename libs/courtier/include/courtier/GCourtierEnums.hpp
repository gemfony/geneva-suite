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
#include <array>
#include <chrono>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

// Boost headers go here

// Geneva headers go here
#include "common/GCommonEnums.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/** @brief Indicates whether the client executes the init-, run- or finally()-function */
enum class run_state : Gem::Common::ENUMBASETYPE {
    INIT = 0,
    RUN = 1,
    FINALLY = 2
};

/******************************************************************************/
/**
 * @brief The CLOSED frame vocabulary of the networked consumer/worker session protocol -- the part of a
 * message courtier itself acts on.
 *
 * A frame says what the message does to the *transport*: greet, pull for work, hand work out, check
 * a result back in, fetch a blob. What the message MEANS to the using library (evaluate this, here
 * are the results, ...) is not expressed here: it rides the frame as an opaque library tag plus a
 * library-defined payload that courtier relays without interpreting (see GCommandContainerT).
 *
 * Every message has a head, which is read first and by itself, and may have a payload:
 *  - HELLO is the first message of every connection, from the server: its build (the collection's
 *    version string) and the number of this protocol (GFRAMEPROTOCOL). A client whose own differ
 *    ends.
 *  - PULL asks for one item. The server answers every PULL with one WORK, in order; a server without
 *    work answers when work is there.
 *  - WORK carries an item; its head names the item (correlation id and lineage id, in the outcome),
 *    so that a client that cannot read the payload can still return the item as failed.
 *  - RESULT carries a processed item's outcome in its head and the item, or the library's payload,
 *    in its payload. It asks for nothing.
 *  - BLOB_REQUEST / BLOB_REPLY form the blob-send-once cache-miss fetch, served inside courtier.
 *    BLOB_REPLY is empty if the server's registry misses the blob.
 *
 * NONE is the cleared/default state of a frame container, never a transmitted frame. The values are
 * part of the wire format; the gaps are kinds of earlier protocols. The end of a run is not a
 * frame: the transport's close says it (GPeerClose::RUN_ENDED).
 */
//! [courtier-frame-kind]
enum class GFrameKind : Gem::Common::ENUMBASETYPE {
    NONE = 0,         ///< Cleared / not yet filled in -- never sent
    PULL = 1,         ///< worker -> server: "give me one item"
    WORK = 3,         ///< server -> worker: a work item to process
    BLOB_REQUEST = 6, ///< worker -> server: "send me the blob with this id"
    BLOB_REPLY = 7,   ///< server -> worker: the requested blob
    RESULT = 8,       ///< worker -> server: a processed item's outcome
    HELLO = 10        ///< server -> worker: the first message, the server's build
};
//! [courtier-frame-kind]

/** @brief Whether @p kind is one of the values of GFrameKind (a message from the network may carry
 *  any number) @param kind A frame kind, as read @return Whether GFrameKind has it */
[[nodiscard]] constexpr bool isFrameKind(GFrameKind kind) noexcept {
    switch(kind) {
    case GFrameKind::NONE:
    case GFrameKind::PULL:
    case GFrameKind::WORK:
    case GFrameKind::BLOB_REQUEST:
    case GFrameKind::BLOB_REPLY:
    case GFrameKind::RESULT:
    case GFrameKind::HELLO:
        return true;
    }
    return false;
}

/// The number of the frame protocol, sent with HELLO; a client of another number ends. Raised
/// whenever the frames change.
constexpr std::uint32_t GFRAMEPROTOCOL = 2;

/******************************************************************************/
/**
 * Specification of the different consumer types held in the GConsumerRegistry
 */
enum class consumerType : Gem::Common::ENUMBASETYPE {
    SERIAL = 0,
    MULTITHREADED = 1,
    NETWORKED = 2,
    LAST = consumerType::NETWORKED
};

/******************************************************************************/
/** What a process does in a consumer's protocol. */
enum class processRole : Gem::Common::ENUMBASETYPE {
    SUBMITTER = 0, ///< Builds the consumer and submits work through it
    CLIENT = 1,    ///< Serves work
    BYSTANDER = 2  ///< Takes no part in the consumer's protocol (in an MPI cascade: a member; in a
                   ///< cascade laid out GROUPS_ONLY, every process)
};

/******************************************************************************/
/**
 * Defaults of the socket consumers and their clients.
 */
constexpr unsigned short GCONSUMERDEFAULTPORT = 10000;
const std::string GCONSUMERDEFAULTSERVER = "localhost"; // NOLINT
constexpr std::uint16_t GCONSUMERLISTENERTHREADS = 4;
const Gem::Common::serializationMode GCONSUMERSERIALIZATIONMODE =
    Gem::Common::serializationMode::GEM_BINARY;
/// Websocket keep-alive, in seconds, for both ends of a connection: after this much silence a ping
/// is sent, and after twice as much without any inbound data the connection is closed. The same
/// limit (twice the interval) bounds the websocket handshake and the close handshake.
constexpr std::size_t GBEASTCONSUMERPINGINTERVAL = 15;
/// The limit on the websocket handshake and the close handshake when the keep-alive is switched off
/// (a ping interval of 0), so that neither can wait forever.
constexpr std::chrono::seconds GWEBSOCKETHANDSHAKETIMEOUT{30};
/// The websocket close code that tells a client that the run has ended and it need not come back
/// (GPeerClose::RUN_ENDED): the first code free for applications.
constexpr std::uint16_t GCONSUMERCLOSECODERUNENDED = 4000;
/// The largest websocket message accepted, in bytes; 0 means no limit. A work item or a result
/// travels as one message, and its size is the user's business (the MPI transport has no cap
/// either).
constexpr std::size_t GWEBSOCKETMAXMESSAGEBYTES = 0;
/// The write buffer of a websocket stream, in bytes. With fragmenting on, a message goes out in
/// frames of at most this size, so that a control frame never waits behind a large message.
constexpr std::size_t GWEBSOCKETWRITEBUFFERBYTES = 16384;
/// How many messages a websocket peer reads ahead of its role: while the role has this many
/// received messages still to handle, the peer stops reading until it has caught up. The protocol
/// keeps the number far lower (each end sends only what the other has asked for: at most the
/// client's prefetch depth per direction); the limit only stops a peer that floods. A paused peer
/// reads nothing, so it answers no ping either: the other end, the one that floods, sees no pong
/// and is closed by its own idle limit if the pause lasts two ping intervals.
constexpr std::size_t GWEBSOCKETMAXUNHANDLEDMESSAGES = 1024;
/// How long a stopping websocket server waits, after it has told every client that the run has
/// ended, for the connections to end (GListenerI::stopGrace()). A connection that has not ended by
/// then is cut off, and its client sees a loss.
constexpr std::chrono::milliseconds GWEBSOCKETCONSUMERSTOPGRACE{10'000};

/******************************************************************************
 * Pauses and time limits of the networked clients and servers:
 */
/// How often a networked client looks at its halt conditions (maximum run time, a close request)
/// and at its limits without a server.
constexpr std::chrono::seconds GCLIENTHALTPOLLINTERVAL{1};
/// How long a networked client tries to reach a server before its first contact with one (its
/// HELLO); 0: for as long as it runs (the batch system bounds it).
constexpr std::chrono::seconds GCLIENTFIRSTCONTACTLIMIT{0};
/// How long a networked client tries to reach a server again after it has lost its connection,
/// counted from the end of the last connection on which it had contact (a HELLO); 0: for as long
/// as it runs.
constexpr std::chrono::seconds GCLIENTRECONNECTLIMIT{600};
/// The pause of a networked client before it tries to connect again starts at the first value
/// and doubles with every failed attempt up to the second; the wait is drawn at random from its
/// second half. Contact with a server sets it back to the first value.
constexpr std::chrono::milliseconds GCLIENTRECONNECTPAUSEMIN{250};
constexpr std::chrono::milliseconds GCLIENTRECONNECTPAUSEMAX{30'000};
/// How long a socket server waits after a failed accept before it accepts again.
constexpr std::chrono::milliseconds GCONSUMERACCEPTRETRYPAUSE{100};

/******************************************************************************
 * Defaults of the timeout treatment of the networked consumers (see GNetworkedConsumerT and
 * GNetworkedTimeoutConfig, which both take their defaults from here):
 */
constexpr double GNETWORKEDEMAALPHA = 0.25;   ///< Weight of a new sample in the mean return time
constexpr double GNETWORKEDLEASEFACTOR = 4.0; ///< Reclaim lease = factor * mean return time
constexpr double GNETWORKEDSTALLFACTOR = 8.0; ///< Give-up window = factor * mean return time
constexpr std::chrono::milliseconds GNETWORKEDLEASEBOOTSTRAP{
    10'000
}; ///< Lease before a return was seen
constexpr std::chrono::milliseconds GNETWORKEDMINLEASE{200};     ///< Lower clamp on the lease
constexpr std::chrono::milliseconds GNETWORKEDMAXLEASE{300'000}; ///< Upper clamp on the lease
constexpr std::chrono::milliseconds GNETWORKEDMINSTALL{
    2'000
}; ///< Lower clamp on the give-up window
constexpr std::chrono::milliseconds GNETWORKEDMAXSTALL{
    600'000
}; ///< Upper clamp on the give-up window
constexpr std::chrono::milliseconds GNETWORKEDSWEEPTICK{
    50
}; ///< How often a waiting batch looks again
constexpr std::chrono::milliseconds GNETWORKEDFIXEDSTALLWINDOW{
    300'000
}; ///< Give-up window, "fixed"
constexpr std::chrono::milliseconds GNETWORKEDFIXEDLEASE{300'000}; ///< Reclaim lease, "fixed"
/// The window and the lease of the treatment "wait_indefinitely". A finite but very large value
/// (100 years) rather than milliseconds::max(): it is compared against a steady_clock (nanosecond)
/// duration, and max() would overflow int64 when the comparison promotes it to nanoseconds,
/// whereas 100 years in ns (~3.15e18) stays well within range and can never elapse in a real run.
constexpr std::chrono::milliseconds GNETWORKEDNEVERWINDOW{100LL * 365 * 24 * 3600 * 1000};

/******************************************************************************
 * Constants specifically for the GPU consumer:
 */
/// The default launch block size (x) of a kernel.
constexpr unsigned int GGPUCONSUMERBLOCKX = 256;

/******************************************************************************
 * Constants specifically for the GMPIConsumerT:
 */
/// The rank of the submitter in courtier's communicator
constexpr int RANK_MASTER_NODE = 0;
/// The message tags of the MPI transport's ends (GMPIPeer): the greeting of the submitter's
/// listener, a message of the role, and a close (with what it says).
constexpr int TAG_PEER_OPEN = 46;
constexpr int TAG_PEER_MESSAGE = 47;
constexpr int TAG_PEER_CLOSE = 48;
/// The number of threads the sessions of the MPI consumer run on, by default (MPIConsumerConfig)
constexpr std::uint32_t GMPICONSUMERSESSIONTHREADS = 2;

/******************************************************************************
 * Constants of the MPI poll core (transport/GMPIPollCore.hpp), the defaults of GMPIPollTiming:
 */
/// The pause of the MPI poll between two looks without traffic: it starts at the lower bound and
/// doubles up to the upper one. A received message and a started send reset it.
constexpr std::chrono::microseconds GMPIPEERPOLLPAUSEMIN{50};
constexpr std::chrono::microseconds GMPIPEERPOLLPAUSEMAX{1'000};
/// The longest pause of the MPI poll while the owner of every end it receives for is busy (a client
/// that evaluates has no use for an answer before its evaluation ends): the pause then doubles up to
/// this instead. A started send ends the pause at once, as always.
constexpr std::chrono::microseconds GMPIPEERPOLLPAUSEBUSY{50'000};
/// How many messages the MPI poll receives in one go before it lets other handlers of its
/// executor have their turn.
constexpr int GMPIPEERPOLLROUNDS = 16;
/// How long the MPI poll keeps looking after its last receiver has ended, for sends that have not
/// completed (a large message to a rank that no longer receives never does).
constexpr std::chrono::seconds GMPIPEERDRAINGRACE{2};

/******************************************************************************
 * Constants of the MPI cascade (mpi/GMPICascade.hpp, mpi/GMPICascadeConfig.hpp):
 */
/// The number of processes of a group of the cascade, its lead included, unless a program or the
/// cascade's configuration file says otherwise.
constexpr std::uint16_t GMPICASCADEDEFAULTGROUPSIZE = 4;
/// The configuration file of the cascade, relative to the working directory of rank 0.
inline constexpr std::string_view GMPICASCADECONFIGFILE = "./config/GMPICascade.json";

/******************************************************************************/
/** @brief The default number of threads for parallelization with threads */
constexpr std::uint16_t DEFAULTNSTDTHREADS = 2;

/******************************************************************************/
/**
 * Needed by the submission machinery (workOn / processBatch) to distinguish successfully processed items,
 * items that have not returned (for unknown reasons, e.g. network failure)
 * and items for which an exception was thrown during processing.
 */
enum class processingStatus : Gem::Common::ENUMBASETYPE {
    UNPROCESSED =
        0 // The default/initial (and post-reset) state: the item has not been selected for processing
          // and carries no valid result yet. No defined object value.
        ,
    DO_PROCESS =
        1 // Marks a GProcessingContainerT as "needs to be processed". No defined object value.
        ,
    PROCESSED = 2 // Set when processing has taken place. Object value is well defined.
        ,
    EXCEPTION_CAUGHT =
        3 // Set by GProcessingContainerT when an exception was caught. No defined object value.
        ,
    ERROR_FLAGGED =
        4 // Set by an external entity when errors have occurred during processing. No defined object value.
};

/******************************************************************************/
/**
 * The per-batch SCHEDULING state of a work item, used by the courtier networked consumers. It is
 * distinct from processingStatus (the item's domain/processing truth) and is NOT serialized: it is
 * transient, server-side-only bookkeeping that lets the consumer track, within one dispatch round,
 * which slots are awaiting a client, in flight, or done -- directly on the item, so no side queues
 * are needed.
 */
enum class dispatchState : Gem::Common::ENUMBASETYPE {
    NONE = 0,      ///< Default / not part of an active networked dispatch
    PENDING = 1,   ///< Awaiting a client
    IN_FLIGHT = 2, ///< Handed to a client, result not yet back
    DONE = 3       ///< Result has been received and written into the slot
};

/******************************************************************************/
/**
 * These typedefs steer the types of the routing / lineage ids carried by submitted work items
 */
// 64-bit so the networked consumer can pack a 40-bit, process-unique, never-wrapping batch id with a
// 24-bit slot index (GNetworkedConsumerT). This is the per-DISPATCH ROUTING token: it is minted fresh each
// dispatch and is unique per in-flight slot, so a return is matched back to exactly the slot it was served
// from -- correct even when an item is submitted more than once within one round of its submitter (large
// batches chunked into several spans, in-round re-dispatch). It deliberately does NOT identify the
// item across re-dispatch; that LINEAGE identity is SUBMISSION_UUID_TYPE below.
using CORRELATION_ID_TYPE = std::uint64_t;
/// The split of the correlation id described above: the slot index in the low bits, the batch id
/// above it. The slot field bounds one batch to 2^24 items.
constexpr CORRELATION_ID_TYPE GCORRELATIONIDSLOTBITS = 24;
constexpr CORRELATION_ID_TYPE GCORRELATIONIDBATCHBITS = 40;

// The stable, per-item LINEAGE identity of a work item (D11). Minted ONCE, at construction, and
// carried IMMUTABLY across every (re-)dispatch and serialized round-trip (fresh only on clone() = a new
// item). Distinct from the per-dispatch CORRELATION_ID_TYPE above: the correlation id routes a single
// return to its slot, while the uuid lets a LATE return be reunited with -- and de-duplicated against -- the
// live item it belongs to, even after that item has been resubmitted under a new correlation id.
// 128-bit (process salt + monotonic counter) so ids never collide within or across runs / checkpoint resumes.
// The salt half has a second use: it names the process that handed an item out, so a networked
// consumer drops a result whose item another server process handed out.
using SUBMISSION_UUID_TYPE = std::array<std::uint64_t, 2>;

/******************************************************************************/
// GFrameKind, consumerType and processRole stream as their underlying
// numeric values through the shared machinery in GCommonEnums.hpp (marker
// specializations after the namespace end). Re-export the operators so ADL
// finds them for this namespace's enums. processingStatus keeps a hand-written
// pair: its insertion operator deliberately prints the enumerator NAME for
// diagnostics.
using Gem::Common::operator<<;
using Gem::Common::operator>>;

/** @brief Puts a Gem::Courtier::processingStatus into a stream. Needed for streaming / Gem::Common::fromString<> */
std::ostream &operator<<(std::ostream &o, const Gem::Courtier::processingStatus &srm);
/** @brief Reads a Gem::Courtier::processingStatus item from a stream. Needed for streaming / Gem::Common::fromString<> */
std::istream &operator>>(std::istream &i, Gem::Courtier::processingStatus &srm);

/******************************************************************************/

} /* namespace Gem::Courtier */

/******************************************************************************/
// Numeric streaming opt-in (see numeric_enum_values_v in GCommonEnums.hpp)
namespace Gem::Common {
template <>
inline constexpr auto numeric_enum_values_v<Gem::Courtier::GFrameKind> = std::array{
    Gem::Courtier::GFrameKind::NONE,
    Gem::Courtier::GFrameKind::PULL,
    Gem::Courtier::GFrameKind::WORK,
    Gem::Courtier::GFrameKind::BLOB_REQUEST,
    Gem::Courtier::GFrameKind::BLOB_REPLY,
    Gem::Courtier::GFrameKind::RESULT,
    Gem::Courtier::GFrameKind::HELLO
};
template <>
inline constexpr auto numeric_enum_values_v<Gem::Courtier::consumerType> = numeric_enum_range<
    Gem::Courtier::consumerType::SERIAL,
    Gem::Courtier::consumerType::NETWORKED>();
template <>
inline constexpr auto numeric_enum_values_v<Gem::Courtier::processRole> = numeric_enum_range<
    Gem::Courtier::processRole::SUBMITTER,
    Gem::Courtier::processRole::BYSTANDER>();
} /* namespace Gem::Common */
