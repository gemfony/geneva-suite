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
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <tuple>

// Boost headers go here
#include <boost/asio.hpp>

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
#include "courtier/GWireCodec.hpp"                // shared scope-wrapped (de)serialization + blob fetch
#include "courtier/GWireSerializationContext.hpp"
#include "courtier/transport/GPrefetchingClientT.hpp" // blob send-once: registry + wire scope

namespace Gem::Courtier::Consumers {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Client side of the courtier ASIO transport: pulls work items from the server, evaluates
 * them on a compute pool and returns the results.
 *
 * One exchange equals one short-lived connection (resolve, connect, write request, read response,
 * close). A PULL exchange fetches an item; a RETURN exchange returns a finished result and fetches
 * the next in the same connection. A configurable prefetch depth keeps spare items in flight so the
 * evaluation of one item overlaps the fetch/return connections of others (a depth of 1 reproduces
 * the classic strictly-serial behaviour).
 *
 * @tparam processable_type The work-item type transported and evaluated by this client.
 */
template <typename processable_type>
class GAsioConsumerClientT final
  : public GPrefetchingClientT<GAsioConsumerClientT<processable_type>, processable_type>
  , public std::enable_shared_from_this<GAsioConsumerClientT<processable_type>> {
    // The pipeline base drives the compute dispatch / refill / halt machinery and calls our
    // private transport hooks (refill_ / sendResultAndRefill_ / haltShutdown_).
    friend class GPrefetchingClientT<GAsioConsumerClientT<processable_type>, processable_type>;

    using pipeline_base = GPrefetchingClientT<GAsioConsumerClientT<processable_type>, processable_type>;

    //-------------------------------------------------------------------------
    // Make the code easier to read

    using error_code = boost::system::error_code;
    using resolver = boost::asio::ip::tcp::resolver;
    using socket = boost::asio::ip::tcp::socket;

    // Members and helpers of the dependent pipeline base, pulled into scope
    using pipeline_base::io_context_;
    using pipeline_base::pending_pulls_;
    using pipeline_base::computing_;
    using pipeline_base::prefetch_depth_;
    using pipeline_base::n_nodata_;
    using pipeline_base::command_container_;
    using pipeline_base::wire_registry_;
    using pipeline_base::wire_ctx_;
    using pipeline_base::halt_timer_;
    using pipeline_base::nodata_timer_;
    using pipeline_base::rng_engine_;
    using pipeline_base::schedule_refill_;
    using pipeline_base::dispatch_compute_;
    using pipeline_base::start_halt_timer;

public:
    //-------------------------------------------------------------------------
    /**
	  * @brief Initialization with host/ip and port.
	  *
	  * @param address The IP address or host name of the server to connect to
	  * @param port The TCP port the server listens on
	  * @param serialization_mode The serialization format used on the wire (text, XML or binary)
	  * @param max_reconnects Maximum number of consecutive failed connection attempts before the client gives up
	  * @param prefetch_depth Maximum number of work items kept in flight at once; a value of 0 is clamped to 1 (strictly serial)
	  */
    GAsioConsumerClientT(
        std::string address,
        unsigned short port,
        Gem::Common::serializationMode serialization_mode,
        std::size_t max_reconnects,
        std::size_t prefetch_depth = 1
    )
      : pipeline_base("GAsioConsumerClientT<processable_type>", prefetch_depth)
      , address_(std::move(address))
      , port_(port)
      , serialization_mode_(serialization_mode)
      , max_reconnects_(max_reconnects) {
        // blob send-once. ASIO uses a fresh one-shot connection per exchange, so there is no
        // persistent per-connection peer identity for the server to key its per-peer "already holds this
        // blob" tracking on. The client therefore mints ONE stable, process-unique peer id at startup
        // and announces it on every request (setPeerId); the server uses it as the wire peer. The id
        // need only be unique among the server's concurrent clients, so a 64-bit random draw suffices.
        std::uniform_int_distribution<std::uint64_t> id_dist(1, std::numeric_limits<std::uint64_t>::max());
        peer_id_ = id_dist(rng_engine_);

        // Engage the worker-side wire form: cache every received blob (keyed by content id) so an
        // id-only work item resolves locally, and install a fetch_blob that resolves a cache miss (the
        // unavoidable case on ASIO: an id-only item can arrive on a fresh connection before the blob
        // for that id was ever sent to this client, because the server tracks the client across many
        // short connections and may have marked it as holding the blob in an exchange whose full-payload
        // send was lost, or because the client reconnected). The fetch is a self-contained, blocking
        // BLOB_REQUEST/BLOB_REPLY round trip on its OWN socket + io_context (see fetch_blob_),
        // independent of the async pipeline -- so it can run synchronously from inside load() on the io
        // thread without re-entering the pipeline's connection logic.
        wire_ctx_.enabled = true;
        wire_ctx_.peer = 0; // worker side: the single upstream server
        wire_ctx_.registry = &wire_registry_;
        wire_ctx_.mode = serialization_mode_;
        // A worker never interns: it does not own the registry the server resolves against, so a
        // blob it echoes back must always travel inline (see may_intern_blobs).
        wire_ctx_.may_intern_blobs = false;
        wire_ctx_.fetch_blob =
            [this](const Gem::Courtier::GWireBlobId &id) -> std::expected<std::string, std::string> {
            return this->fetch_blob_(id);
        };
    }

    //-------------------------------------------------------------------------
    // Deleted functions

    // Deleted default-constructor -- enforce usage of a particular constructor
    GAsioConsumerClientT() = delete;
    // Deleted copy-constructors and assignment operators -- the client is non-copyable
    GAsioConsumerClientT(const GAsioConsumerClientT<processable_type> &) = delete;
    GAsioConsumerClientT(GAsioConsumerClientT<processable_type> &&) = delete;
    GAsioConsumerClientT<processable_type> &
    operator=(const GAsioConsumerClientT<processable_type> &) = delete;
    GAsioConsumerClientT<processable_type> &
    operator=(GAsioConsumerClientT<processable_type> &&) = delete;

    //-------------------------------------------------------------------------

private:
    //-------------------------------------------------------------------------
    /**
	  * @brief Starts the main run-loop. Primes the prefetch pipeline, arms the halt timer and blocks
	  * in io_context::run() until shutdown releases the work guard.
	  */
    void run_() override {
        // Prime the pipeline: enqueue up to prefetch_depth_ PULL frames and start the first
        // connection cycle. Each connection performs exactly one exchange (the classic one-shot ASIO
        // session): a PULL fetches an item, a RETURN returns a finished result AND fetches the next in
        // the same exchange. Evaluations run on a pool, so while one item computes a spare is already in
        // hand -- when it finishes, a connection returns it and fetches a replacement, and meanwhile the
        // spare is already computing. A depth of 1 reproduces the classic strictly-serial behaviour.
        maintain_();
        start_halt_timer();

        // This call will block until no more work remains (the work guard is released on shutdown).
        io_context_.run();

        // Let the audience know that we have finished the shutdown
        glogger << "GAsioConsumerClientT<processable_type>::run_(): Client has terminated"
                << '\n'
                << GLOGGING;
    }

    //-------------------------------------------------------------------------
    /** @brief Enqueues PULL frames until the number of in-flight items (queued/in-progress exchanges
		 *  plus items currently being evaluated) reaches the prefetch depth, then starts a connection if
		 *  none is running. Runs on the io thread. */
    void maintain_() {
        while(not this->halt() && (pending_pulls_ + computing_) < prefetch_depth_) {
            ++pending_pulls_;
            GCommandContainerT<processable_type> pull{GFrameKind::PULL};
            // Announce this client's stable peer id so the server can key its per-peer send-once
            // tracking on it across our many short connections.
            pull.setPeerId(peer_id_);
            try {
                // A PULL carries no work item, so no wire context is engaged (null scope).
                exchange_queue_.push_back(
                    Gem::Courtier::wireEncode(pull, nullptr, serialization_mode_)
                );
            }
            catch(const std::exception &e) {
                --pending_pulls_;
                glogger << "In GAsioConsumerClientT<processable_type>::maintain_(): " << e.what()
                        << '\n'
                        << GWARNING;
                break;
            }
        }
        kick_exchange_();
    }

    //-------------------------------------------------------------------------
    // The transport hooks the pipeline base drives (see GPrefetchingClientT)

    /** @brief Pipeline-refill hook: enqueues PULL frames up to the prefetch depth (see maintain_()). */
    void refill_() { maintain_(); }

    /** @brief Halt hook: shuts the client down when a halt condition is reached. */
    void haltShutdown_() { this->shutdown(); }

    //-------------------------------------------------------------------------
    /** @brief Starts the next queued exchange if no connection is currently in progress (only one
		 *  connection at a time -- the classic ASIO model). Runs on the io thread. */
    void kick_exchange_() {
        if(exchanging_ || exchange_queue_.empty() || this->halt()) {
            return;
        }
        exchanging_ = true;
        outgoing_message_str_ = std::move(exchange_queue_.front());
        exchange_queue_.pop_front();
        n_reconnects_ = 0;
        start_connect_();
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Opens a fresh connection for the current exchange (outgoing_message_str_, already set by
	  * kick_exchange_). One exchange = one connection: resolve -> connect -> write request -> read
	  * response -> close. Also re-entered by the reconnect backoff with the SAME outgoing message.
	  */
    void start_connect_() {
        // Prepare a new socket. This will delete the old socket.
        socket_ptr_ = std::make_unique<boost::asio::ip::tcp::socket>(io_context_);

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
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when the async_resolve-operation has completed. On success it
	  * initiates the connection to the resolved endpoints; on error the client shuts down.
	  *
	  * @param ec Indicates a possible error
	  * @param results The results of the resolve-operation (the candidate endpoints)
	  */
    void when_resolved(boost::system::error_code ec, const resolver::results_type &results) {
        if(ec) {
            glogger << "In GAsioConsumerClientT<processable_type>::when_resolved():" << '\n'
                    << "Got ec(\"" << ec.message() << "\"). async_connect() will not be executed."
                    << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;

            // Terminate operation and return
            this->shutdown();
            return;
        }

        // Make the ASIO connection on the endpoint we get from a lookup
        auto self = this->shared_from_this();
        boost::asio::async_connect(
            *socket_ptr_,
            results.begin(),
            results.end(),
            [self](boost::system::error_code ec, [[maybe_unused]] const auto& unused) { self->when_connected(ec); }
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when the async_connect call has completed. On success it disables
	  * Nagle's algorithm and writes the request; on error it either retries (after a randomized async
	  * backoff) or, once max_reconnects_ is exceeded, shuts the client down.
	  *
	  * @param ec Indicates a possible error
	  */
    void when_connected(boost::system::error_code ec) {
        if(ec) {
            if(n_reconnects_++ >= max_reconnects_) { // Terminate the client
                glogger << "In GAsioConsumerClientT<processable_type>::when_connected():"
                        << '\n'
                        << n_reconnects_ << " / " << max_reconnects_
                        << " failed connection attempts" << '\n'
                        << "Got error code \"" << ec.message() << "\"." << '\n'
                        << "The client will terminate now." << '\n'
                        << GLOGGING;

                // Terminate operation and return
                this->shutdown();
            }
            else { // Try to reconnect after issuing a warning
                glogger << "In GAsioConsumerClientT<processable_type>::when_connected():"
                        << '\n'
                        << n_reconnects_ << " / " << max_reconnects_
                        << " failed connection attempts" << '\n'
                        << "Got error code \"" << ec.message() << "\"." << '\n'
                        << "We will try to reconnect" << '\n'
                        << GLOGGING;

                // Get rid of the old socket
                socket_ptr_.reset();

                // Back off a short while (randomly) before retrying the SAME exchange. The wait is an
                // async timer, NOT a blocking sleep: at depth > 1 other items are still being evaluated
                // on the compute pool and their result-returning connections must not be stalled.
                std::uniform_int_distribution<> dist(500, 1000);
                reconnect_timer_.expires_after(std::chrono::milliseconds(dist(rng_engine_)));
                auto self = this->shared_from_this();
                reconnect_timer_.async_wait([self](boost::system::error_code tec) {
                    if(tec) { // cancelled during teardown
                        return;
                    }
                    if(not self->halt()) {
                        self->start_connect_();
                    }
                });
            }

            return;
        }

        // Reset the number of connection attempts so we start at 0 next time
        n_reconnects_ = 0;

        // Disable Nagle's algorithm (TCP_NODELAY). Geneva's wire protocol is a strict, small
        // request/response exchange (PULL/WORK/RETURN). With Nagle enabled, a small segment
        // is held back until the previous one is ACKed; combined with the peer's delayed ACKs this
        // can add tens of milliseconds of latency to every round-trip for no throughput benefit
        // (there is no second small write to coalesce with). We therefore disable it. Best-effort:
        // a failure to set the option is not fatal.
        boost::system::error_code nd_ec;
        std::ignore = socket_ptr_->set_option(boost::asio::ip::tcp::no_delay(true), nd_ec); // failure deliberately tolerated

        // Send the command container off to the remote side
        auto self = this->shared_from_this();
        boost::asio::async_write(
            *socket_ptr_,
            boost::asio::buffer(outgoing_message_str_),
            [self](boost::system::error_code ec, std::size_t nBytesTransferred) {
                self->when_written(ec, nBytesTransferred);
            }
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Code to be executed when the entire message was sent to the remote side. Shuts the
	  * socket down in send direction and initiates the read of the server's response.
	  *
	  * @param ec Indicates a possible error
	  * @param nothing The number of bytes transferred (unused)
	  */
    void when_written(
        boost::system::error_code ec,
        [[maybe_unused]] std::size_t nothing
    ) {
        if(ec) {
            glogger << "In GAsioConsumerClientT<processable_type>::when_written():" << '\n'
                    << "Got ec(\"" << ec.message()
                    << "\"). async_start_read() will not be executed." << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;

            // Terminate operation and return
            this->shutdown();
            return;
        }

        // Shutdown the socket in send direction. This will result in an ec of boost::asio::error::eof
        // on the server side indicating that all data was written. Use the non-throwing overload:
        // the peer may already have closed, and a throw out of this completion handler would
        // unwind the io thread.
        boost::system::error_code sd_ec;
        std::ignore = socket_ptr_->shutdown(boost::asio::socket_base::shutdown_send, sd_ec); // best-effort

        // Clear the outgoing message -- no longer needed
        outgoing_message_str_.clear();

        // Initiate the read-sequence: Every transmission from client to server
        // should be answered, so we expect a response.
        auto self = this->shared_from_this();
        boost::asio::async_read(
            *socket_ptr_,
            boost::asio::dynamic_buffer(incoming_message_str_),
            [self](boost::system::error_code ec, std::size_t nBytesTransferred) {
                self->when_read(ec, nBytesTransferred);
            }
        );
    }

    //-------------------------------------------------------------------------
    /**
	  * Callback to be executed when all data was written. The server session
	  * is expected to shutdown its socket in send-direction, resulting in an
	  * ec of boost::asio::error::eof which we use as an indication that all
	  * data was received.
	  *
	  * @param ec A possible error code (boost::asio::error::eof is the expected end-of-transmission)
	  * @param nothing The number of bytes transferred (unused)
	  */
    void when_read(
        boost::system::error_code ec,
        [[maybe_unused]] std::size_t nothing
    ) {
        if(ec == boost::asio::error::eof) { // The expected outcome
            // Disconnect from the remote side by destroying the socket
            socket_ptr_.reset();

            // The exchange is complete: process the response (dispatch the item to the compute pool or
            // back off on NO_WORK), then start the next queued exchange.
            finish_exchange_();
        }
        else {
            if(ec) {
                glogger << "GAsioConsumerClientT<processable_type>::when_read(): " << '\n'
                        << "Leaving due to error code " << ec.message() << '\n'
                        << GLOGGING;
            }
            else {
                glogger << "GAsioConsumerClientT<processable_type>::when_read(): " << '\n'
                        << "No error code was received. Expected boost::asio::error::eof"
                        << '\n'
                        << "to indicate end of transmission." << '\n'
                        << GLOGGING;
            }

            // Terminate operation and return
            this->shutdown();
            return;
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Completes the current exchange: de-serializes the server's response and acts on it. A
	  * WORK item is handed to the compute pool (so the io thread stays free to run further exchanges
	  * while it evaluates); a NO_WORK is backed off and retried. The connection is now free, so the next
	  * queued exchange (if any) is started.
	  */
    void finish_exchange_() {
        exchanging_ = false;

        // De-serialize the response under the wire scope (blob send-once): a WORK item referencing a
        // blob by id resolves it against this client's local cache, or -- on a miss -- via fetch_blob,
        // which performs a synchronous BLOB_REQUEST/BLOB_REPLY round trip on its own socket (see
        // fetch_blob_). A malformed or truncated message makes this throw; that must not escape
        // into io_context::run() (it would unwind the client's only io thread).
        try {
            Gem::Courtier::wireDecode(
                incoming_message_str_,
                command_container_,
                &wire_ctx_,
                serialization_mode_
            );
        }
        catch(const std::exception &e) {
            glogger << "In GAsioConsumerClientT<processable_type>::finish_exchange_():" << '\n'
                    << "Could not de-serialize an incoming message:" << '\n'
                    << e.what() << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            this->shutdown();
            return;
        }

        // Clear the buffer, ready for the next exchange's response.
        incoming_message_str_.clear();

        // The exchange (a PULL or RETURN pull) has been answered.
        if(pending_pulls_ > 0) {
            --pending_pulls_;
        }

        const auto inbound_kind = command_container_.kind();
        switch(inbound_kind) {
            using enum Gem::Courtier::GFrameKind;
        case WORK:
            // Work arrived. Move it out (so command_container_ is free for the next exchange's response)
            // and evaluate it on the compute pool. One pull became one computation, so the in-flight
            // total is unchanged; no top-up needed. Start the next queued exchange.
            ++computing_;
            dispatch_compute_(std::move(command_container_));
            kick_exchange_();
            break;

        case NO_WORK:
            // No work available yet. The in-flight total dropped by one; back off (async timer, never a
            // blocking sleep) and top up later. Still start any queued RETURN exchange now.
            n_nodata_++;
            schedule_refill_();
            kick_exchange_();
            break;

        default:
            // An unknown/invalid frame is unrecoverable; log and shut down cleanly.
            glogger << "In GAsioConsumerClientT<processable_type>::finish_exchange_():" << '\n'
                    << "Got unknown or invalid frame " << Gem::Courtier::fkToStr(inbound_kind) << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            this->shutdown();
            return;
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Result-transmission hook driven by the pipeline base: queues a RETURN exchange that
		 *  returns the result AND fetches a replacement in one connection, then tops the pipeline up.
		 *
		 *  @param container The RETURN frame the base built from the evaluated work item */
    void sendResultAndRefill_(
        GCommandContainerT<processable_type> container
    ) {
        container.setPeerId(peer_id_); // announce our stable peer id (blob send-once)
        try {
            // Serialize the returned item under the wire scope so its (unchanged) blob is sent in full
            // to the server only the first time, by id thereafter (blob send-once).
            exchange_queue_.push_back(
                Gem::Courtier::wireEncode(container, &wire_ctx_, serialization_mode_)
            );
        }
        catch(const std::exception &e) {
            --pending_pulls_;
            glogger << "In GAsioConsumerClientT<processable_type>::sendResultAndRefill_(): " << e.what()
                    << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            this->shutdown();
            return;
        }
        kick_exchange_();
        maintain_(); // cover any deficit left by an earlier NO_WORK (a no-op when already at full depth)
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Worker-side cache-miss fetch (blob send-once): blocks until the server's blob for @p id is in
	  * hand, then returns the serialized blob (empty on failure). It opens its OWN short-lived,
	  * fully-synchronous connection (a separate socket on a throwaway io_context) and performs a
	  * BLOB_REQUEST -> BLOB_REPLY exchange that mirrors the normal one-shot request/response shape
	  * (write request, shutdown-send, read until eof). Using a dedicated connection keeps this off the
	  * main async pipeline: it does not touch socket_ptr_/exchange_queue_, so it is safe to call
	  * synchronously from inside finish_exchange_'s deserialize on the io thread (the io thread simply
	  * blocks for the brief round trip; the server answers a BLOB_REQUEST promptly from its registry).
	  *
	  * @param id The content id of the blob to fetch from the server.
	  * @return The serialized blob, or an empty string if the fetch failed.
	  */
    std::expected<std::string, std::string> fetch_blob_(const Gem::Courtier::GWireBlobId &id) {
        try {
            // This runs mid-decode (inside a genome load() under the work-item wire scope). The codec's
            // build/parse helpers each (de)serialize under a null scope so the BLOB_REQUEST/BLOB_REPLY
            // exchange does not recurse into the send-once context of the work item being decoded.
            const std::string request_str =
                Gem::Courtier::buildBlobRequest<processable_type>(id, peer_id_, serialization_mode_);

            // A self-contained synchronous exchange on its own io_context/socket.
            boost::asio::io_context fetch_ctx;
            boost::asio::ip::tcp::resolver fetch_resolver(fetch_ctx);
            const auto endpoints = fetch_resolver.resolve(address_, std::to_string(port_));
            boost::asio::ip::tcp::socket fetch_socket(fetch_ctx);
            boost::asio::connect(fetch_socket, endpoints);
            boost::system::error_code nd_ec;
            std::ignore = fetch_socket.set_option(boost::asio::ip::tcp::no_delay(true), nd_ec); // failure deliberately tolerated

            // Write the request, then half-close so the server sees end-of-request (eof), mirroring the
            // normal one-shot protocol.
            boost::asio::write(fetch_socket, boost::asio::buffer(request_str));
            boost::system::error_code sd_ec;
            std::ignore = fetch_socket.shutdown(boost::asio::socket_base::shutdown_send, sd_ec); // best-effort

            // Read the whole response (the server closes its send side at the end -> eof).
            std::string response_str;
            boost::system::error_code read_ec;
            boost::asio::read(fetch_socket, boost::asio::dynamic_buffer(response_str), read_ec);
            if(read_ec && read_ec != boost::asio::error::eof) {
                return std::unexpected("read error: " + read_ec.message());
            }

            // De-serialize the BLOB_REPLY (under a null scope) and pull out the blob. An empty blob
            // means a malformed/empty reply -- a failure, not a usable blob, so report it as such.
            std::string blob = Gem::Courtier::parseBlobReply<processable_type>(response_str, serialization_mode_);
            if(blob.empty()) {
                return std::unexpected(std::string("empty or malformed BLOB_REPLY"));
            }
            return blob;
        }
        catch(const std::exception &e) {
            return std::unexpected(std::string("fetch failed: ") + e.what());
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Shuts down the client: resets the socket, cancels all timers and releases the work
	  * guard so io_context::run() returns.
	  */
    void shutdown() {
        // Clear the socket and cancel the timers
        socket_ptr_.reset();
        halt_timer_.cancel();
        nodata_timer_.cancel();
        reconnect_timer_.cancel();
        // Reset the work object, so it no longer keeps the io_context alive
        work_.reset();
    }

    //-------------------------------------------------------------------------
    // Data

    std::unique_ptr<boost::asio::ip::tcp::socket> socket_ptr_; ///< Holds the current socket
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_ =
        boost::asio::make_work_guard(io_context_); ///< Keeps io_context.run() running
    resolver resolver_{io_context_};              ///< Helps to resolve the peer

    std::string address_; ///< The ip address or name of the peer system
    unsigned int port_;   ///< The peer port
    Gem::Common::serializationMode serialization_mode_ = Gem::Common::serializationMode::
        GEM_BINARY; ///< Determines which seriliztion mode should be used

    std::size_t n_reconnects_ = 0;
    std::size_t max_reconnects_ = 0;

    std::string incoming_message_str_; ///< Receives the current exchange's response
    std::string outgoing_message_str_; ///< Holds the current exchange's request (one exchange at a time)

    /// Serialized requests waiting for a connection (only one connection runs at a time, so completed
    /// evaluations and PULL top-ups queue here and are sent one after another).
    std::deque<std::string> exchange_queue_;
    bool exchanging_ = false; ///< Whether a connection cycle is currently in progress

    boost::asio::steady_timer reconnect_timer_{
        io_context_
    }; ///< Backoff timer for connection retries (async, never blocks the io thread)

    /// blob send-once (worker side): this client's local cache of received blobs, the wire
    /// context engaged around (de)serialisation, and the stable per-client peer id announced on every
    /// request. The context's fetch_blob resolves a cache miss via a synchronous side connection (see
    /// fetch_blob_). Declared before compute_pool_ so the pool (and its threads) are destroyed
    /// first -- a compute thread never touches these, but keep teardown order unsurprising.
    Gem::Courtier::GWirePeerId peer_id_ = 0;

};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Consumer-side handling of a single client connection. A new session is started for each
 * new connection and is shut down when the request has been served.
 *
 * It reads one request (PULL or RETURN), sources/sinks work items through the functors supplied
 * by the consumer, and writes the response. A per-session deadline timer reclaims the socket of a
 * stalled or half-open client.
 *
 * @tparam processable_type The work-item type exchanged with the client.
 */
template <typename processable_type>
class GAsioConsumerSessionT // NOLINT(cppcoreguidelines-special-member-functions)
  : public std::enable_shared_from_this<GAsioConsumerSessionT<processable_type>> {
public:
    //-------------------------------------------------------------------------
    /**
	  * @brief The main constructor for this class.
	  *
	  * @param io_context The io_context driving the asynchronous operations of this session
	  * @param socket The accepted client socket (ownership is taken by move)
	  * @param get_payload_item Functor that returns the next work item to hand to the client (empty pointer if none available)
	  * @param put_payload_item Functor that receives a RETURN frame handed back by the client
	  * @param check_server_stopped Functor returning true once the server is shutting down (suppresses new reads/sessions)
	  * @param serialization_mode The serialization format used on the wire
	  * @param sign_on Functor called with true on construction and false on destruction to track the active-session count
	  * @param wire_registry The consumer-shared blob send-once registry, or nullptr to disable the feature
	  * @param session_timeout The per-exchange connection deadline; a non-positive value disables it
	  */
    // NOLINTNEXTLINE(readability-function-size) -- one constructor wiring up a session's full collaborator set (socket, timers, four callbacks, serialization mode, wire registry, timeout); the parameter count is the session's dependency set, not splittable
    GAsioConsumerSessionT(
        boost::asio::io_context &io_context,
        boost::asio::ip::tcp::socket socket,
        std::move_only_function<std::unique_ptr<processable_type>()> get_payload_item,
        std::move_only_function<void(GCommandContainerT<processable_type> &&)> put_payload_item,
        std::move_only_function<bool()> check_server_stopped,
        Gem::Common::serializationMode serialization_mode,
        std::move_only_function<void(bool)> sign_on,
        Gem::Courtier::GWireBlobRegistry *wire_registry = nullptr,
        std::chrono::milliseconds session_timeout = std::chrono::milliseconds{300'000}
    )
      : socket_(std::move(socket))
      , strand_(io_context.get_executor())
      , deadline_timer_(io_context)
      , session_timeout_(session_timeout)
      , get_payload_item_(std::move(get_payload_item))
      , put_payload_item_(std::move(put_payload_item))
      , check_server_stopped_(std::move(check_server_stopped))
      , f_sign_on_(std::move(sign_on))
      , serialization_mode_(serialization_mode)
      , wire_registry_(wire_registry) {
        // Engage the server side of the blob send-once wire form, if a registry was supplied.
        // The peer is the announcing client's stable id, read from each request in process_request()
        // (ASIO has no persistent per-connection identity). With no registry the scope is never installed
        // and the genome falls back to its self-contained full-payload encoding.
        wire_ctx_.enabled = (wire_registry_ != nullptr);
        wire_ctx_.peer = 0; // overwritten per request from the announced peer id
        wire_ctx_.registry = wire_registry_;
        wire_ctx_.mode = serialization_mode_;

        // Announce that this session has become active (RAII-balanced with the destructor),
        // so the consumer can report the number of concurrently active sessions.
        if(f_sign_on_) {
            f_sign_on_(true);
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief The destructor signs the session off again, so the active-session count stays accurate
	  * however the session ends (served request, error or disconnect).
	  */
    ~GAsioConsumerSessionT() {
        if(f_sign_on_) {
            f_sign_on_(false);
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Starts the read-write cycle as the main purpose of this class. Disables Nagle's
	  * algorithm, arms the deadline timer and initiates the first asynchronous read.
	  */
    void async_start_run() {
        // Disable Nagle's algorithm on the accepted connection: the request/response messages
        // are small and latency-sensitive.
        boost::system::error_code nd_ec;
        std::ignore = socket_.set_option(boost::asio::ip::tcp::no_delay(true), nd_ec); // failure deliberately tolerated

        // Arm the deadline so a client that connects but never completes its request cannot pin the
        // socket/fd forever, then initiate the read session -- we expect an incoming message.
        arm_deadline();
        async_start_read();
    }

    //-------------------------------------------------------------------------
    // Deleted constructors and assignment operators

    GAsioConsumerSessionT() = delete;
    GAsioConsumerSessionT(const GAsioConsumerSessionT<processable_type> &) = delete;
    GAsioConsumerSessionT(GAsioConsumerSessionT<processable_type> &&) = delete;
    GAsioConsumerSessionT<processable_type> &
    operator=(const GAsioConsumerSessionT<processable_type> &) = delete;
    GAsioConsumerSessionT<processable_type> &
    operator=(GAsioConsumerSessionT<processable_type> &&) = delete;

private:
    //-------------------------------------------------------------------------
    /** @brief Arms the per-session deadline timer. On expiry the socket is closed, which aborts the
	 *  outstanding read/write so the session (and its file descriptor) is released. */
    void arm_deadline() {
        if(session_timeout_ <= std::chrono::milliseconds::zero()) {
            return; // per-exchange deadline disabled by configuration
        }
        deadline_timer_.expires_after(session_timeout_);
        auto self = this->shared_from_this();
        deadline_timer_.async_wait(
            boost::asio::bind_executor(strand_, [self](boost::system::error_code ec) {
                self->on_deadline(ec);
            })
        );
    }

    //-------------------------------------------------------------------------
    /** @brief Deadline-timer callback: closes a stalled connection so its socket/fd is reclaimed.
		 *
		 *  @param ec A possible error code; a non-empty code means the timer was cancelled (the session completed normally) */
    void on_deadline(boost::system::error_code ec) {
        if(ec) { // the timer was cancelled because the session completed normally
            return;
        }
        if(not check_server_stopped_()) {
            glogger << "GAsioConsumerSessionT<processable_type>::on_deadline(): " << '\n'
                    << "Connection exceeded the per-session timeout; closing it to reclaim the socket."
                    << '\n'
                    << GLOGGING;
        }
        boost::system::error_code ignore;
        std::ignore = socket_.close(ignore); // aborts the outstanding read/write -> the session ends (best-effort)
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Starts an asynchronous read session, whose termination is signalled by
	  * a call to the when_read()-function.
	  */
    void async_start_read() {
        if(check_server_stopped_()) {
            return;
        }

        auto self = this->shared_from_this();
        boost::asio::async_read(
            socket_,
            boost::asio::dynamic_buffer(incoming_message_str_),
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
	  * A function to be called when all data has been read from the socket.
	  * This is signalled by an eof, as the sender closes its socket in
	  * send-direction.
	  *
	  * @param ec Indicates possible error conditions (boost::asio::error::eof is the expected end of the request)
	  * @param nothing The number of bytes transferred (unused)
	  */
    void when_read(
        boost::system::error_code ec,
        [[maybe_unused]] std::size_t nothing
    ) {
        if(ec ==
           boost::asio::error::
               eof) { // The expected outcome, when the client has shut down its socket in send direction
            // Deal with the message and send a response back
            async_start_write(process_request());
        }
        else {
            // operation_aborted is the expected result of on_deadline() closing a stalled socket --
            // not an error worth logging.
            if(ec && ec != boost::asio::error::operation_aborted) {
                glogger << "GAsioConsumerSessionT<processable_type>::when_read(): " << '\n'
                        << "Leaving due to error code " << ec.message() << '\n'
                        << "Server session will terminate" << '\n'
                        << GLOGGING;
            }
            else if(not ec) {
                glogger << "GAsioConsumerSessionT<processable_type>::when_read(): " << '\n'
                        << "No ec received but expected boost::asio::error::eof" << '\n'
                        << "Server session will terminate" << '\n'
                        << GLOGGING;
            }
            // The session ends here (no write follows); cancel the deadline so it does not keep the
            // session object alive until expiry.
            deadline_timer_.cancel();
        }
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Asynchronously sends a response to the client.
	  *
	  * @param message The serialized message to be sent to the client (stored for the duration of the async write)
	  */
    void async_start_write(std::string message) {
        // We need to persist the message for asynchronous operations
        outgoing_message_str_ = std::move(message);

        // Return an answer
        auto self = this->shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(outgoing_message_str_),
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
	  * A callback to be executed when all data was sent to the client (or when
	  * an error has occurred).
	  *
	  * @param ec Indicates a possible error condition
	  * @param nothing The number of bytes transferred (unused)
	  */
    void when_written(
        boost::system::error_code ec,
        [[maybe_unused]] std::size_t nothing
    ) {
        // operation_aborted is the expected result of on_deadline() closing a stalled socket.
        if(ec && ec != boost::asio::error::operation_aborted) {
            glogger << "GAsioConsumerSessionT<processable_type>::when_written(): " << '\n'
                    << "Got error code " << ec.message() << '\n'
                    << GLOGGING;
        }

        // The exchange is complete: cancel the deadline so it does not keep the session alive.
        deadline_timer_.cancel();

        // Shutdown the socket in send direction. This will result in an ec of boost::asio::error::eof
        // on the client-side indicating that all data was written. Non-throwing overload: a throw
        // out of this completion handler would unwind the io thread.
        boost::system::error_code sd_ec;
        std::ignore = socket_.shutdown(boost::asio::socket_base::shutdown_send, sd_ec); // best-effort

        // Clear the outgoing message string, no longer needed
        outgoing_message_str_.clear();
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Steps to be taken when a request was received from the client: de-serializes the
	  * frame, acts on it (PULL serves an item, RETURN sinks the returned result and serves the next)
	  * and produces the response.
	  *
	  * @return Serialized data to be sent to the client as a response to the request (empty on error or unknown frame)
	  */
    std::string process_request() {
        try {
            // De-serialize the request under the wire scope (blob send-once): a returned work item may
            // reference its blob by id, which is resolved against this consumer's shared registry (the
            // server holds every blob it has sent). The scope's peer does not matter for decoding (only
            // for re-encoding), so it is left at its default here and set from the announced id below.
            Gem::Courtier::wireDecode(
                incoming_message_str_,
                command_container_,
                wire_ctx_.enabled ? &wire_ctx_ : nullptr,
                serialization_mode_
            ); // may throw

            // Clear the buffer, so we may later fill it with data to be sent
            incoming_message_str_.clear();

            // Learn the announcing client's stable peer id (ASIO has no persistent per-connection
            // identity) and use it as the wire peer for the response, so the send-once tracking keys on
            // the client across its many short connections.
            wire_ctx_.peer = command_container_.peerId();

            // Act on the frame and produce the response (shared synchronous server dispatch:
            // PULL serves an item, RETURN sinks the returned result and serves the next, BLOB_REQUEST
            // answers from the registry). The WORK/NO_WORK response is serialized under the wire scope
            // (peer set above) so a work item's blob is sent send-once.
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
            glogger
                << "GAsioConsumerSessionT<processable_type>::process_request(): Caught exception: "
                << e.what() << '\n'
                << GLOGGING;
        }
        catch(
            ...
        ) { // session ends, caller retries
            glogger
                << "GAsioConsumerSessionT<processable_type>::process_request(): Caught non-std exception"
                << '\n'
                << GLOGGING;
        }

        // Make the compiler happy
        return {};
    }

    //-------------------------------------------------------------------------
    // Data

    std::string incoming_message_str_;
    std::string outgoing_message_str_;

    boost::asio::ip::tcp::socket socket_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;

    /// Bounds the lifetime of a single request/response exchange. Because this is a
    /// one-connection-per-request transport and the client sends its request promptly on connect
    /// (it closes the connection BEFORE evaluating a work item, so this never overlaps computation),
    /// a connection still open after this long is a stalled/half-open/dead client and is closed --
    /// otherwise its socket+fd would be pinned forever by the never-completing read, eventually
    /// exhausting the server's file descriptors. This is the per-EXCHANGE deadline, NOT the evaluation
    /// timeout; it is configurable (GNetworkedTimeoutConfig::session_timeout_ms, threaded in via the
    /// consumer), and a non-positive value disables it (arm_deadline() then never arms the timer).
    boost::asio::steady_timer deadline_timer_;
    std::chrono::milliseconds session_timeout_{300'000};

    std::move_only_function<std::unique_ptr<processable_type>()> get_payload_item_;
    std::move_only_function<void(GCommandContainerT<processable_type> &&)> put_payload_item_;
    std::move_only_function<bool()> check_server_stopped_;
    std::move_only_function<void(bool)> f_sign_on_; ///< Signs the session on (true) / off (false) with the consumer

    Gem::Common::serializationMode serialization_mode_ = Gem::Common::serializationMode::GEM_BINARY;

    GCommandContainerT<processable_type> command_container_{
        GFrameKind::NONE
    }; ///< Holds the current frame and its payload (if any)

    /// blob send-once (server side): the consumer-shared registry (not owned) and the wire
    /// scope installed around (de)serialisation. wire_registry_ is null when the feature is disabled, in
    /// which case the scope is never installed and the genome uses its self-contained full encoding. The
    /// scope's peer is set per request from the announcing client's stable id (ASIO one-shot connections
    /// have no persistent per-connection identity).
    Gem::Courtier::GWireBlobRegistry *wire_registry_ = nullptr;
    Gem::Courtier::GWireSerializationContext wire_ctx_;

    //-------------------------------------------------------------------------
};


/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
