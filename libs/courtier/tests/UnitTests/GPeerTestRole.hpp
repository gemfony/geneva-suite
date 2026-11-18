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
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Geneva headers go here
#include "GInProcessTestSupport.hpp" // endName(), closeName()
#include "courtier/transport/GPeerI.hpp"

/******************************************************************************/
/**
 * @file
 * A role for the tests of the rules of GPeerI.hpp, on any transport: it records what its peer tells
 * it and whether a handler ran inside one of its calls of GPeerI (rule 3).
 */

namespace Gem::Courtier::Tests {

/******************************************************************************/
/** @brief @return How deep this thread is inside calls of GPeerI made by a role (rule 3) */
inline int &inPeerCall() {
    thread_local int depth = 0;
    return depth;
}

/** @brief Marks the span of a call of GPeerI, so that a handler run inside it is seen (rule 3) */
struct InPeerCall {
    InPeerCall() {
        ++inPeerCall();
    }
    ~InPeerCall() {
        --inPeerCall();
    }
    InPeerCall(const InPeerCall &) = delete;
    InPeerCall(InPeerCall &&) = delete;
    InPeerCall &operator=(const InPeerCall &) = delete;
    InPeerCall &operator=(InPeerCall &&) = delete;
};

/******************************************************************************/
/**
 * A role for the tests: records what its peer tells it ("open", "msg:<text>",
 * "closed:<end>:<how>") and whether a handler ran inside one of its calls of GPeerI. What it does
 * when it is opened or gets a message is up to the test. Like the real roles it is owned by its
 * peer's handlers and refers to the peer weakly.
 */
class Role : public std::enable_shared_from_this<Role> {
public:
    std::function<void(Role &)> whenOpen;                         ///< Done on onOpen
    std::function<void(Role &, const std::string &)> whenMessage; ///< Done on onMessage

    /** @brief @return Handlers that report to this role (and own it) */
    Gem::Courtier::Consumers::GPeerHandlers handlers() {
        using Gem::Courtier::Consumers::GPeerClose;
        using Gem::Courtier::Consumers::GPeerEnd;
        auto self = this->shared_from_this();
        return Gem::Courtier::Consumers::GPeerHandlers{
            .onOpen =
                [self] {
                    self->note_("open");
                    if(self->whenOpen) {
                        self->whenOpen(*self);
                    }
                },
            .onMessage =
                [self](std::string &&message) {
                    const std::string text{std::move(message)};
                    self->note_("msg:" + text);
                    if(self->whenMessage) {
                        self->whenMessage(*self, text);
                    }
                },
            .onClosed =
                [self](GPeerEnd end, GPeerClose how, std::string_view) {
                    self->note_("closed:" + endName(end) + ":" + closeName(how));
                    self->n_closed_.fetch_add(1);
                },
        };
    }

    /** @brief Starts @p peer for this role, which then holds it weakly @param peer The peer */
    void start(const std::shared_ptr<Gem::Courtier::Consumers::GPeerI> &peer) {
        peer_ = peer;
        const InPeerCall call;
        peer->start(this->handlers());
    }

    /** @brief Sends through the peer (on its executor) @param message What is sent */
    void send(std::string message) {
        if(auto peer = peer_.lock()) {
            const InPeerCall call;
            peer->send(Gem::Courtier::Consumers::makeMessage(std::move(message)));
        }
    }

    /** @brief Closes the peer (any thread) @param how What the other end is told */
    void close(Gem::Courtier::Consumers::GPeerClose how) {
        if(auto peer = peer_.lock()) {
            const InPeerCall call;
            peer->close(how);
        }
    }

    /** @brief @return A copy of what has been recorded */
    std::vector<std::string> events() const {
        const std::scoped_lock lock(mutex_);
        return events_;
    }

    /** @brief @return How many messages have been recorded */
    std::size_t nMessages() const {
        const std::scoped_lock lock(mutex_);
        std::size_t n = 0;
        for(const auto &event : events_) {
            n += event.starts_with("msg:") ? 1 : 0;
        }
        return n;
    }

    /** @brief @return How often onClosed was called */
    int nClosed() const {
        return n_closed_.load();
    }
    /** @brief @return How often a handler ran inside a call of GPeerI made by this role */
    int nNested() const {
        return n_nested_.load();
    }

private:
    /** @brief Records one event @param event What happened */
    void note_(std::string event) {
        if(inPeerCall() > 0) {
            n_nested_.fetch_add(1);
        }
        const std::scoped_lock lock(mutex_);
        events_.push_back(std::move(event));
    }

    mutable std::mutex mutex_;        ///< Guards events_ (the test reads them on its own thread)
    std::vector<std::string> events_; ///< What happened, in order
    std::atomic<int> n_closed_{0};    ///< onClosed calls
    std::atomic<int> n_nested_{0};    ///< Handlers run inside a call of GPeerI
    std::weak_ptr<Gem::Courtier::Consumers::GPeerI> peer_; ///< The peer, weakly (rule 4)
};

/** @brief @return A role, owned by the caller until its peer takes it over */
inline std::shared_ptr<Role> makeRole() {
    return std::make_shared<Role>();
}

/******************************************************************************/

} /* namespace Gem::Courtier::Tests */
