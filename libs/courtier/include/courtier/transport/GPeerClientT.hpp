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
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// Boost headers go here
#include <boost/asio/post.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GLogger.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GCourtierHelperFunctions.hpp"
#include "courtier/GWireCodec.hpp" // scope-wrapped (de)serialization
#include "courtier/GWireSerializationContext.hpp"
#include "courtier/transport/GPeerI.hpp"              // the connection and how to get one
#include "courtier/transport/GPrefetchingClientT.hpp" // the client's work pipeline

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/** @brief How a GPeerClientT is set up. */
struct GPeerClientOptions {
    std::string name = "GPeerClientT<processable_type>"; ///< Who this client is, for its log lines
    Gem::Common::serializationMode mode =
        Gem::Common::serializationMode::GEM_BINARY; ///< The wire format
    /// Maximum number of work items kept in flight at once (0 is treated as 1)
    std::size_t prefetchDepth = 1;
    /// How many items are evaluated at the same time (0: as many as the prefetch depth)
    std::size_t computeWidth = 0;
    /// At most one request is unanswered at any time, and a result that finishes while one is
    /// unanswered travels as a RESULT (see the class comment). Meant for a depth of 2 with a
    /// compute width of 1: one item is evaluated and at most one more is held or asked for.
    bool lookAhead = false;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The client side of the networked consumer protocol, over any transport that offers a
 * GConnectorI. The transport makes the connection; this class gives the messages their meaning.
 *
 * Threads. The io thread runs the connection's handlers and keeps the books (requests without an
 * answer, items held and items evaluated; GPrefetchingClientT); every decision about an item is
 * taken there. It neither decodes nor encodes an item: messages are decoded on a strand of the
 * compute pool, and each result is encoded on the compute thread that evaluated it. However long
 * that takes, the io thread stays free for the connection (GPeerI.hpp, rule 2).
 *
 * Without look-ahead the protocol is that of the websocket client: PULL and RETURN are both
 * requests the server answers with one WORK (or NO_WORK), and the client keeps as many items in
 * flight as its prefetch depth says.
 *
 * Look-ahead. A client at depth 2 without it has two requests with the server whenever it is
 * idle, and when work is scarce a woken session hands it two items while other clients idle. With
 * look-ahead the client has at most ONE request that is not answered:
 *  - a PULL is sent when nothing is requested and fewer items are in flight than the depth allows
 *    (at the start, after a NO_WORK, and when a WORK arrives);
 *  - a finished result is sent as RETURN (which asks for the next item) if no request is
 *    unanswered, and otherwise as RESULT: an outcome that asks for nothing.
 * In steady state this is two messages per item.
 *
 * Which of the two a result is, is decided on the io thread when its bytes arrive there. The
 * compute thread encodes it as the io thread last expected (resultKindHint_()); if that has
 * changed meanwhile and the encoded result would ask although the client may not, the result is
 * encoded once more, as a RESULT, on the compute pool, and sent after that. A result encoded as a
 * RESULT where a RETURN would have been allowed is sent as it is; a PULL follows it.
 *
 * Halting. Once a halt condition holds (a time or an item limit, a close request), the client
 * asks for nothing more and starts no evaluation: the items it holds, and those that are still
 * being decoded or arrive, are dropped (the server puts them back when the connection ends). The
 * results of the evaluations that run are sent as RESULT as they finish, and the client closes when
 * none runs any more. So the number of items processed may pass an item limit by the evaluations
 * that were running when it was reached. While its compute pool evaluates an item the client tells
 * its peer that it is busy (GPeerI::setOwnerBusy()).
 *
 * A message it cannot decode is reported to the server with UNREADABLE (after the pause that follows
 * a NO_WORK), which asks for the next
 * item in place of the request whose answer was lost: the server puts back what the client held and
 * sends it its blobs in full again (the item that could not be read may have carried one). The
 * client stays, whatever its connector.
 *
 * @tparam processable_type The work-item type exchanged with the server (must be processable)
 */
template <typename processable_type>
class GPeerClientT
  : public GPrefetchingClientT<GPeerClientT<processable_type>, processable_type>
  , public std::enable_shared_from_this<GPeerClientT<processable_type>> {
    // The pipeline base drives the compute dispatch / refill / halt machinery and calls the
    // private transport hooks (see GPrefetchingClientT).
    friend class GPrefetchingClientT<GPeerClientT<processable_type>, processable_type>;

    using pipeline_base = GPrefetchingClientT<GPeerClientT<processable_type>, processable_type>;

    // Members and helpers of the dependent pipeline base, pulled into scope
    using pipeline_base::compute_pool_;
    using pipeline_base::computing_;
    using pipeline_base::dispatch_message_;
    using pipeline_base::drop_held_;
    using pipeline_base::in_flight_;
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
     * @brief Builds the client on a connector. The client must be held by a std::shared_ptr.
     *
     * @param connector How a connection to the server is made; the client owns it. Must not be null.
     * @param options The settings of this client
     */
    GPeerClientT(std::unique_ptr<GConnectorI> connector, GPeerClientOptions options)
      : pipeline_base(options.name, options.prefetchDepth, options.computeWidth)
      , options_(std::move(options))
      , connector_(std::move(connector)) {
        // Engage the blob send-once wire form. The client caches every blob it receives, keyed by
        // content id, so an id-only work item resolves locally. No fetch_blob is installed: the
        // server sends the full blob inline on the first item of each connection and delivery is
        // ordered, so an id-only item is only ever seen after its blob has been received and
        // cached -- a miss would indicate a protocol error, which load() surfaces by throwing.
        wire_ctx_.enabled = true;
        wire_ctx_.peer = 0; // the single upstream server
        wire_ctx_.registry = &wire_registry_;
        wire_ctx_.mode = options_.mode;
        // A worker never interns: it does not own the registry the server resolves against, so a
        // blob it echoes back must always travel inline (see may_intern_blobs).
        wire_ctx_.may_intern_blobs = false;
    }

    //-------------------------------------------------------------------------
    /** @brief The destructor */
    ~GPeerClientT() override = default;

    //-------------------------------------------------------------------------
    // Deleted functions
    GPeerClientT() = delete;
    GPeerClientT(const GPeerClientT<processable_type> &) = delete;
    GPeerClientT(GPeerClientT<processable_type> &&) = delete;
    GPeerClientT<processable_type> &operator=(const GPeerClientT<processable_type> &) = delete;
    GPeerClientT<processable_type> &operator=(GPeerClientT<processable_type> &&) = delete;

    //-------------------------------------------------------------------------
    /** @brief How the connection ended, once it has. To be read after run() has returned.
     *  @return The reason given by the peer; FAILED while there was no connection */
    [[nodiscard]] GPeerEnd connectionEnd() const noexcept {
        return end_;
    }
    /** @brief How often a NO_WORK answer was received. To be read after run() has returned.
     *  @return The count */
    [[nodiscard]] std::uint64_t noWorkCount() const noexcept {
        return n_nodata_;
    }

private:
    //-------------------------------------------------------------------------
    /**
     * @brief The main run-loop: asks the connector for a connection, runs the io_context until no
     * work remains, then makes sure the connection is over.
     */
    void run_() override {
        auto self = this->shared_from_this();
        connector_->connect(
            io_context_.get_executor(),
            [self](const std::shared_ptr<GPeerI> &peer, std::string_view error) {
                self->when_connected_(peer, error);
            }
        );

        // This call will block until no more work remains in the ASIO work queue
        io_context_.run();

        // The io context has stopped, so nothing can be waited for any more. A peer that is still
        // there is ended at once (after an orderly close it has ended already). This also takes
        // this client out of the peer's handlers.
        if(auto peer = peer_.lock()) {
            peer->abort();
        }

        glogger << options_.name << "::run_(): Client session has terminated" << '\n' << GLOGGING;
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Called when the connector is done: starts the peer, which owns this client through
     * its handlers for as long as the connection lasts; the client refers to the peer weakly.
     *
     * @param peer The connection, not started yet; empty if none could be made
     * @param error Why there is no connection
     */
    void when_connected_(const std::shared_ptr<GPeerI> &peer, std::string_view error) {
        if(not peer) {
            glogger << "In " << options_.name << "::when_connected_():" << '\n'
                    << "No connection to the server: " << error << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;
            return;
        }

        auto self = this->shared_from_this();
        peer_ = peer;
        peer->start(
            GPeerHandlers{
                .onOpen = [self] { self->when_open_(); },
                .onMessage =
                    [self](std::string &&message) { self->dispatch_message_(std::move(message)); },
                .onClosed =
                    [self](GPeerEnd end, GPeerClose how, std::string_view detail) {
                        self->when_closed_(end, how, detail);
                    }
            }
        );
    }

    //-------------------------------------------------------------------------
    /** @brief Called when the connection is usable: starts the halt timer and asks for work. */
    void when_open_() {
        start_halt_timer();
        request_more_();
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Called once when the connection is over, for whatever reason. Stops the pipeline's
     * timers, so that io_context::run() -- and with it the client -- can return.
     *
     * @param end Why the connection ended
     * @param how What the server said, if it closed in the regular way
     * @param detail The transport's own words about it
     */
    void when_closed_(GPeerEnd end, GPeerClose how, std::string_view detail) {
        end_ = end;
        if(how == GPeerClose::RUN_ENDED) {
            // The server has announced the end of the run: this client is done, whatever else is
            // set on it.
            this->flagCloseRequested();
            glogger << options_.name << ": the server announced the end of the run" << '\n'
                    << GLOGGING;
        }
        else if(end != GPeerEnd::CLOSED_HERE) {
            glogger << "In " << options_.name << ": the connection to the server has ended:" << '\n'
                    << detail << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;
        }
        if(how == GPeerClose::RUN_ENDED || not connector_->reconnectable()) {
            // No result can be delivered any more: items that wait for the compute pool, or are
            // still being decoded, are not evaluated (one that is being evaluated runs to its end).
            drop_held_();
        }
        this->stopPipelineTimers_();
    }

    //-------------------------------------------------------------------------
    /** @brief Called on the io thread when a WORK item has been counted (see
     *  GPrefetchingClientT::decoded_()): with look-ahead the next item is asked for now. */
    void when_work_() {
        if(options_.lookAhead) {
            request_more_();
        }
        else {
            this->refreshResultKindHint_();
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Called on the io thread for a decoded message other than WORK. NO_WORK: the request
     * was refused; back off, then ask again (an async timer: other items may still be computing and
     * their results must not be stalled on the io thread). Anything else is unrecoverable.
     *
     * @param kind The kind of the frame decoded
     */
    void when_decoded_(Gem::Courtier::GFrameKind kind) {
        if(kind == Gem::Courtier::GFrameKind::NO_WORK) {
            n_nodata_++;
            this->refreshResultKindHint_();
            schedule_refill_();
            return;
        }
        glogger << "In " << options_.name << "::when_decoded_():" << '\n'
                << "Received invalid frame " << fkToStr(kind) << '\n'
                << "The client will shut down." << '\n'
                << GWARNING;
        do_close_(GPeerClose::ERROR);
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Called on the io thread for a message that could not be decoded: it answered a request,
     * and the client says so with UNREADABLE, which is a request again (the number of unanswered
     * requests stays as it is). What identifies the item travels inside the item that failed to
     * load, so the server puts back everything the client held. The UNREADABLE is sent after the
     * pause that follows a NO_WORK: an item that no load can read must not make the client and the
     * server pass it back and forth as fast as they can. A client that halts asks for nothing
     * more: for it the request is answered.
     *
     * @param error Why the message could not be decoded
     */
    void when_unreadable_(const std::string &error) {
        glogger << "In " << options_.name << ":" << '\n'
                << "A message from the server could not be decoded:" << '\n'
                << error << '\n'
                << "The client tells the server after a pause and asks again." << '\n'
                << GWARNING;
        if(this->halt()) {
            if(pending_pulls_ > 0) {
                --pending_pulls_;
            }
            this->refreshResultKindHint_();
            return;
        }
        ++n_unreadable_owed_;
        schedule_refill_();
    }

    //-------------------------------------------------------------------------
    /**
     * Issues PULL frames for as long as fewer items are in flight than the prefetch depth -- and,
     * with look-ahead, for as long as no request is unanswered, which makes it at most one. Runs on
     * the io thread.
     */
    void request_more_() {
        while(not this->halt() && in_flight_() < prefetch_depth_ &&
              not(options_.lookAhead && pending_pulls_ > 0)) {
            ++pending_pulls_;
            const GCommandContainerT<processable_type> pull{GFrameKind::PULL};
            send_command_(pull);
        }
        this->refreshResultKindHint_();
    }

    //-------------------------------------------------------------------------
    /** @brief Serializes @p container (a PULL or an UNREADABLE: nothing to speak of) and hands it
     *  to the peer,
     *  guarding against serialization failures (which must not unwind the io thread). Nothing is
     *  sent once the connection is gone.
     *
     *  @param container The command container to serialize and transmit */
    void send_command_(const GCommandContainerT<processable_type> &container) {
        auto peer = peer_.lock();
        if(not peer) {
            return;
        }
        try {
            peer->send(Gem::Courtier::wireEncode(container, &wire_ctx_, options_.mode));
        }
        catch(const std::exception &e) {
            glogger << "In " << options_.name << "::send_command_():" << '\n'
                    << "Could not serialize the outgoing message:" << '\n'
                    << e.what() << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            do_close_(GPeerClose::ERROR);
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Closes the connection to the server and stops the pipeline's timers. A result that
     * is still being encoded once more is sent first: the close waits for it. The peer closes
     * asynchronously and within its time limit; when_closed_() follows. Idempotent.
     *
     * @param how What the server is told
     */
    void do_close_(GPeerClose how) {
        // Stop the halt-poll and NO_WORK-backoff timers for good, so io_context::run() can return.
        this->stopPipelineTimers_();
        if(n_reencoding_ > 0) {
            close_after_reencoding_ = how;
            close_pending_ = true;
            return;
        }
        if(auto peer = peer_.lock()) {
            peer->close(how);
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Whether a finished result may ask for the next item: no halt condition holds and,
     *  with look-ahead, no other request is unanswered. On the io thread.
     *  @return true if it may be a RETURN */
    [[nodiscard]] bool mayAsk_() {
        return not this->halt() && not(options_.lookAhead && pending_pulls_ > 0);
    }

    /** @brief Publishes what mayAsk_() says now, for the compute threads (resultKindHint_()). */
    void refreshResultKindHint_() {
        may_ask_hint_.store(this->mayAsk_(), std::memory_order_relaxed);
    }

    //-------------------------------------------------------------------------
    // The transport hooks the pipeline base drives (see GPrefetchingClientT)

    /** @brief Pipeline-refill hook: sends the UNREADABLE frames that are due (see
     *  when_unreadable_()), then issues PULL frames (see request_more_()). */
    void refill_() {
        for(; n_unreadable_owed_ > 0; --n_unreadable_owed_) {
            const GCommandContainerT<processable_type> unreadable{GFrameKind::UNREADABLE};
            send_command_(unreadable);
        }
        request_more_();
    }

    /** @brief The kind a finished result is encoded with, as the io thread last expected it.
     *  From any thread (a compute thread asks).
     *  @return RETURN or RESULT */
    [[nodiscard]] GFrameKind resultKindHint_() const {
        return may_ask_hint_.load(std::memory_order_relaxed) ? GFrameKind::RETURN
                                                             : GFrameKind::RESULT;
    }

    /**
     * @brief Result hook, on the io thread: decides whether the finished result asks for the next
     * item (RETURN) or not (RESULT), sends it -- encoding it once more on the compute pool if it
     * was encoded as a RETURN and may not ask -- and then either closes (a halt condition holds) or
     * asks for more.
     *
     * @param bytes The result as the compute thread encoded it
     * @param error Why it could not be encoded; empty if it could
     * @param frame The result's frame (its kind says how it was encoded)
     */
    void sendResult_(
        std::string bytes,
        const std::string &error,
        GCommandContainerT<processable_type> frame
    ) {
        if(not error.empty()) {
            glogger << "In " << options_.name << ": could not serialize a result:" << '\n'
                    << error << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            do_close_(GPeerClose::ERROR);
            return;
        }

        const bool may_ask = this->mayAsk_();
        if(frame.kind() == GFrameKind::RETURN && not may_ask) {
            // Encoded as a request, but the client may not ask now: encode it as a RESULT.
            this->reencodeAsResult_(std::move(frame));
            return;
        }
        if(frame.kind() == GFrameKind::RETURN) {
            ++pending_pulls_; // the RETURN is a request (the server replies with an item)
        }
        this->transmit_(std::move(bytes));
        this->afterResult_();
    }

    /** @brief Encodes @p frame as a RESULT on the compute pool and sends it on the io thread.
     *  @param frame The finished result's frame */
    void reencodeAsResult_(GCommandContainerT<processable_type> frame) {
        ++n_reencoding_;
        auto self = this->shared_from_this();
        auto guard = boost::asio::make_work_guard(io_context_);
        boost::asio::post(
            compute_pool_,
            [self, frame = std::move(frame), guard = std::move(guard)]() mutable {
                frame.setKind(GFrameKind::RESULT);
                std::string error;
                std::string bytes = self->encodeResult_(frame, error);
                // The pool's handler gives up its hold on the client here: if it kept it until it is
                // destroyed, after the io thread may have finished with the client, the client could be
                // destroyed on a thread of its own pool, which cannot join itself.
                auto &io = self->io_context_;
                boost::asio::post(
                    io,
                    [self = std::move(self),
                     bytes = std::move(bytes),
                     error = std::move(error),
                     guard = std::move(guard)]() mutable {
                        self->reencoded_(std::move(bytes), error);
                    }
                );
            }
        );
    }

    /** @brief On the io thread, once a result has been encoded as a RESULT once more: sends it,
     *  then carries out a close that waited for it, or goes on.
     *  @param bytes The RESULT @param error Why it could not be encoded; empty if it could */
    void reencoded_(std::string bytes, const std::string &error) {
        --n_reencoding_;
        if(not error.empty()) {
            glogger << "In " << options_.name << ": could not serialize a result:" << '\n'
                    << error << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            do_close_(GPeerClose::ERROR);
            return;
        }
        this->transmit_(std::move(bytes));
        if(close_pending_ && n_reencoding_ == 0) {
            close_pending_ = false;
            do_close_(close_after_reencoding_);
            return;
        }
        this->afterResult_();
    }

    /** @brief After a result has been sent: a client that halts closes once no evaluation runs
     *  any more (see the class comment), any other asks for more. */
    void afterResult_() {
        if(this->halt()) {
            this->closeWhenIdle_();
            return;
        }
        request_more_();
    }

    /** @brief A halt condition holds: what is held is dropped, and the connection is closed if
     *  no evaluation runs; otherwise the last result to arrive closes it (afterResult_()). */
    void closeWhenIdle_() {
        drop_held_();
        if(computing_ == 0) {
            do_close_(GPeerClose::NORMAL);
        }
    }

    /** @brief Hands a message to the peer, if there is one (io thread). @param bytes The message */
    void transmit_(std::string bytes) {
        if(auto peer = peer_.lock()) {
            peer->send(std::move(bytes));
        }
    }

    /** @brief Halt hook (the halt timer): closes the connection in the regular way when a halt
     *  condition is reached, after the evaluations that run (see the class comment). */
    void haltShutdown_() {
        this->closeWhenIdle_();
    }

    /** @brief Busy hook: tells the peer whether the compute pool evaluates an item.
     *  @param busy Whether it does */
    void when_busy_(bool busy) {
        if(auto peer = peer_.lock()) {
            peer->setOwnerBusy(busy);
        }
    }

    //-------------------------------------------------------------------------
    // Data

    GPeerClientOptions options_; ///< The settings of this client

    /// How connections are made. Declared in this class, so that it is destroyed before the
    /// io context of the pipeline base (see GConnectorI).
    std::unique_ptr<GConnectorI> connector_;
    std::weak_ptr<GPeerI> peer_; ///< The connection, once there is one

    GPeerEnd end_ = GPeerEnd::FAILED; ///< How the connection ended (io thread; read after run())

    /// What mayAsk_() said last on the io thread; read by the compute threads
    std::atomic<bool> may_ask_hint_{true};
    std::size_t n_reencoding_ = 0; ///< Results being encoded once more (io thread)
    /// Messages that could not be read and are still to be reported with UNREADABLE (io thread)
    std::size_t n_unreadable_owed_ = 0;
    bool close_pending_ = false;   ///< A close waits for them (io thread)
    GPeerClose close_after_reencoding_ = GPeerClose::NORMAL; ///< What that close says

    //-------------------------------------------------------------------------
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
