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
 * A frame says what the message does to the *transport*: pull for work, hand work out, check a result
 * back in, shut a worker down, fetch a blob. What the message MEANS to the using library (evaluate this,
 * here are the results, …) is not expressed here: it rides the frame as an opaque library tag plus a
 * library-defined payload that courtier relays without interpreting (see GCommandContainerT).
 *
 * BLOB_REQUEST / BLOB_REPLY form the blob-send-once cache-miss fetch: a worker that receives a work item
 * referencing a blob id it does not hold asks the server for it (BLOB_REQUEST), and the server answers
 * from its own registry (BLOB_REPLY). Both are served entirely inside courtier, hence frames rather than
 * library tags.
 *
 * NONE is the cleared/default state of a frame container, never a transmitted frame.
 */
//! [courtier-frame-kind]
enum class GFrameKind : Gem::Common::ENUMBASETYPE {
    NONE = 0,         ///< Cleared / not yet filled in -- never sent
    PULL = 1,         ///< worker -> server: "give me work" (may carry a result, see RETURN)
    NO_WORK = 2,      ///< server -> worker: "nothing available right now"
    WORK = 3,         ///< server -> worker: a work item to process
    RETURN = 4,       ///< worker -> server: a processed slot's outcome (+ item or library payload)
    SHUTDOWN = 5,     ///< server -> worker: stop working and terminate
    BLOB_REQUEST = 6, ///< worker -> server: "send me the blob with this id"
    BLOB_REPLY = 7    ///< server -> worker: the requested blob (empty on a registry miss)
};
//! [courtier-frame-kind]

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
/**
 * Global variables for failed transfers and connection attempts.
 */
constexpr std::uint32_t GASIOCONSUMERMAXCONNECTIONATTEMPTS = 10;
/// How long an asio client waits for the server's reply to a request before it gives up. A server
/// answers every request at once (with work or with NO_WORK), so a longer silence means it is gone.
constexpr std::chrono::seconds GASIOCLIENTREPLYTIMEOUT{120};
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
/// The largest websocket message accepted, in bytes; 0 means no limit. A work item or a result
/// travels as one message, and its size is the user's business (the MPI transport has no cap
/// either).
constexpr std::size_t GWEBSOCKETMAXMESSAGEBYTES = 0;
/// The write buffer of a websocket stream, in bytes. With fragmenting on, a message goes out in
/// frames of at most this size, so that a control frame never waits behind a large message.
constexpr std::size_t GWEBSOCKETWRITEBUFFERBYTES = 16384;

/******************************************************************************
 * Pauses and time limits of the networked clients and servers:
 */
/// A worker that is answered with NO_WORK waits a random time between these two bounds, in
/// milliseconds, before it asks again. Random, so that workers that were sent away together do
/// not come back together. (A websocket client is rarely answered with NO_WORK: its request waits
/// at the server instead.)
constexpr std::uint32_t GCLIENTNOWORKPAUSELOWERBOUNDARYMSEC = 50;
constexpr std::uint32_t GCLIENTNOWORKPAUSEUPPERBOUNDARYMSEC = 200;
/// How often a socket client looks at its halt conditions (maximum run time, a close request).
constexpr std::chrono::seconds GCLIENTHALTPOLLINTERVAL{1};
/// An asio client whose connection attempt failed waits a random time between these two bounds,
/// in milliseconds, before it tries again.
constexpr std::uint32_t GASIOCLIENTRECONNECTPAUSELOWERBOUNDARYMSEC = 500;
constexpr std::uint32_t GASIOCLIENTRECONNECTPAUSEUPPERBOUNDARYMSEC = 1000;
/// The deadline of one exchange with an asio client: a client that connects but does not complete
/// its request is dropped after this time. It is not a limit on the evaluation (the client does
/// not hold the connection while it computes). Zero switches it off.
constexpr std::chrono::milliseconds GASIOSESSIONTIMEOUT{300'000};
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
/// The MPI worker's names for the pause after NO_WORK (see GCLIENTNOWORKPAUSELOWERBOUNDARYMSEC).
constexpr std::uint32_t GMPICONSUMERWORKERNODERETRYINTERVALLOWERBOUNDARYMSEC =
    GCLIENTNOWORKPAUSELOWERBOUNDARYMSEC;
constexpr std::uint32_t GMPICONSUMERWORKERNODERETRYINTERVALUPPERBOUNDARYMSEC =
    GCLIENTNOWORKPAUSEUPPERBOUNDARYMSEC;
/// The message tags that the master and the worker nodes use.
constexpr int TAG_REQUEST_WORK_ITEM = 42;
constexpr int TAG_SEND_WORK_ITEM = 43;
/// blob send-once cache-miss fetch (worker <-> master). A worker that receives an id-only
/// work item whose blob it does not hold sends a BLOB_REQUEST frame on TAG_REQUEST_BLOB; the
/// master's receiver loop picks it up (it matches any tag) and answers with the serialized blob on
/// TAG_SEND_BLOB. A distinct send tag keeps the reply from being mistaken for an ordinary work-item
/// response by the worker's (double-buffered) MPI_ANY_TAG receive.
constexpr int TAG_REQUEST_BLOB = 44;
constexpr int TAG_SEND_BLOB = 45;
constexpr int RANK_MASTER_NODE = 0;
/// Once the master has been asked to stop, how long it keeps waiting for stragglers (a live
/// worker's final double-buffered request, or an open session to finish) before abandoning them.
/// Bounds shutdown so the loss of a worker cannot wedge the master at teardown.
constexpr std::chrono::seconds GMPICONSUMERSHUTDOWNGRACE{10};
/// How long a worker waits for the master to complete a send/receive before giving up. The master
/// always answers a request promptly (work or NO_WORK), so this only trips when the master has died
/// or gone silent -- it is generous to avoid ever false-killing a worker while a live master is busy.
constexpr std::chrono::seconds GMPICONSUMERWORKERMPITIMEOUT{120};
/// How long the master blocks for a pending item before it answers a worker with NO_WORK.
constexpr std::chrono::milliseconds GMPICONSUMERCHECKOUTWAIT{50};
/// The pause between two looks at an MPI operation that is polled for completion.
constexpr std::chrono::milliseconds GMPICONSUMERPOLLPAUSE{1};
/// The default time in ms between two checks whether the send of a work item has completed.
constexpr std::uint32_t GMPICONSUMERMASTERCLEANSESSINTERVALMSEC = 1'000;

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
// from -- correct even when an individual is submitted more than once within a single OA iteration (large
// populations chunked into several spans, in-iteration re-dispatch). It deliberately does NOT identify the
// individual across re-dispatch; that LINEAGE identity is SUBMISSION_UUID_TYPE below.
using CORRELATION_ID_TYPE = std::uint64_t;
/// The split of the correlation id described above: the slot index in the low bits, the batch id
/// above it. The slot field bounds one batch to 2^24 items.
constexpr CORRELATION_ID_TYPE GCORRELATIONIDSLOTBITS = 24;
constexpr CORRELATION_ID_TYPE GCORRELATIONIDBATCHBITS = 40;

// The stable, per-individual LINEAGE identity of a work item (D11). Minted ONCE, at construction, and
// carried IMMUTABLY across every (re-)dispatch and serialized round-trip (fresh only on clone() = a new
// individual). Distinct from the per-dispatch CORRELATION_ID_TYPE above: the correlation id routes a single
// return to its slot, while the uuid lets a LATE return be reunited with -- and de-duplicated against -- the
// live individual it belongs to, even after that individual has been resubmitted under a new correlation id.
// 128-bit (process salt + monotonic counter) so ids never collide within or across runs / checkpoint resumes.
using SUBMISSION_UUID_TYPE = std::array<std::uint64_t, 2>;

/******************************************************************************/
// GFrameKind and consumerType stream as their underlying
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
// Numeric streaming opt-in (see numeric_enum_io_v in GCommonEnums.hpp)
namespace Gem::Common {
template <> inline constexpr bool numeric_enum_io_v<Gem::Courtier::GFrameKind> = true;
template <> inline constexpr bool numeric_enum_io_v<Gem::Courtier::consumerType> = true;
} /* namespace Gem::Common */
