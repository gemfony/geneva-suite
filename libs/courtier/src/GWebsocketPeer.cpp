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

#include "courtier/transport/GWebsocketPeer.hpp"

// Standard headers go here
#include <tuple>
#include <utility>

// Boost headers go here
#include <boost/asio/buffer.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>
#include <boost/asio/strand.hpp>

// Geneva headers go here
#include "common/GLogger.hpp"

namespace Gem::Courtier::Consumers {

namespace {

using error_code = boost::system::error_code;
using boost::beast::websocket::stream_base;

/** @brief Beast's time limits for a ping interval: the idle limit and the limit on both handshakes
 *  are twice the interval, with keep-alive pings; an interval of zero leaves only the handshakes
 *  bounded. */
stream_base::timeout limitsFor(std::chrono::milliseconds ping_interval) {
    const bool keep_alive = ping_interval > std::chrono::milliseconds::zero();
    const stream_base::duration idle_limit = 2 * ping_interval;
    return stream_base::timeout{
        .handshake_timeout =
            keep_alive ? idle_limit : stream_base::duration{GWEBSOCKETHANDSHAKETIMEOUT},
        .idle_timeout = keep_alive ? idle_limit : stream_base::none(),
        .keep_alive_pings = keep_alive
    };
}

/** @brief The name of a control frame, for the log. */
const char *frameName(boost::beast::websocket::frame_type kind) {
    switch(kind) {
        using enum boost::beast::websocket::frame_type;
    case close:
        return "close";
    case ping:
        return "ping";
    case pong:
        return "pong";
    }
    return "control";
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * Takes over a connected TCP socket and configures the websocket stream on it.
 *
 * @param socket The connected socket; its executor must serialize the handlers it runs
 * @param options How this end is set up
 * @param handlers What the role wants to be told
 * @param handler_executor Where the role's handlers run (on a strand of it); empty: on the socket's
 *        executor
 */
GWebsocketPeer::GWebsocketPeer(
    boost::asio::ip::tcp::socket socket,
    GOptions options,
    GHandlers handlers,
    const boost::asio::any_io_executor &handler_executor
)
  : ws_(std::move(socket))
  , options_(std::move(options))
  , handlers_(std::move(handlers))
  , io_executor_(ws_.get_executor())
  , handler_strand_(
        handler_executor ? boost::asio::any_io_executor(boost::asio::make_strand(handler_executor))
                         : io_executor_
    )
  , owner_work_(
        boost::asio::prefer(handler_strand_, boost::asio::execution::outstanding_work_t::tracked)
    ) {
    this->configure_();
}

/******************************************************************************/
/**
 * Sets the stream options: fragmenting (so control frames are delivered in time), the write buffer,
 * Beast's time limits, no limit on the size of a message, the frame type, and what happens on a
 * control frame: its arrival is noted (for the log when the idle limit expires), and it is logged
 * if that was asked for. The callback runs inside the peer's read, on the peer's executor.
 */
void GWebsocketPeer::configure_() {
    ws_.auto_fragment(true);
    ws_.write_buffer_bytes(GWEBSOCKETWRITEBUFFERBYTES);
    ws_.set_option(limitsFor(options_.pingInterval));
    ws_.read_message_max(GWEBSOCKETMAXMESSAGEBYTES);
    ws_.binary(options_.mode == Gem::Common::serializationMode::GEM_BINARY);

    ws_.control_callback([this](
                             boost::beast::websocket::frame_type kind,
                             [[maybe_unused]] boost::beast::string_view payload
                         ) {
        last_inbound_ = std::chrono::steady_clock::now();
        if(kind == boost::beast::websocket::frame_type::close) {
            close_received_ = true;
        }
        if(options_.verboseControlFrames) {
            glogger << options_.name << " has received a " << frameName(kind) << " frame" << '\n'
                    << GLOGGING;
        }
    });
}

/******************************************************************************/
/**
 * Server side: waits for the client's websocket handshake. Nagle's algorithm is switched off first;
 * the messages are small and wait for one another.
 */
void GWebsocketPeer::acceptHandshake() {
    error_code ignored;
    std::ignore = ws_.next_layer().set_option(boost::asio::ip::tcp::no_delay(true), ignored);

    auto self = this->shared_from_this();
    ws_.async_accept([self](const error_code &ec) { self->handshakeDone_(ec); });
}

/******************************************************************************/
/**
 * Client side: performs the websocket handshake with the server.
 *
 * @param host The server's name, as sent in the handshake
 */
void GWebsocketPeer::connectHandshake(const std::string &host) {
    error_code ignored;
    std::ignore = ws_.next_layer().set_option(boost::asio::ip::tcp::no_delay(true), ignored);

    auto self = this->shared_from_this();
    ws_.async_handshake(host, "/", [self](const error_code &ec) { self->handshakeDone_(ec); });
}

/******************************************************************************/
/**
 * Called when either handshake has completed. On success the read that stays outstanding for the
 * rest of the connection is started, and the role is told that it may send.
 *
 * @param ec The outcome of the handshake
 */
void GWebsocketPeer::handshakeDone_(const error_code &ec) {
    if(ec) {
        this->finish_(ec);
        return;
    }
    open_ = true;
    last_inbound_ = std::chrono::steady_clock::now();
    this->read_();
    boost::asio::post(handler_strand_, [self = this->shared_from_this()] {
        if(self->handlers_.onOpen) {
            self->handlers_.onOpen();
        }
    });
}

/******************************************************************************/
/** Starts the next read. Exactly one read is outstanding while the connection is open. */
void GWebsocketPeer::read_() {
    auto self = this->shared_from_this();
    ws_.async_read(incoming_, [self](const error_code &ec, [[maybe_unused]] std::size_t n_bytes) {
        self->readDone_(ec);
    });
}

/******************************************************************************/
/**
 * Called when a read has completed: hands the message to the role's strand and reads on at once, so
 * that the keep-alive goes on while the role works on the message. Reading pauses only while the
 * role has GWEBSOCKETMAXUNHANDLEDMESSAGES messages still to handle. An error ends the connection;
 * this is also where an expired idle limit and the other end's close arrive. A message that arrives
 * after close() was called is not handed on: the role has said that it is done, and a client would
 * otherwise start an evaluation it can no longer return.
 *
 * @param ec The outcome of the read
 */
void GWebsocketPeer::readDone_(const error_code &ec) {
    if(ec) {
        this->finish_(ec);
        return;
    }

    last_inbound_ = std::chrono::steady_clock::now();
    std::string message = boost::beast::buffers_to_string(incoming_.data());
    incoming_.consume(incoming_.size());
    if(not close_requested_.load()) {
        if(unhandled_.fetch_add(1) + 1 >= GWEBSOCKETMAXUNHANDLEDMESSAGES) {
            read_paused_ = true;
        }
        boost::asio::post(
            handler_strand_,
            [self = this->shared_from_this(), message = std::move(message)]() mutable {
                self->deliver_(message);
            }
        );
    }

    if(not closing_ && not read_paused_) {
        this->read_();
    }
}

/******************************************************************************/
/**
 * On the handler strand: hands one message to the role (unless close() has been called since), and
 * lets reading go on if it waited for the role.
 *
 * @param message The message, as it arrived
 */
void GWebsocketPeer::deliver_(std::string &message) {
    if(handlers_.onMessage && not close_requested_.load()) {
        handlers_.onMessage(std::move(message));
    }
    if(unhandled_.fetch_sub(1) == GWEBSOCKETMAXUNHANDLEDMESSAGES) {
        boost::asio::post(io_executor_, [self = this->shared_from_this()] {
            self->resumeReading_();
        });
    }
}

/******************************************************************************/
/** On the socket's executor: reads on after a pause, if the connection is still open. */
void GWebsocketPeer::resumeReading_() {
    if(read_paused_ && not closing_) {
        read_paused_ = false;
        this->read_();
    }
}

/******************************************************************************/
/**
 * Queues a message on the socket's executor and starts writing if no write is outstanding. A message
 * handed over before the handshake has completed, or after a close has begun, is dropped.
 *
 * @param message The complete message to send
 */
void GWebsocketPeer::send(std::string message) {
    boost::asio::post(
        io_executor_,
        [self = this->shared_from_this(), message = std::move(message)]() mutable {
            if(self->closing_ || not self->open_) {
                return;
            }
            self->write_queue_.push_back(std::move(message));
            self->pump_();
        }
    );
}

/******************************************************************************/
/** Writes the next queued message, unless a write is outstanding or the connection is closing. */
void GWebsocketPeer::pump_() {
    if(writing_ || closing_ || write_queue_.empty()) {
        return;
    }
    writing_ = true;
    outgoing_ = std::move(write_queue_.front());
    write_queue_.pop_front();

    auto self = this->shared_from_this();
    ws_.async_write(
        boost::asio::buffer(outgoing_),
        [self](const error_code &ec, [[maybe_unused]] std::size_t n_bytes) { self->writeDone_(ec); }
    );
}

/******************************************************************************/
/**
 * Called when a write has completed: continues with the next queued message. An error ends the
 * connection.
 *
 * @param ec The outcome of the write
 */
void GWebsocketPeer::writeDone_(const error_code &ec) {
    writing_ = false;
    outgoing_.clear();
    if(ec) {
        this->finish_(ec);
        return;
    }
    this->pump_();
}

/******************************************************************************/
/**
 * Closes the connection in the regular way. The work is posted to the peer's executor, so this may
 * be called from any thread, and from inside one of the peer's handlers. From this call on no
 * message is delivered any more, also not one whose read has completed already.
 *
 * @param code The close code sent to the other end
 */
void GWebsocketPeer::close(close_code code) {
    close_requested_.store(true);
    auto self = this->shared_from_this();
    boost::asio::post(io_executor_, [self, code] { self->beginClose_(code); });
}

/******************************************************************************/
/**
 * Begins the close handshake. Beast sends the close frame as soon as the frame being written is
 * out -- a message of several frames ends there, incomplete -- and bounds the handshake by the
 * handshake limit; messages that still wait in the write queue are not sent. Without an open
 * websocket (the handshake never completed, or the connection has failed or timed out) there is
 * nothing to shake hands about, and the connection is ended at once.
 *
 * @param code The close code sent to the other end
 */
void GWebsocketPeer::beginClose_(close_code code) {
    if(closing_) {
        return;
    }
    closing_ = true;
    local_close_ = true;

    if(not ws_.is_open()) {
        this->finish_({});
        return;
    }

    auto self = this->shared_from_this();
    ws_.async_close(code, [self]([[maybe_unused]] const error_code &ec) { self->finish_({}); });
}

/******************************************************************************/
/**
 * Ends the connection at once, without a close handshake. For a caller on the peer's executor, or
 * one whose io context no longer runs -- but not from inside one of the peer's handlers: the
 * handlers are dropped here, and with them possibly the object the calling handler belongs to.
 * A handler that wants the connection gone calls close().
 */
void GWebsocketPeer::abort() {
    closing_ = true;
    local_close_ = true;
    this->finish_({}, /*report_inline=*/true);
}

/******************************************************************************/
/** @return The strand all handlers of this peer run on (to post work that came from elsewhere) */
boost::asio::any_io_executor GWebsocketPeer::executor() {
    return handler_strand_;
}

/******************************************************************************/
/**
 * The end of the connection, whatever led to it. Runs its body once: switches Beast's time limits
 * off (Beast leaves its timer armed after a failed read, which would keep the io context busy),
 * closes the socket, and tells the role on its strand -- after the messages handed to it before,
 * which it may still handle. abort() has the role told at once instead: no handler runs then.
 *
 * @param cause Why the connection ended; reported as "no error" if this end began the close
 * @param report_inline Whether the role is told here and now (abort()) instead of on its strand
 */
void GWebsocketPeer::finish_(const error_code &cause, bool report_inline) {
    if(finished_) {
        if(report_inline) {
            this->report_({}); // a report posted before may never run: its context has stopped
        }
        return;
    }
    finished_ = true;
    closing_ = true;

    if(cause == boost::beast::error::timeout && open_ && not close_received_) {
        // Which end gave up, and after how much silence: the other end is either gone, or it could
        // not answer a ping in time because its executor was busy (with a large message, say).
        const auto silent_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_inbound_
        );
        glogger << options_.name << ": the idle limit of " << (2 * options_.pingInterval).count()
                << " ms has expired; the last complete message or control frame of the other end"
                << " arrived " << silent_ms.count() << " ms ago" << '\n'
                << GLOGGING;
    }

    ws_.set_option(
        stream_base::timeout{
            .handshake_timeout = stream_base::none(),
            .idle_timeout = stream_base::none(),
            .keep_alive_pings = false
        }
    );

    if(ws_.next_layer().is_open()) {
        error_code ignored; // the other end may be gone already
        std::ignore =
            ws_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        std::ignore = ws_.next_layer().close(ignored);
    }

    // The other end's close is its close, also if the end of the TCP connection then failed or
    // timed out: it did say that it closes.
    const error_code reported = local_close_ ? error_code{}
                                : close_received_
                                    ? error_code{boost::beast::websocket::error::closed}
                                    : cause;
    if(report_inline) {
        this->report_(reported);
    }
    else {
        boost::asio::post(handler_strand_, [self = this->shared_from_this(), reported] {
            self->report_(reported);
        });
    }
}

/******************************************************************************/
/**
 * On the handler strand (or, from abort(), where no handler runs): tells the role that the
 * connection is over, once, drops the handlers and with them this peer's hold on the role, and
 * lets go of the handler context.
 *
 * @param cause What onClosed is told
 */
void GWebsocketPeer::report_(const error_code &cause) {
    GHandlers handlers = std::move(handlers_);
    handlers_ = GHandlers{};
    if(handlers.onClosed) {
        handlers.onClosed(cause);
    }
    owner_work_ = {};
}

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
