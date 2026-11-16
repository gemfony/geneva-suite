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
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"                // Gem::Common::ENUMBASETYPE
#include "courtier/GWireSerializationContext.hpp" // GWirePeerId

/******************************************************************************/
/**
 * @file
 * The connection between a server and one of its clients, independent of the transport that
 * carries it: a peer (GPeerI) is one end of one connection; a listener (GListenerI) makes the
 * peers on the server side, a connector (GConnectorI) the peer on the client side. The server's
 * session, the client and the server's shell are written against these three and know nothing of
 * the transport; a transport implements them and knows nothing of frames, items or roles.
 *
 * Boost.Asio is part of the interface: every transport works on an executor that its user hands
 * it, and a transport loaded from a module must be built against the same Boost.
 *
 * **The rules every transport keeps.** They are the contract between the roles and the
 * transports; each implementation (and the in-process pair of the tests) is checked against them.
 * "The role" is whoever installed the handlers: the server's session or the client.
 *
 *  1. **Two phases.** Nothing is reported before GPeerI::start(); onOpen precedes any onMessage.
 *     A peer that cannot be opened reports onClosed (GPeerEnd::FAILED) without onOpen.
 *  2. **One executor, serialized.** send() and setOwnerBusy() are called on the peer's
 *     executor; abort() only at teardown (see there); close() and executor() from any thread; start() from any thread, once, before
 *     anything else. start() puts its work on the executor, and a close() made from any thread
 *     after start() has returned is ordered after that work. On a server the handlers run on the
 *     executor the shell hands to the listener, and the transport moves the bytes on threads of
 *     its own: a session may take any time in a handler without its connection, or another,
 *     falling silent. A client's handlers run on the executor given to the connector, which the
 *     transport shares: a client keeps its handlers short (GPrefetchingClientT encodes and
 *     decodes on its compute pool).
 *  3. **No handler runs inside a call** of GPeerI, abort() excepted (see there). A failure found in
 *     send() is posted to the executor, not reported inline: the role's member function that
 *     called send() is still on the stack.
 *  4. **Ownership.** Pending work on the executor owns the peer; the peer owns the role through
 *     its handlers and drops them after onClosed; the role refers to the peer weakly. A peer keeps
 *     its executor's context busy (it has work pending there) from start() until it has ended,
 *     also when the transport works on threads of its own (it holds work on the handlers'
 *     context, as GWebsocketPeer does).
 *  5. **onClosed at most once**, and exactly once while the executor runs. A peer whose pending
 *     work is discarded with its context ends without it. A role therefore releases what it holds
 *     in its destructor, never in onClosed. A peer that this end closed reports
 *     GPeerEnd::CLOSED_HERE only if the other end confirmed the close (rule 6), and
 *     GPeerEnd::CLOSED_HERE_UNCONFIRMED otherwise -- when its grace or time limit passed, when what
 *     was queued could not be written, when the close ended with an error, when the other end's
 *     close came before this end's had gone out, or when it crossed this end's close.
 *  6. **A close sends what was handed over before it.** Messages handed to send() before close()
 *     reach the other end, in order, before the close does; what the close says (GPeerClose)
 *     reaches the other end too. After close() a send() is dropped. What arrives after close()
 *     has been called is still delivered to the role until this end's close has gone out: the
 *     other end sent it before it knew of the close, and the role decides what it means (a
 *     session checks a late result in and answers nothing, a client starts no evaluation). What
 *     arrives after this end's close has gone out may be dropped. A role that asks for the close
 *     before it has been told of the open is told nothing (rule 1); its end reads nothing before
 *     its close has gone out, so that what arrived for it can never be confirmed to the other
 *     end. abort() is the exception: it ends at once and may cut what has not been sent.
 *     **Confirmed** means: the other end answered this end's close -- it sent its own close only
 *     after this end's close had arrived there, and so after its role had been handed everything
 *     that arrived before. For the websocket transport that is a close frame, read after ours was
 *     written, that echoes our close code (the two ends begin their closes with codes of their
 *     own, GWebsocketPeer); for the in-process pair of the tests its acknowledgement. Only a
 *     confirmed close (CLOSED_HERE) proves that everything handed to send() before close() has
 *     been handed to the other end's role; an unconfirmed one (CLOSED_HERE_UNCONFIRMED) proves
 *     nothing about it. The other end's own close is no answer, also when it arrives after this
 *     end's close had been sent (the two crossed): what this end sent may have arrived there after
 *     the other end's close had gone out, and been dropped. A transport whose close carries no
 *     answer of the other end (a message passing library, say) confirms a close only by an answer
 *     it sends for that purpose, after its role has been handed what arrived before the close.
 *     "Before" is the order of the calls: a send() that has returned before close() is called
 *     (on the same thread, or ordered by the role). A send() on the executor and a close() from
 *     another thread at the same moment are not ordered; the message may be dropped. Nothing is
 *     lost by that: the server's lease puts back what the client held.
 *  7. **The transport's own work** (an accept loop, a poll, its I/O threads: whatever it runs
 *     besides its peers' handlers) ends with GListenerI::endWork() on the server and with the
 *     destruction of the GConnectorI on the client; both happen when no handler runs any more, and
 *     before the executor's context is destroyed. endWork() stops and joins the transport's
 *     threads and shuts its I/O context down (GIoContext::shutdown(): the pending handlers go,
 *     the context stays); the listener, and its context with it, is destroyed after the shell's
 *     context. A connector's peers do their I/O on the executor given to connect() (a transport
 *     whose client end needs I/O of its own keeps it on that executor's thread, too), so that
 *     nothing of a client's peer runs once that executor's context has stopped.
 *  8. **The end of a run belongs to the process, not to a session.** The server's shell tells
 *     every peer that has not ended that the run is over (close(GPeerClose::RUN_ENDED)) and waits,
 *     at most GListenerI::stopGrace(), until they have ended. GListenerI::stop() means "no new
 *     peers, and no call of on_peer under way": every peer the listener has made before has
 *     reached the shell when stop() returns, so the shell's look at its peers after the stop
 *     finds them all. A client that is told RUN_ENDED does not come back.
 */

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * A message as GPeerI::send() takes it: shared and immutable, so that whoever sends it may keep it
 * (to send it once more on another connection, say) while the transport writes it, without a copy.
 */
using GMessage = std::shared_ptr<const std::string>;

/** @brief Makes a message for GPeerI::send() @param bytes The complete message
 *  @return It, shared and immutable */
inline GMessage makeMessage(std::string bytes) {
    return std::make_shared<const std::string>(std::move(bytes));
}

/******************************************************************************/
/** @brief Why a connection ended. */
enum class GPeerEnd : Gem::Common::ENUMBASETYPE {
    CLOSED_HERE = 0,  ///< This end closed it, and the other end confirmed the close (rule 6)
    CLOSED_THERE = 1, ///< The other end closed it in the regular way
    SILENT = 2,       ///< The other end was not heard from within the transport's limit
    FAILED = 3,       ///< The transport failed, or a message could not be sent
    CLOSED_HERE_UNCONFIRMED = 4 ///< This end closed it, and the other end did not confirm (rule 6)
};

/** @brief What a regular close says; each transport maps it to what it can carry. */
enum class GPeerClose : Gem::Common::ENUMBASETYPE {
    NORMAL = 0,   ///< This end has nothing more to say
    ERROR = 1,    ///< This end gives up after a failure
    RUN_ENDED = 2 ///< The run has ended; the other end need not come back
};

/******************************************************************************/
/**
 * What a role (the server's session, or the client) wants to be told by its peer. All three are
 * called on the peer's executor, one at a time, and never from inside a call of GPeerI (rule 3).
 */
struct GPeerHandlers {
    /// The connection is usable: messages may be sent.
    std::move_only_function<void()> onOpen;
    /// One complete message has arrived. The role may take the string.
    std::move_only_function<void(std::string &&)> onMessage;
    /// The connection is over. Called at most once, and exactly once while the executor runs
    /// (rule 5). The second argument is what the other end's close said, if one arrived (an
    /// acknowledgement may echo this end's own close); NORMAL if none arrived. The third is a
    /// description for the log.
    std::move_only_function<void(GPeerEnd, GPeerClose, std::string_view)> onClosed;
};

/******************************************************************************/
/**
 * One end of one connection to one other process. It moves whole messages and reports three
 * events; it knows nothing about frames, items or roles. It keeps rules 1 to 6 of this file.
 */
class GPeerI {
public:
    /** @brief The default constructor */
    GPeerI() = default;
    /** @brief The destructor */
    virtual ~GPeerI() = default;

    GPeerI(const GPeerI &) = delete;
    GPeerI(GPeerI &&) = delete;
    GPeerI &operator=(const GPeerI &) = delete;
    GPeerI &operator=(GPeerI &&) = delete;

    /** @brief Installs the handlers and opens the connection. From any thread; once, and before
     *  anything else is done with the peer. Its work is put on the executor (rule 2). */
    virtual void start(GPeerHandlers) = 0;

    /** @brief Sends one complete message. On the executor. Messages arrive in the order they were
     *  sent; one handed over before onOpen or after close() is dropped. The transport shares the
     *  message with the caller and never changes it; it may hold it until it has been written. */
    virtual void send(GMessage) = 0;

    /** @brief Closes the connection in the regular way, after what was handed to send() before
     *  (rule 6). From any thread; idempotent: the first call decides what the other end is told,
     *  a later one changes nothing (a session that closes with ERROR and then sees the end of the
     *  run must not turn the ERROR into RUN_ENDED). */
    virtual void close(GPeerClose) = 0;

    /** @brief Ends the connection at once, without waiting for the other end; may cut what has not
     *  been sent. For teardown only: called when no thread runs a handler of the peer or does its
     *  I/O any more -- on a client after its executor's context has stopped running (rule 7: the
     *  connector's peers do their I/O there). A listener's peers are not aborted: endWork() lets
     *  them go. Calls onClosed itself (if the peer has not ended) and drops the handlers. A peer
     *  aborted before the work of its start() has run does not open afterwards. What onClosed
     *  then says: CLOSED_HERE for a peer that had not been closed before (the teardown is this
     *  end's own end, and nothing was sent that a role waits to see proven); for a peer whose
     *  close() had been called and not confirmed, CLOSED_HERE_UNCONFIRMED; for a peer that had
     *  ended and whose report had not run, the end it had found. */
    virtual void abort() = 0;

    /** @brief The executor all handlers of this peer run on. From any thread. */
    [[nodiscard]] virtual boost::asio::any_io_executor executor() = 0;

    /** @brief Tells the transport whether the owner of this peer (a client) is busy evaluating. On
     *  the executor. A polling transport may look less often while its owner is busy; the
     *  default ignores it. */
    virtual void setOwnerBusy(bool) { /* nothing */
    }
};

/******************************************************************************/
/**
 * The server side of a transport: makes a peer for every client. It lives as long as the server's
 * shell that owns it (questions about the transport, such as its port, go to it at any time),
 * and its own work ends with endWork() (rule 7).
 */
class GListenerI {
public:
    /** @brief What is called for every client: a peer that has not been started, and the
     *  client's id in the blob registry */
    using on_peer_type = std::move_only_function<void(std::shared_ptr<GPeerI>, GWirePeerId)>;

    /** @brief The default constructor */
    GListenerI() = default;
    /** @brief The destructor. A listener that was started is destroyed only after endWork(). */
    virtual ~GListenerI() = default;

    GListenerI(const GListenerI &) = delete;
    GListenerI(GListenerI &&) = delete;
    GListenerI &operator=(const GListenerI &) = delete;
    GListenerI &operator=(GListenerI &&) = delete;

    /** @brief Starts making peers. Once. The peers' handlers run on strands of the executor;
     *  on_peer is called on one of its threads. May throw (a port that cannot be bound); a start()
     *  that throws leaves the listener as if it had not been started: it holds nothing of the
     *  executor and of on_peer, and runs nothing. Its owner then calls neither stop() nor
     *  endWork(), and does not call start() again. */
    virtual void start(boost::asio::any_io_executor, on_peer_type) = 0;

    /** @brief No new peers from here on, and none on its way: when stop() returns, no call of
     *  on_peer is under way and none will come. A listener counts the calls of on_peer it has
     *  posted and waits until they have returned; they are short (a session is made and
     *  started). It does not wait for clients (the shell does, rule 8), and its wait has a short
     *  bound of its own, so that a stop takes one grace, not two; a listener whose bound is hit
     *  says so in the log. Not on a thread of the executor; the executor still runs.
     *  Idempotent. */
    virtual void stop() = 0;

    /** @brief Ends the transport's own work (rule 7). Called once, by the shell, when no handler
     *  runs any more (after the executor's threads have been joined) and before the executor's
     *  context is destroyed. Peers that have not ended are let go without onClosed (rule 5).
     *  Afterwards the listener holds nothing that refers to that context (no executor, no strand,
     *  no handler): the context is destroyed before the listener. */
    virtual void endWork() = 0;

    /** @brief How long the shell waits, after it has told every client that the run has ended,
     *  for the peers to end (rule 8). */
    [[nodiscard]] virtual std::chrono::milliseconds stopGrace() const = 0;

    /** @brief Whether the loss of a client ends its peer by itself, within a bounded time (a
     *  transport without a keep-alive answers false). If not, the consumer reclaims items by the
     *  time lease. */
    [[nodiscard]] virtual bool detectsLoss() const = 0;
};

/******************************************************************************/
/**
 * The client side of a transport: makes a peer to the server, as often as asked. Destroying it
 * cancels an attempt in progress and ends the transport's own work (rule 7); it is destroyed
 * after the executor's context has stopped running and before that context is destroyed.
 *
 * The executor handed to connect() must run its handlers one at a time (a single-threaded
 * context or a strand): neither the attempt nor the peer it makes serializes itself further.
 */
class GConnectorI {
public:
    /** @brief What is called when an attempt is over: a peer that has not been started and an
     *  empty text, or no peer and the reason */
    using done_type = std::move_only_function<void(std::shared_ptr<GPeerI>, std::string_view)>;

    /** @brief The default constructor */
    GConnectorI() = default;
    /** @brief The destructor. Cancels an attempt in progress; done is then not called. */
    virtual ~GConnectorI() = default;

    GConnectorI(const GConnectorI &) = delete;
    GConnectorI(GConnectorI &&) = delete;
    GConnectorI &operator=(const GConnectorI &) = delete;
    GConnectorI &operator=(GConnectorI &&) = delete;

    /** @brief Makes a connection to the server; one attempt at a time. From any thread, also
     *  before the executor runs, but not at the same time as cancel(). done is called on the
     *  executor, never inside this call, and not after cancel(). */
    virtual void connect(boost::asio::any_io_executor, done_type) = 0;

    /** @brief Gives up an attempt in progress; done is then not called. On the executor. */
    virtual void cancel() = 0;

    /** @brief Whether connect() may be called again after an attempt that failed or a peer that
     *  ended. A client whose connector answers false makes one attempt and ends with its peer. */
    [[nodiscard]] virtual bool reconnectable() const = 0;
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
