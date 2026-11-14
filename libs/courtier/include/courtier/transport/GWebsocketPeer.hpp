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
 * Threads. The socket's executor must serialize the handlers it runs: the server accepts each
 * connection onto its own strand, and a client runs its io context on one thread. The handlers are
 * called on that executor, and send() and abort() are to be called there. A handshake starts the
 * connection and may be requested from anywhere, as long as nothing else is done with the peer
 * until it has completed; close() may be called from any thread at any time.
 *
 * Ownership. Pending operations own the peer, and the peer owns the role through its handlers; the
 * role refers to the peer only weakly. A peer whose operations are discarded with their io context
 * is therefore destroyed, and its role with it. When the connection ends in the regular way, the
 * peer drops its handlers right after it has called onClosed.
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

    /** @brief What the role wants to be told. All three are called on the socket's executor. */
    struct GHandlers {
        /// The handshake is done and messages may be sent.
        std::move_only_function<void()> onOpen;
        /// One complete message has arrived.
        std::move_only_function<void(const std::string &)> onMessage;
        /// The connection is over; called exactly once. The error code says why: none if this end
        /// closed it, boost::beast::websocket::error::closed if the other end closed it in the
        /// regular way, boost::beast::error::timeout if a time limit expired, and the transport's
        /// error otherwise.
        std::move_only_function<void(const boost::system::error_code &)> onClosed;
    };

    /** @brief Takes over a connected TCP socket. One of the two handshakes must follow. */
    GWebsocketPeer(boost::asio::ip::tcp::socket socket, GOptions options, GHandlers handlers);
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
     *  close begins. */
    void send(std::string message);
    /** @brief Closes the connection in the regular way, from any thread. Idempotent. */
    void close(close_code code);
    /** @brief Ends the connection at once, without a close handshake. Not inside a handler. */
    void abort();
    /** @brief The executor all handlers of this peer run on. May be asked for from any thread. */
    [[nodiscard]] boost::asio::any_io_executor executor();

private:
    void configure_();
    void handshakeDone_(const boost::system::error_code &ec);
    void read_();
    void readDone_(const boost::system::error_code &ec);
    void pump_();
    void writeDone_(const boost::system::error_code &ec);
    void beginClose_(close_code code);
    void finish_(const boost::system::error_code &cause);

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_; ///< The connection
    GOptions options_;
    GHandlers handlers_; ///< Dropped once the connection is over

    boost::beast::multi_buffer incoming_; ///< Receives the message being read
    std::deque<std::string> write_queue_; ///< Messages waiting to be written
    std::string outgoing_;                ///< The message being written
    bool writing_ = false;                ///< Whether a write is outstanding

    bool open_ = false; ///< The handshake has completed
    bool closing_ =
        false; ///< A close has begun, or the connection is over: nothing is sent any more
    bool local_close_ = false;                 ///< This end began the close
    std::atomic<bool> close_requested_{false}; ///< close() was called (from any thread)
    bool finished_ = false;                    ///< onClosed has been called
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
