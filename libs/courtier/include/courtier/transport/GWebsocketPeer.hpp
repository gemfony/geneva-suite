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
#include <memory>
#include <string>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp" // Gem::Common::serializationMode
#include "courtier/GCourtierEnums.hpp"
#include "courtier/transport/GPeerI.hpp" // the interface it implements

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * One end of a courtier websocket connection, as a GPeerI: the same class serves the client and the
 * server's session. It owns the stream and moves whole messages; what a message means is the
 * business of the role that started it (the client, or the server's session). It keeps the rules of
 * GPeerI.hpp.
 *
 * What it does for both ends:
 *  - configures the stream (fragmenting, write buffer, binary or text frames) and Beast's own time
 *    limits: after one ping interval without inbound data a ping is sent, after a second one the
 *    connection is closed; the websocket handshake and the close handshake have the same limit. Any
 *    received frame counts as activity, control frames included;
 *  - performs its side of the websocket handshake when it is started: the server's end waits for
 *    the client's handshake, the client's end (constructed with the server's host name) performs
 *    it. onOpen follows the handshake; a handshake that fails is reported as FAILED;
 *  - keeps one read outstanding from the handshake to the end of the connection, so pings are
 *    answered and the idle limit is watched while the role does something else;
 *  - sends through a queue, because Beast allows one write at a time;
 *  - closes in the regular way after what was handed to send() before (rule 6): the close frame
 *    goes out when the write queue is empty, so a message of several frames is never cut. The
 *    wait for the queue and the close handshake are bounded by the handshake limit, so a peer that
 *    no longer reads cannot hold the connection. A close during the handshake waits for it on
 *    the server's end (the client still hears what it says) and ends the client's end at once,
 *    as FAILED. Nothing that arrives after close() is delivered.
 *    Once the connection is over Beast's limits are switched off (Beast leaves its timer armed
 *    after a failed read, which would keep an io context busy). abort() ends at once and may cut.
 *
 * What a close says (GPeerClose) travels as the websocket close code: NORMAL as 1000, ERROR as
 * 1011, RUN_ENDED as GCONSUMERCLOSECODERUNENDED (4000); onClosed reports the code received from
 * the other end the same way (an acknowledgement echoes this end's own code).
 *
 * Threads. The peer works on two executors. The stream lives on the socket's executor, which must
 * serialize the handlers it runs (a strand, or a context with one thread): reading, writing, the
 * keep-alive and the close happen there and nowhere else, the handshake included (start() posts
 * it there). The role's handlers run on a strand of the handler executor given to the constructor
 * (the socket's executor if none is given); executor() hands that strand out. The two are apart so
 * that the role may take as long as it likes over a message -- decoding it, encoding the answer --
 * without the connection looking idle: the peer goes on reading, and answering pings, while the role
 * works. Messages that arrive meanwhile wait for the role, in order; at
 * GWEBSOCKETMAXUNHANDLEDMESSAGES of them the peer stops reading until the role has caught up.
 * send() is posted to the socket's executor, in call order with close(). Liveness therefore needs
 * the socket's executor to have threads that do not run role code; with one executor for both it is
 * as live as the role is quick.
 *
 * Ownership. Pending operations own the peer, and the peer owns the role through its handlers; the
 * role refers to the peer only weakly (rule 4). A peer whose operations are discarded with their io
 * context is therefore destroyed, and its role with it. When the connection ends, the peer drops its
 * handlers right after it has called onClosed. The peer keeps the handler executor's context busy
 * until onClosed has run (or abort() has ended it), so that a role whose only work is this
 * connection does not see its io context run dry while the connection lasts. Whoever owns the two
 * contexts destroys the I/O context's pending handlers before the handler context (see GIoContext).
 */
class GWebsocketPeer final
  : public GPeerI
  , public std::enable_shared_from_this<GWebsocketPeer> {
public:
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

    /** @brief Takes over a connected TCP socket; nothing happens before start(). The role's
     *  handlers run on a strand of the handler executor, or on the socket's executor if it is
     *  empty. With a host name this is the client's end, which performs the websocket handshake
     *  with that name; without one it is the server's end, which waits for the client's. */
    GWebsocketPeer(
        boost::asio::ip::tcp::socket,
        GOptions,
        const boost::asio::any_io_executor & = {},
        std::string = {}
    );
    /** @brief The destructor */
    ~GWebsocketPeer() override = default;

    GWebsocketPeer(const GWebsocketPeer &) = delete;
    GWebsocketPeer(GWebsocketPeer &&) = delete;
    GWebsocketPeer &operator=(const GWebsocketPeer &) = delete;
    GWebsocketPeer &operator=(GWebsocketPeer &&) = delete;

    /** @brief Installs the handlers and begins the handshake (on the socket's executor). */
    void start(GPeerHandlers) override;
    /** @brief Queues a message; written after those handed over before. */
    void send(GMessage) override;
    /** @brief Closes in the regular way after what was handed to send() before. Any thread. */
    void close(GPeerClose) override;
    /** @brief Ends the connection at once, without a close handshake. For teardown (GPeerI::abort()):
     *  when neither the socket's executor nor the handler strand runs anything of this peer, e.g. on
     *  a client after its io context has stopped. */
    void abort() override;
    /** @brief The strand all handlers of this peer run on. Any thread. */
    [[nodiscard]] boost::asio::any_io_executor executor() override;

private:
    void configure_();
    void handshakeDone_(const boost::system::error_code &);
    void read_();
    void readDone_(const boost::system::error_code &);
    void deliver_(std::string &);
    void resumeReading_();
    void report_(GPeerEnd, GPeerClose, const std::string &);
    void pump_();
    void writeDone_(const boost::system::error_code &);
    void beginClose_(GPeerClose);
    void closeWhenSent_();
    void finish_(const boost::system::error_code &, bool = false);

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_; ///< The connection
    GOptions options_;
    std::string host_; ///< The server's name for the client's handshake; empty on the server's end
    GPeerHandlers handlers_; ///< Set by start(); on the handler strand; dropped once it is over

    boost::asio::any_io_executor io_executor_;    ///< The socket's executor (where the bytes move)
    boost::asio::any_io_executor handler_strand_; ///< Where the role's handlers run
    boost::asio::any_io_executor owner_work_;     ///< Keeps the handler context busy until the end
    /// Received messages not yet handled by the role (incremented on the socket's executor,
    /// decremented on the handler strand)
    std::atomic<std::size_t> unhandled_{0};
    bool read_paused_ = false; ///< Reading waits for the role to catch up (socket's executor)

    boost::beast::multi_buffer incoming_; ///< Receives the message being read
    std::deque<GMessage> write_queue_;    ///< Messages waiting to be written
    GMessage outgoing_;                   ///< The message being written (held until it is)
    bool writing_ = false;                ///< Whether a write is outstanding

    /// Bounds the wait for the write queue before a close, and nothing else (socket's executor)
    boost::asio::steady_timer close_timer_;
    GPeerClose close_how_ = GPeerClose::NORMAL; ///< What the close says (socket's executor)

    bool open_ = false; ///< The handshake has completed
    bool closing_ = false;    ///< A close has begun, or the connection is over: nothing is queued
    bool close_sent_ = false; ///< async_close has been issued
    bool local_close_ = false;                 ///< This end began the close
    bool close_received_ = false;              ///< The other end's close frame has arrived
    std::atomic<bool> close_requested_{false}; ///< close() was called (from any thread)
    bool finished_ = false;                    ///< The end has been decided

    /// When the last complete message or control frame of the other end arrived (or the handshake
    /// completed): what the log names when the idle limit has expired
    std::chrono::steady_clock::time_point last_inbound_;
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
