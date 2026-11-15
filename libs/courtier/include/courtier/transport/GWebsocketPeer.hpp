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
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp" // Gem::Common::serializationMode
#include "courtier/GCourtierEnums.hpp"

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * One end of a courtier websocket connection: the part that is the same for the client and for the
 * server's session. It owns the stream and moves whole messages; what a message means is the
 * business of the role that uses it (the client's work pipeline, or the session's handling of
 * requests).
 *
 * What it does for both ends:
 *  - configures the stream (fragmenting, write buffer, binary or text frames) and Beast's own time
 *    limits: after one ping interval without inbound data a ping is sent, after a second one the
 *    connection is closed; the websocket handshake and the close handshake have the same limit. Any
 *    received frame counts as activity, control frames included;
 *  - keeps one read outstanding from the handshake to the end of the connection, so pings are
 *    answered and the idle limit is watched while the role does something else;
 *  - sends through a queue, because Beast allows one write at a time;
 *  - closes asynchronously and within the handshake limit, so a peer that no longer answers cannot
 *    hold an io thread, and switches Beast's limits off once the connection is over (Beast leaves
 *    its timer armed after a failed read, which would keep an io context busy). A close ends the
 *    exchange of messages at once: a message that is being written is cut short if it has more
 *    than one frame (Beast sends the close frame between two frames), messages still waiting in
 *    the queue are dropped, and nothing that arrives after the call to close() is delivered. Both
 *    roles can live with that: an item that did not get through is put back by the server.
 *
 * Threads. The peer works on two executors. The stream lives on the socket's executor, which must
 * serialize the handlers it runs (the server accepts each connection onto a strand of its own, a
 * client runs its io context on one thread): reading, writing, the keep-alive and the close happen
 * there and nowhere else. The role's handlers run on a strand of the handler executor given to the
 * constructor (the socket's executor if none is given). The two are apart so that the role may take
 * as long as it likes over a message -- decoding it, encoding the answer -- without the connection
 * looking idle: the peer goes on reading, and answering pings, while the role works. Messages that
 * arrive meanwhile wait for the role, in order; at GWEBSOCKETMAXUNHANDLEDMESSAGES of them the peer
 * stops reading until the role has caught up. send() and abort() are to be called on the handler
 * strand (executor() hands it out); a handshake starts the connection and may be requested from
 * anywhere, as long as nothing else is done with the peer until it has completed; close() may be
 * called from any thread at any time. Liveness therefore needs the socket's executor to have threads
 * that do not run role code; with one executor for both it is as live as the role is quick.
 *
 * Ownership. Pending operations own the peer, and the peer owns the role through its handlers; the
 * role refers to the peer only weakly. A peer whose operations are discarded with their io context
 * is therefore destroyed, and its role with it. When the connection ends in the regular way, the
 * peer drops its handlers right after it has called onClosed. The peer keeps the handler executor's
 * context busy until onClosed has run (or abort() has ended it), so that a role whose only work is
 * this connection does not see its io context run dry while the connection lasts. Whoever owns the
 * two contexts destroys the I/O context's pending handlers before the handler context (see
 * GIoContext).
 */
class GWebsocketPeer final : public std::enable_shared_from_this<GWebsocketPeer> {
public:
    using close_code = boost::beast::websocket::close_code;

    /** @brief How this end of the connection is set up. */
    struct GOptions {
        std::string name; ///< Who this end is, for the lines logged about control frames
        Gem::Common::serializationMode mode =
            Gem::Common::serializationMode::GEM_BINARY; ///< Binary or text frames
        bool verboseControlFrames = false; ///< Whether every close/ping/pong frame is logged
        /// The silence after which a ping is sent; the connection is closed after twice that time
        /// without inbound data. Zero switches the idle limit off.
        std::chrono::milliseconds pingInterval{std::chrono::seconds(GBEASTCONSUMERPINGINTERVAL)};
    };

    /** @brief What the role wants to be told. All three are called on the handler strand. */
    struct GHandlers {
        /// The handshake is done and messages may be sent.
        std::move_only_function<void()> onOpen;
        /// One complete message has arrived. The role may take the string.
        std::move_only_function<void(std::string &&)> onMessage;
        /// The connection is over; called exactly once. The error code says why: none if this end
        /// closed it, boost::beast::websocket::error::closed if the other end closed it in the
        /// regular way (its close frame arrived, whatever became of the TCP connection after
        /// it), boost::beast::error::timeout if a time limit expired, and the transport's error
        /// otherwise.
        std::move_only_function<void(const boost::system::error_code &)> onClosed;
    };

    /** @brief Takes over a connected TCP socket. One of the two handshakes must follow. The role's
     *  handlers run on a strand of @p handler_executor, or on the socket's executor if it is empty. */
    GWebsocketPeer(
        boost::asio::ip::tcp::socket socket,
        GOptions options,
        GHandlers handlers,
        const boost::asio::any_io_executor &handler_executor = {}
    );
    ~GWebsocketPeer() = default;

    GWebsocketPeer(const GWebsocketPeer &) = delete;
    GWebsocketPeer(GWebsocketPeer &&) = delete;
    GWebsocketPeer &operator=(const GWebsocketPeer &) = delete;
    GWebsocketPeer &operator=(GWebsocketPeer &&) = delete;

    /** @brief Server side: waits for the client's websocket handshake, then reads. */
    void acceptHandshake();
    /** @brief Client side: performs the websocket handshake with the server, then reads. */
    void connectHandshake(const std::string &host);
    /** @brief Queues a message; messages are written one after another, in this order, until a
     *  close begins. On the handler strand; the write itself is done on the socket's executor. */
    void send(std::string message);
    /** @brief Closes the connection in the regular way, from any thread. Idempotent. */
    void close(close_code code);
    /** @brief Ends the connection at once, without a close handshake. Not inside a handler, and
     *  only when the socket's executor runs no handler of this peer (on it, or after its context
     *  has stopped). */
    void abort();
    /** @brief The strand all handlers of this peer run on (to post work that came from
     *  elsewhere). May be asked for from any thread. */
    [[nodiscard]] boost::asio::any_io_executor executor();

private:
    void configure_();
    void handshakeDone_(const boost::system::error_code &ec);
    void read_();
    void readDone_(const boost::system::error_code &ec);
    void deliver_(std::string &message);
    void resumeReading_();
    void report_(const boost::system::error_code &cause);
    void pump_();
    void writeDone_(const boost::system::error_code &ec);
    void beginClose_(close_code code);
    void finish_(const boost::system::error_code &cause, bool report_inline = false);

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_; ///< The connection
    GOptions options_;
    GHandlers handlers_; ///< On the handler strand; dropped once the connection is over

    boost::asio::any_io_executor io_executor_;    ///< The socket's executor (where the bytes move)
    boost::asio::any_io_executor handler_strand_; ///< Where the role's handlers run
    boost::asio::any_io_executor owner_work_;     ///< Keeps the handler context busy until the end
    /// Received messages not yet handled by the role (incremented on the socket's executor,
    /// decremented on the handler strand)
    std::atomic<std::size_t> unhandled_{0};
    bool read_paused_ = false; ///< Reading waits for the role to catch up (socket's executor)

    boost::beast::multi_buffer incoming_; ///< Receives the message being read
    std::deque<std::string> write_queue_; ///< Messages waiting to be written
    std::string outgoing_;                ///< The message being written
    bool writing_ = false;                ///< Whether a write is outstanding

    bool open_ = false; ///< The handshake has completed
    bool closing_ =
        false; ///< A close has begun, or the connection is over: nothing is sent any more
    bool local_close_ = false;                 ///< This end began the close
    bool close_received_ = false;              ///< The other end's close frame has arrived
    std::atomic<bool> close_requested_{false}; ///< close() was called (from any thread)
    bool finished_ = false;                    ///< onClosed has been called

    /// When the last complete message or control frame of the other end arrived (or the handshake
    /// completed): what the log names when the idle limit has expired
    std::chrono::steady_clock::time_point last_inbound_;
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
