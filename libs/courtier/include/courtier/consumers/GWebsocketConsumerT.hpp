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
#include <cstddef>
#include <memory>
#include <utility>

// Boost headers
#include <boost/asio.hpp>

// Geneva headers
#include "courtier/transport/GWebsocketTransportT.hpp" // reuse the existing session + client + protocol
#include "courtier/consumers/GTcpAcceptingConsumerT.hpp"

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
 * @tparam processable_type The work-item type handed to clients and reconciled back into the population
 */
template <typename processable_type>
class GWebsocketConsumerT final : public GTcpAcceptingConsumerT<processable_type> {
public:
    using session_type = Gem::Courtier::Consumers::GWebsocketConsumerSessionT<processable_type>;

    /***************************************************************************/
    /** @brief Constructs the websocket consumer.
     *
     *  @param port The TCP port the server listens on (0 lets the OS pick; the chosen port is read back)
     *  @param n_threads Number of io threads to run; 0 selects the hardware concurrency
     *  @param serialization_mode Which serialization format (binary/XML/text) the wire protocol uses
     *  @param ping_interval Time in seconds between keep-alive pings each session sends to its client
     *  @param verbose_control_frames If true, sessions log a diagnostic message for every control frame */
    explicit GWebsocketConsumerT(
        unsigned short port,
        std::size_t n_threads = 0,
        Gem::Common::serializationMode serialization_mode = Gem::Common::serializationMode::GEM_BINARY,
        std::size_t ping_interval = 5,
        bool verbose_control_frames = false
    )
        : GTcpAcceptingConsumerT<processable_type>("Gem::Courtier::GWebsocketConsumerT", port, n_threads)
        , serialization_mode_(serialization_mode)
        , ping_interval_(ping_interval)
        , verbose_control_frames_(verbose_control_frames)
    { /* nothing */ }

protected:
    /***************************************************************************/
    /** @brief The websocket consumer has a real client-liveness signal (its persistent session +
     *  keep-alive ping/pong), so it reclaims a lost item immediately on disconnect via the session's
     *  CheckoutLease and does NOT use the time lease -- a live-but-slow client keeps its item. */
    [[nodiscard]] bool usesTimeLease() const override { return false; }

private:
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
        // The weak pointer is kept at the CONCRETE type: requeue() is protected on the networked
        // base, so it is only accessible through a pointer of this class's own type.
        std::weak_ptr<GWebsocketConsumerT<processable_type>> const w =
            std::static_pointer_cast<GWebsocketConsumerT<processable_type>>(this->shared_from_this());
        lease->on_abandon = [w](Gem::Courtier::CORRELATION_ID_TYPE id) {
            if(auto s = w.lock()) {
                s->requeue(id);
            }
        };

        auto self = this->shared_from_this();
        std::make_shared<session_type>(
            this->io_context_,
            std::move(socket),
            typename session_type::SessionHooks{
                .get_payload_item =
                    [self, this, lease]() -> std::unique_ptr<processable_type> {
                        auto p = this->checkout();
                        lease->add(p); // no-op for a null item; records p's correlation id
                        return p;
                    },
                .put_payload_item =
                    [self, this, lease](Gem::Courtier::GCommandContainerT<processable_type> &&frame) {
                        // returned normally -> nothing for the lease to reclaim
                        lease->remove(frame.outcome().correlation_id);
                        this->checkin(std::move(frame));
                    },
                .check_server_stopped = [self, this]() -> bool { return this->stopped(); },
                .server_sign_on = [self, this](bool sign_on) { this->adjustSessionCount(sign_on); },
            },
            serialization_mode_,
            ping_interval_,
            verbose_control_frames_,
            &this->wire_registry_,
            next_peer_id_.fetch_add(1) // a fresh peer id per session (connection)
        )
            ->async_start_run();
    }

    /***************************************************************************/
    Gem::Common::serializationMode serialization_mode_;
    std::size_t ping_interval_;
    bool verbose_control_frames_;

    /// Hands each session a distinct peer id for the blob send-once registry's per-peer ack tracking.
    std::atomic<Gem::Courtier::GWirePeerId> next_peer_id_{1};
};

/******************************************************************************/

} /* namespace Gem::Courtier */
