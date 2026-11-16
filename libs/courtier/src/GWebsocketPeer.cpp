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
#include <cstdint>
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
using boost::beast::websocket::close_code;
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

/** @brief The websocket close code with which one end begins a close. The two ends of a
 *  connection never begin a close with the same code (see GCONSUMERCLOSECODESERVERNORMAL): an
 *  answer to a close echoes its code, so a close read after this end's own is an answer exactly
 *  when it carries this end's code.
 *  @param how What the close says @param server_end Whether it is the server's end that closes
 *  @return The code that says it */
close_code codeFor(GPeerClose how, bool server_end) {
    switch(how) {
    case GPeerClose::NORMAL:
        return server_end ? static_cast<close_code>(GCONSUMERCLOSECODESERVERNORMAL)
                          : close_code::normal;
    case GPeerClose::ERROR:
        return server_end ? static_cast<close_code>(GCONSUMERCLOSECODESERVERERROR)
                          : close_code::internal_error;
    case GPeerClose::RUN_ENDED:
        return static_cast<close_code>(
            server_end ? GCONSUMERCLOSECODERUNENDED : GCONSUMERCLOSECODECLIENTRUNENDED
        );
    }
    return close_code::normal;
}

/** @brief @param code A received websocket close code @return What it says, whichever end began
 *  the close; no code at all, or a regular one, says NORMAL, a code of a failure (Beast's own
 *  closes among them) ERROR */
GPeerClose closeFor(std::uint16_t code) {
    if(code == GCONSUMERCLOSECODERUNENDED || code == GCONSUMERCLOSECODECLIENTRUNENDED) {
        return GPeerClose::RUN_ENDED;
    }
    if(code == close_code::normal || code == close_code::none ||
       code == GCONSUMERCLOSECODESERVERNORMAL) {
        return GPeerClose::NORMAL;
    }
    return GPeerClose::ERROR;
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * Takes over a connected TCP socket and configures the websocket stream on it.
 *
 * @param socket The connected socket; its executor must serialize the handlers it runs
 * @param options How this end is set up
 * @param handler_executor Where the role's handlers run (on a strand of it); empty: on the socket's
 *        executor
 * @param host The server's name for the websocket handshake on the client's end; empty on the
 *        server's end
 */
GWebsocketPeer::GWebsocketPeer(
    boost::asio::ip::tcp::socket socket,
    GOptions options,
    const boost::asio::any_io_executor &handler_executor,
    std::string host
)
  : ws_(std::move(socket))
  , options_(std::move(options))
  , host_(std::move(host))
  , io_executor_(ws_.get_executor())
  , handler_strand_(
        handler_executor ? boost::asio::any_io_executor(boost::asio::make_strand(handler_executor))
                         : io_executor_
    )
  , owner_work_(
        boost::asio::prefer(handler_strand_, boost::asio::execution::outstanding_work_t::tracked)
    )
  , close_timer_(io_executor_) {
    this->configure_();
}

/******************************************************************************/
/**
 * Sets the stream options: fragmenting (so control frames are delivered in time), the write buffer,
 * Beast's time limits, no limit on the size of a message, the frame type, and what happens on a
 * control frame: its arrival is noted (for the log when the idle limit expires), and it is logged
 * if that was asked for. The callback runs inside the peer's read, on the socket's executor.
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
            // After ours, and an answer to it: it confirms it (GPeerI.hpp, rule 6)
            close_confirmed_ = close_sent_ && this->answersOurClose_();
        }
        if(options_.verboseControlFrames) {
            glogger << options_.name << " has received a " << frameName(kind) << " frame" << '\n'
                    << GLOGGING;
        }
    });
}

/******************************************************************************/
/**
 * Installs the handlers and begins this end's websocket handshake on the socket's executor (the
 * server's end waits for the client's, the client's end performs it). Nagle's algorithm is
 * switched off first; the messages are small and wait for one another. A close() after this call
 * is ordered after the handshake's beginning (both are posted to the socket's executor).
 *
 * @param handlers What the role wants to be told
 */
void GWebsocketPeer::start(GPeerHandlers handlers) {
    handlers_ = std::move(handlers);
    boost::asio::post(io_executor_, [self = this->shared_from_this()] {
        if(self->finished_) {
            return;
        }
        error_code ignored;
        std::ignore =
            self->ws_.next_layer().set_option(boost::asio::ip::tcp::no_delay(true), ignored);
        if(self->host_.empty()) {
            self->ws_.async_accept([self](const error_code &ec) { self->handshakeDone_(ec); });
        }
        else {
            self->ws_.async_handshake(self->host_, "/", [self](const error_code &ec) {
                self->handshakeDone_(ec);
            });
        }
    });
}

/******************************************************************************/
/**
 * Called when either handshake has completed. On success the role is told that it may send, and
 * then the read that stays outstanding for the rest of the connection is started -- unless the role
 * has asked for the close meanwhile: then neither onOpen nor any message is delivered (rule 1), no
 * read is started, and the close goes out first (Beast's close operation reads what follows; rule
 * 6).
 *
 * @param ec The outcome of the handshake
 */
void GWebsocketPeer::handshakeDone_(const error_code &ec) {
    if(finished_) {
        return;
    }
    if(ec) {
        this->finish_(ec);
        return;
    }
    open_ = true;
    last_inbound_ = std::chrono::steady_clock::now();
    if(closing_) {
        // A close requested while the handshake was under way: the role is told nothing (rule 1),
        // and the close goes out before anything is read (rule 6); Beast's close reads from here
        this->closeWhenSent_();
        return;
    }
    // Until the read is started, nothing of this peer is pending on the socket's executor: it is
    // kept busy meanwhile, so that its context does not run dry in between
    auto io_work =
        boost::asio::prefer(io_executor_, boost::asio::execution::outstanding_work_t::tracked);
    boost::asio::post(handler_strand_, [self = this->shared_from_this(), io_work]() mutable {
        // A role that asked for the close before it was told of the open is told nothing (rule 1:
        // onOpen precedes any onMessage), and nothing is read before this end's close (which
        // close() has posted) has gone out: an answer to the other end's close would otherwise
        // prove what no role has received (rule 6). Reading starts once the role knows.
        if(self->close_requested_.load()) {
            return;
        }
        self->role_open_ = true;
        if(self->handlers_.onOpen) {
            self->handlers_.onOpen();
        }
        boost::asio::post(self->io_executor_, [self, io_work = std::move(io_work)] {
            if(not self->finished_ && not self->close_sent_) {
                self->read_();
            }
        });
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
 * role has GWEBSOCKETMAXUNHANDLEDMESSAGES messages still to handle, and ends once the close frame
 * has been sent (Beast reads the answer to it itself). An error ends the connection; this is also
 * where an expired idle limit and the other end's close arrive. A message that arrives after
 * close() was called is handed on as well (GPeerI.hpp, rule 6): the other end sent it before it
 * knew of the close, and a close that it answers proves to it that its role has been handed what
 * it sent before. The role decides what a late message means.
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
    if(unhandled_.fetch_add(1) + 1 >= GWEBSOCKETMAXUNHANDLEDMESSAGES) {
        read_paused_ = true;
    }
    boost::asio::post(
        handler_strand_,
        [self = this->shared_from_this(), message = std::move(message)]() mutable {
            self->deliver_(message);
        }
    );

    if(not finished_ && not close_sent_ && not read_paused_) {
        this->read_();
    }
}

/******************************************************************************/
/**
 * On the handler strand: hands one message to the role (also after close(), see readDone_(); but
 * not to a role that was not told of the open), and lets reading go on if it waited for the role.
 *
 * @param message The message, as it arrived
 */
void GWebsocketPeer::deliver_(std::string &message) {
    if(handlers_.onMessage && role_open_) {
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
    if(read_paused_ && not finished_ && not close_sent_) {
        read_paused_ = false;
        this->read_();
    }
}

/******************************************************************************/
/**
 * Queues a message on the socket's executor and starts writing if no write is outstanding. A message
 * handed over after close() is dropped; one handed over before the handshake has completed is
 * dropped too.
 *
 * @param message The complete message to send
 */
void GWebsocketPeer::send(GMessage message) {
    if(close_requested_.load()) {
        return;
    }
    boost::asio::post(
        io_executor_,
        [self = this->shared_from_this(), message = std::move(message)]() mutable {
            if(self->closing_ || not self->open_ || not message) {
                return;
            }
            self->write_queue_.push_back(std::move(message));
            self->pump_();
        }
    );
}

/******************************************************************************/
/** Writes the next queued message, unless a write is outstanding or the connection is over. When
 *  the queue has run empty during a close, the close frame goes out. */
void GWebsocketPeer::pump_() {
    if(writing_ || finished_ || close_sent_) {
        return;
    }
    if(write_queue_.empty()) {
        if(closing_) {
            this->closeWhenSent_();
        }
        return;
    }
    writing_ = true;
    outgoing_ = std::move(write_queue_.front());
    write_queue_.pop_front();

    auto self = this->shared_from_this();
    ws_.async_write(
        boost::asio::buffer(*outgoing_),
        [self](const error_code &ec, [[maybe_unused]] std::size_t n_bytes) { self->writeDone_(ec); }
    );
}

/******************************************************************************/
/**
 * Called when a write has completed: continues with the next queued message (or the close frame).
 * An error ends the connection.
 *
 * @param ec The outcome of the write
 */
void GWebsocketPeer::writeDone_(const error_code &ec) {
    writing_ = false;
    outgoing_.reset();
    if(ec) {
        this->finish_(ec);
        return;
    }
    this->pump_();
}

/******************************************************************************/
/**
 * Closes the connection in the regular way. The work is posted to the socket's executor after the
 * messages handed to send() before, so this may be called from any thread, and from inside one of
 * the peer's handlers. From this call on no message is accepted any more; what the peer reads until
 * its close frame has gone out is still delivered (readDone_()).
 *
 * @param how What the other end is told
 */
void GWebsocketPeer::close(GPeerClose how) {
    if(close_requested_.exchange(true)) {
        return;
    }
    boost::asio::post(io_executor_, [self = this->shared_from_this(), how] {
        self->beginClose_(how);
    });
}

/******************************************************************************/
/**
 * Begins the close: nothing is queued any more, and the close frame goes out once what was queued
 * before has been written (closeWhenSent_()). The wait is bounded by the handshake limit: a peer
 * whose other end no longer reads is ended then. Without a connection to close (the handshake
 * failed, or the connection is over) the peer is ended at once. During the handshake the server's
 * end waits for it, so that the client still hears what the close says (the end of the run); the
 * client's end ends at once and reports FAILED (it never opened): nothing has been said on the
 * connection yet, and a client that halts while its server does not answer does not wait for the
 * handshake limit.
 *
 * @param how What the other end is told
 */
void GWebsocketPeer::beginClose_(GPeerClose how) {
    if(closing_ || finished_) {
        return;
    }
    closing_ = true;
    local_close_ = true;
    close_how_ = how;

    if(not open_ && not ws_.next_layer().is_open()) {
        this->finish_({});
        return;
    }
    if(not open_ && not host_.empty()) {
        this->finish_(boost::asio::error::operation_aborted); // the client's end, see above
        return;
    }

    close_timer_.expires_after(limitsFor(options_.pingInterval).handshake_timeout);
    close_timer_.async_wait([self = this->shared_from_this()](const error_code &ec) {
        if(not ec && not self->close_sent_) {
            // Open: what was queued could not be written in time. Not open: the handshake has not
            // completed within its limit -- a handshake that ran out of time, as Beast reports it.
            self->finish_(self->open_ ? error_code{} : error_code{boost::beast::error::timeout});
        }
    });

    if(open_) {
        this->pump_();
    }
}

/******************************************************************************/
/** Sends the close frame: the write queue is empty and no write is outstanding. Beast bounds the
 *  close handshake by the handshake limit. From here on Beast reads the connection itself, and
 *  discards the messages that arrive before the other end's close frame: the peer cannot hand
 *  them on. A close handshake that completes without an error has read the other end's close
 *  frame after ours (Beast reads until it arrives); the close is confirmed if that frame answers
 *  ours (answersOurClose_()) -- the other end then read our close frame before it sent its own,
 *  and so had not sent anything that Beast could discard here. The other end's own close, crossing
 *  ours, does not confirm it, nor does a close handshake that ends with an error (the time limit, a
 *  connection that broke), unless the peer's own read saw the answer. */
void GWebsocketPeer::closeWhenSent_() {
    if(close_sent_ || finished_ || writing_ || not write_queue_.empty()) {
        return;
    }
    close_sent_ = true;
    close_timer_.cancel();
    auto self = this->shared_from_this();
    ws_.async_close(codeFor(close_how_, host_.empty()), [self](const error_code &ec) {
        // Beast's close operation reads the other end's close frame without the control callback:
        // a close handshake that completed has read it, and one that ended with an error may have
        // (its code is then known). What it said is reported either way (finish_()).
        if(not ec || self->ws_.reason().code != close_code::none) {
            self->close_received_ = true;
        }
        if(not ec && self->answersOurClose_()) {
            self->close_confirmed_ = true;
        }
        self->close_error_ = ec;
        self->finish_({});
    });
}

/******************************************************************************/
/** @brief Whether the other end's close frame, as Beast has read it, answers this end's close: it
 *  carries the code this end's close was sent with, which the other end's own closes never carry
 *  (codeFor()). On the socket's executor, once a close frame has been read.
 *  @return true if it is an answer */
bool GWebsocketPeer::answersOurClose_() const {
    return ws_.reason().code == codeFor(close_how_, host_.empty());
}

/******************************************************************************/
/**
 * Ends the connection at once, without a close handshake. For a caller on the socket's executor, or
 * one whose io context no longer runs -- but not from inside one of the peer's handlers: the
 * handlers are dropped here, and with them possibly the object the calling handler belongs to.
 * A handler that wants the connection gone calls close().
 */
void GWebsocketPeer::abort() {
    abort_unclosed_ = not close_requested_.exchange(true);
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
 * Why it ended: if this end began the close (and the connection had opened, or nothing failed),
 * CLOSED_HERE when the other end confirmed it -- its close frame arrived after ours and answers it
 * (GPeerI.hpp, rule 6) -- and CLOSED_HERE_UNCONFIRMED when it did not: what was queued could not be
 * written within the handshake limit, the close handshake ended with an error or ran out of time,
 * the other end's close came before ours had gone out, or it was the other end's own close and
 * crossed ours (abort() of a peer that had not been closed reports CLOSED_HERE, see
 * GPeerI::abort()); CLOSED_THERE if the other end's close frame arrived, whatever became of the
 * TCP connection after it; SILENT if a time limit expired on a connection that was open; FAILED
 * for any other error -- a handshake that failed or ran out of time among them, also when a close had been
 * asked for while it was under way (rule 1: a peer that cannot be opened reports FAILED).
 *
 * @param cause The error that ended it; none if this end began the close
 * @param report_inline Whether the role is told here and now (abort()) instead of on its strand
 */
void GWebsocketPeer::finish_(const error_code &cause, bool report_inline) {
    if(finished_) {
        if(report_inline) {
            // a report posted before may never run: its context has stopped. It says what was
            // found then (report_() tells the role once).
            this->report_(end_, end_how_, end_detail_);
        }
        return;
    }
    finished_ = true;
    closing_ = true;
    close_timer_.cancel();
    write_queue_.clear();

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
    GPeerEnd end = GPeerEnd::FAILED;
    if(local_close_ && (open_ || not cause)) {
        end = (close_confirmed_ || abort_unclosed_) ? GPeerEnd::CLOSED_HERE
                                                    : GPeerEnd::CLOSED_HERE_UNCONFIRMED;
    }
    else if(close_received_) {
        end = GPeerEnd::CLOSED_THERE;
    }
    else if(cause == boost::beast::error::timeout && open_) {
        end = GPeerEnd::SILENT; // a handshake that ran out of time stays FAILED
    }
    const GPeerClose how = close_received_ ? closeFor(ws_.reason().code) : GPeerClose::NORMAL;
    std::string detail = cause.message();
    if(end == GPeerEnd::CLOSED_HERE) {
        detail = "closed by this end";
    }
    else if(end == GPeerEnd::CLOSED_HERE_UNCONFIRMED) {
        detail = "closed by this end; the other end did not confirm the close";
        if(report_inline) {
            detail += " (aborted)";
        }
        else if(close_error_) {
            detail += " (" + close_error_.message() + ")";
        }
        else if(not close_sent_ && open_) {
            detail += close_received_ ? " (its own close came first)"
                                      : " (what was queued could not be written in time)";
        }
        else if(close_received_) {
            detail += " (its own close crossed ours)";
        }
    }
    else if(end == GPeerEnd::CLOSED_THERE) {
        detail = "closed by the other end";
    }
    end_ = end;
    end_how_ = how;
    end_detail_ = detail;
    if(report_inline) {
        this->report_(end, how, detail);
    }
    else {
        boost::asio::post(
            handler_strand_,
            [self = this->shared_from_this(), end, how, detail = std::move(detail)] {
                self->report_(end, how, detail);
            }
        );
    }
}

/******************************************************************************/
/**
 * On the handler strand (or, from abort(), where no handler runs): tells the role that the
 * connection is over, once, drops the handlers and with them this peer's hold on the role, and
 * lets go of the handler context.
 *
 * @param end Why the connection ended
 * @param how What the other end's close said
 * @param detail Words about it, for the log
 */
void GWebsocketPeer::report_(GPeerEnd end, GPeerClose how, const std::string &detail) {
    GPeerHandlers handlers = std::move(handlers_);
    handlers_ = GPeerHandlers{};
    if(handlers.onClosed) {
        handlers.onClosed(end, how, detail);
    }
    owner_work_ = {};
}

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
