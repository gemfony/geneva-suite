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
#include "courtier/GCourtierEnums.hpp" // GASIOSESSIONTIMEOUT
#include "courtier/consumers/GAsioConsumerConfig.hpp"
#include "courtier/consumers/GTcpAcceptingConsumerT.hpp"
#include "courtier/transport/GAsioTransportT.hpp" // reuse the existing session + client + wire protocol

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
 * It is configured by a GAsioConsumerConfig, the struct its client is built from too.
 *
 * @tparam processable_type The work-item type served to clients and collected back from them.
 */
template <typename processable_type>
class GAsioConsumerT final : public GTcpAcceptingConsumerT<processable_type> {
public:
    using session_type = Gem::Courtier::Consumers::GAsioConsumerSessionT<processable_type>;
    /// The configuration this consumer (and its client) is built from
    using config_type = GAsioConsumerConfig;

    /***************************************************************************/
    /** @brief Initialization from the configuration of the asio family.
     *
     *  @param config Reads port (0 lets the OS pick an ephemeral port, learned in startServer()),
     *         nThreads (io threads; 0 means the hardware concurrency) and serializationMode (the
     *         wire format). The client members (ip, maxReconnects, clientPrefetchDepth) are
     *         ignored. */
    explicit GAsioConsumerT(const config_type &config)
      : GTcpAcceptingConsumerT<processable_type>(
            "Gem::Courtier::GAsioConsumerT",
            config.port,
            config.nThreads
        )
      , serialization_mode_(config.serializationMode) { /* nothing */
    }

    /** @brief Stops the server while this object is still complete (see the base class). */
    ~GAsioConsumerT() override {
        this->stopServerNoThrow_();
    }

    GAsioConsumerT(const GAsioConsumerT &) = delete;
    GAsioConsumerT(GAsioConsumerT &&) = delete;
    GAsioConsumerT &operator=(const GAsioConsumerT &) = delete;
    GAsioConsumerT &operator=(GAsioConsumerT &&) = delete;

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
        std::make_shared<session_type>(
            this->io_context_,
            std::move(socket),
            [this]() -> std::unique_ptr<processable_type> { return this->checkout(); },
            [this](Gem::Courtier::GCommandContainerT<processable_type> &&frame) {
                this->checkin(std::move(frame));
            },
            [this]() -> bool { return this->stopped(); },
            serialization_mode_,
            [this](bool sign_on) { this->adjustSessionCount(sign_on); },
            &this->wire_registry_, // blob send-once: the registry shared by all of this server's sessions
            session_timeout_       // per-exchange connection deadline (configurable; 0 disables it)
        )
            ->async_start_run();
    }

    /***************************************************************************/
    /// The per-exchange connection deadline handed to each new session (0 disables it); overwritten
    /// by applyTimeoutConfig().
    std::chrono::milliseconds session_timeout_{Gem::Courtier::GASIOSESSIONTIMEOUT};
    Gem::Common::serializationMode serialization_mode_;
};

/******************************************************************************/

} /* namespace Gem::Courtier */
