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
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>

// Geneva headers go here
#include "common/concurrency/GThreadBudget.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GWireSerializationContext.hpp" // GWirePeerId
#include "courtier/transport/GIoContext.hpp"
#include "courtier/transport/GPeerI.hpp"         // the interfaces
#include "courtier/transport/GWebsocketPeer.hpp" // the connection itself

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * The server side of the websocket transport (a GListenerI): the acceptor, the accept loop with its
 * pause after a failed accept, and the threads that move the bytes.
 *
 * Threads. The listener owns an I/O context with nThreads threads of its own (accounted in the
 * thread budget as "<name>:io"); the accept loop and every connection run on strands of it, and
 * nothing else does. The peers' handlers -- the server's sessions -- run on strands of the executor
 * the shell hands to start(), and on_peer is called there too (GPeerI.hpp, rule 2). So a session
 * may take any time over a message without its connection, or another, missing a ping.
 *
 * Every accepted connection becomes a GWebsocketPeer (the server's end, which waits for the
 * client's websocket handshake when it is started) and is handed to the shell before the handshake:
 * a connection that never completes the handshake has a session all the same, and the handshake
 * limit of the peer ends it. Peer ids are a counter on the accept strand.
 *
 * stop() closes the acceptor, so that a client that connects from then on is refused, and waits for
 * that a short time of its own (the shell waits for the clients, GPeerI.hpp rule 8). endWork() stops
 * and joins the I/O threads and shuts the I/O context down (GIoContext::shutdown()): pending
 * operations go, and with them the connections that are still open, without onClosed. The listener,
 * and its I/O context with it, is destroyed after the shell's context.
 *
 * Loss of a client: with the keep-alive on (a ping interval above 0) a connection whose client is
 * gone ends by the idle limit; without it a client that vanishes without a TCP reset may never be
 * noticed, and detectsLoss() says so.
 */
class GWebsocketListener final : public GListenerI {
public:
    /** @brief Prepares the listener; nothing happens before start(). */
    GWebsocketListener(
        std::string,
        unsigned short,
        GWebsocketPeer::GOptions,
        std::size_t,
        std::chrono::milliseconds = GWEBSOCKETCONSUMERSTOPGRACE
    );

    /** @brief Ends the I/O threads if endWork() has not done so (a listener whose owner did not
     *  keep the contract of GListenerI). */
    ~GWebsocketListener() override;

    GWebsocketListener(const GWebsocketListener &) = delete;
    GWebsocketListener(GWebsocketListener &&) = delete;
    GWebsocketListener &operator=(const GWebsocketListener &) = delete;
    GWebsocketListener &operator=(GWebsocketListener &&) = delete;

    /** @brief Opens, binds and listens, then starts the I/O threads and the accept loop. Throws
     *  (and is then as if not started) if the port cannot be had. */
    void start(boost::asio::any_io_executor, on_peer_type) override;
    /** @brief Closes the acceptor: no new peers. Bounded; idempotent. */
    void stop() override;
    /** @brief Joins the I/O threads and shuts the I/O context down. */
    void endWork() override;
    /** @brief How long the shell waits for its clients at the end of a run. */
    [[nodiscard]] std::chrono::milliseconds stopGrace() const override;
    /** @brief Whether a lost client ends its connection by itself: with the keep-alive on. */
    [[nodiscard]] bool detectsLoss() const override;

    /** @brief The port: the one asked for before start() (0: the system chooses), the bound one
     *  afterwards. Any thread. */
    [[nodiscard]] unsigned short getPort() const noexcept;

private:
    void accept_();
    void whenAccepted_(const boost::system::error_code &, boost::asio::ip::tcp::socket);
    void closeAcceptor_();
    void joinIo_();

    std::string name_;                     ///< The owner's name, for the log and the thread budget
    std::atomic<unsigned short> port_;     ///< The port (see getPort())
    GWebsocketPeer::GOptions options_;     ///< How every connection's peer is set up
    std::size_t n_threads_;                ///< The number of I/O threads
    std::chrono::milliseconds stop_grace_; ///< What stopGrace() answers

    GIoContext io_context_; ///< Where the bytes move: the accept loop and every connection
    std::optional<boost::asio::executor_work_guard<GIoContext::executor_type>> work_guard_;
    /// Serializes the acceptor: the next accept, the close, the retry timer
    boost::asio::strand<GIoContext::executor_type> accept_strand_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer retry_timer_; ///< The pause after a failed accept

    /// Where on_peer and the peers' handlers run (the shell's executor); dropped by endWork()
    boost::asio::any_io_executor handler_executor_;
    /// Serializes the calls of on_peer, on the shell's executor; dropped by endWork()
    std::optional<boost::asio::strand<boost::asio::any_io_executor>> on_peer_strand_;
    on_peer_type on_peer_; ///< Set by start(), called on on_peer_strand_, dropped by endWork()
    GWirePeerId next_peer_id_ = 1; ///< Accept strand only

    /// The I/O threads' reservation in the process-wide thread budget
    Gem::Common::Concurrency::GThreadBudget::Reservation budget_;
    Gem::Common::Concurrency::GThreadGroup threads_; ///< The I/O threads
    std::atomic<bool> running_{false};               ///< The I/O threads run
    std::atomic<bool> stopped_{false};               ///< stop() was called
};

/******************************************************************************/
/**
 * The client side of the websocket transport (a GConnectorI): lookup and connect. The peer it
 * reports is the client's end of a GWebsocketPeer, which performs the websocket handshake when it is
 * started; a handshake that fails ends that peer (FAILED). Its handlers run on the executor given
 * to connect(), which also moves its bytes: a client keeps its handlers short.
 *
 * Every connect() is an attempt of its own, with a fresh resolver and socket, so the connector can
 * be asked again after a loss (reconnectable() is true).
 *
 * Threads: everything on the executor given to connect(), which must run its handlers one at a
 * time (a client runs its io context on one thread); the destructor after that context has stopped
 * running.
 */
class GWebsocketConnector final : public GConnectorI {
public:
    /** @brief Prepares the connector; nothing happens before connect(). */
    GWebsocketConnector(std::string, unsigned short, GWebsocketPeer::GOptions);

    /** @brief Gives up an attempt in progress. */
    ~GWebsocketConnector() override;

    GWebsocketConnector(const GWebsocketConnector &) = delete;
    GWebsocketConnector(GWebsocketConnector &&) = delete;
    GWebsocketConnector &operator=(const GWebsocketConnector &) = delete;
    GWebsocketConnector &operator=(GWebsocketConnector &&) = delete;

    /** @brief Looks the server up and connects. */
    void connect(boost::asio::any_io_executor, done_type) override;
    /** @brief Gives up the attempt in progress; its done is not called. */
    void cancel() override;
    /** @brief @return true: a new attempt can be made after a loss */
    [[nodiscard]] bool reconnectable() const override;

private:
    struct GAttempt; ///< Resolver, socket and callback of one attempt; owned by its pending work

    std::string address_;              ///< The server's name or address
    unsigned short port_;              ///< The server's port
    GWebsocketPeer::GOptions options_; ///< How the peer is set up
    std::weak_ptr<GAttempt> attempt_;  ///< The attempt in progress, if any
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
