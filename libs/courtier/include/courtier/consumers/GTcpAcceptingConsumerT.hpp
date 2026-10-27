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

#include "common/GGlobalDefines.hpp"

// Standard headers
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

// Boost headers
#include <boost/asio.hpp>

// Geneva headers
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GThreadBudget.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/consumers/GNetworkedConsumerT.hpp"
#include "courtier/GWireSerializationContext.hpp" // blob send-once: shared registry

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The shared TCP-accepting server shell of the socket consumers (ASIO and websocket). Both are TCP
 * servers that hand out work items from the current batch to connected clients and collect the
 * processed results; the ~90%-identical server lifecycle -- acceptor open/bind/listen, the
 * strand-serialized accept loop with EMFILE backoff, the io-thread group, idempotent shutdown, the
 * session counter and the blob send-once registry -- lives here ONCE. A concrete consumer
 * contributes only its session construction via the start_session_() hook (plus any
 * transport-specific configuration and knobs).
 *
 * @tparam processable_type The work-item type served to clients and collected back from them.
 */
template <typename processable_type>
class GTcpAcceptingConsumerT
  : public GNetworkedConsumerT<processable_type>
  , public std::enable_shared_from_this<GTcpAcceptingConsumerT<processable_type>> {
public:
    /***************************************************************************/
    /** @brief The destructor stops the server (idempotent). stopServer() posts to a strand and joins
     *  the io threads, either of which may throw; a destructor must not propagate, so teardown is
     *  best-effort and any exception is swallowed. */
    ~GTcpAcceptingConsumerT() override {
        try {
            this->stopServer();
        } catch(...) { // NOLINT(bugprone-empty-catch) -- deliberate best-effort teardown
            // never let an exception escape a destructor
        }
    }

    GTcpAcceptingConsumerT(const GTcpAcceptingConsumerT &) = delete;
    GTcpAcceptingConsumerT(GTcpAcceptingConsumerT &&) = delete;
    GTcpAcceptingConsumerT &operator=(const GTcpAcceptingConsumerT &) = delete;
    GTcpAcceptingConsumerT &operator=(GTcpAcceptingConsumerT &&) = delete;

    /***************************************************************************/
    /** @brief The port the server listens on (useful when 0 was passed to pick an ephemeral port).
     *  @return The actual TCP port the acceptor is bound to. */
    [[nodiscard]] unsigned short getPort() const noexcept { return port_; }

    /** @brief Number of clients currently connected and being served.
     *  @return The current count of active sessions. */
    [[nodiscard]] std::size_t getNActiveSessions() const noexcept { return n_active_sessions_.load(); }

    /** @brief The number of distinct blobs the server has interned for transport (blob
     *  send-once). One per distinct genome structure across all clients -- so a whole population of one
     *  problem type interns a single blob, however many work items and clients are involved.
     *  @return The count of interned blobs. */
    [[nodiscard]] std::size_t getInternedBlobCount() const { return wire_registry_.size(); }

    /** @brief Bounds the number of distinct blobs the server caches for transport (blob send-once);
     *  0 (the default) keeps them all. Beyond the bound the least-recently-used blob is evicted and the
     *  next work item that needs it re-sends it in full (or, for a worker that has since dropped it too,
     *  is re-fetched) -- so this only trades a re-send for memory and never affects correctness.
     *  @param max_blobs The maximum number of cached blobs (0 == unbounded). */
    void setInternedBlobCapacity(std::size_t max_blobs) { wire_registry_.setCapacity(max_blobs); }

    /***************************************************************************/
    /**
     * @brief Opens the acceptor and starts the io threads. Must be called once, after the consumer
     * has been registered with the GConsumerRegistry and before any batch is submitted.
     *
     * @throws geneva_exception if the acceptor cannot be opened, bound or set to listen
     */
    void startServer() {
        boost::system::error_code ec;

        boost::asio::ip::tcp::endpoint const endpoint{boost::asio::ip::tcp::v4(), port_};
        std::ignore = acceptor_.open(endpoint.protocol(), ec); // returned ec duplicates the checked out-param
        if(ec || not acceptor_.is_open()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In " << consumer_name_ << "::startServer(): could not open the acceptor: "
                << ec.message() << '\n'
            );
        }
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
        std::ignore = acceptor_.bind(endpoint, ec); // returned ec duplicates the checked out-param
        if(ec) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In " << consumer_name_ << "::startServer(): could not bind to port "
                << port_ << ": " << ec.message() << '\n'
            );
        }
        // If port 0 was requested, learn the ephemeral port the OS assigned.
        port_ = acceptor_.local_endpoint().port();

        std::ignore = acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec); // returned ec duplicates the checked out-param
        if(ec) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In " << consumer_name_ << "::startServer(): could not listen: "
                << ec.message() << '\n'
            );
        }

        this->async_start_accept();

        // Account the io threads in the process-wide thread budget (io threads mostly block, but
        // they are threads all the same; the budget's warning factor allows for the overlap).
        io_budget_ = Gem::Common::Concurrency::threadBudget().reserve(
            consumer_name_,
            static_cast<unsigned int>(n_threads_),
            Gem::Common::Concurrency::ThreadElasticity::Elastic
        );

        for(std::size_t t = 0; t < n_threads_; ++t) {
            gtg_.create_thread([this] { io_context_.run(); });
        }
    }

    /***************************************************************************/
    /**
     * @brief Stops accepting connections, lets the io threads drain and joins them. Idempotent.
     */
    void stopServer() {
        if(stopped_already_.exchange(true)) {
            return;
        }
        this->requestStop();

        // Close the acceptor and cancel the retry timer ON the accept strand, so this never runs
        // concurrently with the accept handler (a tcp::acceptor is not thread-safe). All other
        // acceptor access also happens on the strand.
        boost::asio::post(accept_strand_, [this]() {
            boost::system::error_code ec;
            std::ignore = acceptor_.close(ec); // best-effort teardown
            accept_retry_timer_.cancel();
        });

        // Drop the work guard and stop the context so run() returns on every io thread.
        work_guard_.reset();
        io_context_.stop();
        gtg_.join_all();

        // The io threads are joined -- return their reservation to the thread budget.
        io_budget_.release();
    }

protected:
    /***************************************************************************/
    /** @brief Initializes the shared server shell.
     *
     *  @param consumer_name The concrete consumer's qualified class name, used in diagnostics
     *  @param port The TCP port to listen on (0 lets the OS pick an ephemeral port, learned in startServer())
     *  @param n_threads Number of io threads to run; 0 means use the hardware concurrency */
    GTcpAcceptingConsumerT(std::string_view consumer_name, unsigned short port, std::size_t n_threads)
        : consumer_name_(consumer_name)
        , port_(port)
        , n_threads_(n_threads == 0 ? default_threads() : n_threads)
    { /* nothing */ }

    /***************************************************************************/
    /** @brief Starts a transport-specific session for a freshly accepted connection -- the ONE hook a
     *  concrete socket consumer implements. Called on the accept strand, with the object kept alive
     *  by a shared_ptr in the accept chain.
     *
     *  @param socket The freshly accepted client socket (ownership taken by move) */
    virtual void start_session_(boost::asio::ip::tcp::socket socket) = 0;

    /***************************************************************************/
    /** @brief Adjusts the live-session counter from a session's sign-on/sign-off callback.
     *  @param sign_on True when a session starts, false when it ends */
    void adjustSessionCount(bool sign_on) noexcept {
        if(sign_on) {
            n_active_sessions_.fetch_add(1, std::memory_order_relaxed);
        }
        else {
            // Race-free decrement: sign-on/sign-off are balanced by the session
            // lifecycle, so the counter cannot underflow (the former load()-then-
            // decrement check-then-act could double-decrement under contention).
            n_active_sessions_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    /***************************************************************************/
    /// The io context all sessions run on; accessible to the concrete session factory.
    boost::asio::io_context io_context_;

    /// The blob send-once registry shared by all of this server's sessions: a content-addressed
    /// store of the blobs the server has sent, with per-peer ack tracking, so a given blob
    /// travels to a given client only once. A worker that misses (id-only item with no cached blob)
    /// fetches it back via the REQUEST_BLOB / SEND_BLOB command pair.
    Gem::Courtier::GWireBlobRegistry wire_registry_;

private:
    /***************************************************************************/
    /** @brief Default number of io threads when none was requested.
     *  @return The hardware concurrency, or 1 if it cannot be determined. */
    static std::size_t default_threads() {
        const unsigned int hc = std::thread::hardware_concurrency();
        return hc == 0 ? 1u : hc;
    }

    /***************************************************************************/
    /** @brief Arms the next asynchronous accept on the accept strand (which serializes all acceptor
     *  access across the io threads). */
    void async_start_accept() {
        // Use the connectionless async_accept overload: each accept yields its own fresh socket, so
        // there is no shared socket_ member to race on. The handler is bound to accept_strand_, which
        // serializes all acceptor access across the io threads (acceptor is not thread-safe).
        auto self = this->shared_from_this();
        acceptor_.async_accept(
            boost::asio::bind_executor(
                accept_strand_,
                [self](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
                    self->when_accepted(ec, std::move(socket));
                }
            )
        );
    }

    /***************************************************************************/
    /** @brief Accept-completion handler: on success it spins up a new session for the connection (via
     *  the start_session_() hook) and re-arms the accept; on a transient error it backs off and
     *  retries; on shutdown it stops.
     *
     *  @param ec A possible error code from the accept operation
     *  @param socket The freshly accepted client socket (ownership taken by move) */
    void when_accepted(boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
        if(this->stopped()) {
            return; // shutting down: do not start a session and do not re-arm
        }

        if(ec) {
            // A closed acceptor (operation_aborted / bad_descriptor) means we are stopping -- give up.
            if(ec == boost::asio::error::operation_aborted
               || ec == boost::asio::error::bad_descriptor) {
                return;
            }
            // A transient accept failure -- most importantly EMFILE/ENFILE ("too many open files")
            // -- must NOT be retried in a tight loop: that would pin an io thread at 100% and never
            // let the listen backlog drain. Back off briefly and try again, giving file descriptors
            // time to be reclaimed.
            glogger << "In " << consumer_name_ << "::when_accepted(): " << ec.message()
                    << " -- backing off before retrying accept" << '\n'
                    << GWARNING;
            accept_retry_timer_.expires_after(std::chrono::milliseconds(100));
            auto self = this->shared_from_this();
            accept_retry_timer_.async_wait(
                boost::asio::bind_executor(accept_strand_, [self](boost::system::error_code tec) {
                    if(not tec && not self->stopped()) {
                        self->async_start_accept();
                    }
                })
            );
            return;
        }

        // Got a connection -- hand it to a new transport-specific session.
        this->start_session_(std::move(socket));

        // Re-arm for the next connection.
        this->async_start_accept();
    }

    /***************************************************************************/
    std::string consumer_name_; ///< The concrete consumer's qualified class name (diagnostics)
    unsigned short port_;
    std::size_t n_threads_;

    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_ =
        boost::asio::make_work_guard(io_context_);
    boost::asio::ip::tcp::acceptor acceptor_{io_context_};
    /// Serializes all acceptor access (async_accept re-arm, close, retry timer) across the io threads.
    boost::asio::strand<boost::asio::io_context::executor_type> accept_strand_{
        io_context_.get_executor()
    };
    /// Backoff timer used to retry accept after a transient failure (e.g. EMFILE) without busy-spinning.
    boost::asio::steady_timer accept_retry_timer_{io_context_};

    /// The io threads' reservation in the process-wide thread budget (held from startServer()
    /// until the threads are joined in stopServer()). Declared before gtg_ as belt-and-braces:
    /// stopServer() releases it explicitly right after join_all().
    Gem::Common::Concurrency::GThreadBudget::Reservation io_budget_;

    Gem::Common::Concurrency::GThreadGroup gtg_;
    std::atomic<std::size_t> n_active_sessions_{0};
    std::atomic<bool> stopped_already_{false};
};

/******************************************************************************/

} /* namespace Gem::Courtier */
