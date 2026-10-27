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
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

// Boost headers
#include <boost/asio.hpp>

// Geneva headers
#include "courtier/transport/GAsioTransportT.hpp" // reuse the existing session + client + wire protocol
#include "courtier/consumers/GTcpAcceptingConsumerT.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The courtier ASIO consumer. It is a TCP server that hands out work items from the current batch
 * to connected clients and collects the processed results, reconciling them against the submission
 * policy via the inherited GTcpAcceptingConsumerT / GNetworkedConsumerT / GBaseConsumerT machinery.
 * The whole server lifecycle (accept loop, io threads, shutdown) lives on the shared
 * GTcpAcceptingConsumerT shell; this class contributes only the ASIO session construction and the
 * ASIO-specific knobs.
 *
 * It deliberately reuses the existing courtier session (Gem::Courtier::Consumers::
 * GAsioConsumerSessionT) and its GCommandContainerT wire protocol, so an unmodified
 * Gem::Courtier::Consumers::GAsioConsumerClientT can serve a courtier server (the rework is
 * behaviour-neutral at the protocol level).
 *
 * @tparam processable_type The work-item type served to clients and collected back from them.
 */
template <typename processable_type>
class GAsioConsumerT final : public GTcpAcceptingConsumerT<processable_type> {
public:
    using session_type = Gem::Courtier::Consumers::GAsioConsumerSessionT<processable_type>;

    /***************************************************************************/
    /** @brief Initialization with the listen port, the number of io threads and the serialization
     *  mode used on the wire.
     *
     *  @param port The TCP port to listen on (0 lets the OS pick an ephemeral port, learned in startServer())
     *  @param n_threads Number of io threads to run; 0 means use the hardware concurrency
     *  @param serialization_mode The serialization format used on the wire (defaults to binary) */
    explicit GAsioConsumerT(
        unsigned short port,
        std::size_t n_threads = 0,
        Gem::Common::serializationMode serialization_mode = Gem::Common::serializationMode::GEM_BINARY
    )
        : GTcpAcceptingConsumerT<processable_type>("Gem::Courtier::GAsioConsumerT", port, n_threads)
        , serialization_mode_(serialization_mode)
    { /* nothing */ }

    /***************************************************************************/
    /** @brief Applies the timeout configuration, additionally taking the ASIO-only per-exchange session
     *  deadline (session_timeout_ms; a non-positive value disables it) on top of the base treatment/knobs.
     *  The deadline is captured for sessions created after this call (i.e. before the server starts accepting).
     *  @param cfg The parsed timeout configuration to apply */
    void applyTimeoutConfig(const GNetworkedTimeoutConfig &cfg) override {
        GNetworkedConsumerT<processable_type>::applyTimeoutConfig(cfg);
        session_timeout_ = std::chrono::milliseconds(cfg.session_timeout_ms);
    }

private:
    /***************************************************************************/
    /** @brief Starts one ASIO one-shot session for a freshly accepted connection.
     *  @param socket The freshly accepted client socket (ownership taken by move) */
    void start_session_(boost::asio::ip::tcp::socket socket) override {
        auto self = this->shared_from_this();
        std::make_shared<session_type>(
            this->io_context_,
            std::move(socket),
            [self, this]() -> std::unique_ptr<processable_type> { return this->checkout(); },
            [self, this](Gem::Courtier::GCommandContainerT<processable_type> &&frame) {
                this->checkin(std::move(frame));
            },
            [self, this]() -> bool { return this->stopped(); },
            serialization_mode_,
            [self, this](bool sign_on) { this->adjustSessionCount(sign_on); },
            &this->wire_registry_, // blob send-once: the registry shared by all of this server's sessions
            session_timeout_ // per-exchange connection deadline (configurable; 0 disables it)
        )
            ->async_start_run();
    }

    /***************************************************************************/
    /// The per-exchange connection deadline handed to each new session (0 disables it). Defaulted to the
    /// same 300s the session used before it became configurable; overwritten by applyTimeoutConfig().
    std::chrono::milliseconds session_timeout_{300'000};
    Gem::Common::serializationMode serialization_mode_;
};

/******************************************************************************/

} /* namespace Gem::Courtier */
