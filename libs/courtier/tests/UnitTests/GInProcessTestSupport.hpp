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
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

// Geneva headers go here
#include "courtier/transport/GPeerI.hpp"

/******************************************************************************/
/**
 * @file
 * What the tests of the roles over the in-process transport (GInProcessPeer.hpp) share: an io
 * context with threads of its own, a bounded wait for a condition, and the names of the ends and
 * closes for messages.
 */

namespace Gem::Courtier::Tests {

/******************************************************************************/
/** @brief An io context with a few threads; stops and joins them when it goes */
class IoThreads {
public:
    /** @brief @param n_threads How many threads run the context */
    explicit IoThreads(std::size_t n_threads = 4) {
        for(std::size_t i = 0; i < n_threads; ++i) {
            threads_.emplace_back([this] { ctx_.run(); });
        }
    }
    /** @brief Stops the context and joins its threads */
    ~IoThreads() {
        guard_.reset();
        ctx_.stop();
        threads_.clear(); // joins
    }
    IoThreads(const IoThreads &) = delete;
    IoThreads(IoThreads &&) = delete;
    IoThreads &operator=(const IoThreads &) = delete;
    IoThreads &operator=(IoThreads &&) = delete;

    /** @brief @return The executor of the context */
    boost::asio::any_io_executor executor() {
        return ctx_.get_executor();
    }

private:
    boost::asio::io_context ctx_; ///< The context the threads run
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_ =
        boost::asio::make_work_guard(ctx_); ///< Keeps the threads in run() until the end
    std::vector<std::jthread> threads_;     ///< The threads
};

/******************************************************************************/
/** @brief Polls @p condition until it holds or @p patience has passed
 *  @param condition What is waited for @param patience How long at most
 *  @return Whether the condition held in time */
template <typename Condition>
bool eventually(Condition condition, std::chrono::milliseconds patience) {
    const auto give_up_at = std::chrono::steady_clock::now() + patience;
    while(std::chrono::steady_clock::now() < give_up_at) {
        if(condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}

/******************************************************************************/
/** @brief @param end Why a connection ended @return Its name */
inline std::string endName(Gem::Courtier::Consumers::GPeerEnd end) {
    using Gem::Courtier::Consumers::GPeerEnd;
    switch(end) {
    case GPeerEnd::CLOSED_HERE:
        return "CLOSED_HERE";
    case GPeerEnd::CLOSED_THERE:
        return "CLOSED_THERE";
    case GPeerEnd::SILENT:
        return "SILENT";
    case GPeerEnd::FAILED:
        return "FAILED";
    }
    return "?";
}

/** @brief @param how What a close said @return Its name */
inline std::string closeName(Gem::Courtier::Consumers::GPeerClose how) {
    using Gem::Courtier::Consumers::GPeerClose;
    switch(how) {
    case GPeerClose::NORMAL:
        return "NORMAL";
    case GPeerClose::ERROR:
        return "ERROR";
    case GPeerClose::RUN_ENDED:
        return "RUN_ENDED";
    }
    return "?";
}

/******************************************************************************/

} /* namespace Gem::Courtier::Tests */
