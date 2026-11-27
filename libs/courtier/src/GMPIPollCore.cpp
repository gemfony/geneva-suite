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

#include "courtier/transport/GMPIPollCore.hpp"

// Standard headers go here
#include <algorithm>
#include <exception>
#include <thread>
#include <tuple>
#include <utility>

// Boost headers go here
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * Prepares the poll; nothing runs before start().
 *
 * @param calls The calls to MPI on courtier's communicator; must not be null
 * @param executor Where the chain runs (on a strand of its own)
 * @param timing How the poll paces itself
 * @throws gem_exception if @p calls is null or @p timing is not usable
 */
GMPIPollCore::GMPIPollCore(
    std::shared_ptr<GMPICallsI> calls,
    const boost::asio::any_io_executor &executor,
    GMPIPollTiming timing
)
  : calls_(std::move(calls))
  , timing_(timing)
  , size_(calls_ ? calls_->size() : 0)
  , strand_(boost::asio::make_strand(executor))
  , timer_(strand_)
  , tick_timer_(strand_)
  , receivers_(static_cast<std::size_t>(std::max(size_, 0)))
  , receiver_executors_(receivers_.size())
  , pause_(timing.pauseMin)
  , ended_ranks_(receivers_.size(), false)
  , keep_alive_(timing.keepAlive)
  , life_(receivers_.size()) {
    if(not calls_) {
        throw gem_exception(g_error_streamer() << "No calls to MPI were given" << '\n');
    }
    if(timing_.pauseMin.count() <= 0 || timing_.pauseMax < timing_.pauseMin || timing_.rounds < 1 ||
       timing_.drainGrace.count() < 0 || timing_.keepAlive.count() < 0) {
        throw gem_exception(
            g_error_streamer()
            << "The timing is not usable: the shortest pause must be positive and not above the "
            << "longest, at least one round, and a drain grace and a keep-alive interval that are "
            << "not negative" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * The destructor. If the owner has not called shutdown(), it is called here, so that the sends
 * under way are given up rather than lose their bytes while MPI may still read them. No exception
 * leaves it.
 */
GMPIPollCore::~GMPIPollCore() {
    try {
        std::ignore = this->shutdown();
    }
    catch(...) { // NOLINT(bugprone-empty-catch) -- deliberate best-effort teardown
        // never let an exception escape a destructor
    }
}

/******************************************************************************/
/**
 * Adds the receiver of one rank's messages. Before start(), from one thread; one receiver per
 * rank. The receiver's executor is asked here, once.
 *
 * @param rank The rank whose messages the receiver gets
 * @param receiver The receiver; the poll holds it until shutdown()
 * @throws gem_exception after start(), for a rank outside the communicator, a rank that has a
 *         receiver already, or a null receiver
 */
void GMPIPollCore::addReceiver(int rank, std::shared_ptr<GMPIPollReceiverI> receiver) {
    const char *refusal = nullptr;
    if(started_chain_.load()) {
        refusal = "the poll has been started";
    }
    else if(not this->validRank_(rank)) {
        refusal = "the rank is not one of the communicator";
    }
    else if(not receiver) {
        refusal = "the receiver is null";
    }
    else if(receivers_[static_cast<std::size_t>(rank)]) {
        refusal = "the rank has a receiver already";
    }
    if(refusal != nullptr) {
        throw gem_exception(
            g_error_streamer() << "A receiver for rank " << rank << " cannot be added: " << refusal
                               << '\n'
        );
    }
    const auto index = static_cast<std::size_t>(rank);
    receiver_executors_[index] = receiver->executor();
    receivers_[index] = std::move(receiver);
    ++live_receivers_;
}

/******************************************************************************/
/**
 * Starts the chain: its first link is posted to the strand, and the ticks of the signs of life
 * start with it if there is an interval. Once, after the receivers have been added. From any
 * thread. A second call does nothing.
 */
void GMPIPollCore::start() {
    if(started_chain_.exchange(true)) {
        return;
    }
    boost::asio::post(strand_, [self = this->shared_from_this()] {
        self->keepAlive_(self->keep_alive_);
        self->poll_();
    });
}

/******************************************************************************/
/**
 * Starts the ticks of the signs of life with @p interval, or changes it (see the class comment);
 * 0 stops them. Posted to the strand; a poll that has not been started keeps the interval for
 * start(). From any thread.
 *
 * @param interval The keep-alive interval
 */
void GMPIPollCore::setKeepAlive(std::chrono::milliseconds interval) {
    if(shut_down_.load()) {
        return;
    }
    boost::asio::post(strand_, [self = this->shared_from_this(), interval] {
        if(self->started_chain_.load()) {
            self->keepAlive_(interval);
        }
        else {
            self->keep_alive_ = interval; // start() starts the ticks
        }
    });
}

/******************************************************************************/
/**
 * Starts the send of one message and hands it to the poll, which frees it once it has completed.
 * The message is moved to the heap, where its bytes stay until then (a short string keeps its
 * bytes inside the object, which must not move while the send is under way). If no kick is on its
 * way to the strand, one is posted: it resets the pause and ends a pause that is under way, so
 * that the answer to this send is looked for at once. One kick serves every send started before it
 * runs.
 *
 * From any thread (a peer calls it on its strand), also while shutdown() runs; the sends of one
 * caller go out in the order of the calls. A send that finds shutdown() begun is dropped; one that
 * has begun before is waited for by shutdown(), which then gives it up if it has not completed.
 *
 * @param rank The receiving rank
 * @param tag The kind of message
 * @param message The bytes; at most INT_MAX of them (the caller has checked)
 * @throws gem_exception for a rank outside the communicator
 */
void GMPIPollCore::startSend(int rank, int tag, std::string message) {
    if(not this->validRank_(rank)) {
        throw gem_exception(
            g_error_streamer() << "Rank " << rank << " is not one of the " << size_
                               << " ranks of the communicator" << '\n'
        );
    }
    // Counted in before the end is looked at, and out on every way out of this call. shutdown()
    // sets its flag before it looks at the count (both sequentially consistent): either this call
    // sees the flag, or shutdown() sees the call and waits for it.
    sends_in_progress_.fetch_add(1);
    struct GSendScope {
        explicit GSendScope(std::atomic<int> &count)
          : n(count) { /* nothing */
        }
        std::atomic<int> &n;
        GSendScope(const GSendScope &) = delete;
        GSendScope &operator=(const GSendScope &) = delete;
        ~GSendScope() {
            if(n.fetch_sub(1) == 1) {
                n.notify_all();
            }
        }
    } const scope(sends_in_progress_);

    if(shut_down_.load()) {
        glogger << "GMPIPollCore: a send to rank " << rank
                << " after the end of the poll was dropped" << '\n'
                << GWARNING;
        return;
    }

    auto bytes = std::make_unique<std::string>(std::move(message));
    const GMPIRequest request = calls_->startSend(rank, tag, *bytes);
    // An unbounded queue: the push does not fail.
    std::ignore = started_.push(GStartedSend{.request = request, .bytes = std::move(bytes)});

    if(not kick_pending_.exchange(true)) {
        boost::asio::post(strand_, [self = this->shared_from_this()] { self->kick_(); });
    }
}

/******************************************************************************/
/**
 * A receiver tells that it has ended. Once per receiver, after its last startSend(): the chain
 * counts it on its strand, after it has taken over that send. From any thread. Does nothing for a
 * rank without a receiver, a second time, or after shutdown().
 *
 * @param rank The receiver's rank
 */
void GMPIPollCore::receiverEnded(int rank) {
    if(shut_down_.load() || not this->validRank_(rank)) {
        return;
    }
    boost::asio::post(strand_, [self = this->shared_from_this(), rank] { self->ended_(rank); });
}

/******************************************************************************/
/**
 * Waits, at most @p limit, for the sends under way to complete: takes them over and frees those that
 * have completed, with pauses between the looks that grow as the chain's do. An owner calls it
 * when no handler of the poll's executor or of the receivers' executors runs any more -- the chain
 * no longer runs then -- and before shutdown(), so that the last messages of the ends (a close that
 * tells the other rank of the end) are not given up while their ranks still receive. A failed chain
 * is no hindrance: only the probe or receive failed. A look that fails ends the wait.
 *
 * @param limit The longest wait
 * @return The number of sends still under way
 */
std::size_t GMPIPollCore::drainSends(std::chrono::milliseconds limit) {
    if(shut_down_.load()) {
        return 0;
    }
    const auto give_up_at = std::chrono::steady_clock::now() + limit;
    auto pause = timing_.pauseMin;
    try {
        while(true) {
            this->takeStarted_();
            this->reap_();
            if(requests_.empty() || std::chrono::steady_clock::now() >= give_up_at) {
                break;
            }
            std::this_thread::sleep_for(pause);
            pause = std::min(2 * pause, timing_.pauseMax);
        }
    }
    catch(...) {
        glogger << "GMPIPollCore (rank " << calls_->rank()
                << "): a look at the sends under way failed: "
                << Gem::Common::describeException(std::current_exception()) << '\n'
                << GWARNING;
    }
    return requests_.size();
}

/******************************************************************************/
/**
 * Ends the poll's work: no further link runs, the sends that have not completed by now are given
 * up (also after a failure of the chain), and every receiver is let go of (GMPIPollReceiverI::discard(), then dropped). A send that has
 * not completed goes to a rank that does not receive (it hangs, or it left without reading) and
 * would never complete; the calls take its bytes, because MPI may still read them.
 *
 * To be called when no handler of the poll's executor or of the receivers' executors runs any more
 * -- their threads are joined, or run() has returned -- and before those contexts are destroyed.
 * Idempotent. startSend() may still be called from other threads meanwhile: a call that has begun
 * is waited for and its send given up, a later one is dropped.
 *
 * @return The number of sends given up by this call
 */
std::size_t GMPIPollCore::shutdown() {
    if(shut_down_.exchange(true)) {
        return 0;
    }
    // A startSend() that began before the flag was set hands its send over before it returns; wait
    // for those calls (they are short), so that the look below takes their sends over.
    for(int n = sends_in_progress_.load(); n != 0; n = sends_in_progress_.load()) {
        sends_in_progress_.wait(n);
    }
    chain_ended_.store(true);
    timer_.cancel();
    tick_timer_.cancel();

    // One more look at the sends. A look that fails leaves the sends under way, to be given up.
    try {
        this->takeStarted_();
        this->reap_();
    }
    catch(...) {
        glogger << "GMPIPollCore (rank " << calls_->rank()
                << "): the last look at the sends failed: "
                << Gem::Common::describeException(std::current_exception()) << '\n'
                << GWARNING;
    }
    const std::size_t n_given_up = requests_.size();
    for(std::size_t i = 0; i < requests_.size(); ++i) {
        try {
            calls_->giveUp(requests_[i], std::move(buffers_[i]));
        }
        catch(...) {
            glogger << "GMPIPollCore (rank " << calls_->rank()
                    << "): a send could not be given up: "
                    << Gem::Common::describeException(std::current_exception()) << '\n'
                    << GWARNING;
        }
    }
    requests_.clear();
    buffers_.clear();

    for(auto &receiver : receivers_) {
        if(receiver) {
            receiver->discard();
        }
    }
    receivers_.clear();
    receiver_executors_.clear();

    // The chain no longer runs: what it has counted is read here
    glogger << "GMPIPollCore (rank " << calls_->rank() << "): " << n_received_
            << " messages received, " << n_sent_ << " sent, " << n_polls_ << " polls, "
            << std::chrono::duration_cast<std::chrono::milliseconds>(receiving_).count()
            << " ms in receives (longest "
            << std::chrono::duration_cast<std::chrono::microseconds>(longest_receive_).count()
            << " us)"
            << (n_given_up > 0 ? ", " + std::to_string(n_given_up) + " sends given up"
                               : std::string{})
            << (n_pings_ > 0 ? ", " + std::to_string(n_pings_) + " pings sent" : std::string{})
            << '\n'
            << GLOGGING;
    return n_given_up;
}

/******************************************************************************/
/**
 * One link of the chain, on its strand. A call that throws inside it ends the chain as failed
 * (fail_()); no exception leaves it, and so none leaves the executor's run().
 */
void GMPIPollCore::poll_() {
    if(chain_ended_.load()) {
        return;
    }
    try {
        this->link_();
    }
    catch(...) {
        this->fail_(std::current_exception());
    }
}

/******************************************************************************/
/**
 * The work of one link: receives while there is something (at most GMPIPollTiming::rounds
 * messages), then takes over and frees sends (once per link), decides whether the chain has ended, and arranges the
 * next link -- at once if the bound was reached, otherwise after the pause.
 */
void GMPIPollCore::link_() {
    ++n_polls_;

    bool traffic = false;
    bool more = false;
    for(int round = 0; round < timing_.rounds; ++round) {
        more = this->receiveOne_();
        if(not more) {
            break;
        }
        traffic = true;
    }
    // The sends are looked at once per link, not once per message received: a look tests every
    // send under way, and with hundreds of ranks there may be hundreds of them
    this->takeStarted_();
    this->reap_();

    // With every receiver ended nothing is left to wait for but the sends on their way. (A
    // receiver tells of its end after its last send, and through this strand, so every send of an
    // ended receiver has been taken over above.)
    if(live_receivers_ == 0) {
        const auto now = std::chrono::steady_clock::now();
        if(not drain_deadline_) {
            drain_deadline_ = now + timing_.drainGrace;
        }
        if(requests_.empty() || now >= *drain_deadline_) {
            chain_ended_.store(true); // what may be left, shutdown() gives up
            tick_timer_.cancel();     // a tick would keep the executor busy
            return;
        }
    }

    auto self = this->shared_from_this();
    if(more) {
        // The bound was reached and there may be more: let other handlers have their turn.
        pause_ = timing_.pauseMin;
        boost::asio::post(strand_, [self] { self->poll_(); });
        return;
    }
    if(traffic) {
        pause_ = timing_.pauseMin;
    }
    timer_.expires_after(pause_);
    pause_ = std::min(2 * pause_, timing_.pauseMax);
    // The link runs whether the timer expired or was cancelled by a kick.
    timer_.async_wait([self]([[maybe_unused]] const boost::system::error_code &ec) {
        self->poll_();
    });
}

/******************************************************************************/
/**
 * Receives one message if one is there, and posts it to the executor of its rank's receiver. A
 * message from a rank without a receiver is received too (it would block every later probe) and
 * dropped with a warning.
 *
 * @return Whether a message was received
 */
bool GMPIPollCore::receiveOne_() {
    const std::optional<GMPIProbed> probed = calls_->probe();
    if(not probed) {
        return false;
    }
    const auto receive_began = std::chrono::steady_clock::now();
    std::string message = calls_->receive(*probed);
    const auto received_at = std::chrono::steady_clock::now();
    const auto took =
        std::chrono::duration_cast<std::chrono::nanoseconds>(received_at - receive_began);
    receiving_ += took;
    longest_receive_ = std::max(longest_receive_, took);
    ++n_received_;

    // Every message is a sign of life of its rank; a ping is nothing else and goes no further
    if(this->validRank_(probed->source)) {
        GLife &life = life_[static_cast<std::size_t>(probed->source)];
        life.heard = received_at;
        life.everHeard = true;
    }
    if(probed->tag == TAG_PEER_PING) {
        return true;
    }
    if(this->validRank_(probed->source) && receivers_[static_cast<std::size_t>(probed->source)]) {
        const auto index = static_cast<std::size_t>(probed->source);
        boost::asio::post(
            receiver_executors_[index],
            [receiver = receivers_[index],
             tag = probed->tag,
             message = std::move(message)]() mutable {
                receiver->received(tag, std::move(message));
            }
        );
    }
    else {
        glogger << "GMPIPollCore: a message from rank " << probed->source << " with the tag "
                << probed->tag << ", which has no receiver in this process, was dropped" << '\n'
                << GWARNING;
    }
    return true;
}

/******************************************************************************/
/**
 * The chain fails: a call inside a link has thrown. The chain ends, the reason is logged, and every
 * receiver is told once, with the reason, on its executor, after the messages posted to it before.
 * The message the call concerned cannot be skipped -- it stays in MPI's queue -- so nothing more is
 * received. On the strand. Only a failing allocation makes a step here throw; as the function is
 * noexcept, that ends the program through the fatal report (common/GFatalReport.hpp).
 *
 * @param cause What was thrown
 */
void GMPIPollCore::fail_(const std::exception_ptr &cause) noexcept {
    chain_ended_.store(true);
    const std::string reason = Gem::Common::describeException(cause);
    timer_.cancel();
    tick_timer_.cancel();
    glogger << "GMPIPollCore (rank " << calls_->rank()
            << "): the poll has failed and receives nothing more: " << reason << '\n'
            << GWARNING;
    for(std::size_t index = 0; index < receivers_.size(); ++index) {
        if(receivers_[index]) {
            boost::asio::post(receiver_executors_[index], [receiver = receivers_[index], reason] {
                receiver->pollFailed(reason);
            });
        }
    }
}

/******************************************************************************/
/**
 * Takes over the sends that have been started since the last look, and counts them. On the strand,
 * or in drainSends() and shutdown(). Room is made before a send is taken from the queue: once
 * taken, nothing may throw before it is held here, or its bytes would go while MPI may still read
 * them.
 */
void GMPIPollCore::takeStarted_() {
    const auto make_room = [](auto &held) {
        if(held.size() == held.capacity()) {
            held.reserve(std::max<std::size_t>(16, 2 * held.capacity()));
        }
    };
    while(true) {
        make_room(requests_);
        make_room(buffers_);
        auto started = started_.try_pop();
        if(not started) {
            return;
        }
        requests_.push_back(started->request);
        buffers_.push_back(std::move(started->bytes));
        ++n_sent_;
    }
}

/******************************************************************************/
/** Frees the sends that have completed, in one pass. On the strand. */
void GMPIPollCore::reap_() {
    if(requests_.empty() || calls_->testSome(requests_) == 0) {
        return;
    }
    // A completed send is NONE now. Moving a unique_ptr does not move the bytes it points to.
    std::size_t kept = 0;
    for(std::size_t i = 0; i < requests_.size(); ++i) {
        if(requests_[i] == GMPIRequest::NONE) {
            continue;
        }
        if(kept != i) {
            requests_[kept] = requests_[i];
            buffers_[kept] = std::move(buffers_[i]);
        }
        ++kept;
    }
    requests_.resize(kept);
    buffers_.resize(kept);
}

/******************************************************************************/
/**
 * A send was started: the pause is reset and a pause under way ends (the cancelled wait runs the
 * next link). On the strand. The flag is cleared first, so that a send started from here on posts
 * a kick of its own.
 */
void GMPIPollCore::kick_() {
    kick_pending_.store(false);
    pause_ = timing_.pauseMin;
    try {
        timer_.cancel();
    }
    catch(...) {
        this->fail_(std::current_exception());
    }
}

/******************************************************************************/
/**
 * Sets the keep-alive interval and (re)arms the ticks with it; 0 stops them. On the strand. A
 * failure ends the chain (fail_()).
 *
 * @param interval The keep-alive interval
 */
void GMPIPollCore::keepAlive_(std::chrono::milliseconds interval) {
    keep_alive_ = std::max(interval, std::chrono::milliseconds::zero());
    if(chain_ended_.load()) {
        return;
    }
    try {
        if(keep_alive_.count() == 0) {
            tick_timer_.cancel();
        }
        else {
            this->armTick_();
        }
    }
    catch(...) {
        this->fail_(std::current_exception());
    }
}

/******************************************************************************/
/** Arms the next tick, one interval from now; a wait under way is cancelled (its handler does
 *  nothing). On the strand. */
void GMPIPollCore::armTick_() {
    tick_timer_.expires_after(keep_alive_);
    tick_timer_.async_wait([self = this->shared_from_this()](const boost::system::error_code &ec) {
        if(not ec) {
            self->tick_();
        }
    });
}

/******************************************************************************/
/**
 * One tick of the signs of life (see the class comment), on the strand: what has arrived is
 * received first; then every rank with a live receiver that has been heard from and not for more
 * than two intervals is called silent -- unless the tick is late by more than half an interval,
 * which judges nobody and starts every rank's time afresh --, and every other one gets a ping. A
 * call that throws ends the chain (fail_()).
 */
void GMPIPollCore::tick_() {
    if(chain_ended_.load() || keep_alive_.count() == 0) {
        return;
    }
    try {
        const bool late = std::chrono::steady_clock::now() - tick_timer_.expiry() > keep_alive_ / 2;
        for(int round = 0; round < timing_.rounds; ++round) {
            if(not this->receiveOne_()) {
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        for(std::size_t index = 0; index < life_.size(); ++index) {
            GLife &life = life_[index];
            if(not receivers_[index] || ended_ranks_[index] || life.silent) {
                continue;
            }
            const int rank = static_cast<int>(index);
            if(late) {
                life.heard = now; // this process did not run: nobody can be judged
            }
            else if(life.everHeard && now - life.heard > 2 * keep_alive_) {
                life.silent = true;
                glogger << "GMPIPollCore (rank " << calls_->rank() << "): rank " << rank
                        << " has not been heard from for "
                        << std::chrono::duration_cast<std::chrono::milliseconds>(now - life.heard)
                               .count()
                        << " ms (keep-alive interval " << keep_alive_.count()
                        << " ms): it is silent" << '\n'
                        << GWARNING;
                calls_->noteLostRank(rank);
                boost::asio::post(receiver_executors_[index], [receiver = receivers_[index]] {
                    receiver->silent();
                });
                continue;
            }
            this->startSend(rank, TAG_PEER_PING, std::string{});
            ++n_pings_;
        }
        this->armTick_();
    }
    catch(...) {
        this->fail_(std::current_exception());
    }
}

/******************************************************************************/
/**
 * Counts one receiver as ended, once. On the strand.
 *
 * @param rank The receiver's rank
 */
void GMPIPollCore::ended_(int rank) {
    const auto index = static_cast<std::size_t>(rank);
    if(index >= ended_ranks_.size() || ended_ranks_[index] || receivers_.empty() ||
       not receivers_[index]) {
        return;
    }
    ended_ranks_[index] = true;
    --live_receivers_;
}

/******************************************************************************/
/**
 * @param rank A rank
 * @return Whether it is one of the communicator's ranks
 */
bool GMPIPollCore::validRank_(int rank) const noexcept {
    return rank >= 0 && rank < size_;
}

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
