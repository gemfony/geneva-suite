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

#include "courtier/transport/GWebsocketPeerTransport.hpp"

// Standard headers go here
#include <algorithm>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

// Boost headers go here
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"

namespace Gem::Courtier::Consumers {

namespace {

using error_code = boost::system::error_code;
using boost::asio::ip::tcp;

/// How long stop() waits for the accept loop to close the acceptor and for the calls of on_peer
/// under way. Short and independent of the stop grace, so that a stop takes one grace (the shell's
/// wait for its clients), not two.
constexpr std::chrono::milliseconds LISTENER_STOP_BOUND{1000};

} /* anonymous namespace */

/******************************************************************************/
/**
 * Prepares the listener; nothing happens before start().
 *
 * @param name The owner's name, for the log and for the thread budget ("<name>:io")
 * @param port The port to listen on; 0 lets the system choose (see getPort())
 * @param options How the peer of every connection is set up (name, frame type, control-frame
 *        logging, ping interval)
 * @param n_threads The number of I/O threads; 0 means one per hardware thread
 * @param stop_grace How long the shell waits for the clients to confirm the end of a run
 */
GWebsocketListener::GWebsocketListener(
    std::string name,
    unsigned short port,
    GWebsocketPeer::GOptions options,
    std::size_t n_threads,
    std::chrono::milliseconds stop_grace
)
  : name_(std::move(name))
  , port_(port)
  , options_(std::move(options))
  , n_threads_(
        n_threads == 0 ? std::max<std::size_t>(1, std::thread::hardware_concurrency()) : n_threads
    )
  , stop_grace_(stop_grace)
  , accept_strand_(io_context_.get_executor())
  , acceptor_(io_context_)
  , retry_timer_(io_context_) { /* nothing */
}

/******************************************************************************/
/** Ends the I/O threads if they still run: endWork() was not called. */
GWebsocketListener::~GWebsocketListener() {
    try {
        stopped_.store(true);
        this->joinIo_();
        error_code ignored;
        std::ignore = acceptor_.close(ignored);
    }
    catch(...) { // NOLINT(bugprone-empty-catch) -- deliberate best-effort teardown
        // never let an exception escape a destructor
    }
}

/******************************************************************************/
/**
 * Opens the acceptor on the port, binds and listens, writes the bound port back, starts the I/O
 * threads and arms the first accept. If the port cannot be had, the acceptor is closed again, no
 * thread has been started and nothing of @p executor and @p on_peer is kept (GListenerI::start()).
 *
 * @param executor Where on_peer and the peers' handlers run (the shell's session pool)
 * @param on_peer Called for every accepted connection, on a strand of @p executor
 * @throws geneva_exception if the acceptor cannot be opened, bound or set to listen
 */
void GWebsocketListener::start(boost::asio::any_io_executor executor, on_peer_type on_peer) {
    const auto fail = [this](std::string_view what, const error_code &ec) {
        error_code ignored;
        std::ignore = acceptor_.close(ignored);
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In " << name_ << ": " << what << ": " << ec.message() << '\n'
        );
    };

    error_code ec;
    const tcp::endpoint endpoint{tcp::v4(), port_.load()};
    std::ignore = acceptor_.open(endpoint.protocol(), ec); // returned ec duplicates the out-param
    if(ec || not acceptor_.is_open()) {
        fail("could not open the acceptor", ec);
    }
    std::ignore = acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    std::ignore = acceptor_.bind(endpoint, ec);
    if(ec) {
        fail("could not bind to port " + std::to_string(endpoint.port()), ec);
    }
    std::ignore = acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if(ec) {
        fail("could not listen on port " + std::to_string(endpoint.port()), ec);
    }
    // If port 0 was asked for, this is the port the system has chosen.
    port_.store(acceptor_.local_endpoint().port());

    handler_executor_ = std::move(executor);
    on_peer_strand_.emplace(boost::asio::make_strand(handler_executor_));
    on_peer_ = std::move(on_peer);

    work_guard_.emplace(io_context_.get_executor());
    budget_ = Gem::Common::Concurrency::threadBudget().reserve(
        name_ + ":io",
        static_cast<unsigned int>(n_threads_),
        Gem::Common::Concurrency::ThreadElasticity::Elastic
    );
    for(std::size_t t = 0; t < n_threads_; ++t) {
        threads_.create_thread([this] { io_context_.run(); });
    }
    running_.store(true);

    boost::asio::post(accept_strand_, [this] { this->accept_(); });
}

/******************************************************************************/
/**
 * Arms the next accept. Every accept yields a fresh socket on a strand of its own of the I/O
 * context (GWebsocketPeer needs a socket executor that serializes its handlers). The handler runs
 * on the accept strand and refers to this listener through a plain `this`: the I/O threads are
 * joined before the listener is destroyed.
 */
void GWebsocketListener::accept_() {
    acceptor_.async_accept(
        boost::asio::make_strand(io_context_),
        boost::asio::bind_executor(accept_strand_, [this](error_code ec, tcp::socket socket) {
            this->whenAccepted_(ec, std::move(socket));
        })
    );
}

/******************************************************************************/
/**
 * Called on the accept strand when an accept has completed: hands the connection to the shell as a
 * peer that has not been started (on_peer, posted to the shell's executor) and accepts again;
 * pauses after a failure; does nothing once the listener has stopped.
 *
 * @param ec A possible error of the accept
 * @param socket The accepted connection
 */
void GWebsocketListener::whenAccepted_(const error_code &ec, tcp::socket socket) {
    if(stopped_.load()) {
        return; // a connection that arrives now is dropped: its client sees a loss
    }

    if(ec) {
        // A closed acceptor means that the listener stops.
        if(ec == boost::asio::error::operation_aborted ||
           ec == boost::asio::error::bad_descriptor) {
            return;
        }
        // Anything else -- "too many open files" above all -- must not be retried at once: that
        // would occupy an I/O thread and never let descriptors be given back.
        glogger << "In " << name_ << ": " << ec.message() << " -- pausing before the next accept"
                << '\n'
                << GWARNING;
        retry_timer_.expires_after(Gem::Courtier::GCONSUMERACCEPTRETRYPAUSE);
        retry_timer_.async_wait(boost::asio::bind_executor(accept_strand_, [this](error_code tec) {
            if(not tec && not stopped_.load()) {
                this->accept_();
            }
        }));
        return;
    }

    if(not on_peer_strand_) {
        return; // not started (cannot happen while I/O threads run)
    }
    const GWirePeerId peer_id = next_peer_id_++;
    auto peer = std::make_shared<GWebsocketPeer>(std::move(socket), options_, handler_executor_);
    // The call is counted before it is posted, so that stop() -- which closes the acceptor on this
    // strand first -- sees every call that will come. on_peer_ is dropped only by endWork(), when
    // the shell's threads no longer run this.
    on_peer_calls_.set(peer_id, true);
    boost::asio::post(*on_peer_strand_, [this, peer = std::move(peer), peer_id]() mutable {
        // No exception leaves a thread of the shell's executor: a client whose session cannot
        // be made is dropped (its connection ends with the peer), and the call is counted out.
        try {
            if(on_peer_) {
                on_peer_(std::move(peer), peer_id);
            }
        }
        catch(const std::exception &e) {
            glogger << "In " << name_ << ": a new client could not be taken on: " << e.what()
                    << '\n'
                    << GWARNING;
        }
        catch(...) {
            glogger << "In " << name_ << ": a new client could not be taken on" << '\n'
                    << GWARNING;
        }
        on_peer_calls_.remove(peer_id);
    });

    this->accept_();
}

/******************************************************************************/
/** Closes the acceptor and cancels a pending retry. On the accept strand, or when no I/O thread
 *  runs. */
void GWebsocketListener::closeAcceptor_() {
    error_code ignored;
    std::ignore = acceptor_.close(ignored);
    retry_timer_.cancel();
}

/******************************************************************************/
/**
 * No new peers, and none on its way (GListenerI::stop()): the acceptor is closed on its strand, so
 * that a client that connects from now on is refused, and this call waits for that and then for the
 * calls of on_peer already posted to the shell's executor, at most LISTENER_STOP_BOUND for both.
 * Once the acceptor is closed no accept handler runs any more that could post a call (they run on
 * the same strand and look at the stop first). Not on an I/O thread, and not on a thread of the
 * shell's executor (the calls waited for run there).
 */
void GWebsocketListener::stop() {
    if(not running_.load() || stopped_.exchange(true)) {
        return;
    }
    const auto give_up_at = std::chrono::steady_clock::now() + LISTENER_STOP_BOUND;
    // The latch is shared with the handler: this call may give up waiting before it has run.
    auto closed = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    boost::asio::post(accept_strand_, [this, closed] {
        this->closeAcceptor_();
        closed->count_down();
    });
    if(not closed->wait_for(LISTENER_STOP_BOUND)) {
        glogger << "In " << name_ << ": the accept loop did not stop within "
                << LISTENER_STOP_BOUND.count() << " ms" << '\n'
                << GWARNING;
        return;
    }
    const auto left = std::max(
        std::chrono::steady_clock::duration::zero(),
        give_up_at - std::chrono::steady_clock::now()
    );
    if(not on_peer_calls_.waitUntilEmpty(left)) {
        glogger << "In " << name_ << ": " << on_peer_calls_.size()
                << " new client(s) did not reach the server within "
                << LISTENER_STOP_BOUND.count() << " ms of the stop" << '\n'
                << GWARNING;
    }
}

/******************************************************************************/
/**
 * Ends the transport's own work (GPeerI.hpp, rule 7): the I/O threads are stopped and joined and the
 * I/O context is shut down -- its pending operations, and with them the connections that are still
 * open, are destroyed without onClosed. Afterwards nothing here refers to the shell's executor.
 */
void GWebsocketListener::endWork() {
    stopped_.store(true);
    this->joinIo_();
    io_context_.shutdown();
    on_peer_ = nullptr;
    on_peer_strand_.reset();
    handler_executor_ = boost::asio::any_io_executor{};
}

/******************************************************************************/
/** Stops the I/O context, joins its threads and gives back their reservation. */
void GWebsocketListener::joinIo_() {
    if(not running_.exchange(false)) {
        return;
    }
    work_guard_.reset();
    io_context_.stop();
    threads_.join_all();
    budget_.release();
    this->closeAcceptor_(); // no I/O thread runs any more
}

/******************************************************************************/
/** @return How long the shell waits for its clients at the end of a run */
std::chrono::milliseconds GWebsocketListener::stopGrace() const {
    return stop_grace_;
}

/******************************************************************************/
/** @return Whether a lost client ends its connection by itself: only with the keep-alive on */
bool GWebsocketListener::detectsLoss() const {
    return options_.pingInterval > std::chrono::milliseconds::zero();
}

/******************************************************************************/
/** @return The port asked for before start(), the bound one afterwards */
unsigned short GWebsocketListener::getPort() const noexcept {
    return port_.load();
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/** One attempt to reach the server. Its pending operations own it; the connector refers to it
 *  weakly, to give it up. */
struct GWebsocketConnector::GAttempt {
    GAttempt(const boost::asio::any_io_executor &executor, done_type callback)
      : resolver(executor)
      , socket(executor)
      , done(std::move(callback)) { /* nothing */
    }

    /** @brief Reports the outcome, once @param peer The peer, or empty @param error Why not */
    void report(std::shared_ptr<GPeerI> peer, std::string_view error) {
        done_type call = std::move(done);
        done = nullptr;
        if(call && not cancelled) {
            call(std::move(peer), error);
        }
    }

    tcp::resolver resolver; ///< Looks the server up
    tcp::socket socket;     ///< The socket until it is connected
    done_type done;         ///< Who wants to know; dropped when called or given up
    bool cancelled = false; ///< cancel() was called
};

/******************************************************************************/
/**
 * Prepares the connector; nothing happens before connect().
 *
 * @param address The ip address or host name of the server
 * @param port The server's port
 * @param options How the peer of a connection is set up
 */
GWebsocketConnector::GWebsocketConnector(
    std::string address,
    unsigned short port,
    GWebsocketPeer::GOptions options
)
  : address_(std::move(address))
  , port_(port)
  , options_(std::move(options)) { /* nothing */
}

/******************************************************************************/
/** Gives up an attempt in progress. */
GWebsocketConnector::~GWebsocketConnector() {
    this->cancel();
}

/******************************************************************************/
/**
 * Looks the server up and connects to one of the endpoints found. On success the socket becomes the
 * client's end of a GWebsocketPeer, which performs the websocket handshake when it is started.
 * done is called on @p executor, never inside this call.
 *
 * @param executor Where the attempt runs and the peer works; runs one handler at a time
 * @param done Called with the peer, or with no peer and the reason
 */
void GWebsocketConnector::connect(boost::asio::any_io_executor executor, done_type done) {
    this->cancel(); // an attempt that is still in progress is given up

    auto attempt = std::make_shared<GAttempt>(executor, std::move(done));
    attempt_ = attempt;
    attempt->resolver.async_resolve(
        address_,
        std::to_string(port_),
        [attempt,
         options = options_,
         host = address_](error_code ec, const tcp::resolver::results_type &results) mutable {
            if(attempt->cancelled) {
                return;
            }
            if(ec) {
                attempt->report(nullptr, "the lookup of the server failed: " + ec.message());
                return;
            }
            auto &socket = attempt->socket;
            boost::asio::async_connect(
                socket,
                results,
                [attempt = std::move(attempt),
                 options = std::move(options),
                 host = std::move(
                     host
                 )](error_code cec, [[maybe_unused]] const tcp::endpoint &endpoint) mutable {
                    if(attempt->cancelled) {
                        return;
                    }
                    if(cec) {
                        attempt->report(nullptr, "no connection to the server: " + cec.message());
                        return;
                    }
                    attempt->report(
                        std::make_shared<GWebsocketPeer>(
                            std::move(attempt->socket),
                            std::move(options),
                            boost::asio::any_io_executor{},
                            std::move(host)
                        ),
                        std::string_view{}
                    );
                }
            );
        }
    );
}

/******************************************************************************/
/** Gives up the attempt in progress, if there is one; its callback is dropped, not called. */
void GWebsocketConnector::cancel() {
    if(auto attempt = attempt_.lock()) {
        attempt->cancelled = true;
        attempt->done = nullptr;
        attempt->resolver.cancel();
        error_code ignored;
        std::ignore = attempt->socket.close(ignored);
    }
    attempt_.reset();
}

/******************************************************************************/
/** @return true: every connect() is an attempt of its own */
bool GWebsocketConnector::reconnectable() const {
    return true;
}

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
