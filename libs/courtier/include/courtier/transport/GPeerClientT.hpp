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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h> // getpid()

// Boost headers go here
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GLogger.hpp"
#include "common/GSerializationHelperFunctionsT.hpp" // sharedPtrToString()
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
    /// n: how many work items the client holds at once -- the one(s) being evaluated and those
    /// fetched ahead, so that the transfer of the next item overlaps the evaluation of the current
    /// one (0 is treated as 1). How many of them are evaluated at the same time is
    /// parallelEvaluations.
    std::size_t prefetchDepth = 1;
    /// m: how many of the items it holds (prefetchDepth) the client evaluates at the same time; 1
    /// by default, whatever the depth. More than prefetchDepth is refused when the client is
    /// built, and so is 0.
    std::size_t parallelEvaluations = 1;
    /// At most one request is unanswered at any time, and a result that finishes while one is
    /// unanswered travels as a RESULT (see the class comment). Meant for a depth of 2 with one
    /// evaluation at a time: one item is evaluated and at most one more is held or asked for.
    bool lookAhead = false;
    /// How long the client tries to reach a server before its first contact with one; 0: for as
    /// long as it runs (see the class comment)
    std::chrono::milliseconds firstContactLimit = GCLIENTFIRSTCONTACTLIMIT;
    /// How long the client tries to reach a server again after it has lost its connection,
    /// counted from the end of the last connection on which it had contact; 0: for as long as it
    /// runs (see the class comment)
    std::chrono::milliseconds reconnectLimit = GCLIENTRECONNECTLIMIT;
    /// How long a connection without an answer must have been open to count as contact
    std::chrono::milliseconds contactTime = GCLIENTCONTACTTIME;
    /// Where a finished result that can no longer be delivered is saved, one file per evaluated
    /// item (see the class comment); the client's working directory by default
    std::filesystem::path undeliveredDir = ".";
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
 * Connecting. The client asks its connector for a connection when it runs; an attempt that fails
 * is followed by another after a pause (GCLIENTRECONNECTPAUSEMIN, doubling up to
 * GCLIENTRECONNECTPAUSEMAX), for as long as no halt condition holds. A client may therefore be
 * started before its server. Contact with a server is an answer from it, or a connection that has
 * been open for GPeerClientOptions::contactTime (a server that has no work answers nothing). A
 * client that has had no contact within GPeerClientOptions::firstContactLimit (0: no limit) gives
 * up: it flags a terminal error and ends. The limit is looked at while the client has no
 * connection; a connection that is open is not held against it. A connector that cannot connect
 * again (GConnectorI::reconnectable()) gets one attempt.
 *
 * A lost connection. A connection that ends without the end of the run and without a halt (the
 * server closed it, reset it, fell silent and was cut by the idle limit) is lost; the client
 * connects again, as above, until GPeerClientOptions::reconnectLimit has passed since the end of
 * the last connection on which it had contact (0: no limit). What it held is kept: items held for
 * the compute pool and items whose answer is still being decoded are evaluated, and their results
 * go to the next connection. What the server may not have read is sent again there, in order:
 * every message the client hands to its peer stays in a list (unanswered_) until it is known to
 * have been read. The server answers every request it reads (PULL, RETURN, UNREADABLE) with one
 * WORK or NO_WORK, in order, and nothing else (a RESULT asks for nothing): an answer proves the
 * oldest request without one, and every message sent before it. At a loss the requests that asked
 * for nothing but work are dropped (the old session gives back what they asked for); RETURN and
 * RESULT stay and are sent again first on the next connection. A result that has been sent
 * GCLIENTRESENDLIMIT times on connections that were lost is given up; its item is handed out
 * again by the server. The loss is booked only after the messages of the lost connection that are
 * still being decoded have been (LinkState::DRAINING): an answer that arrived before the loss
 * proves what it proves. A result that arrives twice at the server (it had read it after all) is
 * dropped there as a duplicate. The new peer is told whether the client is busy.
 *
 * Results that cannot be delivered. An evaluation is never cut short (it may be an external
 * process). A client that reaches a halt while it has no connection -- a halt condition, a limit
 * without a server, a lost connection of a connector that cannot connect again -- tries no more:
 * it lets its evaluations end (LinkState::FINISHING), then makes one last attempt (bounded by the
 * transport's own limits) to deliver what it has finished. Whatever it still cannot deliver --
 * also a result given up at the resend limit, a result sent before a close of its own that the
 * server did not confirm (GPeerEnd::CLOSED_HERE_UNCONFIRMED: only a confirmed close, CLOSED_HERE,
 * proves that the server has read what was sent before it; GPeerI.hpp rule 6), and a result that
 * finishes after the client has ended -- is saved: the evaluated
 * work item, one file per item, in GPeerClientOptions::undeliveredDir, written with the item's
 * toFile() where its type has one and with sharedPtrToString() otherwise (it loads back with
 * fromFile() or sharedPtrFromString()). The file name (process id, correlation id, a count) is
 * logged. Nothing is saved once the server has announced the end of the run: nobody needs it. A
 * client with nothing finished and nothing being evaluated ends at once.
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
    using pipeline_base::n_decoding_;
    using pipeline_base::n_nodata_;
    using pipeline_base::pending_pulls_;
    using pipeline_base::prefetch_depth_;
    using pipeline_base::schedule_refill_;
    using pipeline_base::start_halt_timer;
    using pipeline_base::wire_ctx_;
    using pipeline_base::wire_registry_;

    using clock = std::chrono::steady_clock;

public:
    //-------------------------------------------------------------------------
    /**
     * @brief Builds the client on a connector. The client must be held by a std::shared_ptr.
     *
     * @param connector How a connection to the server is made; the client owns it. Must not be null.
     * @param options The settings of this client
     */
    GPeerClientT(std::unique_ptr<GConnectorI> connector, GPeerClientOptions options)
      : pipeline_base(options.name, options.prefetchDepth, options.parallelEvaluations)
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
    /** @brief How the last connection ended, once it has. To be read after run() has returned.
     *  @return The reason given by the peer; FAILED if there was no connection */
    [[nodiscard]] GPeerEnd connectionEnd() const noexcept {
        return end_;
    }
    /** @brief How many connections have opened. From any thread. @return The count */
    [[nodiscard]] std::uint64_t connections() const noexcept {
        return n_connections_.load();
    }
    /** @brief How many connections were lost (see the class comment). From any thread.
     *  @return The count */
    [[nodiscard]] std::uint64_t losses() const noexcept {
        return n_losses_.load();
    }
    /** @brief How often a result was sent once more on a new connection. From any thread.
     *  @return The count */
    [[nodiscard]] std::uint64_t resultsResent() const noexcept {
        return n_results_resent_.load();
    }
    /** @brief How many undeliverable results were saved (see the class comment). From any
     *  thread. @return The count */
    [[nodiscard]] std::uint64_t resultsSaved() const noexcept {
        return n_results_saved_.load();
    }
    /** @brief How many undeliverable results could not be saved (no item, or the file could not
     *  be written). From any thread. @return The count */
    [[nodiscard]] std::uint64_t resultsUnsaved() const noexcept {
        return n_results_unsaved_.load();
    }
    /** @brief How many results were given up at the resend limit. From any thread.
     *  @return The count */
    [[nodiscard]] std::uint64_t resultsGivenUp() const noexcept {
        return n_results_given_up_.load();
    }
    /** @brief How often a NO_WORK answer was received. From any thread.
     *  @return The count */
    [[nodiscard]] std::uint64_t noWorkCount() const noexcept {
        return n_nodata_.load();
    }

private:
    //-------------------------------------------------------------------------
    /** @brief Where the client is with its connection (io thread) */
    enum class LinkState : std::uint8_t {
        ATTEMPT, ///< The connector is making a connection
        OPENING, ///< A peer was made and started; it has not opened yet
        OPEN,    ///< The connection is usable
        CLOSING, ///< This client closes the connection (a halt, an error)
        DRAINING, ///< The connection was lost; its messages are still being decoded
        FINISHING, ///< Halted without a connection: waits for its evaluations to end
        PAUSE,   ///< Waits before the next attempt
        ENDED    ///< Nothing is attempted any more
    };

    //-------------------------------------------------------------------------
    /// What a message in the list of those not known to have been read is
    enum class EntryKind : std::uint8_t {
        ASK,    ///< A PULL or an UNREADABLE: a request for work, nothing else
        RETURN, ///< A result that asks for the next item: a request
        RESULT  ///< A result that asks for nothing
    };
    /** @brief A message handed to the peer and not known to have been read */
    struct Entry {
        EntryKind kind = EntryKind::ASK; ///< What it is
        GMessage bytes;                  ///< The message, shared with the transport
        std::uint8_t sentOn = 0;         ///< On how many connections it was sent
        /// A result's evaluated item, kept until the result is known to have arrived (to be
        /// saved if it cannot be delivered); empty for a request and for a failed item
        std::unique_ptr<processable_type> item;
    };

    //-------------------------------------------------------------------------
    /**
     * @brief The main run-loop: makes the first attempt to connect, runs the io_context until no
     * work remains, then makes sure the connection is over. The halt timer runs from here on, so
     * that a client that has no connection notices a halt and its limits (see when_polled_()).
     */
    void run_() override {
        run_started_at_ = clock::now();
        start_halt_timer();
        this->attempt_();

        // This call will block until no more work remains in the io context
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
    /** @brief Asks the connector for a connection (io thread, or run_() before the io context
     *  runs). */
    void attempt_() {
        state_ = LinkState::ATTEMPT;
        auto self = this->shared_from_this();
        connector_->connect(
            io_context_.get_executor(),
            [self](const std::shared_ptr<GPeerI> &peer, std::string_view error) {
                self->when_connected_(peer, error);
            }
        );
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
        if(state_ != LinkState::ATTEMPT) {
            return; // the attempt was given up (a halt); the connector does not call then
        }
        if(not peer) {
            this->attemptFailed_(error);
            return;
        }

        state_ = LinkState::OPENING;
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
    /**
     * @brief An attempt has failed: there was no connection, or the peer ended before it opened.
     * The client tries again after a pause; a connector that cannot connect again ends it.
     *
     * @param reason Why
     */
    void attemptFailed_(std::string_view reason) {
        if(last_attempt_) {
            glogger << "In " << options_.name << ": the last attempt to deliver failed: " << reason
                    << '\n'
                    << GLOGGING;
            this->saveUndelivered_();
            this->endClient_();
            return;
        }
        if(not connector_->reconnectable()) {
            glogger << "In " << options_.name << ":" << '\n'
                    << "No connection to the server: " << reason << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;
            this->flagTerminalError();
            this->endClient_();
            return;
        }
        glogger << "In " << options_.name << ":" << '\n'
                << "No connection to the server: " << reason << '\n'
                << "The client tries again after a pause." << '\n'
                << GLOGGING;
        this->pause_();
    }

    //-------------------------------------------------------------------------
    /** @brief Waits before the next attempt (see the class comment); the pause doubles for the
     *  attempt after it. On the io thread. */
    void pause_() {
        state_ = LinkState::PAUSE;
        const auto longest = pause_length_.count();
        std::uniform_int_distribution<std::int64_t> wait(longest / 2, longest);
        pause_length_ = std::min(2 * pause_length_, GCLIENTRECONNECTPAUSEMAX);
        retry_timer_.expires_after(std::chrono::milliseconds(wait(this->rng_engine_)));
        auto self = this->shared_from_this();
        retry_timer_.async_wait([self](boost::system::error_code ec) {
            if(ec || self->state_ != LinkState::PAUSE) {
                return;
            }
            if(self->halt()) {
                self->haltShutdown_();
                return;
            }
            self->attempt_();
        });
    }

    //-------------------------------------------------------------------------
    /** @brief Called when the connection is usable: asks for work (the halt timer runs since
     *  run_()). */
    void when_open_() {
        state_ = LinkState::OPEN;
        opened_at_ = clock::now();
        contact_on_connection_ = false;
        n_connections_.fetch_add(1);
        // What the last connection may not have delivered goes first, in its order; the server
        // checks a result in before it answers the request it carries.
        for(auto &message : unanswered_) {
            if(message.sentOn > 0) {
                n_results_resent_.fetch_add(1);
            }
            this->transmitEntry_(message);
        }
        if(last_attempt_) {
            do_close_(GPeerClose::NORMAL); // after what it sent; a confirmed close proves it
            return;
        }
        if(busy_) {
            if(auto peer = peer_.lock()) {
                peer->setOwnerBusy(true);
            }
        }
        refill_();
    }

    //-------------------------------------------------------------------------
    /** @brief The client has had contact with a server (see the class comment). On the io thread. */
    void noteContact_() {
        if(contact_on_connection_) {
            return;
        }
        contact_on_connection_ = true;
        contact_ever_ = true;
        pause_length_ = GCLIENTRECONNECTPAUSEMIN;
    }

    //-------------------------------------------------------------------------
    /** @brief Called by the pipeline for every decoded message, before its books: an answer of the
     *  server (WORK, NO_WORK, or a message that could not be read) is contact with it, and proves
     *  the oldest request without an answer and every message before it (see the class comment).
     *  Any other kind is a protocol error (when_decoded_()). @param kind What was decoded; NONE if
     *  the message could not be read */
    void when_answered_(GFrameKind kind) {
        if(kind != GFrameKind::WORK && kind != GFrameKind::NO_WORK && kind != GFrameKind::NONE) {
            return;
        }
        this->noteContact_();
        while(not unanswered_.empty()) {
            const bool request = unanswered_.front().kind != EntryKind::RESULT;
            unanswered_.pop_front();
            if(request) {
                --pending_pulls_;
                return;
            }
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Called by the pipeline when the books of a decoded message are done: the loss of a
     *  connection is booked once its last message is (see the class comment). */
    void after_decoded_() {
        if(state_ == LinkState::DRAINING && n_decoding_ == 0) {
            this->bookLoss_();
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Called by the halt poll when no halt condition holds: a connection that has been
     *  open for the contact time is contact; without a connection, the limit is looked at. */
    void when_polled_() {
        const auto now = clock::now();
        if(state_ == LinkState::OPEN) {
            if(now - opened_at_ >= options_.contactTime) {
                this->noteContact_();
            }
            return;
        }
        if(state_ != LinkState::ATTEMPT && state_ != LinkState::OPENING &&
           state_ != LinkState::PAUSE && state_ != LinkState::DRAINING) {
            return;
        }
        if(not contact_ever_ && options_.firstContactLimit.count() > 0 &&
           now - run_started_at_ >= options_.firstContactLimit) {
            this->limitReached_(options_.firstContactLimit, "first contact");
        }
        else if(
            contact_ever_ && options_.reconnectLimit.count() > 0 &&
            now - last_contact_end_ >= options_.reconnectLimit
        ) {
            this->limitReached_(options_.reconnectLimit, "after a loss");
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief A limit without a server has run out: the client gives up.
     *
     * @param limit The limit
     * @param which Which one, for the log
     */
    void limitReached_(std::chrono::milliseconds limit, std::string_view which) {
        glogger << "In " << options_.name << ":" << '\n'
                << "No server within the limit ("
                << std::chrono::duration_cast<std::chrono::duration<double>>(limit).count()
                << " s, " << which << "). This will terminate the client." << '\n'
                << GLOGGING;
        this->flagTerminalError();
        this->haltShutdown_();
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Called once when a connection is over, for whatever reason: the client ends (the end
     * of the run, a close of its own), or tries again (a peer that ended before it opened, a lost
     * connection; see the class comment).
     *
     * @param end Why the connection ended
     * @param how What the server said, if it closed in the regular way
     * @param detail The transport's own words about it
     */
    void when_closed_(GPeerEnd end, GPeerClose how, std::string_view detail) {
        end_ = end;
        const LinkState was = state_;
        if(how == GPeerClose::RUN_ENDED) {
            // The server has announced the end of the run: this client is done, whatever else is
            // set on it. No result is needed any more: items that wait for the compute pool, or
            // are still being decoded, are not evaluated (one that is being evaluated runs to its
            // end), and nothing is saved.
            this->flagCloseRequested();
            glogger << options_.name << ": the server announced the end of the run" << '\n'
                    << GLOGGING;
            run_ended_ = true;
            drop_held_();
            unanswered_.clear();
            pending_pulls_ = 0;
            this->endClient_();
            return;
        }
        if(was == LinkState::OPENING && (last_attempt_ || not this->halt())) {
            this->attemptFailed_(detail); // the connection never opened (a failed handshake)
            return;
        }
        if(was == LinkState::OPEN && not this->halt() && connector_->reconnectable()) {
            this->lost_(detail);
            return;
        }
        if(end != GPeerEnd::CLOSED_HERE) {
            glogger << "In " << options_.name << ": the connection to the server has ended:" << '\n'
                    << detail << '\n'
                    << GLOGGING;
        }
        if(was == LinkState::CLOSING) {
            if(end == GPeerEnd::CLOSED_HERE) {
                // The server confirmed the close: it has read everything sent before it
                std::erase_if(unanswered_, [](const Entry &message) { return message.sentOn > 0; });
            }
            // Any other end -- a close the server did not confirm (CLOSED_HERE_UNCONFIRMED), a
            // loss -- proves nothing: what was sent is saved with what was never sent.
            this->saveUndelivered_();
            this->endClient_();
            return;
        }
        // Lost while halting, or by a connector that cannot connect again
        this->windUp_();
    }

    //-------------------------------------------------------------------------
    /**
     * @brief The client halts while it has no connection (see the class comment): it tries no
     * more, starts nothing new and lets its evaluations end; then it makes one last attempt to
     * deliver what it has finished, or saves it. Called again by the last result to come back.
     * On the io thread.
     */
    void windUp_() {
        state_ = LinkState::FINISHING;
        this->stopPipelineTimers_();
        retry_timer_.cancel();
        drop_held_();
        n_unreadable_owed_ = 0;
        std::erase_if(unanswered_, [](const Entry &message) {
            return message.kind == EntryKind::ASK;
        });
        pending_pulls_ = static_cast<std::size_t>(
            std::ranges::count(unanswered_, EntryKind::RETURN, &Entry::kind)
        );
        if(computing_ > 0 || n_reencoding_ > 0) {
            return; // afterResult_() calls again
        }
        if(not unanswered_.empty() && connector_->reconnectable() && not last_attempt_) {
            last_attempt_ = true;
            glogger << "In " << options_.name << ": " << unanswered_.size()
                    << " finished result(s) not known to have arrived; one last attempt to "
                       "deliver them"
                    << '\n'
                    << GLOGGING;
            this->attempt_();
            return;
        }
        this->saveUndelivered_();
        this->endClient_();
    }

    //-------------------------------------------------------------------------
    /** @brief Saves every result in the list that cannot be delivered any more (see the class
     *  comment) and empties the list. On the io thread. */
    void saveUndelivered_() {
        for(auto &message : unanswered_) {
            if(message.kind != EntryKind::ASK) {
                this->saveResult_(std::move(message.item));
            }
        }
        unanswered_.clear();
        pending_pulls_ = 0;
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Saves the evaluated item of a result that cannot be delivered, unless the run has
     * ended (see the class comment). A directory that cannot be written is a warning.
     *
     * @param item The evaluated item; empty if the result carries none (a failed item)
     */
    void saveResult_(std::unique_ptr<processable_type> item) {
        if(run_ended_) {
            return;
        }
        if(not item) {
            n_results_unsaved_.fetch_add(1);
            glogger << "In " << options_.name
                    << ": a result that cannot be delivered holds no item (its item failed); it "
                       "is lost"
                    << '\n'
                    << GWARNING;
            return;
        }
        const bool binary = options_.mode == Gem::Common::serializationMode::GEM_BINARY;
        const std::filesystem::path file =
            options_.undeliveredDir / std::format(
                                          "undelivered-{}-{}-{}.{}",
                                          ::getpid(),
                                          item->getCorrelationId(),
                                          Gem::Courtier::nextUndeliveredFileNumber(),
                                          binary ? "bin" : "json"
                                      );
        try {
            if constexpr(requires(const processable_type &p) {
                             p.toFile(file, Gem::Common::serializationMode::GEM_BINARY);
                         }) {
                item->toFile(file, options_.mode);
            }
            else {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << Gem::Common::sharedPtrToString(
                    std::shared_ptr<processable_type>(std::move(item)),
                    options_.mode
                );
                out.close();
                if(not out) {
                    throw std::runtime_error("the file could not be written");
                }
            }
            n_results_saved_.fetch_add(1);
            glogger << "In " << options_.name
                    << ": a finished result could not be delivered; its evaluated item was saved "
                       "as "
                    << file.string() << '\n'
                    << GWARNING;
        }
        catch(...) {
            n_results_unsaved_.fetch_add(1);
            glogger << "In " << options_.name
                    << ": a finished result could not be delivered, and its item could not be "
                       "saved as "
                    << file.string() << ": "
                    << Gem::Courtier::describeException(std::current_exception()) << '\n'
                    << GWARNING;
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief The connection was lost (see the class comment). The loss is booked once the
     * messages of the connection that are still being decoded have been. On the io thread.
     *
     * @param detail The transport's words about it
     */
    void lost_(std::string_view detail) {
        n_losses_.fetch_add(1);
        lost_at_ = clock::now();
        glogger << "In " << options_.name << ": the connection to the server was lost:" << '\n'
                << detail << '\n'
                << "The client connects again." << '\n'
                << GLOGGING;
        this->nodata_timer_.cancel();
        state_ = LinkState::DRAINING;
        if(n_decoding_ == 0) {
            this->bookLoss_();
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Books a loss (see the class comment): drops the requests that only asked for work
     *  and the results that reached the resend limit, then pauses before the next attempt. */
    void bookLoss_() {
        if(contact_on_connection_) {
            last_contact_end_ = lost_at_;
        }
        n_unreadable_owed_ = 0; // the new session sends every blob in full anyway
        std::vector<std::unique_ptr<processable_type>> given_up;
        std::erase_if(unanswered_, [this, &given_up](Entry &message) {
            if(message.kind == EntryKind::ASK) {
                return true;
            }
            if(message.sentOn >= GCLIENTRESENDLIMIT) {
                n_results_given_up_.fetch_add(1);
                given_up.push_back(std::move(message.item));
                glogger << "In " << options_.name << ": a result was sent "
                        << static_cast<int>(message.sentOn)
                        << " times on connections that were lost; it is given up" << '\n'
                        << GWARNING;
                return true;
            }
            return false;
        });
        pending_pulls_ = static_cast<std::size_t>(
            std::ranges::count(unanswered_, EntryKind::RETURN, &Entry::kind)
        );
        for(auto &item : given_up) {
            this->saveResult_(std::move(item));
        }
        this->refreshResultKindHint_();
        if(this->halt()) {
            this->windUp_();
            return;
        }
        this->pause_();
    }

    //-------------------------------------------------------------------------
    /** @brief The client ends: nothing is attempted any more, and no timer is armed, so that
     *  io_context::run() returns once the evaluations that run have finished. On the io thread. */
    void endClient_() {
        state_ = LinkState::ENDED;
        this->stopPipelineTimers_();
        retry_timer_.cancel();
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
     * @brief Called on the io thread for a message that could not be decoded: it answered a request
     * (when_answered_() has booked that), and the client says so with UNREADABLE, which is a
     * request again (it counts as one from now on). What identifies the item travels inside the item that failed to
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
            this->refreshResultKindHint_();
            return; // the request was answered (when_answered_()); nothing more is asked for
        }
        // The UNREADABLE that is owed takes the place of the request that was answered
        ++n_unreadable_owed_;
        ++pending_pulls_;
        schedule_refill_();
    }

    //-------------------------------------------------------------------------
    /**
     * Issues PULL frames for as long as fewer items are in flight than the prefetch depth -- and,
     * with look-ahead, for as long as no request is unanswered, which makes it at most one. Runs on
     * the io thread.
     */
    void request_more_() {
        while(state_ == LinkState::OPEN && not this->halt() && in_flight_() < prefetch_depth_ &&
              not(options_.lookAhead && pending_pulls_ > 0)) {
            const GCommandContainerT<processable_type> pull{GFrameKind::PULL};
            send_command_(pull);
        }
        this->refreshResultKindHint_();
    }

    //-------------------------------------------------------------------------
    /** @brief Serializes @p container (a PULL or an UNREADABLE: nothing to speak of; a request)
     *  and hands it to the peer (post_()), guarding against serialization failures (which must
     *  not unwind the io thread).
     *
     *  @param container The command container to serialize and transmit */
    void send_command_(const GCommandContainerT<processable_type> &container) {
        try {
            this->post_(
                Entry{
                    .kind = EntryKind::ASK,
                    .bytes =
                        makeMessage(Gem::Courtier::wireEncode(container, &wire_ctx_, options_.mode))
                }
            );
        }
        catch(...) {
            glogger << "In " << options_.name << "::send_command_():" << '\n'
                    << "Could not serialize the outgoing message:" << '\n'
                    << Gem::Courtier::describeException(std::current_exception()) << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            do_close_(GPeerClose::ERROR);
        }
    }

    //-------------------------------------------------------------------------
    /** @brief The only way a message reaches the peer: it joins the list of messages that are not
     *  known to have been read (see the class comment) and is sent if the connection is open;
     *  otherwise it waits for the next one. A request is counted in pending_pulls_. On the io
     *  thread. @param message The message */
    void post_(Entry message) {
        if(state_ == LinkState::ENDED) {
            // The client has ended (an error, or the end of the run): nothing can be delivered
            if(message.kind != EntryKind::ASK) {
                this->saveResult_(std::move(message.item));
            }
            return;
        }
        if(message.kind != EntryKind::RESULT) {
            ++pending_pulls_;
        }
        unanswered_.push_back(std::move(message));
        if(state_ == LinkState::OPEN) {
            this->transmitEntry_(unanswered_.back());
        }
    }

    /** @brief Hands one message of the list to the peer @param message The message */
    void transmitEntry_(Entry &message) {
        ++message.sentOn;
        if(auto peer = peer_.lock()) {
            peer->send(message.bytes);
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
            state_ = LinkState::CLOSING;
            peer->close(how);
        }
        else {
            this->endClient_();
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
        if(state_ != LinkState::OPEN) {
            return; // the next connection asks (when_open_())
        }
        for(; n_unreadable_owed_ > 0; --n_unreadable_owed_) {
            --pending_pulls_; // counted while owed; send_command_() counts it as sent
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
     * @param kept The evaluated item if the frame does not carry it; empty otherwise
     */
    void sendResult_(
        std::string bytes,
        const std::string &error,
        GCommandContainerT<processable_type> frame,
        std::unique_ptr<processable_type> kept
    ) {
        if(not error.empty()) {
            this->saveResult_(kept ? std::move(kept) : frame.releaseItem());
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
            this->reencodeAsResult_(std::move(frame), std::move(kept));
            return;
        }
        // A RETURN is a request (the server replies with an item); post_() counts it
        const EntryKind kind =
            frame.kind() == GFrameKind::RETURN ? EntryKind::RETURN : EntryKind::RESULT;
        this->post_(
            Entry{
                .kind = kind,
                .bytes = makeMessage(std::move(bytes)),
                .item = kept ? std::move(kept) : frame.releaseItem()
            }
        );
        this->afterResult_();
    }

    /** @brief Encodes @p frame as a RESULT on the compute pool and sends it on the io thread.
     *  @param frame The finished result's frame
     *  @param kept The evaluated item if the frame does not carry it; empty otherwise */
    void reencodeAsResult_(
        GCommandContainerT<processable_type> frame,
        std::unique_ptr<processable_type> kept
    ) {
        ++n_reencoding_;
        auto self = this->shared_from_this();
        auto guard = boost::asio::make_work_guard(io_context_);
        boost::asio::post(
            compute_pool_,
            [self,
             frame = std::move(frame),
             kept = std::move(kept),
             guard = std::move(guard)]() mutable {
                frame.setKind(GFrameKind::RESULT);
                std::string error;
                std::string bytes = self->encodeResult_(frame, error);
                auto item = kept ? std::move(kept) : frame.releaseItem();
                // The pool's handler gives up its hold on the client here: if it kept it until it is
                // destroyed, after the io thread may have finished with the client, the client could be
                // destroyed on a thread of its own pool, which cannot join itself.
                auto &io = self->io_context_;
                boost::asio::post(
                    io,
                    [self = std::move(self),
                     bytes = std::move(bytes),
                     error = std::move(error),
                     item = std::move(item),
                     guard = std::move(guard)]() mutable {
                        self->reencoded_(std::move(bytes), error, std::move(item));
                    }
                );
            }
        );
    }

    /** @brief On the io thread, once a result has been encoded as a RESULT once more: sends it,
     *  then carries out a close that waited for it, or goes on.
     *  @param bytes The RESULT @param error Why it could not be encoded; empty if it could
     *  @param item The evaluated item (empty for a failed one) */
    void reencoded_(
        std::string bytes,
        const std::string &error,
        std::unique_ptr<processable_type> item
    ) {
        --n_reencoding_;
        if(not error.empty()) {
            this->saveResult_(std::move(item));
            glogger << "In " << options_.name << ": could not serialize a result:" << '\n'
                    << error << '\n'
                    << "The client will shut down." << '\n'
                    << GWARNING;
            do_close_(GPeerClose::ERROR);
            return;
        }
        this->post_(
            Entry{
                .kind = EntryKind::RESULT,
                .bytes = makeMessage(std::move(bytes)),
                .item = std::move(item)
            }
        );
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
        if(state_ == LinkState::FINISHING) {
            this->windUp_();
            return;
        }
        if(this->halt()) {
            this->haltShutdown_();
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

    /** @brief Halt hook (the halt timer, a limit, the retry timer): closes the connection in the
     *  regular way when a halt condition is reached, after the evaluations that run (see the class
     *  comment). A client that is still connecting gives up the connector's attempt, or closes the
     *  peer that has not opened yet; one that pauses ends. So run() returns. */
    void haltShutdown_() {
        switch(state_) {
        case LinkState::OPEN:
            this->closeWhenIdle_();
            break;
        case LinkState::OPENING:
            this->do_close_(GPeerClose::NORMAL); // a close during a handshake reports FAILED
            break;
        case LinkState::ATTEMPT:
            connector_->cancel();
            this->windUp_();
            break;
        case LinkState::PAUSE:
        case LinkState::DRAINING:
            this->windUp_();
            break;
        case LinkState::FINISHING:
        case LinkState::CLOSING:
        case LinkState::ENDED:
            break;
        }
    }

    /** @brief Busy hook: tells the peer whether the compute pool evaluates an item.
     *  @param busy Whether it does */
    void when_busy_(bool busy) {
        busy_ = busy;
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

    LinkState state_ = LinkState::ATTEMPT; ///< Where the client is with its connection (io thread)
    boost::asio::steady_timer retry_timer_{io_context_}; ///< Ends a pause between two attempts
    /// The longest wait of the next pause (io thread)
    std::chrono::milliseconds pause_length_ = GCLIENTRECONNECTPAUSEMIN;
    clock::time_point run_started_at_; ///< When run_() began (io thread)
    clock::time_point opened_at_;      ///< When the current connection opened (io thread)
    bool contact_ever_ = false;        ///< There was contact with a server (io thread)
    bool contact_on_connection_ =
        false;                  ///< There was contact on the current connection (io thread)
    clock::time_point lost_at_; ///< When the last connection was lost (io thread)
    /// The end of the last connection on which there was contact (io thread)
    clock::time_point last_contact_end_;
    bool busy_ = false; ///< What when_busy_() was told last (io thread)
    bool last_attempt_ = false; ///< The last attempt to deliver has been made (io thread)
    bool run_ended_ = false;    ///< The server announced the end of the run (io thread)
    std::atomic<std::uint64_t> n_results_saved_{0};   ///< Undeliverable results saved
    std::atomic<std::uint64_t> n_results_unsaved_{0}; ///< Undeliverable results not saved

    /// The messages handed to the peer and not known to have been read, in the order they were
    /// handed over (see the class comment; io thread)
    std::deque<Entry> unanswered_;

    std::atomic<std::uint64_t> n_connections_{0};      ///< Connections that opened
    std::atomic<std::uint64_t> n_losses_{0};           ///< Connections lost
    std::atomic<std::uint64_t> n_results_resent_{0};   ///< Messages sent once more
    std::atomic<std::uint64_t> n_results_given_up_{0}; ///< Results given up at the resend limit

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
