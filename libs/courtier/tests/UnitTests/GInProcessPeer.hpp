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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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
 * messages, never acknowledge a close, or fail to start.
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
 * on the acknowledgement, or after its grace if none comes. Two ends that close at once take each
 * other's close as the acknowledgement. Messages that arrive before start() has opened the end
 * are kept and delivered after onOpen.
 *
 * While it is open, an end has a wait pending on its strand (a timer that never expires on its
 * own); that pending work owns the end and keeps the executor's context running (rule 4). It is
 * cancelled when the end has ended.
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
        first->other_ = second;
        second->other_ = first;
        return {std::move(first), std::move(second)};
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

    /** @brief Hands a message to the other end; dropped before onOpen and after close().
     *  @param message The complete message */
    void send(std::string message) override {
        if(state_ != state::OPEN || close_requested_.load()) {
            return;
        }
        this->transmit_(wire::MESSAGE, std::move(message));
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

    /** @brief Tells the other end and ends at once, reporting onClosed inline. */
    void abort() override {
        if(state_ == state::ENDED) {
            return;
        }
        close_requested_.store(true);
        if(state_ == state::OPEN) {
            this->transmit_(wire::CLOSE, encode_(GPeerClose::NORMAL));
        }
        this->finish_(GPeerEnd::CLOSED_HERE, GPeerClose::NORMAL, "aborted");
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
     *  end hands over arrives in order. Lost if the other end is gone.
     *  @param kind What it is @param payload Its bytes */
    void transmit_(wire kind, std::string payload) {
        if(auto other = other_.lock()) {
            boost::asio::post(
                other->strand_,
                [other, kind, payload = std::move(payload)]() mutable {
                    other->received_(kind, std::move(payload));
                }
            );
        }
    }

    /** @brief Runs on the strand after start(): the end is open. The pending wait that owns it
     *  is armed, the role is told, and what arrived before is delivered. */
    void opened_() {
        state_ = state::OPEN;
        pending_.expires_at(boost::asio::steady_timer::time_point::max());
        pending_.async_wait([self = this->shared_from_this()](const boost::system::error_code &) {
            // Only holds the end; it is cancelled when the end has ended.
        });
        if(handlers_.onOpen) {
            handlers_.onOpen();
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
        switch(state_) {
        case state::NEW:
            early_.push_back(Arrival{.kind = kind, .payload = std::move(payload)});
            return;
        case state::ENDED:
            return;
        case state::OPEN:
        case state::CLOSING:
            break;
        }

        switch(kind) {
        case wire::MESSAGE:
            // After close() nothing is delivered (rule 6).
            if(state_ == state::OPEN && not close_requested_.load() &&
               n_delivered_.load() < faults_.deliverAtMost && handlers_.onMessage) {
                n_delivered_.fetch_add(1);
                handlers_.onMessage(std::move(payload));
            }
            break;

        case wire::CLOSE:
            if(not faults_.acknowledgeClose) {
                break;
            }
            if(state_ == state::CLOSING) {
                // Both ends closed at once: the other's close confirms ours.
                this->finish_(GPeerEnd::CLOSED_HERE, decode_(payload), "both ends closed");
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
                        GPeerEnd::CLOSED_HERE,
                        GPeerClose::NORMAL,
                        "the other end did not confirm the close in time"
                    );
                }
            }
        );
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
    std::weak_ptr<GInProcessPeer> other_;   ///< The other end (set once, before any start())

    Gem::Courtier::Consumers::GPeerHandlers handlers_; ///< On the strand; dropped at the end
    state state_ = state::NEW;                         ///< On the strand
    std::deque<Arrival> early_;                        ///< On the strand: what came before onOpen

    std::atomic<bool> close_requested_{false}; ///< close() was called, or the end has ended
    std::atomic<bool> ended_{false};           ///< For the test: the end has ended
    std::atomic<bool> owner_busy_{false};      ///< For the test: the last setOwnerBusy()
    std::atomic<std::size_t> n_delivered_{0};  ///< For the test: messages given to the role
};

/******************************************************************************/
/**
 * The server side of the in-process transport. A GInProcessConnector asks it for a connection;
 * it makes a pair, hands the server end to the shell (on_peer, on a strand of the executor given
 * to start()) and the client end to the connector. It has no work of its own.
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
    };

    /** @brief @param options What the pairs are made with */
    explicit GInProcessListener(Options options)
      : options_(options) { /* nothing */
    }

    /** @brief Starts accepting connections.
     *  @param executor Where the server ends and on_peer run
     *  @param on_peer What is called for every connection */
    void start(boost::asio::any_io_executor executor, on_peer_type on_peer) override {
        const std::scoped_lock lock(mutex_);
        executor_ = std::move(executor);
        accept_strand_.emplace(boost::asio::make_strand(executor_));
        on_peer_ = std::move(on_peer);
        accepting_ = true;
    }

    /** @brief No new connections */
    void stop() override {
        const std::scoped_lock lock(mutex_);
        accepting_ = false;
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
        boost::asio::post(*accept_strand_, [this, server_end, id] {
            // endWork() runs when no handler runs, so on_peer_ cannot change under this.
            if(on_peer_) {
                on_peer_(server_end, id);
            }
        });
        return client_end;
    }

    /** @brief @return How many connections have been made */
    [[nodiscard]] std::size_t nConnections() const {
        return next_id_.load() - 1;
    }
    /** @brief @return Whether endWork() has been called */
    [[nodiscard]] bool workEnded() const {
        return work_ended_.load();
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
    bool accepting_ = false;                             ///< Between start() and stop()
    std::atomic<bool> work_ended_{false};                ///< endWork() has been called
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
/**
 * The client side of the in-process transport: asks a GInProcessListener for a connection. The
 * listener must outlive every attempt (it is held weakly; a listener that is gone refuses).
 */
class GInProcessConnector final : public Gem::Courtier::Consumers::GConnectorI {
public:
    /** @brief @param listener The server @param reconnectable What reconnectable() answers */
    explicit GInProcessConnector(
        std::weak_ptr<GInProcessListener> listener,
        bool reconnectable = true
    )
      : listener_(std::move(listener))
      , reconnectable_(reconnectable) { /* nothing */
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
        auto cancelled = std::make_shared<bool>(false);
        cancelled_ = cancelled;
        boost::asio::post(
            executor,
            [listener = listener_, executor, cancelled, done = std::move(done)]() mutable {
                if(*cancelled) {
                    return;
                }
                auto server = listener.lock();
                auto peer = server ? server->connect_(executor) : nullptr;
                done(peer, peer ? std::string_view{} : std::string_view{"the server refuses"});
            }
        );
    }

    /** @brief The attempt in progress does not call done */
    void cancel() override {
        if(cancelled_) {
            *cancelled_ = true;
        }
    }

    /** @brief @return What the constructor was told */
    [[nodiscard]] bool reconnectable() const override {
        return reconnectable_;
    }

private:
    std::weak_ptr<GInProcessListener> listener_; ///< The server
    bool reconnectable_;                         ///< What reconnectable() answers
    std::shared_ptr<bool> cancelled_;            ///< The flag of the last attempt (on the executor)
};

/******************************************************************************/

} /* namespace Gem::Courtier::Tests */
