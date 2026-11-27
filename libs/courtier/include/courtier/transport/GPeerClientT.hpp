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
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>

// Boost headers go here
#include <boost/asio.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GThreadBudget.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GWireCodec.hpp"               // scope-wrapped (de)serialization, HELLO
#include "courtier/GWireSerializationContext.hpp"
#include "courtier/transport/GPeerI.hpp" // the connection and how to get one

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/** @brief How a GPeerClientT is set up. */
struct GPeerClientOptions {
    std::string name = "GPeerClientT<processable_type>"; ///< Who this client is, for its log lines
    Gem::Common::serializationMode mode =
        Gem::Common::serializationMode::GEM_BINARY; ///< The wire format
    /// n: how many work items the client holds at once -- the one(s) being evaluated and those
    /// fetched ahead, so that the transfer of the next item overlaps the evaluation of the current
    /// one (0 is treated as 1)
    std::size_t prefetchDepth = 1;
    /// m: how many of the items it holds the client evaluates at the same time; between 1 and
    /// prefetchDepth, else the client is refused when it is built
    std::size_t parallelEvaluations = 1;
    /// How long the client tries to reach a server before its first contact (a HELLO); 0: for as
    /// long as it runs
    std::chrono::milliseconds firstContactLimit = GCLIENTFIRSTCONTACTLIMIT;
    /// How long the client tries to reach a server again, counted from the end of the last
    /// connection on which it had contact; 0: for as long as it runs
    std::chrono::milliseconds reconnectLimit = GCLIENTRECONNECTLIMIT;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The client of the networked consumer protocol (GFrameKind), over any transport that offers a
 * GConnectorI. The transport makes the connection; this class gives the messages their meaning.
 *
 * The link, on the client's one io thread:
 *  - CONNECTING: the connector's attempt, the peer's start, the wait for the server's HELLO. A
 *    HELLO whose build (the Geneva commit) differs from this client's ends the client (a terminal
 *    error; it does not come back), and so does one whose program identity differs where both
 *    sides have one; before it closes, the client sends its own HELLO, so that the server can say
 *    who left. Where one side has no identity, the client says so once per run in its log. The
 *    HELLO is the contact with a server; it names the keep-alive interval, which the client hands
 *    to its end (GPeerI::setKeepAlive()).
 *  - OPEN: the client keeps its items in flight -- PULLs without an answer, items held, items
 *    being evaluated -- at the prefetch depth by sending PULLs. Every finished item goes back as a
 *    RESULT, which asks for nothing; a PULL follows it if there is room.
 *  - PAUSE: after a lost connection, or an attempt that failed, a random pause before the next
 *    attempt; it doubles with every attempt up to GCLIENTRECONNECTPAUSEMAX and starts again at
 *    GCLIENTRECONNECTPAUSEMIN after a contact.
 *  - ENDING: after a halt condition (a time or an item limit, a close request), the end of the run
 *    (GPeerClose::RUN_ENDED), a message it cannot handle, a HELLO that does not match, or a limit
 *    without a server. The client asks for nothing more and starts no evaluation: what it holds,
 *    and what arrives, is dropped (the server's lease puts it back). Evaluations that run finish;
 *    their RESULT is sent if the connection is open; then the connection is closed and run()
 *    returns.
 *
 * Delivery, best effort. Messages on one connection are read in order, so a WORK, which answers a
 * PULL, shows that the server has read everything the client sent before that PULL. The client
 * keeps the list of what it has sent and not seen answered (RESULTs and PULLs, in order); a WORK
 * removes the oldest PULL and every RESULT before it. When a connection is lost (a peer that was
 * opened and ends without RUN_ENDED and without a halt), the PULLs leave the list, the client
 * connects again and sends the RESULTs on the list once more after the HELLO. A result that
 * arrives twice is dropped by the server. When the client ends, what is on the list is dropped.
 *
 * Limits. A client that has had no contact within GPeerClientOptions::firstContactLimit, or none
 * again within GPeerClientOptions::reconnectLimit of the end of the last connection with contact,
 * gives up (a terminal error); an open connection is not held against them. A connector that cannot
 * connect again (GConnectorI::reconnectable()) gets one attempt and one connection.
 *
 * Messages that cannot be read. A message whose head cannot be read, or whose kind a server does
 * not send, ends the client (GPeerClose::ERROR). A WORK whose payload cannot be read is returned as
 * failed: a RESULT of the ids in its head, without a result; the server sends the client its blobs
 * in full again. An item that cannot be processed or returned, or whose result cannot be encoded or
 * is longer than the transport carries, is returned as failed in the same way.
 *
 * Threads. The io thread runs the connection's handlers and keeps every book; it neither decodes
 * nor encodes an item. A message is decoded on a strand of the compute pool (one at a time, in
 * order), an item is evaluated on the pool -- at most m (parallelEvaluations) at a time, the rest
 * held, up to the depth n (prefetchDepth) -- and its RESULT is encoded there and handed back to the
 * io thread. With m below n the pool has one thread more than m, so that an item held ahead is
 * decoded while m are evaluated. Shared between threads: the halt flags of the base and the two
 * counters losses() and resultsResent().
 *
 * @tparam processable_type The work-item type exchanged with the server (must be processable)
 */
template <typename processable_type>
class GPeerClientT
  : public Gem::Courtier::GBaseClientT<processable_type>
  , public std::enable_shared_from_this<GPeerClientT<processable_type>> {
    using clock = std::chrono::steady_clock;
    using frame_type = GCommandContainerT<processable_type>;

public:
    //-------------------------------------------------------------------------
    /**
     * @brief Builds the client on a connector. The client must be held by a std::shared_ptr.
     *
     * @param connector How a connection to the server is made; the client owns it. Must not be null.
     * @param options The settings of this client
     * @throws gem_exception if the parallel evaluations are 0 or above the prefetch depth
     */
    GPeerClientT(std::unique_ptr<GConnectorI> connector, GPeerClientOptions options)
      : options_(checkedOptions_(std::move(options)))
      , connector_(std::move(connector)) {
        // The client caches every blob it receives, keyed by content id, so an id-only work item
        // resolves locally (delivery is ordered: a blob arrives before its id). It never interns:
        // a blob it echoes back travels inline.
        wire_ctx_.enabled = true;
        wire_ctx_.peer = 0; // the single upstream server
        wire_ctx_.registry = &wire_registry_;
        wire_ctx_.mode = options_.mode;
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
    /** @brief How the last connection ended. To be read after run() has returned.
     *  @return The reason given by the peer; FAILED if there was no connection */
    [[nodiscard]] GPeerEnd connectionEnd() const noexcept {
        return end_;
    }
    /** @brief How many connections have opened. To be read after run() has returned.
     *  @return The count */
    [[nodiscard]] std::uint64_t connections() const noexcept {
        return n_connections_;
    }
    /** @brief How many connections were lost (see the class comment). From any thread.
     *  @return The count */
    [[nodiscard]] std::uint64_t losses() const noexcept {
        return n_losses_.load();
    }
    /** @brief How often a RESULT was sent once more on a new connection. From any thread.
     *  @return The count */
    [[nodiscard]] std::uint64_t resultsResent() const noexcept {
        return n_results_resent_.load();
    }

private:
    //-------------------------------------------------------------------------
    /** @brief Where the client is with its connection (see the class comment; io thread) */
    enum class LinkState : std::uint8_t {
        CONNECTING, ///< An attempt, a peer that opens, or the wait for its HELLO
        OPEN,       ///< The HELLO has arrived: the client works
        PAUSE,      ///< Waits before the next attempt
        ENDING      ///< Asks for nothing more; ends when no evaluation runs
    };

    /** @brief A message sent and not seen answered (see the class comment) */
    struct Entry {
        GMessage bytes;    ///< The message, shared with the transport
        bool pull = false; ///< A PULL; otherwise a RESULT
        bool sent = false; ///< It has been handed to a connection
    };

    /** @brief A message as the decode strand hands it to the io thread */
    struct Decoded {
        frame_type frame{GFrameKind::NONE}; ///< The frame (its head at least, if it could be read)
        bool head_read = false;             ///< The head could be read
        GFrameKind kind =
            GFrameKind::NONE;      ///< The kind the head named (the frame may be rewritten)
        std::string error;         ///< Why the head or a WORK's payload could not be read
        std::string failed_result; ///< A WORK whose payload could not be read: its RESULT, encoded
    };

    //-------------------------------------------------------------------------
    /** @brief The main loop: the first attempt, then the io context until no work remains. */
    void run_() override {
        run_started_at_ = clock::now();
        this->armHaltTimer_();
        this->attempt_();
        io_context_.run();
        // Nothing can be waited for any more: a peer that is still there is ended at once
        if(auto peer = peer_.lock()) {
            peer->abort();
        }
        // The last lines are read by the speed-up benchmarks (testSpeedupTools.py)
        glogger << options_.name << ": the client has ended. Processed " << this->getNProcessed()
                << " items in total" << '\n'
                << "prefetch depth was " << options_.prefetchDepth << '\n'
                << "parallel evaluations were " << options_.parallelEvaluations << '\n'
                << GLOGGING;
    }

    //-------------------------------------------------------------------------
    /** @brief Asks the connector for a connection (io thread, or run_() before the io context
     *  runs). */
    void attempt_() {
        state_ = LinkState::CONNECTING;
        attempting_ = true;
        peer_opened_ = false;
        auto self = this->shared_from_this();
        connector_->connect(
            io_context_.get_executor(),
            [self](const std::shared_ptr<GPeerI> &peer, std::string_view error) {
                self->whenConnected_(peer, error);
            }
        );
    }

    //-------------------------------------------------------------------------
    /** @brief The connector is done: starts the peer, which owns this client through its handlers
     *  for as long as the connection lasts. @param peer The connection, not started yet; empty if
     *  none could be made @param error Why there is no connection */
    void whenConnected_(const std::shared_ptr<GPeerI> &peer, std::string_view error) {
        if(not attempting_) {
            return; // the attempt was given up (the connector does not call then)
        }
        attempting_ = false;
        if(not peer) {
            this->connectionOver_(false, error);
            return;
        }
        const std::uint64_t connection = ++connection_;
        peer_ = peer;
        peer_live_ = true;
        auto self = this->shared_from_this();
        peer->start(
            GPeerHandlers{
                .onOpen =
                    [self] {
                        self->peer_opened_ = true;
                        ++self->n_connections_;
                        if(auto opened = self->peer_.lock()) {
                            self->max_message_bytes_ = opened->maxMessageBytes();
                        }
                    },
                .onMessage = [self, connection](
                                 std::string &&message
                             ) { self->dispatchMessage_(std::move(message), connection); },
                .onClosed =
                    [self](GPeerEnd end, GPeerClose how, std::string_view detail) {
                        self->whenClosed_(end, how, detail);
                    }
            }
        );
    }

    //-------------------------------------------------------------------------
    /** @brief The connection has ended (see the class comment): the end of the run ends the
     *  client; any other end is a loss (if the peer had opened) or a failed attempt, and the client
     *  tries again after a pause. @param end Why it ended @param how What the server said
     *  @param detail The transport's own words */
    void whenClosed_(GPeerEnd end, GPeerClose how, std::string_view detail) {
        end_ = end;
        peer_live_ = false;
        if(contact_on_connection_) {
            last_contact_end_ = clock::now();
            contact_on_connection_ = false;
        }
        if(how == GPeerClose::RUN_ENDED) {
            // No result is needed any more: nothing is sent, evaluated or kept
            this->flagCloseRequested();
            glogger << options_.name << ": the server announced the end of the run" << '\n'
                    << GLOGGING;
            unanswered_.clear();
            pending_pulls_ = 0;
            this->beginEnding_(GPeerClose::NORMAL);
            return;
        }
        if(state_ == LinkState::ENDING) {
            this->finishIfIdle_();
            return;
        }
        std::erase_if(unanswered_, [](const Entry &entry) { return entry.pull; });
        pending_pulls_ = 0;
        this->connectionOver_(peer_opened_, detail);
    }

    //-------------------------------------------------------------------------
    /** @brief A connection is over without the end of the run, or none could be made: the client
     *  tries again after a pause, or ends (see the class comment). @param opened Whether the peer
     *  had opened (a loss) @param reason Why */
    void connectionOver_(bool opened, std::string_view reason) {
        if(opened) {
            n_losses_.fetch_add(1);
        }
        glogger << "In " << options_.name << ": "
                << (opened ? "the connection to the server was lost: "
                           : "no connection to the server: ")
                << reason << '\n'
                << GLOGGING;
        if(not connector_->reconnectable()) {
            if(not opened) {
                this->flagTerminalError();
            }
            this->beginEnding_(GPeerClose::NORMAL);
            return;
        }
        if(this->halt()) {
            this->beginEnding_(GPeerClose::NORMAL);
            return;
        }
        this->pause_();
    }

    //-------------------------------------------------------------------------
    /** @brief Waits before the next attempt (see the class comment). */
    void pause_() {
        state_ = LinkState::PAUSE;
        const auto longest = pause_length_.count();
        std::uniform_int_distribution<std::int64_t> wait(longest / 2, longest);
        pause_length_ = std::min(2 * pause_length_, GCLIENTRECONNECTPAUSEMAX);
        retry_timer_.expires_after(std::chrono::milliseconds(wait(rng_engine_)));
        auto self = this->shared_from_this();
        retry_timer_.async_wait([self](boost::system::error_code ec) {
            if(ec || self->state_ != LinkState::PAUSE) {
                return;
            }
            if(self->halt()) {
                self->beginEnding_(GPeerClose::NORMAL);
                return;
            }
            self->attempt_();
        });
    }

    //-------------------------------------------------------------------------
    /** @brief Arms the poll of the halt conditions and of the limits without a server. */
    void armHaltTimer_() {
        halt_timer_.expires_after(GCLIENTHALTPOLLINTERVAL);
        auto self = this->shared_from_this();
        halt_timer_.async_wait([self](boost::system::error_code ec) {
            if(ec || self->state_ == LinkState::ENDING) {
                return; // cancelled, or the client ends: the timer is not armed again
            }
            if(self->halt()) {
                self->beginEnding_(GPeerClose::NORMAL);
                return;
            }
            if(self->state_ != LinkState::OPEN) {
                self->checkLimits_();
            }
            if(self->state_ != LinkState::ENDING) {
                self->armHaltTimer_();
            }
        });
    }

    //-------------------------------------------------------------------------
    /** @brief Ends the client if a limit without a server has run out (see the class comment). */
    void checkLimits_() {
        const auto now = clock::now();
        std::chrono::milliseconds limit{0};
        if(not contact_ever_ && options_.firstContactLimit.count() > 0 &&
           now - run_started_at_ >= options_.firstContactLimit) {
            limit = options_.firstContactLimit;
        }
        else if(
            contact_ever_ && options_.reconnectLimit.count() > 0 &&
            now - last_contact_end_ >= options_.reconnectLimit
        ) {
            limit = options_.reconnectLimit;
        }
        if(limit.count() == 0) {
            return;
        }
        glogger << "In " << options_.name << ":" << '\n'
                << "No server within the limit ("
                << std::chrono::duration_cast<std::chrono::duration<double>>(limit).count()
                << " s, " << (contact_ever_ ? "after a loss" : "first contact")
                << "). This will terminate the client." << '\n'
                << GLOGGING;
        this->flagTerminalError();
        this->beginEnding_(GPeerClose::NORMAL);
    }

    //-------------------------------------------------------------------------
    /** @brief Decodes a message off the io thread, on the strand of the compute pool, and hands
     *  it back to the io thread (decoded_()). A WORK whose payload cannot be read is turned into
     *  its failed RESULT there. @param message One complete message from the server
     *  @param connection The connection it came on */
    void dispatchMessage_(std::string message, std::uint64_t connection) {
        auto self = this->shared_from_this();
        auto guard = boost::asio::make_work_guard(io_context_);
        boost::asio::post(
            decode_strand_,
            [self, message = std::move(message), connection, guard = std::move(guard)]() mutable {
                Decoded decoded;
                try {
                    const std::string_view payload = Gem::Courtier::frameHeadFromString(
                        std::string_view{message},
                        decoded.frame,
                        self->options_.mode
                    );
                    decoded.head_read = true;
                    decoded.kind = decoded.frame.kind();
                    if(decoded.kind == GFrameKind::WORK) {
                        Gem::Courtier::wireDecodePayload(
                            payload,
                            decoded.frame,
                            &self->wire_ctx_,
                            self->options_.mode
                        );
                    }
                }
                catch(...) {
                    decoded.error = Gem::Common::describeException(std::current_exception());
                }
                if(decoded.head_read && not decoded.error.empty()) {
                    decoded.failed_result = self->failedResult_(
                        decoded.frame,
                        "The work item could not be read: " + decoded.error
                    );
                }
                // The pool's handler gives up its hold on the client here: the client must never
                // be destroyed on a thread of its own pool, which cannot join itself.
                auto &io = self->io_context_;
                boost::asio::post(
                    io,
                    [self = std::move(self),
                     decoded = std::move(decoded),
                     connection,
                     guard = std::move(guard)]() mutable {
                        self->decoded_(std::move(decoded), connection);
                    }
                );
            }
        );
    }

    //-------------------------------------------------------------------------
    /** @brief A message has been decoded (io thread): a HELLO opens the link, a WORK answers the
     *  oldest PULL and is evaluated (or its failed RESULT sent), anything else ends the client.
     *  A message of a connection that is over changes no book: its WORK is evaluated, and its
     *  RESULT goes with the next connection. @param decoded The message
     *  @param connection The connection it came on */
    void decoded_(Decoded decoded, std::uint64_t connection) {
        const bool current = connection == connection_ && peer_live_;
        const GFrameKind kind = decoded.kind;
        if(not decoded.head_read || (kind != GFrameKind::WORK && kind != GFrameKind::HELLO)) {
            if(current) {
                this->protocolError_(
                    decoded.head_read
                        ? "a frame of a kind a server does not send: " + Gem::Common::enumName(kind)
                        : decoded.error
                );
            }
        }
        else if(kind == GFrameKind::HELLO) {
            if(current) {
                this->whenHello_(decoded.frame);
            }
        }
        else if(current && contact_on_connection_ && pending_pulls_ == 0) {
            this->protocolError_("a WORK that answers no PULL");
        }
        else {
            if(current && contact_on_connection_) {
                this->answered_();
            }
            if(decoded.failed_result.empty()) {
                this->acceptWork_(std::move(decoded.frame));
            }
            else {
                this->postResult_(makeMessage(std::move(decoded.failed_result)));
                this->refill_();
            }
        }
        this->finishIfIdle_();
    }

    //-------------------------------------------------------------------------
    /** @brief The server's HELLO: a matching one opens the link -- the RESULTs not seen answered
     *  are sent again, then PULLs -- and one that does not match ends the client (see the class
     *  comment). @param hello The HELLO */
    void whenHello_(const frame_type &hello) {
        if(state_ != LinkState::CONNECTING) {
            return; // the client ends
        }
        if(const std::string mismatch = helloMismatch(hello); not mismatch.empty()) {
            glogger << "In " << options_.name << ":" << '\n'
                    << mismatch << '\n'
                    << "This will terminate the client." << '\n'
                    << GLOGGING;
            // The server learns why: this client's HELLO goes before the close (GPeerI.hpp, rule 6)
            if(auto peer = peer_.lock()) {
                peer->send(makeMessage(
                    buildHello<processable_type>(options_.mode, std::chrono::milliseconds::zero())
                ));
            }
            this->flagTerminalError();
            this->beginEnding_(GPeerClose::ERROR);
            return;
        }
        if(not program_unchecked_told_) {
            if(const std::string unchecked = helloProgramUnchecked(hello); not unchecked.empty()) {
                glogger << "In " << options_.name << ":" << '\n' << unchecked << '\n' << GLOGGING;
                program_unchecked_told_ = true;
            }
        }
        // The server sets the keep-alive interval; this client's end takes it (on the executor)
        if(auto peer = peer_.lock()) {
            peer->setKeepAlive(hello.keepAlive());
        }
        state_ = LinkState::OPEN;
        contact_on_connection_ = true;
        contact_ever_ = true;
        pause_length_ = GCLIENTRECONNECTPAUSEMIN;
        for(auto &entry : unanswered_) { // only RESULTs: the PULLs left at the loss
            if(entry.sent) {
                n_results_resent_.fetch_add(1);
            }
            this->send_(entry);
        }
        this->refill_();
    }

    //-------------------------------------------------------------------------
    /** @brief A WORK has arrived on the open link: it answers the oldest PULL, and every RESULT
     *  sent before that PULL has been read (see the class comment). */
    void answered_() {
        while(not unanswered_.empty()) {
            const bool pull = unanswered_.front().pull;
            unanswered_.pop_front();
            if(pull) {
                --pending_pulls_;
                return;
            }
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Evaluates a work item, or holds it while m are evaluated; drops it if the client
     *  ends (the server puts it back). @param frame The WORK */
    void acceptWork_(frame_type frame) {
        if(state_ == LinkState::ENDING) {
            return;
        }
        if(this->halt()) {
            this->beginEnding_(GPeerClose::NORMAL);
            return;
        }
        if(computing_ < options_.parallelEvaluations) {
            this->evaluate_(std::move(frame));
        }
        else {
            held_.push_back(std::move(frame));
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Evaluates a work item on the compute pool, makes its RESULT and encodes it there; the
     *  bytes go back to the io thread (evaluated_()). A work guard keeps the io context running
     *  until they have arrived. @param frame The WORK */
    void evaluate_(frame_type frame) {
        ++computing_;
        auto self = this->shared_from_this();
        auto guard = boost::asio::make_work_guard(io_context_);
        boost::asio::post(
            compute_pool_,
            [self,
             frame = std::move(frame),
             limit = max_message_bytes_,
             guard = std::move(guard)]() mutable {
                std::string bytes;
                try {
                    // A failing evaluation leaves the item flagged; it is returned like any result
                    Gem::Courtier::processWorkFrame(
                        frame,
                        self->options_.name,
                        self->itemProcessor()
                    );
                    std::ignore = Gem::Courtier::makeReturnFrame(frame);
                    bytes = self->encodeResult_(frame, limit);
                }
                catch(...) {
                    bytes = self->failedResult_(
                        frame,
                        "The work item could not be processed or returned: " +
                            Gem::Common::describeException(std::current_exception())
                    );
                }
                auto &io = self->io_context_;
                boost::asio::post(
                    io,
                    [self = std::move(self), bytes = std::move(bytes), guard = std::move(guard)]() {
                        self->evaluated_(makeMessage(bytes));
                    }
                );
            }
        );
    }

    //-------------------------------------------------------------------------
    /** @brief Encodes a RESULT; one that cannot be encoded, or that is longer than the transport
     *  carries, is returned as failed (failedResult_()). On a compute thread.
     *  @param frame The RESULT @param limit The longest message the transport carries
     *  @return The message */
    std::string encodeResult_(frame_type &frame, std::size_t limit) {
        try {
            std::string bytes = Gem::Courtier::wireEncode(frame, &wire_ctx_, options_.mode);
            if(bytes.size() > limit) {
                throw Gem::Common::gem_exception(
                    Gem::Common::g_error_streamer()
                    << "the message of " << bytes.size()
                    << " bytes is longer than the transport can carry (" << limit << " bytes)"
                    << '\n'
                );
            }
            return bytes;
        }
        catch(...) {
            return this->failedResult_(
                frame,
                "The result could not be encoded: " +
                    Gem::Common::describeException(std::current_exception())
            );
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Makes @p frame the RESULT of an item that failed with nothing to return, and encodes
     *  it (any thread but the io thread). Only a failing allocation could make the encoding of an
     *  outcome throw; that ends the program. @param frame Its frame; the ids in its head name the
     *  item @param reason Why it failed @return The message */
    std::string failedResult_(frame_type &frame, const std::string &reason) {
        glogger << "In " << options_.name << ": " << reason << '\n'
                << "It is returned to the server as failed; the client keeps running." << '\n'
                << GWARNING;
        Gem::Courtier::makeFailedResult(frame, frame.outcome(), reason);
        return Gem::Courtier::wireEncode(frame, &wire_ctx_, options_.mode);
    }

    //-------------------------------------------------------------------------
    /** @brief An evaluation has ended and its RESULT is encoded (io thread): the RESULT is sent,
     *  a held item is evaluated, and more is asked for -- or the client ends.
     *  @param result The RESULT */
    void evaluated_(GMessage result) {
        this->incrementProcessingCounter();
        --computing_;
        this->postResult_(std::move(result));
        if(state_ != LinkState::ENDING && this->halt()) {
            this->beginEnding_(GPeerClose::NORMAL);
        }
        if(state_ == LinkState::ENDING) {
            this->finishIfIdle_();
            return;
        }
        while(not held_.empty() && computing_ < options_.parallelEvaluations) {
            auto next = std::move(held_.front());
            held_.pop_front();
            this->evaluate_(std::move(next));
        }
        this->refill_();
    }

    //-------------------------------------------------------------------------
    /** @brief Puts a RESULT on the list and sends it if the link is open @param result It */
    void postResult_(GMessage result) {
        unanswered_.push_back(Entry{.bytes = std::move(result), .pull = false});
        if(contact_on_connection_) {
            this->send_(unanswered_.back());
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Sends PULLs while fewer items are in flight than the prefetch depth (OPEN only). */
    void refill_() {
        if(state_ != LinkState::OPEN) {
            return;
        }
        if(this->halt()) {
            this->beginEnding_(GPeerClose::NORMAL);
            return;
        }
        while(pending_pulls_ + held_.size() + computing_ < options_.prefetchDepth) {
            unanswered_.push_back(Entry{.bytes = pull_, .pull = true});
            ++pending_pulls_;
            this->send_(unanswered_.back());
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Hands a message of the list to the peer @param entry The message */
    void send_(Entry &entry) {
        entry.sent = true;
        if(auto peer = peer_.lock()) {
            peer->send(entry.bytes);
        }
    }

    //-------------------------------------------------------------------------
    /** @brief A message the client cannot handle ends it @param what Why */
    void protocolError_(const std::string &what) {
        glogger << "In " << options_.name
                << ": a message from the server cannot be handled:" << '\n'
                << what << '\n'
                << "The client will shut down." << '\n'
                << GWARNING;
        this->beginEnding_(GPeerClose::ERROR);
    }

    //-------------------------------------------------------------------------
    /** @brief The client ends (see the class comment): no timer, no attempt, nothing held.
     *  @param how What the server is told when the connection is closed */
    void beginEnding_(GPeerClose how) {
        if(state_ == LinkState::ENDING) {
            return;
        }
        state_ = LinkState::ENDING;
        close_how_ = how;
        halt_timer_.cancel();
        retry_timer_.cancel();
        held_.clear();
        if(attempting_) {
            connector_->cancel(); // so that run() does not wait for the attempt to end
            attempting_ = false;
        }
        this->finishIfIdle_();
    }

    //-------------------------------------------------------------------------
    /** @brief Closes the connection once an ending client evaluates nothing any more; run()
     *  returns when the peer has ended. The close sends what was handed over before it. */
    void finishIfIdle_() {
        if(state_ != LinkState::ENDING || computing_ > 0 || not peer_live_) {
            return;
        }
        if(auto peer = peer_.lock()) {
            peer->close(close_how_); // idempotent
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Checks m against n (m between 1 and n; n of 0 is taken as 1)
     *  @param options The options as given @return The options
     *  @throws gem_exception if m is 0 or above n */
    static GPeerClientOptions checkedOptions_(GPeerClientOptions options) {
        options.prefetchDepth = std::max<std::size_t>(options.prefetchDepth, 1);
        if(options.parallelEvaluations == 0 ||
           options.parallelEvaluations > options.prefetchDepth) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer()
                << "In " << options.name << ": Error!" << '\n'
                << "The client is to evaluate " << options.parallelEvaluations
                << " items at the same time (parallel evaluations), but it holds at most "
                << options.prefetchDepth << " (the prefetch depth). The parallel evaluations must "
                << "lie between 1 and the prefetch depth." << '\n'
            );
        }
        return options;
    }

    /** @brief @return The threads of the compute pool: m, and one more if m is below n */
    [[nodiscard]] std::size_t poolThreads_() const noexcept {
        return options_.parallelEvaluations +
               (options_.parallelEvaluations < options_.prefetchDepth ? 1 : 0);
    }

    //-------------------------------------------------------------------------
    // Data. Everything is touched on the io thread only, except where noted. The order matters:
    // the pool is destroyed (its threads joined) first, the connector before the io context.

    GPeerClientOptions options_;             ///< The settings (read on every thread, never written)
    boost::asio::io_context io_context_;     ///< The io thread's context
    std::unique_ptr<GConnectorI> connector_; ///< How connections are made
    std::weak_ptr<GPeerI> peer_;             ///< The connection, once there is one
    boost::asio::steady_timer halt_timer_{io_context_};  ///< Polls the halt conditions and limits
    boost::asio::steady_timer retry_timer_{io_context_}; ///< Ends a pause
    std::mt19937 rng_engine_{std::random_device{}()};    ///< Draws the pauses

    LinkState state_ = LinkState::CONNECTING;   ///< Where the client is with its connection
    GPeerClose close_how_ = GPeerClose::NORMAL; ///< What an ending client's close says
    GPeerEnd end_ = GPeerEnd::FAILED;     ///< How the last connection ended (read after run())
    std::uint64_t connection_ = 0;        ///< The number of the current (or last) connection
    bool attempting_ = false;             ///< The connector is making an attempt
    bool peer_live_ = false;              ///< A peer has been started and has not ended
    bool peer_opened_ = false;            ///< The current peer has opened
    bool contact_on_connection_ = false;  ///< The HELLO has arrived on the current connection
    bool contact_ever_ = false;           ///< A HELLO has arrived on some connection
    bool program_unchecked_told_ = false; ///< The log has said that the program is not checked
    clock::time_point run_started_at_;    ///< When run_() began
    clock::time_point last_contact_end_;  ///< The end of the last connection with contact
    std::chrono::milliseconds pause_length_ = GCLIENTRECONNECTPAUSEMIN; ///< The next pause, at most
    std::size_t max_message_bytes_ = std::numeric_limits<std::size_t>::max(); ///< The transport's

    std::deque<Entry> unanswered_;  ///< What was sent and not seen answered, in order
    std::size_t pending_pulls_ = 0; ///< The PULLs on that list
    std::deque<frame_type> held_;   ///< WORK items that wait for an evaluation slot
    std::size_t computing_ = 0;     ///< Items being evaluated

    std::uint64_t n_connections_ = 0;                ///< Connections that opened (read after run())
    std::atomic<std::uint64_t> n_losses_{0};         ///< Connections lost (any thread)
    std::atomic<std::uint64_t> n_results_resent_{0}; ///< RESULTs sent once more (any thread)

    /// The cache of received blobs and the wire scope around every (de)serialisation (blob
    /// send-once); used on the decode strand and the compute threads
    Gem::Courtier::GWireBlobRegistry wire_registry_;
    Gem::Courtier::GWireSerializationContext wire_ctx_;
    /// The PULL, encoded once and shared by every send
    GMessage pull_ = makeMessage(container_to_string(frame_type{GFrameKind::PULL}, options_.mode));

    /// The compute pool's reservation in the process-wide thread budget, released after the pool's
    /// threads are joined
    Gem::Common::Concurrency::GThreadBudget::Reservation compute_budget_{
        Gem::Common::Concurrency::threadBudget().reserve(
            "client:compute",
            static_cast<unsigned int>(this->poolThreads_()),
            Gem::Common::Concurrency::ThreadElasticity::Fixed
        )
    };
    boost::asio::thread_pool compute_pool_{this->poolThreads_()}; ///< Decodes and evaluates
    /// Decodes the server's messages one at a time, in order, on the compute pool
    boost::asio::strand<boost::asio::thread_pool::executor_type> decode_strand_{
        boost::asio::make_strand(compute_pool_)
    };
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
