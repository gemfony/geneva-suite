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
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

// Boost headers
#include <boost/asio.hpp>

// Geneva headers
#include "common/concurrency/GThreadBudget.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/consumers/GTcpAcceptingConsumerT.hpp"
#include "courtier/consumers/GWebsocketConsumerConfig.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp" // the session, the client, the protocol

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The courtier websocket consumer. Functionally the websocket twin of GAsioConsumerT: a TCP server
 * whose accepted connections are upgraded to websocket sessions (with keep-alive ping/pong) that
 * hand out work from the current batch and collect results, reconciled via the inherited
 * GTcpAcceptingConsumerT / GNetworkedConsumerT / GBaseConsumerT machinery. The whole server
 * lifecycle (accept loop, io threads, shutdown) lives on the shared GTcpAcceptingConsumerT shell;
 * this class contributes only the websocket session construction (with its liveness-driven
 * CheckoutLease) and the websocket-specific knobs.
 *
 * As with the ASIO consumer it reuses the existing courtier session
 * (Gem::Courtier::Consumers::GWebsocketConsumerSessionT) and GCommandContainerT wire protocol, so an
 * unmodified Gem::Courtier::Consumers::GWebsocketClientT serves a courtier server.
 *
 * It is configured by a GWebsocketConsumerConfig, the struct its client is built from too.
 *
 * Threads. The connections do their I/O on the io threads of the shell (nThreads of them), which
 * run nothing else; the sessions run on as many threads of their own. A session may therefore take
 * as long as it likes over a message without its connection, or any other, missing a ping.
 *
 * @tparam processable_type The work-item type handed to clients and reconciled back into the population
 */
template <typename processable_type>
class GWebsocketConsumerT final : public GTcpAcceptingConsumerT<processable_type> {
public:
    using session_type = Gem::Courtier::Consumers::GWebsocketConsumerSessionT<processable_type>;
    /// The configuration this consumer (and its client) is built from
    using config_type = GWebsocketConsumerConfig;

    /***************************************************************************/
    /** @brief Constructs the websocket consumer from the configuration of its family.
     *
     *  @param config Reads port (0 lets the OS pick; the chosen port is read back), nThreads (the
     *         io threads, and as many threads for the sessions; 0 selects the hardware
     *         concurrency), serializationMode (the wire format),
     *         pingInterval (seconds of silence after which a session pings its client; after twice
     *         that time without any inbound data the client is presumed gone; 0 switches this off)
     *         and verboseControlFrames. The client members (ip, clientPrefetchDepth) are
     *         ignored. */
    explicit GWebsocketConsumerT(const config_type &config)
      : GTcpAcceptingConsumerT<processable_type>(
            "Gem::Courtier::GWebsocketConsumerT",
            config.port,
            config.nThreads
        )
      , n_session_threads_(
            config.nThreads == 0 ? std::max(1U, std::thread::hardware_concurrency())
                                 : config.nThreads
        )
      , serialization_mode_(config.serializationMode)
      , ping_interval_(config.pingInterval)
      , verbose_control_frames_(config.verboseControlFrames) { /* nothing */
    }

    /** @brief Stops the server while this object is still complete (see the base class). */
    ~GWebsocketConsumerT() override {
        this->stopServerNoThrow_();
        // No thread runs now. The connections refer to both contexts (see GIoContext): the I/O
        // context's pending handlers go first, while the sessions' context still exists; the rest
        // goes with session_context_, while the I/O context's services still exist.
        this->io_context_.shutdown();
    }

    GWebsocketConsumerT(const GWebsocketConsumerT &) = delete;
    GWebsocketConsumerT(GWebsocketConsumerT &&) = delete;
    GWebsocketConsumerT &operator=(const GWebsocketConsumerT &) = delete;
    GWebsocketConsumerT &operator=(GWebsocketConsumerT &&) = delete;

protected:
    /***************************************************************************/
    /** @brief The websocket consumer has a real client-liveness signal (its persistent session +
     *  keep-alive ping/pong), so it reclaims a lost item immediately on disconnect via the session's
     *  CheckoutLease and does NOT use the time lease -- a live-but-slow client keeps its item. */
    [[nodiscard]] bool usesTimeLease() const override { return false; }

private:
    /***************************************************************************/
    /** @brief Starts the threads the sessions run on. */
    void startTransport_() override {
        session_budget_ = Gem::Common::Concurrency::threadBudget().reserve(
            "Gem::Courtier::GWebsocketConsumerT:sessions",
            static_cast<unsigned int>(n_session_threads_),
            Gem::Common::Concurrency::ThreadElasticity::Elastic
        );
        for(std::size_t t = 0; t < n_session_threads_; ++t) {
            session_threads_.create_thread([this] { session_context_.run(); });
        }
    }

    /** @brief Stops the threads the sessions run on and joins them. */
    void stopTransport_() override {
        session_work_.reset();
        session_context_.stop();
        session_threads_.join_all();
        session_budget_.release();
    }

    /***************************************************************************/
    /** @brief Starts one persistent websocket session for a freshly accepted connection, with a
     *  CheckoutLease that requeues any in-flight items if the client dies.
     *  @param socket The freshly accepted TCP socket, moved into the new session */
    void start_session_(boost::asio::ip::tcp::socket socket) override {
        // The websocket session is persistent (the client keeps the connection open while it
        // evaluates), so a disconnect IS a real death signal. Give the session a CheckoutLease: if it
        // dies still holding items, the lease requeues all of them immediately for other clients --
        // liveness-driven put-back, no time lease needed (see usesTimeLease()). A prefetching client may
        // hold several items at once, so the lease tracks the whole in-flight set, not just the latest.
        auto lease = std::make_shared<typename GNetworkedConsumerT<processable_type>::CheckoutLease>();
        // The lease and the hooks below refer to this consumer through `this`. They live in a
        // session, and every session is gone before the networked base of this object is destroyed
        // (see the lifetime note in GTcpAcceptingConsumerT).
        lease->on_abandon = [this](Gem::Courtier::CORRELATION_ID_TYPE id) { this->requeue(id); };

        std::make_shared<session_type>(
            std::move(socket),
            typename session_type::SessionHooks{
                .get_payload_item =
                    [this, lease](Gem::Courtier::GWakeFn wake) {
                        auto result = this->checkoutOrPark(std::move(wake));
                        lease->add(
                            result.item
                        ); // no-op without an item; records its correlation id
                        return result;
                    },
                .put_payload_item =
                    [this, lease](Gem::Courtier::GCommandContainerT<processable_type> &&frame) {
                        // returned normally -> nothing for the lease to reclaim
                        lease->remove(frame.outcome().correlation_id);
                        this->checkin(std::move(frame));
                    },
                .abandonItems = [lease] { lease->abandonAll(); },
                .check_server_stopped = [this]() -> bool { return this->stopped(); },
                .server_sign_on = [this](bool sign_on) { this->adjustSessionCount(sign_on); },
            },
            serialization_mode_,
            ping_interval_,
            verbose_control_frames_,
            &this->wire_registry_,
            next_peer_id_.fetch_add(1) // a fresh peer id per session (connection)
        )
            ->async_start_run(session_context_.get_executor());
    }

    /***************************************************************************/
    std::size_t n_session_threads_; ///< The threads the sessions run on
    /// Where the sessions run: their handlers, and with them the encoding and decoding of
    /// messages, the check-out and the check-in. Declared after nothing it refers to; destroyed
    /// before the shell's io context (see the destructor).
    boost::asio::io_context session_context_;
    std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
        session_work_{boost::asio::make_work_guard(session_context_)};
    Gem::Common::Concurrency::GThreadGroup session_threads_;
    Gem::Common::Concurrency::GThreadBudget::Reservation session_budget_;

    Gem::Common::serializationMode serialization_mode_;
    std::size_t ping_interval_;
    bool verbose_control_frames_;

    /// Hands each session a distinct peer id for the blob send-once registry's per-peer ack tracking.
    std::atomic<Gem::Courtier::GWirePeerId> next_peer_id_{1};
};

/******************************************************************************/

} /* namespace Gem::Courtier */
