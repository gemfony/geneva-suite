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

// Standard headers go here
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>

// Geneva headers go here
#include "common/concurrency/GThreadSafeKeyedStoreT.hpp"
#include "courtier/GWireSerializationContext.hpp" // GWirePeerId
#include "courtier/transport/GPeerI.hpp"

/******************************************************************************/
/**
 * @file
 * A transport that never leaves the process, for tests of the roles (session, client, shell) and
 * of the rules of GPeerI.hpp: two GInProcessPeer objects are the two ends of one connection, each
 * on a strand of an executor the test chooses; a message handed to one end's send() is posted to
 * the other end's strand. GInProcessListener and GInProcessConnector make such pairs, so that a
 * server and its clients can run in one process without a socket or MPI.
 *
 * A test can make an end misbehave (GInProcessFaults): lose what arrives after a number of
 * messages, never acknowledge a close, or fail to start. It can cut a connection as a reset would
 * (GInProcessPeer::sever()), and point the connectors of a client at another listener, or at none
 * (GInProcessRoute).
 */

namespace Gem::Courtier::Tests {

/******************************************************************************/
/** @brief What a test can make one end of an in-process connection do wrong. */
struct GInProcessFaults {
    /// Messages that arrive at this end after this many have been delivered are lost
    std::size_t deliverAtMost = std::numeric_limits<std::size_t>::max();
    /// false: a close that arrives at this end is ignored (neither acknowledged nor reported)
    bool acknowledgeClose = true;
    /// true: start() fails; onClosed (FAILED) is reported, onOpen never
    bool failStart = false;
};

/******************************************************************************/
/**
 * One end of an in-process connection. Everything but start(), close() and executor() happens on
 * the end's strand. The close protocol is the one a real transport has: a close is a message to
 * the other end, which reports CLOSED_THERE and acknowledges; the closing end reports CLOSED_HERE
 * on the acknowledgement (the close is confirmed, GPeerI.hpp rule 6), or CLOSED_HERE_UNCONFIRMED
 * after its grace if none comes. As the websocket peer, an end delivers what arrives after close()
 * until its own close has gone out (then it drops what arrives), and two ends whose closes cross
 * both report CLOSED_HERE_UNCONFIRMED: the other end's own close is no acknowledgement. Messages
 * that arrive before start() has opened the end are kept and delivered after onOpen; an end whose
 * role asked for the close before it was told of the open handles nothing that arrived before its
 * own close has gone out (GPeerI.hpp, rule 6).
 *
 * While it is open, an end has a wait pending on its strand (a timer that never expires on its
 * own); that pending work owns the end and keeps the executor's context running (rule 4). It is
 * cancelled when the end has ended.
 *
 * The two ends never own each other. An end's strand and timers live on its executor's context, so
 * the end must be gone before that context is: only work pending on its own strand (and a role,
 * briefly, on that context's threads) may own it. The ends reach each other through a link they
 * share: an end posts to the other end's strand under the link's mutex, only while the other end
 * exists (its destructor takes it off the link under the same mutex), and the posted handler holds
 * the other end weakly. So no thread of one end's context keeps the other end alive, destroys it,
 * or posts into its context after that context may have gone.
 */
class GInProcessPeer final
  : public Gem::Courtier::Consumers::GPeerI
  , public std::enable_shared_from_this<GInProcessPeer> {
    /** @brief Lets makePair() use the constructor, and nobody else */
    struct PassKey {
        explicit PassKey() = default;
    };

    using GPeerEnd = Gem::Courtier::Consumers::GPeerEnd;
    using GPeerClose = Gem::Courtier::Consumers::GPeerClose;

public:
    using peer_ptr = std::shared_ptr<GInProcessPeer>;

    /***************************************************************************/
    /**
     * @brief Makes the two ends of one connection. Neither has been started.
     *
     * @param first_executor The executor the first end makes its strand on
     * @param first_faults How the first end misbehaves
     * @param second_executor The executor the second end makes its strand on
     * @param second_faults How the second end misbehaves
     * @param close_grace How long a closing end waits for the acknowledgement
     * @return The first and the second end
     */
    static std::pair<peer_ptr, peer_ptr> makePair(
        const boost::asio::any_io_executor &first_executor,
        GInProcessFaults first_faults,
        const boost::asio::any_io_executor &second_executor,
        GInProcessFaults second_faults,
        std::chrono::milliseconds close_grace
    ) {
        auto first =
            std::make_shared<GInProcessPeer>(PassKey{}, first_executor, first_faults, close_grace);
        auto second = std::make_shared<GInProcessPeer>(
            PassKey{},
            second_executor,
            second_faults,
            close_grace
        );
        auto link = std::make_shared<Link>();
        link->ends = {first.get(), second.get()};
        first->link_ = link;
        first->side_ = 0;
        second->link_ = link;
        second->side_ = 1;
        return {std::move(first), std::move(second)};
    }

    /***************************************************************************/
    /** @brief Takes this end off the link: the other end no longer posts to it */
    ~GInProcessPeer() override {
        if(link_) {
            const std::scoped_lock lock(link_->mutex);
            link_->ends.at(side_) = nullptr;
        }
    }

    GInProcessPeer(const GInProcessPeer &) = delete;
    GInProcessPeer(GInProcessPeer &&) = delete;
    GInProcessPeer &operator=(const GInProcessPeer &) = delete;
    GInProcessPeer &operator=(GInProcessPeer &&) = delete;

    /** @brief For a test: @p hook runs on this end's strand each time it hands something to the
     *  other end, before it reaches for it, and is shown what is handed over (a message's bytes; a
     *  close's word as one byte). A hook may sever() the connection: what it was shown is then not
     *  delivered. Before start(). @param hook What runs */
    void setTransmitHook(std::move_only_function<void(std::string_view)> hook) {
        transmit_hook_ = std::move(hook);
    }

    /** @brief Cuts the connection as a reset would. From any thread. Both ends end with
     *  onClosed (GPeerEnd::FAILED), each on its strand; from now on nothing is delivered to either
     *  end, also what was handed over before and has not been delivered yet, and no close
     *  travels. */
    void sever() {
        link_->severed.store(true);
        const std::scoped_lock lock(link_->mutex);
        for(GInProcessPeer *end : link_->ends) {
            if(end == nullptr) {
                continue;
            }
            boost::asio::post(end->strand_, [weak = end->weak_from_this()] {
                if(auto alive = weak.lock()) {
                    alive->finish_(GPeerEnd::FAILED, GPeerClose::NORMAL, "the connection was cut");
                }
            });
        }
    }

    /***************************************************************************/
    /**
     * @brief Makes one end; see makePair().
     *
     * @param key Only makePair() has one
     * @param executor The executor this end makes its strand on
     * @param faults How this end misbehaves
     * @param close_grace How long a closing end waits for the acknowledgement
     */
    GInProcessPeer(
        [[maybe_unused]] PassKey key,
        const boost::asio::any_io_executor &executor,
        GInProcessFaults faults,
        std::chrono::milliseconds close_grace
    )
      : strand_(boost::asio::make_strand(executor))
      , executor_(strand_)
      , pending_(strand_)
      , close_timer_(strand_)
      , faults_(faults)
      , close_grace_(close_grace) { /* nothing */
    }

    /***************************************************************************/
    /** @brief Installs the handlers on the strand and opens this end there (or fails, if the
     *  test said so). @param handlers What the role wants to be told */
    void start(Gem::Courtier::Consumers::GPeerHandlers handlers) override {
        boost::asio::post(
            strand_,
            [self = this->shared_from_this(), handlers = std::move(handlers)]() mutable {
                self->handlers_ = std::move(handlers);
                if(self->state_ == state::ENDED) {
                    // Aborted before this ran: it does not open; the role hears the end now.
                    auto ended = std::move(self->handlers_);
                    self->handlers_ = Gem::Courtier::Consumers::GPeerHandlers{};
                    if(ended.onClosed) {
                        ended.onClosed(self->aborted_end_, GPeerClose::NORMAL, "aborted");
                    }
                    return;
                }
                if(self->faults_.failStart) {
                    self->finish_(
                        GPeerEnd::FAILED,
                        GPeerClose::NORMAL,
                        "the test made start() fail"
                    );
                }
                else {
                    self->opened_();
                }
            }
        );
    }

    /** @brief Hands a message to the other end; dropped before onOpen and after close(). The
     *  other end gets a copy of its own, as from a wire.
     *  @param message The complete message */
    void send(Gem::Courtier::Consumers::GMessage message) override {
        if(state_ != state::OPEN || close_requested_.load() || not message ||
           link_->severed.load()) {
            return;
        }
        this->transmit_(wire::MESSAGE, std::string(*message));
    }

    /** @brief Closes after what was handed to send() before. Any thread; idempotent.
     *  @param how What the other end is told */
    void close(GPeerClose how) override {
        if(close_requested_.exchange(true)) {
            return;
        }
        boost::asio::post(strand_, [self = this->shared_from_this(), how] {
            self->beginClose_(how);
        });
    }

    /** @brief Tells the other end and ends at once, reporting onClosed inline: CLOSED_HERE, or
     *  CLOSED_HERE_UNCONFIRMED if close() had been called before (GPeerI::abort()). Beyond GPeerI's
     *  teardown, this test pair also allows it on its strand: the tests lose a connection so. */
    void abort() override {
        if(state_ == state::ENDED) {
            return;
        }
        if(close_requested_.exchange(true)) {
            aborted_end_ = GPeerEnd::CLOSED_HERE_UNCONFIRMED; // a close that had not been confirmed
        }
        if(state_ == state::OPEN) {
            this->transmit_(wire::CLOSE, encode_(GPeerClose::NORMAL));
        }
        this->finish_(aborted_end_, GPeerClose::NORMAL, "aborted");
    }

    /** @brief @return The strand of this end */
    [[nodiscard]] boost::asio::any_io_executor executor() override {
        return executor_;
    }

    /** @brief Records what the owner said, for the test to read. @param busy Whether it is busy */
    void setOwnerBusy(bool busy) override {
        owner_busy_.store(busy);
    }

    /***************************************************************************/
    // What a test may observe, from any thread

    /** @brief @return What the owner said last through setOwnerBusy() */
    [[nodiscard]] bool ownerBusy() const {
        return owner_busy_.load();
    }
    /** @brief @return How many messages have been delivered to the role */
    [[nodiscard]] std::size_t nDelivered() const {
        return n_delivered_.load();
    }
    /** @brief @return Whether this end has ended */
    [[nodiscard]] bool ended() const {
        return ended_.load();
    }

private:
    /** @brief Where an end is; read and written on its strand only */
    enum class state : std::uint8_t {
        NEW,
        OPEN,
        CLOSING,
        ENDED
    };
    /** @brief What travels between the two ends */
    enum class wire : std::uint8_t {
        MESSAGE,
        CLOSE,
        CLOSE_ACK
    };

    /** @brief One thing that arrived before this end was opened */
    struct Arrival {
        wire kind;           ///< What it is
        std::string payload; ///< Its bytes
    };

    /***************************************************************************/
    /** @brief @param how A close's word @return It, as the payload of a close */
    static std::string encode_(GPeerClose how) {
        return std::string(1, static_cast<char>(how));
    }
    /** @brief @param payload The payload of a close @return The close's word */
    static GPeerClose decode_(const std::string &payload) {
        return payload.empty() ? GPeerClose::NORMAL : static_cast<GPeerClose>(payload.front());
    }

    /** @brief Posts one thing to the other end's strand. On this end's strand, so that what one
     *  end hands over arrives in order. Lost if the other end is gone. The post is made under the
     *  link's mutex while the other end exists, and the handler holds it weakly (see the class
     *  comment): this end never owns the other one.
     *  @param kind What it is @param payload Its bytes */
    void transmit_(wire kind, std::string payload) {
        if(transmit_hook_) {
            transmit_hook_(payload);
        }
        if(link_->severed.load()) {
            return;
        }
        const std::scoped_lock lock(link_->mutex);
        GInProcessPeer *other = link_->ends.at(1 - side_);
        if(other == nullptr) {
            return;
        }
        boost::asio::post(
            other->strand_,
            [weak = other->weak_from_this(), kind, payload = std::move(payload)]() mutable {
                if(auto end = weak.lock()) {
                    end->received_(kind, std::move(payload));
                }
            }
        );
    }

    /** @brief Runs on the strand after start(): the end is open. The pending wait that owns it
     *  is armed, the role is told (unless it has closed meanwhile: after close() onOpen is not
     *  delivered, as by the websocket peer), and what arrived before is delivered (also after
     *  close(): this end's close has not gone out yet, GPeerI.hpp rule 6). */
    void opened_() {
        state_ = state::OPEN;
        pending_.expires_at(boost::asio::steady_timer::time_point::max());
        pending_.async_wait([self = this->shared_from_this()](const boost::system::error_code &) {
            // Only holds the end; it is cancelled when the end has ended.
        });
        if(not close_requested_.load()) {
            role_open_ = true;
            if(handlers_.onOpen) {
                handlers_.onOpen();
            }
        }
        else {
            // The role asked for the close before it was told of the open: it is told nothing,
            // and what has arrived waits until this end's close has gone out (beginClose_()), as
            // with the websocket peer: an acknowledgement now would prove what no role received
            // (GPeerI.hpp, rule 6)
            return;
        }
        while(not early_.empty() && state_ != state::ENDED) {
            Arrival arrival = std::move(early_.front());
            early_.pop_front();
            this->received_(arrival.kind, std::move(arrival.payload));
        }
    }

    /** @brief Something from the other end, on this end's strand.
     *  @param kind What it is @param payload Its bytes */
    void received_(wire kind, std::string &&payload) {
        if(link_->severed.load()) {
            return; // cut: what was on its way is lost (sever() ends this end)
        }
        if(state_ == state::NEW || (state_ == state::OPEN && not role_open_)) {
            // Not opened yet, or opened for a role that asked for the close before (see opened_())
            early_.push_back(Arrival{.kind = kind, .payload = std::move(payload)});
            return;
        }
        switch(state_) {
        case state::NEW:
            return; // kept in early_ above
        case state::ENDED:
            return;
        case state::OPEN:
        case state::CLOSING:
            break;
        }

        switch(kind) {
        case wire::MESSAGE:
            // Delivered until this end's close has gone out (CLOSING), also after close() (rule 6)
            // -- but never to a role that was not told of the open (rule 1)
            if(state_ == state::OPEN && role_open_ && n_delivered_.load() < faults_.deliverAtMost &&
               handlers_.onMessage) {
                n_delivered_.fetch_add(1);
                handlers_.onMessage(std::move(payload));
            }
            break;

        case wire::CLOSE:
            if(not faults_.acknowledgeClose) {
                break;
            }
            if(state_ == state::CLOSING) {
                // The two closes crossed: the other end's own close is no acknowledgement of ours
                // (what this end sent may have arrived there after its close went out)
                this->finish_(
                    GPeerEnd::CLOSED_HERE_UNCONFIRMED,
                    decode_(payload),
                    "the other end's own close crossed ours"
                );
            }
            else {
                this->transmit_(wire::CLOSE_ACK, payload); // echoes what the close said
                this->finish_(GPeerEnd::CLOSED_THERE, decode_(payload), "closed by the other end");
            }
            break;

        case wire::CLOSE_ACK:
            if(state_ == state::CLOSING) {
                this->finish_(GPeerEnd::CLOSED_HERE, decode_(payload), "closed");
            }
            break;
        }
    }

    /** @brief Begins the close on the strand: tells the other end (after everything sent before)
     *  and waits for its acknowledgement, at most the grace.
     *  @param how What the other end is told */
    void beginClose_(GPeerClose how) {
        if(state_ != state::OPEN) {
            return; // never opened (start() failed), closing already, or ended
        }
        state_ = state::CLOSING;
        this->transmit_(wire::CLOSE, encode_(how));
        close_timer_.expires_after(close_grace_);
        close_timer_.async_wait(
            [self = this->shared_from_this()](const boost::system::error_code &ec) {
                if(not ec && self->state_ == state::CLOSING) {
                    self->finish_(
                        GPeerEnd::CLOSED_HERE_UNCONFIRMED,
                        GPeerClose::NORMAL,
                        "the other end did not confirm the close in time"
                    );
                }
            }
        );
        // What arrived for a role that closed before it was told of the open meets this end's close
        // now: a message is dropped, a close of the other end has crossed this one
        while(not early_.empty() && state_ == state::CLOSING) {
            Arrival arrival = std::move(early_.front());
            early_.pop_front();
            this->received_(arrival.kind, std::move(arrival.payload));
        }
    }

    /** @brief The end, whatever led to it; runs its body once. Tells the role and drops the
     *  handlers, and with them this end's hold on the role.
     *  @param end Why it ended @param how What the other end's close said
     *  @param detail Words about it */
    void finish_(GPeerEnd end, GPeerClose how, std::string_view detail) {
        if(state_ == state::ENDED) {
            return;
        }
        state_ = state::ENDED;
        close_requested_.store(true);
        early_.clear();
        close_timer_.cancel();
        pending_.cancel();
        ended_.store(true);

        Gem::Courtier::Consumers::GPeerHandlers handlers = std::move(handlers_);
        handlers_ = Gem::Courtier::Consumers::GPeerHandlers{};
        if(handlers.onClosed) {
            handlers.onClosed(end, how, detail);
        }
    }

    /***************************************************************************/
    boost::asio::strand<boost::asio::any_io_executor> strand_; ///< Serializes this end
    boost::asio::any_io_executor executor_; ///< The strand, as executor() hands it out
    boost::asio::steady_timer pending_;     ///< The wait that owns this end while it is open
    boost::asio::steady_timer close_timer_; ///< Bounds the wait for an acknowledgement
    GInProcessFaults faults_;               ///< How this end misbehaves
    std::chrono::milliseconds close_grace_; ///< How long a close waits for its acknowledgement
    /** @brief What the two ends of a pair share: where each one is, while it exists */
    struct Link {
        std::mutex mutex;                        ///< Guards ends, and every post to an end
        std::array<GInProcessPeer *, 2> ends{}; ///< Non-owning; null once that end is destroyed
        std::atomic<bool> severed{false};       ///< sever() was called: nothing is delivered
    };
    std::shared_ptr<Link> link_; ///< Shared with the other end (set once, before any start())
    std::size_t side_ = 0;       ///< This end's place in the link
    std::move_only_function<void(std::string_view)> transmit_hook_; ///< See setTransmitHook()

    Gem::Courtier::Consumers::GPeerHandlers handlers_; ///< On the strand; dropped at the end
    state state_ = state::NEW;                         ///< On the strand
    bool role_open_ = false; ///< On the strand: the role was told of the open (or had no onOpen)
    std::deque<Arrival> early_;                        ///< On the strand: what came before onOpen

    std::atomic<bool> close_requested_{false}; ///< close() was called, or the end has ended
    /// What abort() reports, also to a start() whose work runs after it; where abort() runs (the
    /// strand, or a teardown when nothing runs)
    GPeerEnd aborted_end_ = GPeerEnd::CLOSED_HERE;
    std::atomic<bool> ended_{false};           ///< For the test: the end has ended
    std::atomic<bool> owner_busy_{false};      ///< For the test: the last setOwnerBusy()
    std::atomic<std::size_t> n_delivered_{0};  ///< For the test: messages given to the role
};

/******************************************************************************/
/**
 * The server side of the in-process transport. A GInProcessConnector asks it for a connection;
 * it makes a pair, hands the server end to the shell (on_peer, on a strand of the executor given
 * to start()) and the client end to the connector. It has no work of its own. stop() waits, at most
 * Options::stopBound, for the calls of on_peer it has posted (GListenerI::stop()); a test can hold
 * the strand those calls run on (holdAcceptStrand()).
 */
class GInProcessListener final : public Gem::Courtier::Consumers::GListenerI {
public:
    /** @brief What a test sets for the pairs this listener makes */
    struct Options {
        GInProcessFaults serverFaults{};            ///< How the server ends misbehave
        GInProcessFaults clientFaults{};            ///< How the client ends misbehave
        std::chrono::milliseconds closeGrace{1000}; ///< The ends' wait for an acknowledgement
        std::chrono::milliseconds stopGrace{2000};  ///< What stopGrace() answers
        bool detectsLoss = true;                    ///< What detectsLoss() answers
        bool startThrows = false; ///< start() throws, as one whose port cannot be bound
        /// How long stop() waits for the calls of on_peer under way
        std::chrono::milliseconds stopBound{1000};
        bool stopThrows = false; ///< stop() throws (after it has stopped accepting)
    };

    /** @brief @param options What the pairs are made with */
    explicit GInProcessListener(Options options)
      : options_(options) { /* nothing */
    }

    /** @brief Starts accepting connections.
     *  @param executor Where the server ends and on_peer run
     *  @param on_peer What is called for every connection */
    void start(boost::asio::any_io_executor executor, on_peer_type on_peer) override {
        if(options_.startThrows) {
            // As GListenerI::start() demands: nothing is kept of a start that failed.
            throw std::runtime_error("the test made the listener's start() fail");
        }
        const std::scoped_lock lock(mutex_);
        executor_ = std::move(executor);
        accept_strand_.emplace(boost::asio::make_strand(executor_));
        on_peer_ = std::move(on_peer);
        accepting_ = true;
    }

    /** @brief No new connections, and no call of on_peer under way: waits for those posted, at
     *  most Options::stopBound (GListenerI::stop()) */
    void stop() override {
        {
            const std::scoped_lock lock(mutex_);
            accepting_ = false;
            n_stops_.fetch_add(1);
        }
        if(not on_peer_calls_.waitUntilEmpty(options_.stopBound)) {
            n_stop_bounds_hit_.fetch_add(1);
        }
        if(options_.stopThrows) {
            throw std::runtime_error("the test made the listener's stop() fail");
        }
    }

    /** @brief Nothing of its own runs; lets go of on_peer and of everything that refers to the
     *  executor's context, which is destroyed before this listener (GPeerI.hpp, rule 7) */
    void endWork() override {
        const std::scoped_lock lock(mutex_);
        accepting_ = false;
        on_peer_ = nullptr;
        accept_strand_.reset();
        executor_ = boost::asio::any_io_executor{};
        work_ended_.store(true);
    }

    /** @brief @return What the options say */
    [[nodiscard]] std::chrono::milliseconds stopGrace() const override {
        return options_.stopGrace;
    }

    /** @brief @return What the options say */
    [[nodiscard]] bool detectsLoss() const override {
        return options_.detectsLoss;
    }

    /***************************************************************************/
    /**
     * @brief Makes a connection for a connector: the server end goes to on_peer (posted to the
     * accept strand), the client end is returned. From the client's executor.
     *
     * @param client_executor Where the client end makes its strand
     * @return The client end, or nothing if this listener does not accept
     */
    std::shared_ptr<Gem::Courtier::Consumers::GPeerI>
    connect_(const boost::asio::any_io_executor &client_executor) {
        const std::scoped_lock lock(mutex_);
        if(not accepting_) {
            return nullptr;
        }
        auto [server_end, client_end] = GInProcessPeer::makePair(
            executor_,
            options_.serverFaults,
            client_executor,
            options_.clientFaults,
            options_.closeGrace
        );
        const Gem::Courtier::GWirePeerId id = next_id_.fetch_add(1);
        last_server_end_ = server_end;
        // Counted under the lock that stop() takes first: a call posted before it is waited for.
        on_peer_calls_.set(id, true);
        boost::asio::post(*accept_strand_, [this, server_end, id] {
            // endWork() runs when no handler runs, so on_peer_ cannot change under this. As with
            // the websocket listener, no exception leaves a thread of the shell's executor: a
            // client whose session cannot be made is dropped, and the call is counted out.
            try {
                if(on_peer_) {
                    on_peer_(server_end, id);
                }
            }
            catch(...) { // NOLINT(bugprone-empty-catch) -- the connection ends with its server end
            }
            on_peer_calls_.remove(id);
        });
        return client_end;
    }

    /** @brief Runs @p hold on the strand the calls of on_peer run on: a call posted while it
     *  runs waits for it. After start(). @param hold What occupies the strand */
    void holdAcceptStrand(std::move_only_function<void()> hold) {
        const std::scoped_lock lock(mutex_);
        boost::asio::post(*accept_strand_, std::move(hold));
    }

    /** @brief @return The server end of the last connection made, if it still exists (a test
     *  may hold its strand, or close it as its session would) */
    [[nodiscard]] std::shared_ptr<GInProcessPeer> lastServerEnd() {
        const std::scoped_lock lock(mutex_);
        return last_server_end_.lock();
    }
    /** @brief @return How many connections have been made */
    [[nodiscard]] std::size_t nConnections() const {
        return next_id_.load() - 1;
    }
    /** @brief @return Whether endWork() has been called */
    [[nodiscard]] bool workEnded() const {
        return work_ended_.load();
    }
    /** @brief @return How often stop() has been called */
    [[nodiscard]] std::size_t nStops() const {
        return n_stops_.load();
    }
    /** @brief @return How often stop() gave up waiting for the calls of on_peer */
    [[nodiscard]] std::size_t nStopBoundsHit() const {
        return n_stop_bounds_hit_.load();
    }

private:
    Options options_; ///< What the pairs are made with
    /// Guards executor_, accept_strand_ and accepting_: connect_() runs on a client's executor,
    /// start(), stop() and endWork() on the shell's side
    std::mutex mutex_;
    boost::asio::any_io_executor executor_; ///< Set by start(), dropped by endWork()
    /// Serializes the calls of on_peer; set by start(), dropped by endWork()
    std::optional<boost::asio::strand<boost::asio::any_io_executor>> accept_strand_;
    on_peer_type on_peer_;                               ///< Set by start(), dropped by endWork()
    std::weak_ptr<GInProcessPeer> last_server_end_;      ///< See lastServerEnd()
    bool accepting_ = false;                             ///< Between start() and stop()
    std::atomic<bool> work_ended_{false};                ///< endWork() has been called
    std::atomic<std::size_t> n_stops_{0};                ///< How often stop() has been called
    std::atomic<std::size_t> n_stop_bounds_hit_{0}; ///< How often stop() gave up waiting
    /// The calls of on_peer posted and not returned, by connection id (stop() waits for them)
    Gem::Common::Concurrency::GThreadSafeKeyedStoreT<Gem::Courtier::GWirePeerId, bool>
        on_peer_calls_;
    std::atomic<Gem::Courtier::GWirePeerId> next_id_{1}; ///< The id of the next connection
};

/******************************************************************************/
/**
 * A listener that a server's shell can own (it takes a std::unique_ptr<GListenerI>) while the test
 * keeps the GInProcessListener it forwards to, for its connectors (which hold it weakly) and for
 * what it observes.
 */
class GInProcessListenerRef final : public Gem::Courtier::Consumers::GListenerI {
public:
    /** @brief @param listener The listener every call is forwarded to */
    explicit GInProcessListenerRef(std::shared_ptr<GInProcessListener> listener)
      : listener_(std::move(listener)) { /* nothing */
    }

    /** @brief Forwards @param executor See GListenerI @param on_peer See GListenerI */
    void start(boost::asio::any_io_executor executor, on_peer_type on_peer) override {
        listener_->start(std::move(executor), std::move(on_peer));
    }
    /** @brief Forwards */
    void stop() override {
        listener_->stop();
    }
    /** @brief Forwards */
    void endWork() override {
        listener_->endWork();
    }
    /** @brief Forwards @return What the listener says */
    [[nodiscard]] std::chrono::milliseconds stopGrace() const override {
        return listener_->stopGrace();
    }
    /** @brief Forwards @return What the listener says */
    [[nodiscard]] bool detectsLoss() const override {
        return listener_->detectsLoss();
    }

private:
    std::shared_ptr<GInProcessListener> listener_; ///< Where every call goes
};

/******************************************************************************/
/** @brief Where a test sees the client ends a GInProcessConnector has made (the connector itself
 *  is owned by the client under test). */
class GInProcessPeerWatch {
public:
    /** @brief Records a client end, and shows it to what setOnMade() installed (before the end
     *  is started) @param peer The end the connector has made */
    void made(const std::shared_ptr<GInProcessPeer> &peer) {
        const std::scoped_lock lock(mutex_);
        last_ = peer;
        if(on_made_) {
            on_made_(*peer);
        }
    }
    /** @brief @p on_made is shown every client end made from now on, before it is started (on
     *  the connector's executor) @param on_made What is called */
    void setOnMade(std::function<void(GInProcessPeer &)> on_made) {
        const std::scoped_lock lock(mutex_);
        on_made_ = std::move(on_made);
    }
    /** @brief @return The last client end made, if it still exists */
    [[nodiscard]] std::shared_ptr<GInProcessPeer> last() const {
        const std::scoped_lock lock(mutex_);
        return last_.lock();
    }

private:
    mutable std::mutex mutex_;           ///< Guards last_ (the connector's executor writes it)
    std::weak_ptr<GInProcessPeer> last_; ///< The last client end made
    std::function<void(GInProcessPeer &)> on_made_; ///< Shown every end made (may be empty)
};

/******************************************************************************/
/**
 * What a GInProcessConnector connects to: a listener, which a test may set, clear (connects are
 * then refused) or switch at any time, from any thread. The listener is held weakly.
 */
class GInProcessRoute {
public:
    /** @brief @param listener The listener connects go to; empty: they are refused */
    explicit GInProcessRoute(std::weak_ptr<GInProcessListener> listener = {})
      : listener_(std::move(listener)) { /* nothing */
    }

    /** @brief Connects from now on go to @p listener @param listener The listener */
    void set(std::weak_ptr<GInProcessListener> listener) {
        const std::scoped_lock lock(mutex_);
        listener_ = std::move(listener);
    }
    /** @brief Connects from now on are refused */
    void clear() {
        this->set({});
    }
    /** @brief @return The listener connects go to now, if there is one */
    [[nodiscard]] std::shared_ptr<GInProcessListener> listener() const {
        const std::scoped_lock lock(mutex_);
        return listener_.lock();
    }

private:
    mutable std::mutex mutex_;                   ///< Guards listener_
    std::weak_ptr<GInProcessListener> listener_; ///< Where connects go
};

/******************************************************************************/
/**
 * The client side of the in-process transport: asks the listener its route points at for a
 * connection (a route without one refuses).
 *
 * The order a test keeps (rule 7): a connector is destroyed when no handler of its executor runs
 * any more, so it is declared before the IoThreads (or io context) its client ends run on and goes
 * after them. Likewise a test that holds an end (or a role that holds one) lets go of it before
 * that end's context goes: declare it after the context. The flag an attempt shares with the
 * connector is atomic all the same, so a connector destroyed early does not race with an attempt.
 */
class GInProcessConnector final : public Gem::Courtier::Consumers::GConnectorI {
public:
    /** @brief @param route Where the server is @param reconnectable What reconnectable() answers
     *  @param watch Where the client ends made are recorded (may be empty) */
    explicit GInProcessConnector(
        std::shared_ptr<GInProcessRoute> route,
        bool reconnectable = true,
        std::shared_ptr<GInProcessPeerWatch> watch = nullptr
    )
      : route_(std::move(route))
      , reconnectable_(reconnectable)
      , watch_(std::move(watch)) { /* nothing */
    }

    /** @brief Cancels an attempt in progress */
    ~GInProcessConnector() override {
        this->cancel();
    }

    GInProcessConnector(const GInProcessConnector &) = delete;
    GInProcessConnector(GInProcessConnector &&) = delete;
    GInProcessConnector &operator=(const GInProcessConnector &) = delete;
    GInProcessConnector &operator=(GInProcessConnector &&) = delete;

    /** @brief Asks the listener for a connection; done is posted to @p executor.
     *  @param executor Where the client end and done run
     *  @param done What is told the outcome */
    void connect(boost::asio::any_io_executor executor, done_type done) override {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        cancelled_ = cancelled;
        boost::asio::post(
            executor,
            [route = route_,
             watch = watch_,
             executor,
             cancelled,
             done = std::move(done)]() mutable {
                if(cancelled->load()) {
                    return;
                }
                auto server = route->listener();
                auto peer = server ? server->connect_(executor) : nullptr;
                if(peer && watch) {
                    watch->made(std::static_pointer_cast<GInProcessPeer>(peer));
                }
                done(peer, peer ? std::string_view{} : std::string_view{"the server refuses"});
            }
        );
    }

    /** @brief The attempt in progress does not call done */
    void cancel() override {
        if(cancelled_) {
            cancelled_->store(true);
        }
    }

    /** @brief @return What the constructor was told */
    [[nodiscard]] bool reconnectable() const override {
        return reconnectable_;
    }

private:
    std::shared_ptr<GInProcessRoute> route_;     ///< Where the server is
    bool reconnectable_;                         ///< What reconnectable() answers
    std::shared_ptr<GInProcessPeerWatch> watch_; ///< Where the client ends are recorded, or empty
    std::shared_ptr<std::atomic<bool>> cancelled_; ///< The flag of the last attempt
};

/******************************************************************************/

} /* namespace Gem::Courtier::Tests */
