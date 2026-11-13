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
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

// Boost headers go here
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/system/error_code.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GLogger.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GCourtierHelperFunctions.hpp"
#include "courtier/GServerSessionLogic.hpp"       // shared synchronous server dispatch (PULL/RETURN/...)
#include "courtier/GWireCodec.hpp"                // shared scope-wrapped (de)serialization
#include "courtier/GWireSerializationContext.hpp"
#include "courtier/transport/GPrefetchingClientT.hpp" // the client's work pipeline
#include "courtier/transport/GWebsocketPeer.hpp"      // the connection, shared by both ends

namespace Gem::Courtier::Consumers {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class is responsible for the client side of network communication with Boost::Beast.
 * Connections are kept open permanently.
 *
 * The client can keep up to prefetch_depth_ work items in flight at once (requested-but-unanswered +
 * currently computing), overlapping network transfer with computation. A depth of 1 is the classic
 * strictly-serial behaviour (one item at a time). The connection itself -- the stream, the read
 * that stays outstanding, the write queue, the keep-alive and the close -- is a GWebsocketPeer; this
 * class resolves and connects, and gives the messages their meaning. Because the peer keeps reading while
 * items evaluate on the compute pool, pings are answered throughout and an evaluation may take any
 * length of time; and a server that has fallen silent is noticed by the peer's idle limit.
 *
 * All connection state (the in-flight counters and the peer) is touched only on the single io
 * thread; the compute pool threads only ever touch the work-item container handed to them and then
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

    using resolver = boost::asio::ip::tcp::resolver;
    using close_code = GWebsocketPeer::close_code;

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
	  * @param serialization_mode Which serialization format (binary or JSON) the wire protocol uses
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
      , address_(std::move(address))
      , port_(port)
      , serialization_mode_(serialization_mode)
      , verbose_control_frames_(verbose_control_frames)
      , ping_interval_(ping_interval) {
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
	  * remains, then makes sure the connection is over.
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

        // The io context has stopped, so no close handshake can run any more. A peer that is still
        // there is ended at once (after an orderly close it is gone already).
        if(auto peer = peer_.lock()) {
            peer->abort();
        }

        // Let the audience know that we have finished the shutdown
        glogger << "GWebsocketClientT<processable_type>::run_(): Client session has terminated"
                << '\n'
                << GLOGGING;
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
            return;
        }

        // Make the ASIO connection on the endpoint we get from a lookup
        auto self = this->shared_from_this();
        boost::asio::async_connect(
            socket_,
            results.begin(),
            results.end(),
            [self](boost::system::error_code ec, [[maybe_unused]] const auto &unused) {
                self->when_connected(ec);
            }
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when the async_connect call has completed; on success the socket
	  * becomes a GWebsocketPeer, which performs the websocket handshake.
	  *
	  * @param ec Indicates a possible error during the TCP connect
	  */
    void when_connected(boost::system::error_code ec) {
        if(ec) {
            glogger << "In GWebsocketClientT<processable_type>::when_connected():" << '\n'
                    << "Got ec(\"" << ec.message() << "\"). The handshake will not be executed."
                    << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;
            return;
        }

        // The peer owns this client through its handlers for as long as the connection lasts; the
        // client refers to the peer weakly (see GWebsocketPeer, "Ownership").
        auto self = this->shared_from_this();
        auto peer = std::make_shared<GWebsocketPeer>(
            std::move(socket_),
            GWebsocketPeer::GOptions{
                .name = "GWebsocketClientT<> client",
                .mode = serialization_mode_,
                .verboseControlFrames = verbose_control_frames_,
                .pingInterval = std::chrono::seconds(ping_interval_)
            },
            GWebsocketPeer::GHandlers{
                .onOpen = [self] { self->when_open_(); },
                .onMessage = [self](const std::string &message) { self->handle_message(message); },
                .onClosed =
                    [self](const boost::system::error_code &cause) { self->when_closed_(cause); }
            }
        );
        peer_ = peer;
        peer->connectHandshake(address_);
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Called when the websocket handshake has completed: starts the halt timer and primes the
	  * prefetch pipeline. Further pulls are issued as RETURNs are sent (finish_compute_()) and as
	  * NO_WORK replies are backed off and retried (schedule_refill_()).
	  */
    void when_open_() {
        start_halt_timer();
        request_more_();
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Called once when the connection is over, for whatever reason. Stops the pipeline's
	  * timers, so that io_context::run() -- and with it the client -- can return.
	  *
	  * @param cause Why the connection ended; no error if this client closed it
	  */
    void when_closed_(const boost::system::error_code &cause) {
        if(cause) {
            // The handshake failed, the server closed the connection or vanished, or it has been
            // silent for longer than the idle limit allows.
            glogger
                << "In GWebsocketClientT<processable_type>: the connection to the server has ended:"
                << '\n'
                << cause.message() << '\n'
                << "This will terminate the client." << '\n'
                << GLOGGING;
        }
        this->stopPipelineTimers_();
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Processing of incoming messages takes place here. A WORK item is moved to the compute
	  * pool; NO_WORK triggers a backoff refill; bad messages shut the client down.
	  *
	  * @param message One complete message from the server
	  */
    void handle_message(const std::string &message) {
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
            do_close(close_code::internal_error);
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
            // Work arrived. Move it out (so command_container_ is free for the next message) and
            // hand the (possibly long-running, unbounded) evaluation to the compute pool, so the io thread
            // stays free while it runs. The result is written back from the pipeline base's
            // finish_compute_() once the worker is done. One more item is now computing; the in-flight
            // total is unchanged (one pull became one computation), so no top-up is needed here.
            ++computing_;
            dispatch_compute_(std::move(command_container_));
            break;

        case NO_WORK:
            // No work available yet. The in-flight total has dropped by one; back off, then top up
            // again. The wait is an async timer (NOT a blocking sleep): at depth > 1 other items are
            // still computing and their results must not be stalled on the io thread.
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
            do_close(close_code::internal_error);
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
    /** @brief Serializes @p container and hands it to the peer for sending, guarding against
	 *  serialization failures (which must not unwind the io thread). Nothing is sent once a halt
	 *  condition holds or the connection is gone.
	 *
	 *  @param container The command container to serialize and transmit */
    void send_command_(const GCommandContainerT<processable_type> &container) {
        auto peer = peer_.lock();
        if(this->halt() || not peer) {
            return;
        }
        try {
            peer->send(Gem::Courtier::wireEncode(container, &wire_ctx_, serialization_mode_));
        }
        catch(const std::exception &e) {
            glogger << "In GWebsocketClientT<processable_type>::send_command_():" << '\n'
                    << "Could not serialize the outgoing message:" << '\n'
                    << e.what() << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            do_close(close_code::internal_error);
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Closes the connection to the server and stops the pipeline's timers. The peer performs
	  * the close handshake asynchronously and within its time limit. Idempotent.
	  *
	  * @param cc The close code to be sent to the server
	  */
    void do_close(close_code cc) {
        // Stop the halt-poll and NO_WORK-backoff timers for good, so io_context::run() can return.
        this->stopPipelineTimers_();
        if(auto peer = peer_.lock()) {
            peer->close(cc);
        }
    }

    //-------------------------------------------------------------------------
    // The transport hooks the pipeline base drives (see GPrefetchingClientT)

    /** @brief Pipeline-refill hook: issues PULL frames until the in-flight total reaches the
     *  prefetch depth (see request_more_()). */
    void refill_() { request_more_(); }

    /** @brief Result-transmission hook: writes the RETURN to the server and tops the pipeline
     *  back up (the RETURN itself is a pull; request_more_ covers any NO_WORK-left deficit).
     *  @param container The RETURN frame the base built from the evaluated work item */
    void sendResultAndRefill_(GCommandContainerT<processable_type> container) {
        send_command_(container);
        request_more_();
    }

    /** @brief Halt hook: closes the websocket cleanly when a halt condition is reached. */
    void haltShutdown_() {
        do_close(close_code::normal);
    }

    //-------------------------------------------------------------------------
    // Data

    resolver resolver_{io_context_};                   ///< Helps to resolve the server
    boost::asio::ip::tcp::socket socket_{io_context_}; ///< The socket until it is connected
    std::weak_ptr<GWebsocketPeer> peer_; ///< The connection, once the socket is connected

    std::string address_; ///< The ip address or name of the server
    unsigned int port_;   ///< The server's port

    Gem::Common::serializationMode serialization_mode_ =
        Gem::Common::serializationMode::GEM_BINARY; ///< Determines which serialization mode is used
    bool verbose_control_frames_ = false; ///< Whether a message is logged for every control frame
    std::size_t ping_interval_ =
        Gem::Courtier::GBEASTCONSUMERPINGINTERVAL; ///< Keep-alive, in seconds

    //-------------------------------------------------------------------------
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Consumer-side handling of a client connection. A new session is started for each new connection
 * and lasts until the connection ends or the server stops. The connection itself is a
 * GWebsocketPeer; this class answers the requests that arrive on it.
 *
 * The session is owned by its peer (through the peer's handlers), and the peer by its pending
 * operations, so a session lives exactly as long as its connection.
 *
 * @tparam processable_type The work-item type exchanged with the connected client (must be processable)
 */
template <typename processable_type>
class GWebsocketConsumerSessionT final
  : public std::enable_shared_from_this<GWebsocketConsumerSessionT<processable_type>> {
    using close_code = GWebsocketPeer::close_code;

public:
    //-------------------------------------------------------------------------
    /** @brief The session's server-side hooks: how it pulls work, returns results, checks for a stopped
     *  server, and signs itself on/off. Bundled so the constructor stays within the parameter budget. */
    struct SessionHooks {
        std::move_only_function<std::unique_ptr<processable_type>()> get_payload_item;
        std::move_only_function<void(GCommandContainerT<processable_type> &&)> put_payload_item;
        std::move_only_function<bool()> check_server_stopped;
        std::move_only_function<void(bool)> server_sign_on;
    };

    //-------------------------------------------------------------------------
    /**
	  * @brief The only allowed constructor for this class.
	  *
	  * @param socket The already-accepted TCP socket; its executor must serialize the handlers it runs
	  * @param hooks How the session pulls work, returns results, sees a stopped server and signs on/off
	  * @param serialization_mode Which serialization format (binary or JSON) the wire protocol uses
	  * @param ping_interval Seconds of silence after which a ping is sent; after twice that time
	  *        without any inbound data the client is presumed gone and the session ends (0 switches
	  *        this off)
	  * @param verbose_control_frames If true, a diagnostic message is logged for every control frame received
	  * @param wire_registry The consumer's blob send-once registry, or nullptr to send every blob in full
	  * @param peer_id This connection's id in that registry
	  */
    GWebsocketConsumerSessionT(
        boost::asio::ip::tcp::socket socket,
        SessionHooks hooks,
        Gem::Common::serializationMode serialization_mode,
        std::size_t ping_interval,
        bool verbose_control_frames,
        Gem::Courtier::GWireBlobRegistry *wire_registry = nullptr,
        Gem::Courtier::GWirePeerId peer_id = 0
    )
      : socket_(std::move(socket))
      , get_payload_item_(std::move(hooks.get_payload_item))
      , put_payload_item_(std::move(hooks.put_payload_item))
      , check_server_stopped_(std::move(hooks.check_server_stopped))
      , server_sign_on_(std::move(hooks.server_sign_on))
      , serialization_mode_(serialization_mode)
      , ping_interval_(ping_interval)
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

        // Make it known to the server that a new session has started
        this->server_sign_on_(true);
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
	  * @brief Turns the accepted socket into a GWebsocketPeer and waits for the client's websocket
	  * handshake. Returns at once; all work proceeds asynchronously. From here on the peer owns this
	  * session through its handlers.
	  */
    void async_start_run() {
        auto self = this->shared_from_this();
        auto peer = std::make_shared<GWebsocketPeer>(
            std::move(socket_),
            GWebsocketPeer::GOptions{
                .name = "GWebsocketConsumerSessionT<> session",
                .mode = serialization_mode_,
                .verboseControlFrames = verbose_control_frames_,
                .pingInterval = std::chrono::seconds(ping_interval_)
            },
            GWebsocketPeer::GHandlers{
                .onOpen = {},
                .onMessage = [self](const std::string &message) { self->when_message_(message); },
                .onClosed =
                    [self](const boost::system::error_code &cause) { self->when_closed_(cause); }
            }
        );
        peer_ = peer;
        peer->acceptHandshake();
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
	  * @brief Called for every message from the client: answers the request and, if the server has
	  * been stopped in the meantime, closes the connection after the answer.
	  *
	  * @param message One complete message from the client
	  */
    void when_message_(const std::string &message) {
        auto peer = peer_.lock();
        if(not peer) {
            return;
        }
        peer->send(process_request(message));

        if(this->check_server_stopped_()) {
            // The close is queued behind the answer just sent.
            peer->close(close_code::normal);
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Called once when the connection is over. A regular close by either end and the end of
	  * the server are no news; anything else is logged, the expiry of the idle limit among it (the
	  * client has been silent for too long and is presumed gone).
	  *
	  * @param cause Why the connection ended; no error if this session closed it
	  */
    void when_closed_(const boost::system::error_code &cause) {
        if(cause && cause != boost::beast::websocket::error::closed &&
           not this->check_server_stopped_()) {
            glogger << "GWebsocketConsumerSessionT<processable_type>: the connection has ended: "
                    << cause.message() << '\n'
                    << GLOGGING;
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Processing of an incoming message and creation of the response. Handles PULL (hands out
	  * a work item) and RETURN (sinks the returned result, then hands out the next item).
	  *
	  * @param message One complete message from the client
	  * @return The response to be sent to the peer (a NO_WORK frame if the request could not be
	  * decoded or handled, so the client asks again later)
	  */
    std::string process_request(const std::string &message) {
        try {
            // De-serialize the object (under the wire scope, so an id-referenced blob in a returned
            // result resolves against this server's registry).
            Gem::Courtier::wireDecode(
                message,
                command_container_,
                wire_ctx_.enabled ? &wire_ctx_ : nullptr,
                serialization_mode_
            ); // may throw

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
        // is still usable: answer NO_WORK, as the other transports do. Closing the connection instead
        // would end the client.
        return Gem::Courtier::refusalReply<processable_type>(serialization_mode_);
    }

    //-------------------------------------------------------------------------
    // Data

    boost::asio::ip::tcp::socket
        socket_; ///< The accepted socket until async_start_run() hands it on
    std::weak_ptr<GWebsocketPeer> peer_; ///< The connection (it owns this session, not the reverse)

    std::move_only_function<std::unique_ptr<processable_type>()> get_payload_item_;
    std::move_only_function<void(GCommandContainerT<processable_type> &&)> put_payload_item_;
    std::move_only_function<bool()> check_server_stopped_;
    std::move_only_function<void(bool)> server_sign_on_;

    Gem::Common::serializationMode serialization_mode_ = Gem::Common::serializationMode::GEM_BINARY;
    std::size_t ping_interval_ =
        Gem::Courtier::GBEASTCONSUMERPINGINTERVAL; ///< Keep-alive, in seconds
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
