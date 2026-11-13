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
#include <cstddef>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <tuple>

// Boost headers go here
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/rfc6455.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"
#include "common/concurrency/GThreadPool.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GCourtierHelperFunctions.hpp"
#include "courtier/GServerSessionLogic.hpp"       // shared synchronous server dispatch (PULL/RETURN/...)
#include "courtier/GWireCodec.hpp"                // shared scope-wrapped (de)serialization
#include "courtier/GWireSerializationContext.hpp"
#include "courtier/transport/GPrefetchingClientT.hpp" // blob send-once: wire (de)serialization scope

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * @brief Applies the time limits and the message-size limit that both ends of a courtier websocket
 * connection share.
 *
 * Beast watches the connection itself: after @p ping_interval without any inbound data it sends a
 * ping, and after a second such interval it closes the socket and ends the outstanding read with an
 * error. Any received frame counts, control frames included, so a peer that only answers pings is
 * alive. The websocket handshake and the close handshake get the same limit as the idle time. All
 * of this needs a read to be outstanding, which both ends keep.
 *
 * @tparam WsStream The websocket stream type
 * @param ws The stream to configure
 * @param ping_interval The silence after which a ping is sent; zero switches the idle limit off
 */
template <typename WsStream>
void applyWebsocketLimits(WsStream &ws, std::chrono::seconds ping_interval) {
    using boost::beast::websocket::stream_base;
    const bool keep_alive = ping_interval > std::chrono::seconds::zero();
    const stream_base::duration idle_limit = 2 * ping_interval;
    const stream_base::duration handshake_limit =
        keep_alive ? idle_limit : stream_base::duration{Gem::Courtier::GWEBSOCKETHANDSHAKETIMEOUT};
    ws.set_option(
        stream_base::timeout{
            .handshake_timeout = handshake_limit,
            .idle_timeout = keep_alive ? idle_limit : stream_base::none(),
            .keep_alive_pings = keep_alive
        }
    );
    ws.read_message_max(Gem::Courtier::GWEBSOCKETMAXMESSAGEBYTES);
}

/******************************************************************************/
/**
 * @brief Switches the time limits of applyWebsocketLimits() off again, for a connection that is
 * over.
 *
 * Beast cancels its internal timer after an orderly close, but not after a read or write that
 * failed (the peer vanished). The armed timer is pending work on the io context: a client's
 * io_context::run() would not return until it fires, up to one ping interval later. Switching the
 * limits off cancels the timer.
 *
 * @tparam WsStream The websocket stream type
 * @param ws The stream whose connection has ended
 */
template <typename WsStream>
void disarmWebsocketLimits(WsStream &ws) {
    using boost::beast::websocket::stream_base;
    ws.set_option(
        stream_base::timeout{
            .handshake_timeout = stream_base::none(),
            .idle_timeout = stream_base::none(),
            .keep_alive_pings = false
        }
    );
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class is responsible for the client side of network communication with Boost::Beast.
 * Connections are kept open permanently.
 *
 * The client can keep up to prefetch_depth_ work items in flight at once (requested-but-unanswered +
 * currently computing), overlapping network transfer with computation. A depth of 1 is the classic
 * strictly-serial behaviour (one item at a time). It keeps a single async_read outstanding throughout
 * so Beast can service pings -> auto-pong even while items evaluate on the compute pool (an
 * evaluation may therefore take any length of time), and so that Beast's idle limit can notice a
 * server that has fallen silent (see applyWebsocketLimits()). It serializes
 * its outgoing messages through a write queue (Beast permits only one write at a time). All connection
 * state (the in-flight counters, the write queue, the websocket stream) is touched only on the single
 * io thread; the compute pool threads only ever touch the work-item container handed to them and then
 * post the result back onto the io thread.
 *
 * The wire protocol is unchanged: PULL and RETURN are both "pulls" that the server answers with one
 * WORK/NO_WORK, so the server (and its sessions) need no knowledge of the client's prefetch depth.
 * The client treats a late result as an ordinary RETURN -- whether it still matters is entirely the
 * server's concern (it reconciles by correlation id and silently drops results for finished batches).
 *
 * @tparam processable_type The work-item type exchanged with the server (must be processable)
 */
template <typename processable_type>
class GWebsocketClientT final
  : public GPrefetchingClientT<GWebsocketClientT<processable_type>, processable_type>
  , public std::enable_shared_from_this<GWebsocketClientT<processable_type>> {
    // The pipeline base drives the compute dispatch / refill / halt machinery and calls our
    // private transport hooks (refill_ / sendResultAndRefill_ / haltShutdown_).
    friend class GPrefetchingClientT<GWebsocketClientT<processable_type>, processable_type>;

    using pipeline_base = GPrefetchingClientT<GWebsocketClientT<processable_type>, processable_type>;

    //-------------------------------------------------------------------------
    // Make the code easier to read

    using error_code = boost::system::error_code;
    using resolver = boost::asio::ip::tcp::resolver;
    using socket = boost::asio::ip::tcp::socket;
    using close_code = boost::beast::websocket::close_code;
    using frame_type = boost::beast::websocket::frame_type;
    using string_view = boost::beast::string_view;

    // Members and helpers of the dependent pipeline base, pulled into scope
    using pipeline_base::command_container_;
    using pipeline_base::computing_;
    using pipeline_base::dispatch_compute_;
    using pipeline_base::io_context_;
    using pipeline_base::n_nodata_;
    using pipeline_base::pending_pulls_;
    using pipeline_base::prefetch_depth_;
    using pipeline_base::schedule_refill_;
    using pipeline_base::start_halt_timer;
    using pipeline_base::wire_ctx_;
    using pipeline_base::wire_registry_;

public:
    //-------------------------------------------------------------------------
    /**
	  * @brief Initialization with host/ip and port.
	  *
	  * @param address The IP address or host name of the server to connect to
	  * @param port The TCP port the server is listening on
	  * @param serialization_mode Which serialization format (binary/XML/text) the wire protocol uses
	  * @param verbose_control_frames If true, a diagnostic message is logged for every control frame received
	  * @param prefetch_depth Maximum number of work items kept in flight at once (a value of 0 is treated as 1)
	  * @param ping_interval Seconds of silence after which a ping is sent; after twice that time
	  *        without any inbound data the server is presumed gone and the client terminates (0
	  *        switches this off)
	  */
    GWebsocketClientT(
        std::string address,
        unsigned short port,
        Gem::Common::serializationMode serialization_mode,
        bool verbose_control_frames,
        std::size_t prefetch_depth = 1,
        std::size_t ping_interval = Gem::Courtier::GBEASTCONSUMERPINGINTERVAL
    )
      : pipeline_base("GWebsocketClientT<processable_type>", prefetch_depth)
      , resolver_(io_context_)
      , ws_(io_context_)
      , address_(std::move(address))
      , port_(port)
      , serialization_mode_(serialization_mode)
      , verbose_control_frames_(verbose_control_frames) {
        // Set the auto_fragment option, so control frames are delivered timely
        ws_.auto_fragment(true);
        ws_.write_buffer_bytes(16384);
        applyWebsocketLimits(ws_, std::chrono::seconds(ping_interval));

        // Set the transfer mode
        switch(serialization_mode_) {
        case Gem::Common::serializationMode::GEM_BINARY: // GArchive binary -> binary websocket frames
            ws_.binary(true);
            break;
        case Gem::Common::serializationMode::GEM_JSON: // GArchive JSON -> text websocket frames
            ws_.binary(false);
            break;
        }

        // Set a control-frame callback
        f_when_control_frame_arrived_ = [this](frame_type frame_t, [[maybe_unused]] string_view s) {
            // Let the audience know what type of control frame we have received
            // if the user has requested it.
            if(this->verbose_control_frames_) {
                if(boost::beast::websocket::frame_type::close == frame_t) {
                    glogger << "GWebsocketClientT<> client has received a close frame" << '\n'
                            << GLOGGING;
                }
                else if(boost::beast::websocket::frame_type::ping == frame_t) {
                    glogger << "GWebsocketClientT<> client has received a ping frame" << '\n'
                            << GLOGGING;
                }
                else if(boost::beast::websocket::frame_type::pong == frame_t) {
                    glogger << "GWebsocketClientT<> client has received a pong frame" << '\n'
                            << GLOGGING;
                }
            }
        };

        // Set the callback to be executed on every incoming control frame.
        ws_.control_callback(f_when_control_frame_arrived_);

        // Engage the blob send-once wire form. The client caches every blob it receives,
        // keyed by content id, so an id-only work item resolves locally. No fetch_blob is installed: on
        // websocket the server sends the full blob inline on the first item of each (re)connection and
        // delivery is ordered, so an id-only item is only ever seen after its blob has been received
        // and cached -- a miss would indicate a protocol error, which load() surfaces by throwing.
        wire_ctx_.enabled = true;
        wire_ctx_.peer = 0; // the single upstream server
        wire_ctx_.registry = &wire_registry_;
        wire_ctx_.mode = serialization_mode_;
        // A worker never interns: it does not own the registry the server resolves against, so a
        // blob it echoes back must always travel inline (see may_intern_blobs).
        wire_ctx_.may_intern_blobs = false;
    }

    //-------------------------------------------------------------------------
    // Deleted functions

    // Deleted default-constructor -- enforce usage of a particular constructor
    GWebsocketClientT() = delete;
    // Deleted copy-constructors and assignment operators -- the client is non-copyable
    GWebsocketClientT(const GWebsocketClientT<processable_type> &) = delete;
    GWebsocketClientT(GWebsocketClientT<processable_type> &&) = delete;
    GWebsocketClientT<processable_type> &
    operator=(const GWebsocketClientT<processable_type> &) = delete;
    GWebsocketClientT<processable_type> &operator=(GWebsocketClientT<processable_type> &&) = delete;

private:
    //-------------------------------------------------------------------------
    /**
	  * @brief Starts the main run-loop: resolves the server, runs the io_context until no work
	  * remains, then makes sure the socket is closed.
	  */
    void run_() override {
        // Start looking up the domain name. This call will return immediately,
        // when_resolved() will be called once the operation is complete.
        auto self = this->shared_from_this();
        resolver_.async_resolve(
            address_,
            std::to_string(port_),
            [self](boost::system::error_code ec, const resolver::results_type &results) {
                self->when_resolved(ec, results);
            }
        );

        // This call will block until no more work remains in the ASIO work queue
        io_context_.run();

        // The io context has stopped, so no close handshake can run any more: just make sure the
        // socket is closed (it already is after an orderly do_close()).
        close_socket_();

        // Let the audience know that we have finished the shutdown
        glogger << "GWebsocketClientT<processable_type>::run_(): Client session has terminated"
                << '\n'
                << GLOGGING;
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Queues a message for sending and pumps the write queue. Beast permits only one
	  * outstanding write, so queued messages are drained one after another.
	  *
	  * @param message The serialized message to be transferred to the peer
	  */
    void async_start_write(std::string message) {
        // Do nothing if we have been asked to stop, or once the close has begun
        if(this->halt() || closing_) {
            return;
        }

        // Beast permits only ONE write to be outstanding at a time, but a prefetching client may want
        // to send several messages close together (a RETURN from a just-finished evaluation plus a
        // PULL top-up). Queue them and let the pump send them one after another.
        write_queue_.push_back(std::move(message));
        pump_write();
    }

    //-------------------------------------------------------------------------
    /** @brief Sends the next queued message if no write is currently outstanding. Runs on the io
		 *  thread only, so the single-writer invariant Beast requires is upheld. */
    void pump_write() {
        if(writing_ || write_queue_.empty() || this->halt() || closing_) {
            return;
        }
        writing_ = true;
        // Persist the message for the duration of the async write (avoids copying the payload).
        outgoing_message_ = std::move(write_queue_.front());
        write_queue_.pop_front();

        auto self = this->shared_from_this();
        ws_.async_write(
            boost::asio::buffer(outgoing_message_),
            [self](boost::system::error_code ec, std::size_t nBytesTransferred) {
                self->when_written(ec, nBytesTransferred);
            }
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Starts a new asynchronous read session, leaving one read outstanding on the io thread.
	  */
    void async_start_read() {
        // Do nothing if we have been asked to stop
        if(this->halt()) {
            return;
        }

        auto self = this->shared_from_this();
        ws_.async_read(
            incoming_buffer_,
            [self](boost::system::error_code ec, std::size_t nBytesTransferred) {
                self->when_read(ec, nBytesTransferred);
            }
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when the async_resolve-operation has completed; on success it
	  * initiates the TCP connect to a resolved endpoint.
	  *
	  * @param ec Indicates a possible error during name resolution
	  * @param results The resolved endpoints of the resolve-operation
	  */
    void when_resolved(boost::system::error_code ec, const resolver::results_type &results) {
        if(ec) {
            glogger << "In GWebsocketClientT<processable_type>::when_resolved():" << '\n'
                    << "Got ec(\"" << ec.message() << "\"). async_connect() will not be executed."
                    << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;

            // Give the audience a hint why we are terminating
            close_code_ = boost::beast::websocket::close_code::going_away;

            return;
        }

        // Make the ASIO connection on the endpoint we get from a lookup
        auto self = this->shared_from_this();
        boost::asio::async_connect(
            ws_.next_layer(),
            results.begin(),
            results.end(),
            [self](boost::system::error_code ec, [[maybe_unused]] const auto& unused) { self->when_connected(ec); }
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when the async_connect call has completed; on success it disables
	  * Nagle's algorithm and starts the websocket handshake.
	  *
	  * @param ec Indicates a possible error during the TCP connect
	  */
    void when_connected(boost::system::error_code ec) {
        if(ec) {
            glogger << "In GWebsocketClientT<processable_type>::when_connected():" << '\n'
                    << "Got ec(\"" << ec.message() << "\"). async_handshake() will not be executed."
                    << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;

            // Give the audience a hint why we are terminating
            close_code_ = boost::beast::websocket::close_code::going_away;

            return;
        }

        // Disable Nagle's algorithm on the underlying TCP socket: the request/response messages
        // are small and latency-sensitive.
        boost::system::error_code nd_ec;
        std::ignore = ws_.next_layer().set_option(boost::asio::ip::tcp::no_delay(true), nd_ec); // failure deliberately tolerated

        // Perform the handshake
        auto self = this->shared_from_this();
        ws_.async_handshake(address_, "/", [self](boost::system::error_code ec) {
            self->when_handshake_complete(ec);
        });
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when the async_handshake-operation has completed; on success it
	  * arms the outstanding read, starts the halt timer and primes the prefetch pipeline.
	  *
	  * @param ec Indicates a possible error during the websocket handshake
	  */

    void when_handshake_complete(boost::system::error_code ec) {
        if(ec) {
            glogger << "In GWebsocketClientT<processable_type>::when_handshake_complete():"
                    << '\n'
                    << "Got ec(\"" << ec.message()
                    << "\"). async_start_write() will not be executed." << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;

            // Give the audience a hint why we are terminating, and close the underlying TCP
            // socket (the connect succeeded, only the websocket handshake failed) so nothing
            // keeps io_context::run() from draining.
            close_code_ = boost::beast::websocket::close_code::going_away;
            do_close(close_code_);
            return;
        }

        // Full-duplex operation: keep exactly one async_read outstanding at all times so Beast can
        // service incoming pings (-> automatic pong) even while work items are being evaluated on the
        // compute pool. Without an in-flight read, a long (unbounded) evaluation would starve the pong
        // and the server would wrongly declare this healthy client dead. The periodic halt poll
        // cancels the outstanding read on shutdown so io_context::run() can drain.
        async_start_read();
        start_halt_timer();

        // Prime the pipeline: send up to prefetch_depth_ PULL frames. Further pulls are issued as
        // RETURNs are sent (finish_compute_()) and as NO_WORK replies are backed off and retried
        // (schedule_refill_()); the read above is re-armed in when_read().
        request_more_();
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when the entire message was sent to the remote side; releases the
	  * write buffer and pumps the next queued message.
	  *
	  * @param ec Indicates a possible error during the write
	  * @param nothing The number of bytes transferred (unused)
	  */
    void when_written(
        boost::system::error_code ec,
        [[maybe_unused]] std::size_t nothing
    ) {
        if(ec) {
            // An error is the expected outcome once do_close() has begun -- not worth a message.
            if(not closing_ && ec != boost::asio::error::operation_aborted) {
                glogger << "In GWebsocketClientT<processable_type>::when_written():" << '\n'
                        << "Got ec(\"" << ec.message() << "\")." << '\n'
                        << "This will terminate the client." << '\n'
                        << GLOGGING;
            }
            close_code_ = boost::beast::websocket::close_code::going_away;
            writing_ = false;
            // Tear the connection down: without this the halt timer keeps re-arming (the base
            // halt() condition never becomes true on a transport error) and io_context::run()
            // never drains -- the client would hang instead of terminating. A repeated
            // do_close() during shutdown (ec == operation_aborted) is a no-op.
            do_close(close_code_);
            return;
        }

        // The request has been sent; the server's response is delivered to the always-outstanding
        // read (re-armed in when_read()), so we do NOT start a read here. Release the buffer and send
        // the next queued message, if any.
        outgoing_message_.clear();
        writing_ = false;
        pump_write();
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when an entire new message was read; dispatches the message and
	  * re-arms the read so exactly one read stays outstanding.
	  *
	  * @param ec Indicates a possible error during the read
	  * @param nothing The number of bytes transferred (unused)
	  */
    void when_read(
        boost::system::error_code ec,
        [[maybe_unused]] std::size_t nothing
    ) {
        if(ec) {
            // An error is the expected outcome once do_close() has begun -- not worth a message.
            // Otherwise it may be the idle limit: the server has been silent for too long.
            if(not closing_ && ec != boost::asio::error::operation_aborted) {
                glogger << "In GWebsocketClientT<processable_type>::when_read():" << '\n'
                        << "Got ec(\"" << ec.message() << "\")." << '\n'
                        << "This will terminate the client." << '\n'
                        << GLOGGING;
            }
            close_code_ = boost::beast::websocket::close_code::going_away;
            // Tear the connection down so io_context::run() drains (see when_written()); a
            // repeated do_close() during shutdown (ec == operation_aborted) is a no-op.
            do_close(close_code_);
            return;
        }

        // Handle the message: a WORK item is moved to the compute pool (so the io thread stays free
        // to service pings while the -- possibly long -- evaluation runs); lighter frames (NO_WORK)
        // are answered directly. Then re-arm the read so exactly one read stays outstanding --
        // unless handle_message() shut the connection down (fatal decode failure / bad command).
        handle_message();

        if(not this->halt() && ws_.is_open()) {
            async_start_read();
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Processing of incoming messages and creation of responses takes place here. A WORK
	  * item is moved to the compute pool; NO_WORK triggers a backoff refill; bad messages shut down.
	  */
    void handle_message() {
        // Extract the string from the buffer
        auto message = boost::beast::buffers_to_string(incoming_buffer_.data());

        // Clear the buffer, so we may later fill it with data to be sent
        incoming_buffer_.consume(incoming_buffer_.size());

        // De-serialize the object. A malformed/truncated message makes this throw; that must
        // not escape into io_context::run() (it would unwind the client's only io thread).
        try {
            Gem::Courtier::wireDecode(message, command_container_, &wire_ctx_, serialization_mode_);
        }
        catch(const std::exception &e) {
            glogger << "In GWebsocketClientT<processable_type>::handle_message():" << '\n'
                    << "Could not de-serialize an incoming message:" << '\n'
                    << e.what() << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            close_code_ = boost::beast::websocket::close_code::internal_error;
            do_close(close_code_); // actually shut down: cancel the timers and close the stream
            return;
        }

        // Both WORK and NO_WORK are answers to a pull (a PULL or RETURN we sent earlier), so one
        // pull is now resolved.
        if(pending_pulls_ > 0) {
            --pending_pulls_;
        }

        // Act on the frame received
        switch(command_container_.kind()) {
            using enum Gem::Courtier::GFrameKind;
        case WORK:
            // Work arrived. Move it out (so command_container_ is free for the next read) and hand the
            // (possibly long-running, unbounded) evaluation to the compute pool, so the io thread stays
            // free to service pings -> auto-pong while it runs. The result is written back from
            // the pipeline base's finish_compute_() once the worker is done. One more item is now computing; the in-flight
            // total is unchanged (one pull became one computation), so no top-up is needed here.
            ++computing_;
            dispatch_compute_(std::move(command_container_));
            break;

        case NO_WORK:
            // No work available yet. The in-flight total has dropped by one; back off, then top up
            // again. The wait is an async timer (NOT a blocking sleep): at depth > 1 other items are
            // still computing and their results / pings must not be stalled on the io thread.
            n_nodata_++;
            schedule_refill_();
            break;

        default:
            // An unknown/invalid frame is unrecoverable; log and shut down cleanly (do NOT throw
            // -- that would unwind the io thread).
            glogger << "In GWebsocketClientT<processable_type>::handle_message():" << '\n'
                    << "Received invalid frame " << fkToStr(command_container_.kind())
                    << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            close_code_ = boost::beast::websocket::close_code::internal_error;
            do_close(close_code_); // actually shut down: cancel the timers and close the stream
            break;
        }
    }

    //-------------------------------------------------------------------------
    /**
		  * Issues PULL frames until the number of in-flight items (sent-but-unanswered pulls plus
		  * items currently being evaluated) reaches the configured prefetch depth. At depth 1 this keeps
		  * exactly one item in flight -- the classic strictly-serial behaviour. Runs on the io thread.
		  */
    void request_more_() {
        while(not this->halt() && (pending_pulls_ + computing_) < prefetch_depth_) {
            ++pending_pulls_;
            GCommandContainerT<processable_type> const pull{GFrameKind::PULL};
            send_command_(pull);
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Serializes @p container and writes it to the server, guarding against serialization
	 *  failures (which must not unwind the io thread).
	 *
	 *  @param container The command container to serialize and transmit */
    void send_command_(
        const GCommandContainerT<processable_type> &container
    ) {
        try {
            this->async_start_write(
                Gem::Courtier::wireEncode(container, &wire_ctx_, serialization_mode_)
            );
        }
        catch(const std::exception &e) {
            glogger << "In GWebsocketClientT<processable_type>::send_command_():" << '\n'
                    << "Could not serialize the outgoing message:" << '\n'
                    << e.what() << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            close_code_ = boost::beast::websocket::close_code::internal_error;
            do_close(close_code_); // actually shut down: cancel the timers and close the stream
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Closes the connection to the peer, cancelling the halt and NO_WORK timers first. The
	  * close handshake runs asynchronously and is bounded by the handshake limit (see
	  * applyWebsocketLimits()), so a peer that no longer answers cannot hold the io thread.
	  * Idempotent.
	  *
	  * @param cc The close code to be sent to the peer
	  */
    void do_close(close_code cc) {
        // Stop the halt-poll and NO_WORK-backoff timers for good, so io_context::run() can return.
        this->stopPipelineTimers_();

        if(closing_) {
            return;
        }
        closing_ = true;

        // Without an open websocket (the handshake never completed, or the connection has failed)
        // there is no close handshake to perform.
        if(not ws_.is_open()) {
            close_socket_();
            return;
        }

        auto self = this->shared_from_this();
        ws_.async_close(cc, [self]([[maybe_unused]] boost::system::error_code ec) {
            self->close_socket_();
        });
    }

    //-------------------------------------------------------------------------
    /** @brief Closes the TCP socket if it is still open, and stops Beast's time limits. */
    void close_socket_() {
        disarmWebsocketLimits(ws_);
        if(ws_.next_layer().is_open()) {
            boost::system::error_code ec;

            std::ignore = ws_.next_layer().shutdown(socket::shutdown_both, ec); // best-effort teardown
            std::ignore = ws_.next_layer().close(ec); // best-effort teardown

            // A failed shutdown/close (commonly the peer already went away) must NOT throw out of
            // this async-handler context -- that would unwind the io thread. Log and move on.
            if(ec && ec != boost::asio::error::not_connected) {
                glogger << "In GWebsocketClientT<processable_type>::do_close():" << '\n'
                        << "Shutdown of the next layer reported: " << ec.message() << '\n'
                        << GLOGGING;
            }
        }
    }

    //-------------------------------------------------------------------------

    //-------------------------------------------------------------------------
    // The transport hooks the pipeline base drives (see GPrefetchingClientT)

    /** @brief Pipeline-refill hook: issues PULL frames until the in-flight total reaches the
     *  prefetch depth (see request_more_()). */
    void refill_() { request_more_(); }

    /** @brief Result-transmission hook: writes the RETURN to the server and tops the pipeline
     *  back up (the RETURN itself is a pull; request_more_ covers any NO_WORK-left deficit).
     *  @param container The RETURN frame the base built from the evaluated work item */
    void sendResultAndRefill_(
        GCommandContainerT<processable_type> container
    ) {
        send_command_(container);
        request_more_();
    }

    /** @brief Halt hook: closes the websocket cleanly when a halt condition is reached. */
    void haltShutdown_() { do_close(boost::beast::websocket::close_code::normal); }

    /** @brief Callback for control frames */
    std::function<void(boost::beast::websocket::frame_type, boost::beast::string_view)>
        f_when_control_frame_arrived_;

    //-------------------------------------------------------------------------
    // Data

    resolver resolver_{io_context_}; ///< Helps to resolve the peer
    boost::beast::websocket::stream<socket> ws_{
        io_context_
    }; ///< All messages are sent and received through this socket

    std::string address_; ///< The ip address or name of the peer system
    unsigned int port_;   ///< The peer port

    boost::beast::multi_buffer incoming_buffer_;
    std::string outgoing_message_; ///< Holds the message currently being written (one write at a time)
    std::deque<std::string> write_queue_; ///< Messages waiting to be written (Beast: one write at a time)
    bool writing_ = false;                ///< Whether an async_write is currently outstanding
    bool closing_ = false;                ///< Whether do_close() has begun (no further writes)

    boost::beast::websocket::close_code close_code_ = boost::beast::websocket::close_code::
        normal; ///< Holds the close code when terminating the connection

    Gem::Common::serializationMode serialization_mode_ = Gem::Common::serializationMode::
        GEM_BINARY; ///< Determines which seriliztion mode should be used
    bool verbose_control_frames_ =
        false; ///< Whether a diagnostic message should be emitted when a control frame arrives

    // Note: the heavy evaluation now runs on compute_pool_ (not inline on the io thread); the io
    // thread keeps an async_read outstanding throughout, so Beast services pings during evaluation.
    // Only the io thread ever touches ws_ (the worker posts the result write back to it), so Beast's
    // single-reader/single-writer rule is upheld.

    //-------------------------------------------------------------------------
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Consumer-side handling of client-connection. A new session is started for each
 * new connection and will be kept open until a stop condition is reached.
 *
 * @tparam processable_type The work-item type exchanged with the connected client (must be processable)
 */
template <typename processable_type>
class GWebsocketConsumerSessionT final
  : public std::enable_shared_from_this<GWebsocketConsumerSessionT<processable_type>> {
    //-------------------------------------------------------------------------
    // Make the code easier to read
    using error_code = boost::system::error_code;
    using frame_type = boost::beast::websocket::frame_type;
    using string_view = boost::beast::string_view;

public:
    //-------------------------------------------------------------------------
    /**
	  * @brief The only allowed constructor for this class.
	  *
	  * @param io_context The io_context whose executor drives this session's async operations
	  * @param socket The already-accepted TCP socket, moved into the session's websocket stream
	  * @param get_payload_item Callback that hands out the next work item (or null when none is available)
	  * @param put_payload_item Callback that hands a RETURN frame back to the server/broker
	  * @param check_server_stopped Callback returning true when the server has reached a stop condition
	  * @param server_sign_on Callback notifying the server of session start (true) and termination (false)
	  * @param serialization_mode Which serialization format (binary/XML/text) the wire protocol uses
	  * @param ping_interval Seconds of silence after which a ping is sent; after twice that time
	  *        without any inbound data the peer is presumed gone and the session ends (0 switches
	  *        this off)
	  * @param verbose_control_frames If true, a diagnostic message is logged for every control frame received
	  */
    /** @brief The session's server-side hooks: how it pulls work, returns results, checks for a stopped
     *  server, and signs itself on/off. Bundled so the constructor stays within the parameter budget. */
    struct SessionHooks {
        std::move_only_function<std::unique_ptr<processable_type>()> get_payload_item;
        std::move_only_function<void(GCommandContainerT<processable_type> &&)> put_payload_item;
        std::move_only_function<bool()> check_server_stopped;
        std::move_only_function<void(bool)> server_sign_on;
    };

    GWebsocketConsumerSessionT(
        boost::asio::io_context &io_context,
        boost::asio::ip::tcp::socket socket,
        SessionHooks hooks,
        Gem::Common::serializationMode serialization_mode,
        std::size_t ping_interval,
        bool verbose_control_frames,
        Gem::Courtier::GWireBlobRegistry *wire_registry = nullptr,
        Gem::Courtier::GWirePeerId peer_id = 0
    )
      : ws_(std::move(socket))
      , strand_(io_context.get_executor())
      , get_payload_item_(std::move(hooks.get_payload_item))
      , put_payload_item_(std::move(hooks.put_payload_item))
      , check_server_stopped_(std::move(hooks.check_server_stopped))
      , server_sign_on_(std::move(hooks.server_sign_on))
      , serialization_mode_(serialization_mode)
      , ping_interval_(std::chrono::seconds(ping_interval))
      , verbose_control_frames_(verbose_control_frames)
      , wire_registry_(wire_registry)
      , peer_id_(peer_id) {
        // Engage the blob send-once wire form for this session's peer: each connection is a
        // distinct peer, so the server sends a given blob in full only on the first work item to this
        // peer and references it by content id thereafter. A reconnecting / late-joining client is a new
        // peer and receives the blob fresh, so no separate fetch is needed on the ordered websocket
        // stream. Disabled (-> self-contained full-payload form) when no registry is supplied.
        wire_ctx_.enabled = (wire_registry_ != nullptr);
        wire_ctx_.peer = peer_id_;
        wire_ctx_.registry = wire_registry_;
        wire_ctx_.mode = serialization_mode_;
        // ---------------------------------------------------
        // Make it known to the server that a new session has started
        this->server_sign_on_(true);

        // ---------------------------------------------------
        // Install the control-frame callback (optional verbose logging) and set the websocket
        // transfer options, among them Beast's own keep-alive (see applyWebsocketLimits()).
        this->installControlFrameCallback();
        this->configureWebsocketOptions();

        // ---------------------------------------------------
    }

    //-------------------------------------------------------------------------
    /**
         * @brief Installs the control-frame callback on the websocket: when verbose_control_frames_
         * is set, every close/ping/pong frame is logged. (Liveness is Beast's business: it counts
         * every received frame as activity by itself.)
         */
    void installControlFrameCallback() {
        f_when_control_frame_arrived_ = [this](frame_type frame_t, [[maybe_unused]] string_view s) {
            // Let the audience know what type of control frame we have received
            // if the user has requested it.
            if(this->verbose_control_frames_) {
                if(boost::beast::websocket::frame_type::close == frame_t) {
                    glogger << "GWebsocketConsumerSessionT<> session has received a close frame"
                            << '\n'
                            << GLOGGING;
                }
                else if(boost::beast::websocket::frame_type::ping == frame_t) {
                    glogger << "GWebsocketConsumerSessionT<> session has received a ping frame"
                            << '\n'
                            << GLOGGING;
                }
                else if(boost::beast::websocket::frame_type::pong == frame_t) {
                    glogger << "GWebsocketConsumerSessionT<> session has received a pong frame"
                            << '\n'
                            << GLOGGING;
                }
            }
        };

        // Set the callback to be executed on every incoming control frame.
        ws_.control_callback(f_when_control_frame_arrived_);
    }

    //-------------------------------------------------------------------------
    /**
         * @brief Sets the websocket transfer options: auto-fragmentation (so control frames are delivered
         * timely), the write buffer size, the time and size limits shared with the client, and the
         * binary/text transfer mode per the serialization mode.
         */
    void configureWebsocketOptions() {
        // Set the auto_fragment option, so control frames are delivered timely
        ws_.auto_fragment(true);
        ws_.write_buffer_bytes(16384);
        applyWebsocketLimits(ws_, ping_interval_);

        // Set the transfer mode according to the serialization mode
        switch(serialization_mode_) {
        case Gem::Common::serializationMode::GEM_BINARY: // GArchive binary -> binary websocket frames
            ws_.binary(true);
            break;
        case Gem::Common::serializationMode::GEM_JSON: // GArchive JSON -> text websocket frames
            ws_.binary(false);
            break;
        }
    }

    //-------------------------------------------------------------------------
    /** @brief The destructor. Signs the session off with the server. */
    ~GWebsocketConsumerSessionT() {
        // Drop this peer's per-session blob-ack state (the connection is gone). The shared blob store
        // is left intact for other peers. A reconnect is a fresh peer and re-receives its blobs.
        if(wire_registry_ != nullptr) {
            wire_registry_->forgetPeer(peer_id_);
        }
        // Make it known to the server that this session has terminated
        this->server_sign_on_(false);
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Initiates all communication and processing: disables Nagle's algorithm and begins
	  * waiting for the websocket handshake. Returns shortly after, all work proceeding asynchronously.
	  */
    void async_start_run() {
        // ---------------------------------------------------
        // Connections and communication

        // Disable Nagle's algorithm on the underlying TCP socket (already connected at this
        // point): the request/response messages are small and latency-sensitive.
        boost::system::error_code nd_ec;
        std::ignore = ws_.next_layer().set_option(boost::asio::ip::tcp::no_delay(true), nd_ec); // failure deliberately tolerated

        // Wait for a new websocket connection. Note that the
        // ASIO connection should already be active at this place.
        async_start_accept();

        // ---------------------------------------------------
        // This function will terminate shortly after it was called,
        // as all operations are performed asynchronously.
    }

    //-------------------------------------------------------------------------
    // Deleted functions
    GWebsocketConsumerSessionT() = delete;
    GWebsocketConsumerSessionT(const GWebsocketConsumerSessionT<processable_type> &) = delete;
    GWebsocketConsumerSessionT(GWebsocketConsumerSessionT<processable_type> &&) = delete;
    GWebsocketConsumerSessionT<processable_type> &
    operator=(const GWebsocketConsumerSessionT<processable_type> &) = delete;
    GWebsocketConsumerSessionT<processable_type> &
    operator=(GWebsocketConsumerSessionT<processable_type> &&) = delete;

private:
    //-------------------------------------------------------------------------
    /**
	  * @brief Initiates a new asynchronous read session, bound to the session strand.
	  */
    void async_start_read() {
        // Read a message into our buffer
        auto self = this->shared_from_this();
        ws_.async_read(
            incoming_buffer_,
            boost::asio::bind_executor(
                strand_,
                [self](boost::system::error_code ec, std::size_t nBytesTransferred) {
                    self->when_read(ec, nBytesTransferred);
                }
            )
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Initiates a new asynchronous write session, persisting the message for the write's duration.
	  *
	  * @param message The serialized message to be sent to the peer
	  */
    void async_start_write(std::string message) {
        // We need to persist the message for asynchronous operations
        outgoing_message_ = std::move(message);

        // Return an answer
        auto self = this->shared_from_this();
        ws_.async_write(
            boost::asio::buffer(outgoing_message_),
            boost::asio::bind_executor(
                strand_,
                [self](boost::system::error_code ec, std::size_t nBytesTransferred) {
                    self->when_written(ec, nBytesTransferred);
                }
            )
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Starts waiting for websocket handshakes over an already established ASIO connection.
	  */
    void async_start_accept() {
        // This function initiates an asynchronous chain of callbacks, where each callback is
        // executed when the previous call (here: async_accept) is completed. Error handling is
        // done in the callback, using an error code provided by Boost.Beast and/or Boost.ASIO.
        auto self = this->shared_from_this();
        ws_.async_accept(
            boost::asio::bind_executor(strand_, [self](boost::system::error_code ec) {
                self->when_connection_accepted(ec);
            })
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when a handshake was accepted; on success it starts the read loop.
	  *
	  * @param ec The error code of a potential error during the accept/handshake
	  */
    void when_connection_accepted(boost::system::error_code ec) {
        if(ec) {
            glogger << "GWebsocketConsumerSessionT<processable_type>::when_connection_accepted(): "
                    << ec.message() << '\n'
                    << GLOGGING;

            do_close(boost::beast::websocket::close_code::going_away);

            return;
        }

        // Start reading an incoming message. This call will return immediately. With a read
        // outstanding, Beast's keep-alive is at work too.
        async_start_read();
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when a new message was read; processes the request and writes the response.
	  *
	  * @param ec The error code of a potential error during the read
	  * @param nothing The number of bytes transferred (unused)
	  */
    void when_read(
        boost::system::error_code ec,
        [[maybe_unused]] std::size_t nothing
    ) {
        if(ec) {
            // An error is the expected outcome once do_close() has begun. Otherwise it may be the
            // idle limit: the peer has been silent for too long and is presumed gone.
            if(not closing_ && ec != boost::beast::websocket::error::closed) {
                glogger << "GWebsocketConsumerSessionT<processable_type>::when_read(): "
                        << ec.message() << '\n'
                        << GLOGGING;
            }

            do_close(boost::beast::websocket::close_code::going_away);
            return;
        }

        // Deal with the message and send a response back
        async_start_write(process_request());
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when a message was written; either closes on a stop condition or
	  * starts another read cycle.
	  *
	  * @param ec The error code of a potential error during the write
	  * @param nothing The number of bytes transferred (unused)
	  */
    void when_written(
        boost::system::error_code ec,
        [[maybe_unused]] std::size_t nothing
    ) {
        if(ec) {
            if(not closing_ && ec != boost::beast::websocket::error::closed) {
                glogger << "GWebsocketConsumerSessionT<processable_type>::when_written(): "
                        << ec.message() << '\n'
                        << GLOGGING;
            }

            do_close(boost::beast::websocket::close_code::going_away);
            return;
        }

        // Clear the outgoing message -- no longer needed
        outgoing_message_.clear();

        if(this->check_server_stopped_()) {
            // Do not continue if a stop criterion was reached
            do_close(boost::beast::websocket::close_code::normal);
        }
        else {
            // Start another read cycle
            async_start_read();
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Callback for control frames */
    std::function<void(boost::beast::websocket::frame_type, boost::beast::string_view)>
        f_when_control_frame_arrived_;

    //-------------------------------------------------------------------------
    /**
	  * @brief Shuts down the (websocket and ASIO) connection to the peer. The close handshake runs
	  * asynchronously and is bounded by the handshake limit (see applyWebsocketLimits()), so a peer
	  * that no longer answers cannot hold an io thread. Idempotent.
	  *
	  * @param cc A close-code to be transmitted to the peer
	  */
    void do_close(boost::beast::websocket::close_code cc) {
        if(closing_) {
            return;
        }
        closing_ = true;

        // Store the close code for later reference
        close_code_ = cc;

        // Without an open websocket (the handshake never completed, or the connection has failed or
        // timed out) there is no close handshake to perform.
        if(not ws_.is_open()) {
            close_socket_();
            return;
        }

        auto self = this->shared_from_this();
        ws_.async_close(
            cc,
            boost::asio::bind_executor(
                strand_,
                [self]([[maybe_unused]] boost::system::error_code ec) { self->close_socket_(); }
            )
        );
    }

    //-------------------------------------------------------------------------
    /** @brief Closes the TCP socket if it is still open, and stops Beast's time limits. */
    void close_socket_() {
        disarmWebsocketLimits(ws_);
        if(ws_.next_layer().is_open()) {
            boost::system::error_code ec;

            // Closing the socket cancels all outstanding operations. They
            // will complete with boost::asio::error::operation_aborted
            std::ignore = ws_.next_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec); // best-effort teardown
            std::ignore = ws_.next_layer().close(ec); // best-effort teardown

            // A failed shutdown/close (commonly the peer already vanished) must NOT throw out of this
            // async-handler context -- that would unwind an io thread. Log and move on.
            if(ec && ec != boost::asio::error::not_connected) {
                glogger << "GWebsocketConsumerSessionT<processable_type>::do_close():" << '\n'
                        << "Shutdown of the next layer reported: " << ec.message() << '\n'
                        << GLOGGING;
            }
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Processing of incoming messages and creation of a response-string. Handles PULL
	  * (hands out a work item) and RETURN (sinks the returned result, then hands out the next item).
	  *
	  * @return The response to be sent to the peer (a NO_WORK frame if the request could not be
	  * decoded or handled, so the client asks again later)
	  */
    std::string process_request() {
        try {
            // Extract the string from the buffer
            auto message = boost::beast::buffers_to_string(incoming_buffer_.data());

            // De-serialize the object (under the wire scope, so an id-referenced blob in a returned
            // result resolves against this server's registry).
            Gem::Courtier::wireDecode(
                message,
                command_container_,
                wire_ctx_.enabled ? &wire_ctx_ : nullptr,
                serialization_mode_
            ); // may throw

            // Clear the buffer, so we may later fill it with data to be sent
            incoming_buffer_.consume(incoming_buffer_.size());

            // Act on the frame and produce the response (shared synchronous server dispatch). A
            // websocket worker never sends BLOB_REQUEST (blobs arrive inline on the ordered
            // connection), so only PULL / RETURN are exercised here; the response is serialized under
            // this session's wire scope (blob send-once).
            return Gem::Courtier::handleServerRequest(
                command_container_,
                get_payload_item_,
                put_payload_item_,
                wire_registry_,
                wire_ctx_.enabled ? &wire_ctx_ : nullptr,
                serialization_mode_
            );
        }
        catch(const std::exception &e) {
            glogger << "GWebsocketConsumerSessionT<processable_type>::process_request(): Caught "
                       "exception: "
                    << e.what() << '\n'
                    << GLOGGING;
        }
        catch(...) {
            glogger << "GWebsocketConsumerSessionT<processable_type>::process_request(): Caught "
                       "non-std exception"
                    << '\n'
                    << GLOGGING;
        }

        // The request was refused. Messages keep their boundaries on a websocket, so the connection
        // is still usable: clear what is left of the bad message and answer NO_WORK, as the other
        // transports do. Closing the connection instead would end the client.
        incoming_buffer_.consume(incoming_buffer_.size());
        return Gem::Courtier::refusalReply<processable_type>(serialization_mode_);
    }

    //-------------------------------------------------------------------------
    // Data

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

    boost::beast::multi_buffer incoming_buffer_;
    std::string outgoing_message_;
    bool closing_ = false; ///< Whether do_close() has begun

    std::move_only_function<std::unique_ptr<processable_type>()> get_payload_item_;
    std::move_only_function<void(GCommandContainerT<processable_type> &&)> put_payload_item_;
    std::move_only_function<bool()> check_server_stopped_;
    std::move_only_function<void(bool)> server_sign_on_;

    boost::beast::websocket::close_code close_code_ = boost::beast::websocket::close_code::
        normal; ///< Holds the close code when terminating the connection

    Gem::Common::serializationMode serialization_mode_ = Gem::Common::serializationMode::GEM_BINARY;

    /// The silence after which Beast sends a ping; after twice that time without inbound data it
    /// closes the connection (see applyWebsocketLimits())
    const std::chrono::seconds ping_interval_{GBEASTCONSUMERPINGINTERVAL};
    bool verbose_control_frames_ = false;

    GCommandContainerT<processable_type> command_container_{
        GFrameKind::NONE
    }; ///< Holds the current frame and its payload (if any)

    /// The shared (consumer-owned) blob registry and this session's peer id, plus the wire scope
    /// installed around (de)serialisation so a work item's blob is sent to this peer only once
    /// (blob send-once). wire_registry_ is null when the feature is disabled.
    Gem::Courtier::GWireBlobRegistry *wire_registry_ = nullptr;
    Gem::Courtier::GWirePeerId peer_id_ = 0;
    Gem::Courtier::GWireSerializationContext wire_ctx_;

    //-------------------------------------------------------------------------
};


/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
