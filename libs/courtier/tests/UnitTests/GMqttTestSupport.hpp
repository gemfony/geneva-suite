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

// Standard headers go here
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/strand.hpp>

// Geneva headers go here
#include "common/concurrency/GCompletionLatchT.hpp"
#include "courtier/transport/GIoContext.hpp"
#include "courtier/transport/GMqttPeerTransport.hpp"

/******************************************************************************/
/**
 * @file
 * What the tests of the MQTT transport share: the port of the tests' own broker (started by the
 * CTest fixture of tests/mqttTestBroker.sh, never the host's broker), a topic prefix of each
 * test's own, a listener whose ends are collected, and a connection made through listener and
 * connector.
 */

namespace Gem::Courtier::Tests {

/******************************************************************************/
/** @brief @return The port of the tests' broker, as the fixture wrote it
 *  @throws std::runtime_error if there is none */
inline unsigned short mqttBrokerPort() {
    std::ifstream file(std::string(GCOURTIER_MQTT_BROKER_DIR) + "/port");
    unsigned int port = 0;
    if(not(file >> port) || port == 0 || port > 65535) {
        throw std::runtime_error(
            "no test broker: " GCOURTIER_MQTT_BROKER_DIR "/port names no port"
        );
    }
    return static_cast<unsigned short>(port);
}

/** @brief @param keep_alive The MQTT keep-alive @return Options for the tests' broker, with a topic
 *  prefix no other test uses */
inline Gem::Courtier::Consumers::GMqttOptions
mqttTestOptions(std::chrono::seconds keep_alive = std::chrono::seconds(15)) {
    static std::mt19937_64 engine{std::random_device{}()};
    static std::mutex mutex;
    std::uint64_t salt = 0;
    {
        const std::scoped_lock lock(mutex);
        salt = engine();
    }
    return Gem::Courtier::Consumers::GMqttOptions{
        .name = "MQTT test",
        .host = "127.0.0.1",
        .port = mqttBrokerPort(),
        .topic = "courtier-test-" + std::to_string(salt),
        .keepAlive = keep_alive
    };
}

/******************************************************************************/
/**
 * An io context with threads that can be joined before the context goes (the order a shell keeps:
 * join the threads, end the listener's work, then destroy the context). join() lets the threads run
 * out of work first, at most five seconds: an MQTT client's operations must complete, not be
 * destroyed with the context (see GMqttListener::endWork()).
 */
class JoinableIo {
public:
    /** @brief @param n_threads How many threads run the context */
    explicit JoinableIo(std::size_t n_threads = 4)
      : idle_(std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(n_threads)) {
        for(std::size_t i = 0; i < n_threads; ++i) {
            threads_.emplace_back([this, idle = idle_] {
                ctx_.run();
                idle->count_down();
            });
        }
    }
    /** @brief Joins the threads if join() has not */
    ~JoinableIo() {
        this->join();
    }
    JoinableIo(const JoinableIo &) = delete;
    JoinableIo(JoinableIo &&) = delete;
    JoinableIo &operator=(const JoinableIo &) = delete;
    JoinableIo &operator=(JoinableIo &&) = delete;

    /** @brief @return The executor of the context */
    boost::asio::any_io_executor executor() {
        return ctx_.get_executor();
    }
    /** @brief Lets the threads run out of work (at most five seconds), then stops the context and
     *  joins them; the context stays */
    void join() {
        guard_.reset();
        std::ignore = idle_->wait_for(std::chrono::seconds(5));
        ctx_.stop();
        threads_.clear(); // joins
    }

private:
    Gem::Courtier::Consumers::GIoContext ctx_; ///< The context the threads run
    std::optional<
        boost::asio::executor_work_guard<Gem::Courtier::Consumers::GIoContext::executor_type>>
        guard_{ctx_.get_executor()}; ///< Keeps the threads in run() until join()
    /// Counted down by every thread that has left run()
    std::shared_ptr<Gem::Common::Concurrency::GCompletionLatchT> idle_;
    std::vector<std::jthread> threads_; ///< The threads
};

/******************************************************************************/
/**
 * A started listener whose ends are collected, with the threads its ends' handlers run on. When it
 * goes it keeps the order of a shell (GPeerI.hpp, rule 7): the listener stops, the handlers'
 * threads are joined, the listener's work ends, the handlers' context goes, the listener last.
 */
class MqttServer {
public:
    using peer_ptr = std::shared_ptr<Gem::Courtier::Consumers::GPeerI>;

    /** @brief Starts the listener @param options Broker, prefix, keep-alive */
    explicit MqttServer(const Gem::Courtier::Consumers::GMqttOptions &options)
      : listener_(
            std::make_unique<Gem::Courtier::Consumers::GMqttListener>("MQTT test listener", options)
        ) {
        listener_->start(handlers_.executor(), [this](peer_ptr peer, Gem::Courtier::GWirePeerId) {
            const std::scoped_lock lock(mutex_);
            peers_.push_back(std::move(peer));
        });
    }
    /** @brief Stops the listener, lets go of the ends, joins the handlers' threads and ends the
     *  listener's work */
    ~MqttServer() {
        listener_->stop();
        {
            const std::scoped_lock lock(mutex_);
            peers_.clear(); // an end nobody started holds the handlers' context until it goes
        }
        handlers_.join();
        listener_->endWork();
    }
    MqttServer(const MqttServer &) = delete;
    MqttServer(MqttServer &&) = delete;
    MqttServer &operator=(const MqttServer &) = delete;
    MqttServer &operator=(MqttServer &&) = delete;

    /** @brief @return How many ends the listener has handed out */
    [[nodiscard]] std::size_t nPeers() const {
        const std::scoped_lock lock(mutex_);
        return peers_.size();
    }
    /** @brief @param i Which @return The i-th end handed out */
    [[nodiscard]] peer_ptr peer(std::size_t i) const {
        const std::scoped_lock lock(mutex_);
        return peers_.at(i);
    }
    /** @brief @return The executor of the ends' handlers */
    boost::asio::any_io_executor handlerExecutor() {
        return handlers_.executor();
    }

private:
    std::unique_ptr<Gem::Courtier::Consumers::GMqttListener> listener_; ///< Goes last
    JoinableIo handlers_;         ///< Where the ends' handlers run
    mutable std::mutex mutex_;    ///< Guards peers_
    std::vector<peer_ptr> peers_; ///< The ends handed out, in order
};

/** @brief Asks @p connector for a connection on @p executor and waits for it, at most ten seconds
 *  @return The client's end, or none and the reason */
inline std::pair<std::shared_ptr<Gem::Courtier::Consumers::GPeerI>, std::string> mqttConnect(
    Gem::Courtier::Consumers::GMqttConnector &connector,
    const boost::asio::any_io_executor &executor
) {
    using result_type = std::pair<std::shared_ptr<Gem::Courtier::Consumers::GPeerI>, std::string>;
    auto promise = std::make_shared<std::promise<result_type>>();
    auto future = promise->get_future();
    connector.connect(
        executor,
        [promise](std::shared_ptr<Gem::Courtier::Consumers::GPeerI> peer, std::string_view error) {
            promise->set_value({std::move(peer), std::string(error)});
        }
    );
    if(future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        return {nullptr, "no answer of the connector within 10 s"};
    }
    return future.get();
}

/******************************************************************************/

} /* namespace Gem::Courtier::Tests */
