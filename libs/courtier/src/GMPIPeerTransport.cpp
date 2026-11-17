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

#include "courtier/transport/GMPIPeerTransport.hpp"

// Standard headers go here
#include <algorithm>
#include <exception>
#include <tuple>
#include <vector>

// Boost headers go here
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "courtier/GCourtierHelperFunctions.hpp"

namespace Gem::Courtier::Consumers {

namespace {

/// How long the listener's stop() waits for the calls of on_peer under way. Short and independent
/// of the stop grace, so that a stop takes one grace (the shell's wait for its clients), not two.
constexpr std::chrono::milliseconds LISTENER_STOP_BOUND{1000};

/// What the listener's stop grace adds to the peers' close grace: a peer whose close is not
/// answered ends by itself (CLOSED_HERE_UNCONFIRMED) before the shell stops waiting for it.
constexpr std::chrono::milliseconds STOP_GRACE_MARGIN{1000};

/** @brief @param how What a close says @return It, as the payload of a close */
std::string encodeClose(GPeerClose how) {
    std::string payload(1, static_cast<char>(how)); // not braced: that would be two characters
    return payload;
}

/** @brief @param payload The payload of a close or of its answer @return What it says; NORMAL if
 *  it says nothing this end knows */
GPeerClose decodeClose(const std::string &payload) {
    if(payload.empty()) {
        return GPeerClose::NORMAL;
    }
    switch(static_cast<GPeerClose>(payload.front())) {
    case GPeerClose::NORMAL:
        return GPeerClose::NORMAL;
    case GPeerClose::ERROR:
        return GPeerClose::ERROR;
    case GPeerClose::RUN_ENDED:
        return GPeerClose::RUN_ENDED;
    }
    return GPeerClose::NORMAL;
}

} /* anonymous namespace */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Prepares the end; nothing happens before start().
 *
 * @param core The poll of this process, which holds this end as the receiver of @p rank
 * @param executor The executor this end makes its strand on
 * @param rank The other end's rank
 * @param opening When this end opens (see the class comment)
 * @param options The close grace, the longest message and the name for the log
 */
GMPIPeer::GMPIPeer(
    std::weak_ptr<GMPIPollCore> core,
    const boost::asio::any_io_executor &executor,
    int rank,
    GOpening opening,
    const GMPIPeerOptions &options
)
  : core_(std::move(core))
  , strand_(boost::asio::make_strand(executor))
  , executor_(strand_)
  , close_timer_(strand_)
  , rank_(rank)
  , opening_(opening)
  , close_grace_(options.closeGrace)
  , max_message_bytes_(std::min(options.maxMessageBytes, static_cast<std::size_t>(INT_MAX)))
  , name_(options.name) { /* nothing */
}

/******************************************************************************/
/**
 * Installs the handlers and opens, on the strand (rule 2): the submitter's end at once, a client's
 * end when the greeting arrives. From any thread; once, before anything else.
 *
 * @param handlers What the role wants to be told
 */
void GMPIPeer::start(GPeerHandlers handlers) {
    boost::asio::post(
        strand_,
        [self = this->shared_from_this(), handlers = std::move(handlers)]() mutable {
            self->started_(std::move(handlers));
        }
    );
}

/******************************************************************************/
/**
 * Sends one message of the role, after those sent before; dropped before the open and after
 * close(). A message longer than the limit is not sent: the end fails, on the strand (rule 3).
 * On the strand.
 *
 * @param message The complete message
 */
void GMPIPeer::send(GMessage message) {
    if(state_ != GState::OPEN || not role_open_ || close_requested_.load() || not message) {
        return;
    }
    if(message->size() > max_message_bytes_) {
        boost::asio::post(strand_, [self = this->shared_from_this(), size = message->size()] {
            self->failSend_(size);
        });
        return;
    }
    if(auto core = core_.lock()) {
        core->startSend(rank_, TAG_PEER_MESSAGE, std::string(*message));
    }
}

/******************************************************************************/
/**
 * Closes after what was handed to send() before (rule 6). From any thread; the first call decides
 * what the other end is told.
 *
 * @param how What the other end is told
 */
void GMPIPeer::close(GPeerClose how) {
    if(close_requested_.exchange(true)) {
        return;
    }
    boost::asio::post(strand_, [self = this->shared_from_this(), how] { self->beginClose_(how); });
}

/******************************************************************************/
/**
 * Ends at once (GPeerI::abort()), for teardown: when no handler of this end runs. The other end is
 * told with a close if it has not been told (it answers nothing that is waited for). Reports
 * CLOSED_HERE, or CLOSED_HERE_UNCONFIRMED if close() had been called before; to a start() whose
 * work has not run yet, when it runs.
 */
void GMPIPeer::abort() {
    if(state_ == GState::ENDED) {
        return;
    }
    const GPeerEnd end =
        close_requested_.exchange(true) ? GPeerEnd::CLOSED_HERE_UNCONFIRMED : GPeerEnd::CLOSED_HERE;
    this->tellOther_(GPeerClose::NORMAL);
    this->finish_(end, GPeerClose::NORMAL, "aborted");
}

/******************************************************************************/
/**
 * The work of the transport ends (the listener's endWork(), the connector's destructor): an end that
 * has not ended tells the other rank, if that rank does not know yet that the connection is over,
 * so that it does not wait for ever (it answers into the void). The poll then lets go of the end
 * (discard()). When no handler of the end runs any more, before the poll's shutdown().
 *
 * @param word What the other rank is told: the end of the run on the submitter, NORMAL on a client
 */
void GMPIPeer::endOfWork(GPeerClose word) {
    if(state_ != GState::ENDED) {
        this->tellOther_(word);
    }
}

/******************************************************************************/
/** @return The longest message this end sends */
std::size_t GMPIPeer::maxMessageBytes() const {
    return max_message_bytes_;
}

/******************************************************************************/
/** @return The strand of this end */
boost::asio::any_io_executor GMPIPeer::executor() {
    return executor_;
}

/******************************************************************************/
/** @param busy Whether the owner is busy (a client: every evaluation slot taken): the poll is
 *  told of a change, while this end has not ended (GMPIPollCore::receiverBusy()) */
void GMPIPeer::setOwnerBusy(bool busy) {
    owner_busy_.store(busy);
    if(busy == busy_told_ || state_ == GState::ENDED) {
        return;
    }
    busy_told_ = busy;
    if(auto core = core_.lock()) {
        core->receiverBusy(busy);
    }
}

/******************************************************************************/
/**
 * One message from the other rank, on the strand, in the order the other rank sent them. What
 * cannot be handled yet (the end has not been started, or its role asked for the close before it
 * was told of the open) waits.
 *
 * @param tag What it is
 * @param bytes Its bytes
 */
void GMPIPeer::received(int tag, std::string &&bytes) {
    if(state_ == GState::ENDED) {
        return;
    }
    if(waiting_.empty() && this->mayHandle_()) {
        this->handle_(tag, std::move(bytes));
        return;
    }
    waiting_.push_back(GArrival{.tag = tag, .bytes = std::move(bytes)});
    this->handleWaiting_();
}

/******************************************************************************/
/** The poll lets go of this end (its work has ended, GPeerI.hpp rule 7): the handlers, and with
 *  them the role, are dropped without onClosed. When no handler runs any more. */
void GMPIPeer::discard() {
    state_ = GState::ENDED;
    close_requested_.store(true);
    ended_.store(true);
    waiting_.clear();
    handlers_ = GPeerHandlers{};
}

/******************************************************************************/
/** @param reason Why the poll has failed: this end ends as FAILED, on the strand */
void GMPIPeer::pollFailed(const std::string &reason) {
    // The sends still work: the other rank is told, or it would wait for ever
    this->tellOther_(GPeerClose::ERROR);
    this->finish_(GPeerEnd::FAILED, GPeerClose::NORMAL, "the MPI poll has failed: " + reason);
}

/******************************************************************************/
/** @return The other end's rank */
int GMPIPeer::rank() const noexcept {
    return rank_;
}

/** @return What the owner said last through setOwnerBusy() */
bool GMPIPeer::ownerBusy() const noexcept {
    return owner_busy_.load();
}

/** @return Whether the end has ended */
bool GMPIPeer::ended() const noexcept {
    return ended_.load();
}

/******************************************************************************/
/**
 * start()'s work, on the strand: installs the handlers, opens (or waits for the greeting) and
 * handles what has arrived. An end that has ended before (abort(), a failed poll) does not open;
 * the role hears that end now.
 *
 * @param handlers What the role wants to be told
 */
void GMPIPeer::started_(GPeerHandlers handlers) {
    handlers_ = std::move(handlers);
    if(state_ == GState::ENDED) {
        GPeerHandlers ended = std::move(handlers_);
        handlers_ = GPeerHandlers{};
        if(ended.onClosed && unreported_) {
            ended.onClosed(unreported_->end, unreported_->how, unreported_->detail);
        }
        unreported_.reset();
        return;
    }
    if(opening_ == GOpening::AT_START) {
        this->open_();
    }
    else {
        state_ = GState::WAITING;
    }
    this->handleWaiting_();
}

/******************************************************************************/
/** The end is open; the role is told, unless it has asked for the close already (rule 6). */
void GMPIPeer::open_() {
    state_ = GState::OPEN;
    if(not close_requested_.load()) {
        role_open_ = true;
        if(handlers_.onOpen) {
            handlers_.onOpen();
        }
    }
}

/******************************************************************************/
/**
 * @return Whether what arrives can be handled now: not before the start; not for a role that asked
 * for the close before it was told of the open, until its own close has been sent (rule 6)
 */
bool GMPIPeer::mayHandle_() const {
    switch(state_) {
    case GState::NEW:
        return false;
    case GState::WAITING:
        return not close_requested_.load();
    case GState::OPEN:
        return role_open_;
    case GState::CLOSING:
    case GState::ENDED:
        return true;
    }
    return false;
}

/******************************************************************************/
/** Handles what waits, in order, for as long as it can be handled. On the strand. */
void GMPIPeer::handleWaiting_() {
    while(not waiting_.empty() && state_ != GState::ENDED && this->mayHandle_()) {
        GArrival arrival = std::move(waiting_.front());
        waiting_.pop_front();
        this->handle_(arrival.tag, std::move(arrival.bytes));
    }
    if(state_ == GState::ENDED) {
        waiting_.clear();
    }
}

/******************************************************************************/
/**
 * One message from the other rank, handled (see the file comment). On the strand.
 *
 * @param tag What it is
 * @param bytes Its bytes
 */
void GMPIPeer::handle_(int tag, std::string &&bytes) {
    switch(tag) {
    case TAG_PEER_OPEN:
        if(state_ == GState::WAITING) {
            this->open_();
        }
        return; // otherwise late (this end closes already) or meant for an end that is open

    case TAG_PEER_MESSAGE:
        // Delivered until this end's close has been sent (OPEN), also after close() (rule 6)
        if(state_ == GState::OPEN && role_open_ && handlers_.onMessage) {
            handlers_.onMessage(std::move(bytes));
        }
        else if(state_ == GState::WAITING) {
            glogger << name_ << ": a message from rank " << rank_
                    << " that came before the greeting was dropped" << '\n'
                    << GWARNING;
        }
        return;

    case TAG_PEER_CLOSE: {
        const GPeerClose how = decodeClose(bytes);
        other_told_ = true; // it has closed: it knows
        if(state_ == GState::CLOSING) {
            // The two closes crossed: the other end's own close is no answer to this end's (what
            // this end sent may have arrived there after its close had gone out)
            this->finish_(
                GPeerEnd::CLOSED_HERE_UNCONFIRMED,
                how,
                "the other end's own close crossed this end's"
            );
            return;
        }
        // Everything that arrived before the close has been handed to the role: the answer
        // confirms it (it echoes what the close said)
        this->sendControl_(TAG_PEER_CLOSE_ACK, std::move(bytes));
        this->finish_(GPeerEnd::CLOSED_THERE, how, "closed by the other end");
        return;
    }

    case TAG_PEER_CLOSE_ACK:
        other_told_ = true;
        if(state_ == GState::CLOSING) {
            this->finish_(GPeerEnd::CLOSED_HERE, decodeClose(bytes), "closed");
        }
        return;

    default:
        glogger << name_ << ": a message with the unknown tag " << tag << " from rank " << rank_
                << " was dropped" << '\n'
                << GWARNING;
        return;
    }
}

/******************************************************************************/
/**
 * Begins the close on the strand: the close is sent (after everything sent before) and its answer
 * waited for, at most the close grace. What has waited for a role that asked for the close before
 * it was told of the open is handled now: a message is dropped, a close of the other end has
 * crossed this one.
 *
 * @param how What the other end is told
 */
void GMPIPeer::beginClose_(GPeerClose how) {
    if(state_ == GState::CLOSING || state_ == GState::ENDED) {
        return; // closing already, or ended
    }
    // An end that was never started (NEW: the listener closes the end of a rank whose session
    // could not be made) closes all the same: its rank must not wait for ever.
    state_ = GState::CLOSING;
    this->tellOther_(how);
    close_timer_.expires_after(close_grace_);
    close_timer_.async_wait([self = this->shared_from_this()](const boost::system::error_code &ec) {
        if(ec || self->state_ != GState::CLOSING) {
            return;
        }
        glogger << self->name_ << ": rank " << self->rank_ << " did not confirm the close within "
                << self->close_grace_.count() << " ms" << '\n'
                << GWARNING;
        self->finish_(
            GPeerEnd::CLOSED_HERE_UNCONFIRMED,
            GPeerClose::NORMAL,
            "the other end did not confirm the close in time"
        );
    });
    this->handleWaiting_();
}

/******************************************************************************/
/**
 * A message was too long to be sent: the end fails and tells the other end with a close. On the
 * strand.
 *
 * @param size The length of the message
 */
void GMPIPeer::failSend_(std::size_t size) {
    if(state_ == GState::ENDED) {
        return;
    }
    close_requested_.store(true);
    this->tellOther_(GPeerClose::ERROR);
    this->finish_(
        GPeerEnd::FAILED,
        GPeerClose::NORMAL,
        "a message of " + std::to_string(size) + " bytes is longer than MPI can send at once (" +
            std::to_string(max_message_bytes_) + ")"
    );
}

/******************************************************************************/
/**
 * Tells the other rank that this end is over (a close that says @p word), unless it knows already:
 * it was sent this end's close or an answer, or it closed itself. On the strand, or when no handler
 * of the end runs.
 *
 * @param word What the close says
 */
void GMPIPeer::tellOther_(GPeerClose word) {
    if(other_told_) {
        return;
    }
    other_told_ = true;
    this->sendControl_(TAG_PEER_CLOSE, encodeClose(word));
}

/******************************************************************************/
/**
 * Sends a message of the protocol of the ends through the poll.
 *
 * @param tag TAG_PEER_CLOSE or TAG_PEER_CLOSE_ACK
 * @param payload Its bytes
 */
void GMPIPeer::sendControl_(int tag, std::string payload) {
    if(auto core = core_.lock()) {
        core->startSend(rank_, tag, std::move(payload));
    }
}

/******************************************************************************/
/**
 * The end of the connection, whatever led to it; runs its body once. The poll is told (after this
 * end's last send), then the role, and the handlers -- with them this end's hold on the role -- are
 * dropped. An end whose start()'s work has not run keeps the report for it.
 *
 * @param end Why it ended
 * @param how What the other end's close said
 * @param detail Words about it
 */
void GMPIPeer::finish_(GPeerEnd end, GPeerClose how, std::string_view detail) {
    if(state_ == GState::ENDED) {
        return;
    }
    const bool started = state_ != GState::NEW;
    state_ = GState::ENDED;
    close_requested_.store(true);
    ended_.store(true);
    waiting_.clear();
    close_timer_.cancel();
    if(auto core = core_.lock()) {
        if(busy_told_) {
            busy_told_ = false;
            core->receiverBusy(false); // an end that has ended is nobody's busy end
        }
        core->receiverEnded(rank_);
    }
    if(not started) {
        unreported_ = GEndReport{.end = end, .how = how, .detail = std::string(detail)};
        return;
    }
    GPeerHandlers handlers = std::move(handlers_);
    handlers_ = GPeerHandlers{};
    if(handlers.onClosed) {
        handlers.onClosed(end, how, detail);
    }
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Prepares the listener; nothing happens before start().
 *
 * @param calls The MPI calls on courtier's communicator; must not be null
 * @param options How the peers and the poll are set up (the name is the owner's, for the log and
 *        the thread budget)
 * @throws geneva_exception if @p calls is null
 */
GMPIListener::GMPIListener(std::shared_ptr<GMPICallsI> calls, GMPIPeerOptions options)
  : calls_(std::move(calls))
  , options_(std::move(options)) {
    if(not calls_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPIListener::GMPIListener(): Error!" << '\n'
            << "No calls to MPI were given" << '\n'
        );
    }
}

/******************************************************************************/
/** Ends the poll if endWork() has not done so. */
GMPIListener::~GMPIListener() {
    try {
        stopped_.store(true);
        this->joinPoll_();
        this->tellEnds_();
        const std::scoped_lock lock(core_mutex_);
        if(core_) {
            std::ignore = core_->shutdown();
        }
    }
    catch(...) { // NOLINT(bugprone-empty-catch) -- deliberate best-effort teardown
        // never let an exception escape a destructor
    }
}

/******************************************************************************/
/**
 * Makes the poll and a peer for every other rank, starts the poll's thread, greets every client
 * rank (TAG_PEER_OPEN, the first message it gets from here) and hands every peer to the shell:
 * on_peer is posted, once per rank, to the poll's thread, with the rank as the client's id.
 * A start() that throws leaves nothing running and keeps nothing of @p executor and @p on_peer: one
 * that fails after the poll was set up ends the work as endWork() does, so every client rank is
 * told that the run has ended (its client would wait for ever otherwise), and the shell, which
 * then counts the listener as not started, has nothing to end.
 *
 * A communicator of this rank alone is refused: there is no client rank, and an MPI job keeps the
 * ranks it was started with, so the work would wait for ever.
 *
 * @param executor Where the peers' handlers run (the shell's session pool)
 * @param on_peer Called for every client rank, on the poll's thread
 * @throws geneva_exception if the listener was started before, if the communicator has no rank
 *         besides this one, or if the poll or its thread cannot be made
 */
void GMPIListener::start(boost::asio::any_io_executor executor, on_peer_type on_peer) {
    {
        const std::scoped_lock lock(core_mutex_);
        if(core_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In " << options_.name << ": Error!" << '\n'
                << "The listener has been started before" << '\n'
            );
        }
    }
    if(calls_->size() < 2) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In " << options_.name << ": Error!" << '\n'
            << "The MPI consumer has no client rank: its communicator holds this rank only, "
               "which submits the work. The work is evaluated on the other ranks of the job, and "
               "an MPI job keeps the ranks it was started with, so nothing would ever be "
               "evaluated. Start the program under the MPI launcher with at least two "
               "processes, e.g. \"mpirun -np 4 <program> --consumer mpi\" (one submitter and "
               "three clients)."
            << '\n'
        );
    }

    auto core = std::make_shared<GMPIPollCore>(calls_, poll_context_.get_executor(), options_.poll);
    std::vector<std::shared_ptr<GMPIPeer>> peers = this->makePeers_(core, executor);
    this->startPoll_(core);
    {
        const std::scoped_lock lock(core_mutex_);
        core_ = core;
    }
    // (Only a failing allocation gets into the catch outside the tests.)
    try {
        on_peer_ = std::move(on_peer);
        n_peers_.store(peers.size());
        peers_.assign(peers.begin(), peers.end());

        // The greeting goes out before anything a peer sends: a client's end opens on it.
        for(const auto &peer : peers) {
            core->startSend(peer->rank(), TAG_PEER_OPEN, std::string{});
        }
        core->start();
        this->handOver_(std::move(peers));
    }
    catch(...) {
        // The shell starts no thread of its own after a throw, and destroys its context before
        // this listener: the clients are told, and the poll lets go of that context, now.
        this->endWork();
        throw;
    }
}

/******************************************************************************/
/**
 * Makes a peer for every rank but this one, each the receiver of its rank's messages.
 *
 * @param core The poll
 * @param executor Where the peers' handlers run
 * @return The peers, by rank
 */
std::vector<std::shared_ptr<GMPIPeer>> GMPIListener::makePeers_(
    const std::shared_ptr<GMPIPollCore> &core,
    const boost::asio::any_io_executor &executor
) const {
    const int own = calls_->rank();
    std::vector<std::shared_ptr<GMPIPeer>> peers;
    peers.reserve(static_cast<std::size_t>(std::max(calls_->size() - 1, 0)));
    for(int rank = 0; rank < calls_->size(); ++rank) {
        if(rank == own) {
            continue;
        }
        auto peer = std::make_shared<GMPIPeer>(
            core,
            executor,
            rank,
            GMPIPeer::GOpening::AT_START,
            options_
        );
        core->addReceiver(rank, peer);
        peers.push_back(std::move(peer));
    }
    return peers;
}

/******************************************************************************/
/**
 * Starts the poll's thread. If it cannot be made, nothing runs afterwards and the poll has let go
 * of the peers.
 *
 * @param core The poll, not started
 * @throws geneva_exception if the thread cannot be started
 */
void GMPIListener::startPoll_(const std::shared_ptr<GMPIPollCore> &core) {
    work_guard_.emplace(poll_context_.get_executor());
    try {
        // (No test for a reservation or a thread that cannot be made: only a failing allocation
        // gets there.)
        budget_ = Gem::Common::Concurrency::threadBudget().reserve(
            options_.name + ":poll",
            1,
            Gem::Common::Concurrency::ThreadElasticity::Elastic
        );
        thread_.create_thread([this] { poll_context_.run(); });
    }
    catch(...) {
        const std::string why = Gem::Courtier::describeException(std::current_exception());
        work_guard_.reset();
        poll_context_.stop();
        thread_.join_all();
        poll_context_.restart();
        budget_.release();
        std::ignore = core->shutdown(); // lets go of the peers; nothing has been sent
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In " << options_.name << ": could not start the poll's thread: " << why << '\n'
        );
    }
    running_.store(true);
}

/******************************************************************************/
/**
 * Hands every peer to the shell: on_peer is posted, once per rank, to the poll's thread, not to the
 * shell's executor (GListenerI::start()): every session thread may be busy for any time, and the
 * calls are short. Every call is counted before it is posted, so that stop() sees every call that
 * will come; on_peer_ is dropped only by endWork(), when the poll's thread no longer runs this.
 *
 * @param peers The peers, not started
 */
void GMPIListener::handOver_(std::vector<std::shared_ptr<GMPIPeer>> peers) {
    for(auto &peer : peers) {
        const auto peer_id = static_cast<GWirePeerId>(peer->rank());
        on_peer_calls_.set(peer_id, true);
        boost::asio::post(poll_context_, [this, peer = std::move(peer), peer_id]() mutable {
            // No exception leaves the poll's thread: a rank whose session cannot be made is closed
            // with an error (its client would wait for ever otherwise), and the call is counted
            // out.
            try {
                if(on_peer_) {
                    on_peer_(peer, peer_id);
                }
            }
            catch(...) {
                glogger << "In " << options_.name << ": the session of rank " << peer_id
                        << " could not be made: "
                        << Gem::Courtier::describeException(std::current_exception()) << '\n'
                        << GWARNING;
                peer->close(GPeerClose::ERROR);
            }
            on_peer_calls_.remove(peer_id);
        });
    }
}

/******************************************************************************/
/**
 * No new peers (there are none after start()), and none on its way (GListenerI::stop()): waits
 * for the calls of on_peer posted by start(), at most LISTENER_STOP_BOUND. Not on the poll's
 * thread (the calls waited for run there). Idempotent.
 */
void GMPIListener::stop() {
    if(not running_.load() || stopped_.exchange(true)) {
        return;
    }
    if(not on_peer_calls_.waitUntilEmpty(LISTENER_STOP_BOUND)) {
        glogger << "In " << options_.name << ": " << on_peer_calls_.size()
                << " rank(s) did not reach the server within " << LISTENER_STOP_BOUND.count()
                << " ms of the stop" << '\n'
                << GWARNING;
    }
}

/******************************************************************************/
/**
 * Ends the transport's own work (GPeerI.hpp, rule 7): the poll's thread is stopped and joined;
 * every end that has not ended tells its client rank that the run has ended, unless that rank knows
 * (a client is never left waiting for a submitter that has gone); the sends under way get the drain
 * grace to complete; then the poll ends (what has not completed is given up, the peers are let go
 * without onClosed) and its context is shut down. Afterwards nothing here refers to the shell's
 * executor.
 */
void GMPIListener::endWork() {
    stopped_.store(true);
    this->joinPoll_();
    this->tellEnds_();
    std::shared_ptr<GMPIPollCore> core;
    {
        const std::scoped_lock lock(core_mutex_);
        core = core_;
    }
    if(core) {
        // The closes just sent, and what else is under way, get the drain grace to arrive
        std::ignore = core->drainSends(options_.poll.drainGrace);
        std::ignore = core->shutdown();
    }
    poll_context_.shutdown();
    on_peer_ = nullptr;
}

/******************************************************************************/
/** @return How long the shell waits for its clients at the end of a run: the peers' close grace
 *  and a margin */
std::chrono::milliseconds GMPIListener::stopGrace() const {
    return options_.closeGrace + STOP_GRACE_MARGIN;
}

/******************************************************************************/
/** @return false: the loss of a rank is not noticed (see the class comment) */
bool GMPIListener::detectsLoss() const {
    return false;
}

/******************************************************************************/
/**
 * For a submitter whose consumer was never started (GListenerI::announceEndWithoutStart()): makes an
 * end for every client rank on the poll's own context, closes each with GPeerClose::RUN_ENDED -- no
 * greeting goes out, and a client's end that waits for one answers the close and ends -- and waits
 * for the ends, at most stopGrace(). Then the poll's thread is joined and the poll ended. A rank that
 * does not answer (it has ended, or never receives) is logged. Not after start().
 */
void GMPIListener::announceEndWithoutStart() {
    {
        const std::scoped_lock lock(core_mutex_);
        if(core_) {
            return; // started: the shell ends the run
        }
    }
    auto core = std::make_shared<GMPIPollCore>(calls_, poll_context_.get_executor(), options_.poll);
    auto peers = this->makePeers_(core, poll_context_.get_executor());
    if(peers.empty()) {
        std::ignore = core->shutdown();
        return;
    }
    this->startPoll_(core);
    core->start();

    auto ended = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(peers.size());
    for(const auto &peer : peers) {
        peer->start(GPeerHandlers{.onClosed = [ended](GPeerEnd, GPeerClose, std::string_view) {
            ended->count_down();
        }});
        peer->close(GPeerClose::RUN_ENDED);
    }
    if(not ended->wait_for(this->stopGrace())) {
        glogger << "In " << options_.name << ": " << ended->remaining() << " of " << peers.size()
                << " client rank(s) did not answer the end of a run that never started within "
                << this->stopGrace().count() << " ms" << '\n'
                << GWARNING;
    }
    this->joinPoll_();
    std::ignore = core->shutdown();
    poll_context_.shutdown();
}

/******************************************************************************/
/**
 * @brief The number of client ranks (the peers made by start()). Any thread.
 *
 * @return The number of client ranks; 0 before start()
 */
std::size_t GMPIListener::nPeers() const noexcept {
    return n_peers_.load();
}

/******************************************************************************/
/** @return What the poll has done; empty before start() */
std::optional<GMPIPollCounts> GMPIListener::pollCounts() const {
    const std::scoped_lock lock(core_mutex_);
    if(not core_) {
        return std::nullopt;
    }
    return core_->counts();
}

/******************************************************************************/
/** Every end that has not ended tells its client rank that the run has ended, if that rank does not
 *  know yet (GMPIPeer::endOfWork()). When no handler runs, before the poll's shutdown(). */
void GMPIListener::tellEnds_() {
    for(const auto &weak_peer : peers_) {
        if(auto peer = weak_peer.lock()) {
            peer->endOfWork(GPeerClose::RUN_ENDED);
        }
    }
    peers_.clear();
}

/******************************************************************************/
/** Stops the poll's context, joins its thread and gives back its reservation. */
void GMPIListener::joinPoll_() {
    if(not running_.exchange(false)) {
        return;
    }
    work_guard_.reset();
    poll_context_.stop();
    thread_.join_all();
    budget_.release();
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Prepares the connector; nothing happens before connect().
 *
 * @param calls The MPI calls on courtier's communicator, of a client's rank; must not be null
 * @param options How the peer and the poll are set up
 * @throws geneva_exception if @p calls is null or belongs to the submitter's rank
 */
GMPIConnector::GMPIConnector(std::shared_ptr<GMPICallsI> calls, GMPIPeerOptions options)
  : calls_(std::move(calls))
  , options_(std::move(options)) {
    if(not calls_ || calls_->rank() == RANK_MASTER_NODE) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPIConnector::GMPIConnector(): Error!" << '\n'
            << (calls_ ? "The submitter's rank has no connector" : "No calls to MPI were given")
            << '\n'
        );
    }
}

/******************************************************************************/
/** Gives up an attempt in progress and ends the poll: after the client's io context has stopped
 *  running, before it is destroyed (GConnectorI). */
GMPIConnector::~GMPIConnector() {
    cancelled_->store(true);
    try {
        if(auto peer = peer_.lock()) {
            peer->endOfWork(GPeerClose::NORMAL); // the submitter's end must not wait for it
        }
        const std::scoped_lock lock(core_mutex_);
        if(core_) {
            std::ignore = core_->shutdown();
        }
    }
    catch(...) { // NOLINT(bugprone-empty-catch) -- deliberate best-effort teardown
        // never let an exception escape a destructor
    }
}

/******************************************************************************/
/**
 * Makes the poll and the peer to the submitter on @p executor and reports the peer on it; the poll
 * starts after done has started the peer. The first call only: an MPI client has one connection for
 * the whole run, and a later call reports that there is none.
 *
 * @param executor Where the poll and the peer run (the client's io thread)
 * @param done What is told the outcome
 */
void GMPIConnector::connect(boost::asio::any_io_executor executor, done_type done) {
    std::shared_ptr<GMPIPollCore> core;
    {
        const std::scoped_lock lock(core_mutex_);
        if(not core_) {
            core_ = std::make_shared<GMPIPollCore>(calls_, executor, options_.poll);
            core = core_;
        }
    }
    if(not core) {
        boost::asio::post(executor, [cancelled = cancelled_, done = std::move(done)]() mutable {
            if(not cancelled->load()) {
                done(nullptr, "an MPI client has one connection for the whole run");
            }
        });
        return;
    }
    auto peer = std::make_shared<GMPIPeer>(
        core,
        executor,
        RANK_MASTER_NODE,
        GMPIPeer::GOpening::AT_GREETING,
        options_
    );
    core->addReceiver(RANK_MASTER_NODE, peer);
    peer_ = peer;
    boost::asio::post(
        executor,
        [core, peer = std::move(peer), cancelled = cancelled_, done = std::move(done)]() mutable {
            if(cancelled->load()) {
                return;
            }
            done(std::move(peer), std::string_view{}); // the role starts the peer in there
            core->start();
        }
    );
}

/******************************************************************************/
/** The attempt in progress does not call done. On the executor. */
void GMPIConnector::cancel() {
    cancelled_->store(true);
}

/******************************************************************************/
/** @return false: one connection for the whole run */
bool GMPIConnector::reconnectable() const {
    return false;
}

/******************************************************************************/
/** @return What the poll has done; empty before connect() */
std::optional<GMPIPollCounts> GMPIConnector::pollCounts() const {
    const std::scoped_lock lock(core_mutex_);
    if(not core_) {
        return std::nullopt;
    }
    return core_->counts();
}

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
