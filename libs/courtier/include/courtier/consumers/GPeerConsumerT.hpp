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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// Boost headers go here
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GThreadBudget.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "common/concurrency/GThreadSafeKeyedStoreT.hpp"
#include "courtier/GWireSerializationContext.hpp" // blob send-once: shared registry
#include "courtier/consumers/GNetworkedConsumerT.hpp"
#include "courtier/transport/GPeerI.hpp"        // the listener and its peers
#include "courtier/transport/GPeerSessionT.hpp" // one session per peer

namespace Gem::Courtier {

/******************************************************************************/
/**
 * A networked consumer over any transport that offers a GListenerI: the server's shell. It runs
 * the sessions (a GPeerSessionT for every peer the listener makes), keeps the peers that have not
 * ended in a registry, and ends the run: it tells every client that the run is over and waits for
 * them, bounded (GPeerI.hpp, rule 8).
 *
 * Threads. The shell's io context is the session pool: nThreads threads (the hardware concurrency
 * if 0) that run the sessions' handlers and nothing else; they are accounted in the thread budget
 * as "<consumer name>:sessions". The listener is handed the executor of that pool; the threads that
 * move the bytes, if the transport has any, are the listener's own (rule 2).
 *
 * The peers. A session enters the registry (keyed by the client's id in the blob registry) when its
 * peer has been started, and leaves it in its destructor, through its serverSignOn() hook;
 * getNActiveSessions() is the registry's size. The registry holds the peers weakly: it never keeps a
 * connection alive. The order is what keeps rule 2 of GPeerI.hpp: stopServer() closes only peers it
 * finds in the registry, so it never closes a peer before that peer's start(). A session that
 * enters the registry after stopServer() has looked at it sees the stop itself: the stop is
 * requested before the registry is read, and the session looks at the stop after it has entered
 * (startSession_()).
 *
 * The end of a run (stopServer()): the consumer is told to stop (parked sessions wake up and close
 * their connections with GPeerClose::RUN_ENDED); the listener makes no new peers; every peer in the
 * registry is closed with RUN_ENDED (also a session that waits for nothing, e.g. one whose client
 * is evaluating); the shell waits until the registry is empty, but not longer than the listener's
 * stopGrace(), and logs how many peers did not confirm; the session threads are joined; the
 * listener ends its own work (endWork(): its threads joined, its I/O context shut down). The
 * listener lives on until the shell is destroyed, so that questions about the transport (its port,
 * say) can still be asked of it. A session cut off by the grace lives until the shell is destroyed;
 * the items it held are put back then, but after stopServer() the consumer serves nothing anyway.
 *
 * Lost clients. Whether items of a lost client come back by the time lease is the listener's
 * answer (detectsLoss()): a transport that notices the loss of a client by itself needs no time
 * lease. A session's CheckoutLease is used in either case: when a connection ends, by a close or by
 * a loss, what its client held is put back at once. If a client hangs, the time lease may put an
 * item back that the client later still returns when it ends; the item is then evaluated twice and
 * the late duplicate is dropped and counted (GNetworkedConsumerT).
 *
 * Lifetime. The sessions refer to this object through a plain `this`. That is safe because their
 * handlers run on the session threads only, which are joined in stopServer(), and the destructor
 * stops the server. Declaration order matters: the sessions that are still alive when the server
 * has stopped are destroyed with the session pool's context, and their destructors reach the peer
 * registry and the blob registry, which are declared before it; the listener is declared before the
 * context too, so that it -- and with it the transport's I/O context, whose sockets the peers may
 * hold -- is destroyed after it.
 *
 * @tparam processable_type The work-item type served to clients and collected back from them.
 */
template <typename processable_type>
class GPeerConsumerT : public GNetworkedConsumerT<processable_type> {
public:
    using session_type = Gem::Courtier::Consumers::GPeerSessionT<processable_type>;

    /***************************************************************************/
    /**
     * @brief Takes the listener the consumer serves its clients through.
     *
     * @param consumer_name The concrete consumer's qualified class name, used in diagnostics and
     *        in the thread budget
     * @param listener The server side of the transport; the consumer owns it. Must not be null.
     * @param n_threads Number of session threads; 0 means one per hardware thread
     * @param serialization_mode Which serialization format the wire protocol uses
     * @throws geneva_exception if @p listener is empty
     */
    GPeerConsumerT(
        std::string_view consumer_name,
        std::unique_ptr<Consumers::GListenerI> listener,
        std::size_t n_threads,
        Gem::Common::serializationMode serialization_mode
    )
      : consumer_name_(consumer_name)
      , n_threads_(n_threads == 0 ? defaultThreads_() : n_threads)
      , serialization_mode_(serialization_mode)
      , detects_loss_(listener ? listener->detectsLoss() : false)
      , listener_(std::move(listener)) {
        if(not listener_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In " << consumer_name_ << ": Error!" << '\n'
                << "The listener must not be empty" << '\n'
            );
        }
    }

    /** @brief The destructor stops the server if that has not been done already. */
    ~GPeerConsumerT() override {
        this->stopServerNoThrow_();
    }

    GPeerConsumerT(const GPeerConsumerT &) = delete;
    GPeerConsumerT(GPeerConsumerT &&) = delete;
    GPeerConsumerT &operator=(const GPeerConsumerT &) = delete;
    GPeerConsumerT &operator=(GPeerConsumerT &&) = delete;

    /***************************************************************************/
    /** @brief Number of sessions that are alive: the peers in the registry.
     *  @return The current count of sessions. */
    [[nodiscard]] std::size_t getNActiveSessions() const {
        return peers_.size();
    }

    /** @brief The number of distinct blobs the server has interned for transport (blob
     *  send-once). One per distinct genome structure across all clients.
     *  @return The count of interned blobs. */
    [[nodiscard]] std::size_t getInternedBlobCount() const {
        return wire_registry_.size();
    }

    /** @brief Bounds the number of distinct blobs the server caches for transport; 0 (the
     *  default) keeps them all.
     *  @param max_blobs The maximum number of cached blobs (0 == unbounded). */
    void setInternedBlobCapacity(std::size_t max_blobs) {
        wire_registry_.setCapacity(max_blobs);
    }

    /***************************************************************************/
    /**
     * @brief Starts the listener and the session threads. To be called once, after the consumer
     * has been registered with the GConsumerRegistry and before any batch is submitted. Further
     * calls do nothing.
     *
     * If the listener cannot be started (a port that cannot be bound, say), its exception reaches
     * the caller and the consumer stays one that was never started: stopServer() and the
     * destructor have nothing to stop, and the listener is not asked to stop or to end its work
     * (GListenerI::start()). Such a consumer cannot be started any more; a further call throws.
     *
     * @throws whatever the listener's start() throws; geneva_exception if an earlier call failed
     */
    void startServer() {
        if(start_called_.exchange(true)) {
            if(not started_.load()) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In " << consumer_name_ << "::startServer(): Error!" << '\n'
                    << "The listener could not be started before; this consumer cannot be "
                       "started any more"
                    << '\n'
                );
            }
            return;
        }

        // The listener calls back for every client, on a session thread. A session is owned by
        // its peer from start() on. If start() throws, nothing has been started.
        listener_->start(
            io_context_.get_executor(),
            [this](std::shared_ptr<Consumers::GPeerI> peer, GWirePeerId peer_id) {
                this->startSession_(peer, peer_id);
            }
        );
        started_.store(true);

        budget_ = Gem::Common::Concurrency::threadBudget().reserve(
            consumer_name_ + ":sessions",
            static_cast<unsigned int>(n_threads_),
            Gem::Common::Concurrency::ThreadElasticity::Elastic
        );
        for(std::size_t t = 0; t < n_threads_; ++t) {
            threads_.create_thread([this] { io_context_.run(); });
        }
    }

    /***************************************************************************/
    /**
     * @brief Ends the run: tells every client, waits for them (at most the listener's
     * stopGrace()), joins the session threads and ends the transport's own work (see the class
     * comment). Idempotent. Not on a session thread. A server that was never started has nothing
     * to stop.
     */
    void stopServer() {
        if(stopped_already_.exchange(true)) {
            return;
        }
        this->requestStop();

        if(not started_.load()) {
            return;
        }

        listener_->stop();

        // Every peer that has not ended hears that the run is over. A session made after this
        // snapshot sees the stop when it starts (startSession_()).
        for(const auto &weak_peer : peers_.values()) {
            if(auto peer = weak_peer.lock()) {
                peer->close(Consumers::GPeerClose::RUN_ENDED);
            }
        }
        if(not peers_.waitUntilEmpty(listener_->stopGrace())) {
            glogger << "In " << consumer_name_ << "::stopServer(): " << peers_.size()
                    << " client(s) did not confirm the end of the run within "
                    << listener_->stopGrace().count() << " ms" << '\n'
                    << GLOGGING;
        }

        work_guard_.reset();
        io_context_.stop();
        threads_.join_all();

        // No session runs any more: the transport's own work can end.
        listener_->endWork();
        budget_.release();
    }

protected:
    /***************************************************************************/
    /** @brief Stops the server without letting an exception escape -- for destructors. */
    void stopServerNoThrow_() noexcept {
        try {
            this->stopServer();
        }
        catch(...) { // NOLINT(bugprone-empty-catch) -- deliberate best-effort teardown
            // never let an exception escape a destructor
        }
    }

    /***************************************************************************/
    /** @brief A transport that notices the loss of a client by itself needs no time lease; one
     *  that does not, reclaims items by it.
     *  @return true if stuck in-flight items are reclaimed via the time lease */
    [[nodiscard]] bool usesTimeLease() const override {
        return not detects_loss_;
    }

    /***************************************************************************/
    /** @brief The listener, for a concrete consumer that has questions about its transport (a
     *  port, say). It lives as long as this object.
     *  @return The listener */
    [[nodiscard]] Consumers::GListenerI &listener() const {
        return *listener_;
    }

private:
    /***************************************************************************/
    /** @brief Default number of session threads when none was requested.
     *  @return The hardware concurrency, or 1 if it cannot be determined. */
    static std::size_t defaultThreads_() {
        const unsigned int hc = std::thread::hardware_concurrency();
        return hc == 0 ? 1U : hc;
    }

    /***************************************************************************/
    /** @brief Starts one session for a peer the listener has made, with a CheckoutLease that puts
     *  back what the client holds when the session ends. On a session thread.
     *  @param peer The connection, not started yet
     *  @param peer_id The client's id in the blob registry */
    void startSession_(const std::shared_ptr<Consumers::GPeerI> &peer, GWirePeerId peer_id) {
        using lease_type = GNetworkedConsumerT<processable_type>::CheckoutLease;
        auto lease = std::make_shared<lease_type>();
        // The lease and the hooks refer to this consumer through `this` (see the class comment).
        lease->on_abandon = [this](Gem::Courtier::CORRELATION_ID_TYPE id) { this->requeue(id); };

        const std::weak_ptr<Consumers::GPeerI> weak_peer = peer;
        auto session = std::make_shared<session_type>(
            typename session_type::SessionHooks{
                .getPayloadItem =
                    [this, lease](Gem::Courtier::GWakeFn wake) {
                        auto result = this->checkoutOrPark(std::move(wake));
                        lease->add(result.item); // no-op without an item
                        return result;
                    },
                .putPayloadItem =
                    [this, lease](Gem::Courtier::GCommandContainerT<processable_type> &&frame) {
                        lease->remove(frame.outcome().correlation_id);
                        this->checkin(std::move(frame));
                    },
                .abandonItems = [lease] { lease->abandonAll(); },
                .checkServerStopped = [this]() -> bool { return this->stopped(); },
                .serverSignOn =
                    [this, peer_id, weak_peer](bool sign_on) {
                        if(sign_on) {
                            peers_.set(peer_id, weak_peer);
                        }
                        else {
                            peers_.remove(peer_id);
                        }
                    },
            },
            serialization_mode_,
            &wire_registry_,
            peer_id
        );
        session->start(peer);

        // The session has entered the registry in start(), after its peer was started. The stop
        // may have come after stopServer() took its snapshot of the registry: then it is told here.
        if(this->stopped()) {
            peer->close(Consumers::GPeerClose::RUN_ENDED);
        }
    }

    /***************************************************************************/
    // Declaration order matters (see the class comment): what the sessions reach in their
    // destructors, and the listener, before the context the sessions die with.

    std::string consumer_name_; ///< The concrete consumer's qualified class name (diagnostics)
    std::size_t n_threads_;     ///< The number of session threads
    Gem::Common::serializationMode serialization_mode_; ///< The wire format
    bool detects_loss_; ///< The listener's answer to detectsLoss(), asked once

    /// The blob send-once registry shared by all of this server's sessions
    Gem::Courtier::GWireBlobRegistry wire_registry_;
    /// The peers whose sessions are alive, by the client's id; held weakly
    Gem::Common::Concurrency::GThreadSafeKeyedStoreT<GWirePeerId, std::weak_ptr<Consumers::GPeerI>>
        peers_;

    std::unique_ptr<Consumers::GListenerI> listener_; ///< The transport; lives as long as this

    boost::asio::io_context io_context_; ///< The session pool's context
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
        work_guard_{boost::asio::make_work_guard(io_context_)}; ///< Keeps the threads in run()

    /// The session threads' reservation in the process-wide thread budget (held from
    /// startServer() until the threads are joined in stopServer())
    Gem::Common::Concurrency::GThreadBudget::Reservation budget_;
    Gem::Common::Concurrency::GThreadGroup threads_; ///< The session threads

    std::atomic<bool> start_called_{false};    ///< startServer() has been called
    std::atomic<bool> started_{false};         ///< The listener has been started
    std::atomic<bool> stopped_already_{false}; ///< stopServer() has been called
};

/******************************************************************************/

} /* namespace Gem::Courtier */
