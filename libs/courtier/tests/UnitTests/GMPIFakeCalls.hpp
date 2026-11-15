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
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Geneva headers go here
#include "courtier/transport/GMPICallsI.hpp"

/******************************************************************************/
/**
 * @file
 * An in-memory stand-in for the MPI calls of the MPI transport (GMPICallsI), so that the poll and
 * the peers run in one process, under ThreadSanitizer and without mpirun. A GMPIFakeWorld is the
 * communicator: one mailbox per rank, and calls(rank) is that rank's view of it.
 *
 * What it does as MPI does: the messages of one sender reach a receiver in the order they were sent,
 * whatever their tags; a probe for any source and any tag picks among the senders that have
 * something waiting (in turn, so that no sender is favoured); a send's bytes are read when the
 * message is received, not when the send is started, and the send completes only then (as a large
 * message does under MPI's rendezvous protocol), so bytes that are freed or changed too early show.
 *
 * What it checks, and records as a violation (violations()): a probe or receive by a second caller
 * between a probe that found a message and the receive of it (two receivers on one communicator --
 * a probe would no longer mean the message that is then received); calls of the receiving kind that
 * overlap; a receive that is not of the message the last probe reported; a request that is not this
 * rank's or not under way.
 *
 * What a test can arrange: a rank that never receives (swallow(): sends to it never complete),
 * messages from a rank that has no calls of its own (inject()), and a call that throws once
 * (throwOnce()).
 */

namespace Gem::Courtier::Tests {

class GMPIFakeWorld;

/******************************************************************************/
/** @brief One rank's view of the fake communicator */
class GMPIFakeCalls final : public Gem::Courtier::Consumers::GMPICallsI {
public:
    /** @brief @param world The communicator @param rank This rank */
    GMPIFakeCalls(std::shared_ptr<GMPIFakeWorld> world, int rank)
      : world_(std::move(world))
      , rank_(rank) { /* nothing */
    }

    [[nodiscard]] int rank() const override {
        return rank_;
    }
    [[nodiscard]] int size() const override;
    [[nodiscard]] std::optional<Gem::Courtier::Consumers::GMPIProbed> probe() override;
    [[nodiscard]] std::string receive(const Gem::Courtier::Consumers::GMPIProbed &probed) override;
    [[nodiscard]] Gem::Courtier::Consumers::GMPIRequest
    startSend(int rank, int tag, std::string_view bytes) override;
    std::size_t testSome(std::span<Gem::Courtier::Consumers::GMPIRequest> requests) override;
    void giveUp(
        Gem::Courtier::Consumers::GMPIRequest request,
        std::unique_ptr<std::string> bytes
    ) override;

private:
    std::shared_ptr<GMPIFakeWorld> world_; ///< The communicator
    int rank_;                             ///< This rank
};

/******************************************************************************/
/** @brief The fake communicator: mailboxes, requests, counts and the checks */
class GMPIFakeWorld final : public std::enable_shared_from_this<GMPIFakeWorld> {
    friend class GMPIFakeCalls;

public:
    /** @brief What happened on one rank so far */
    struct GCounts {
        std::uint64_t probes = 0;   ///< Calls of probe()
        std::uint64_t receives = 0; ///< Messages received
        std::uint64_t sends = 0;    ///< Sends started
        std::uint64_t givenUp = 0;  ///< Sends given up
    };

    /** @brief @param n_ranks The size of the communicator */
    explicit GMPIFakeWorld(int n_ranks)
      : ranks_(static_cast<std::size_t>(n_ranks)) { /* nothing */
    }

    /** @brief @param rank A rank @return Its view of the communicator */
    std::shared_ptr<GMPIFakeCalls> calls(int rank) {
        return std::make_shared<GMPIFakeCalls>(this->shared_from_this(), rank);
    }

    /** @brief From now on, @p rank receives nothing: sends to it never complete */
    void swallow(int rank) {
        const std::scoped_lock lock(mutex_);
        ranks_.at(static_cast<std::size_t>(rank)).swallowing = true;
    }

    /** @brief Puts a message from @p source into the mailbox of @p destination, as if @p source
     *  had sent it (no request; the fake keeps the bytes) */
    void inject(int source, int destination, int tag, std::string bytes) {
        const std::scoped_lock lock(mutex_);
        auto owned = std::make_shared<const std::string>(std::move(bytes));
        const std::string_view view{*owned};
        ranks_.at(static_cast<std::size_t>(destination))
            .mailbox[source]
            .push_back(GMessage{.tag = tag, .view = view, .owned = std::move(owned), .request = 0});
    }

    /** @brief Where an armed fault strikes */
    enum class GFault {
        PROBE,    ///< The next probe() throws (before it looks)
        RECEIVE,  ///< The next receive() throws; the message stays in the mailbox
        TEST_SOME ///< The next testSome() throws (before it looks)
    };

    /** @brief The next call of the kind @p fault on @p rank throws once: a std::runtime_error, or
     *  -- with @p standard false -- an int, which is not a std::exception */
    void throwOnce(int rank, GFault fault, bool standard = true) {
        const std::scoped_lock lock(mutex_);
        ranks_.at(static_cast<std::size_t>(rank)).faults.push_back(GArmedFault{fault, standard});
    }

    /** @brief @param rank A rank @return What happened on it so far */
    GCounts counts(int rank) const {
        const std::scoped_lock lock(mutex_);
        return ranks_.at(static_cast<std::size_t>(rank)).counts;
    }

    /** @brief @param rank A rank @return How many messages wait in its mailbox */
    std::size_t waiting(int rank) const {
        const std::scoped_lock lock(mutex_);
        std::size_t n = 0;
        for(const auto &[source, queue] : ranks_.at(static_cast<std::size_t>(rank)).mailbox) {
            n += queue.size();
        }
        return n;
    }

    /** @brief @return How many sends are under way (started, neither completed nor given up) */
    std::size_t sendsUnderWay() const {
        const std::scoped_lock lock(mutex_);
        return requests_.size();
    }

    /** @brief @return Every violation of the contract of GMPICallsI seen so far */
    std::vector<std::string> violations() const {
        const std::scoped_lock lock(mutex_);
        return violations_;
    }

private:
    /** @brief A message in a mailbox */
    struct GMessage {
        int tag = 0;
        std::string_view view;                    ///< The sender's bytes, read when received
        std::shared_ptr<const std::string> owned; ///< The bytes of an injected message
        std::uint64_t request = 0;                ///< The sender's request; 0 for an injected one
    };

    /** @brief A send under way */
    struct GRequest {
        int owner = -1;       ///< The sending rank
        int destination = -1; ///< The receiving rank
        bool done = false;    ///< Received at the other end
    };

    /** @brief A fault that has been armed and has not struck yet */
    struct GArmedFault {
        GFault where;
        bool standard;
    };

    /** @brief One rank */
    struct GRank {
        std::vector<GArmedFault> faults;             ///< Armed by throwOnce()
        std::map<int, std::deque<GMessage>> mailbox; ///< By source, in the order they were sent
        int next_source = 0;                         ///< Where the next probe starts to look
        bool swallowing = false;                     ///< Sends to this rank never complete
        std::optional<Gem::Courtier::Consumers::GMPIProbed> probed; ///< Reported, not yet received
        std::thread::id prober;      ///< Who the probe reported it to
        std::atomic<int> in_call{0}; ///< Calls of the receiving kind in progress
        GCounts counts;
    };

    /** @brief Throws, if a fault of the kind @p where is armed on @p me; the mutex is held */
    static void strike_(GRank &me, GFault where) {
        const auto armed = std::ranges::find(me.faults, where, &GArmedFault::where);
        if(armed == me.faults.end()) {
            return;
        }
        const bool standard = armed->standard;
        me.faults.erase(armed);
        if(standard) {
            throw std::runtime_error("GMPIFakeCalls: a fault armed by the test");
        }
        throw 42; // NOLINT(hicpp-exception-baseclass,cert-err60-cpp) -- not a std::exception, on purpose
    }

    /** @brief Records a violation; the mutex is held */
    void violation_(std::string what) {
        violations_.push_back(std::move(what));
    }

    /** @brief Counts a call of the receiving kind on @p rank in and out, and records an overlap */
    class GCallGuard {
    public:
        GCallGuard(GMPIFakeWorld &world, int rank)
          : world_(world)
          , rank_(world.ranks_.at(static_cast<std::size_t>(rank))) {
            if(rank_.in_call.fetch_add(1) != 0) {
                const std::scoped_lock lock(world_.mutex_);
                world_.violation_(
                    "two calls of the receiving kind at once on rank " + std::to_string(rank)
                );
            }
        }
        ~GCallGuard() {
            rank_.in_call.fetch_sub(1);
        }
        GCallGuard(const GCallGuard &) = delete;
        GCallGuard(GCallGuard &&) = delete;
        GCallGuard &operator=(const GCallGuard &) = delete;
        GCallGuard &operator=(GCallGuard &&) = delete;

    private:
        GMPIFakeWorld &world_;
        GRank &rank_;
    };

    mutable std::mutex mutex_;                   ///< Guards everything below
    std::vector<GRank> ranks_;                   ///< By rank
    std::map<std::uint64_t, GRequest> requests_; ///< Sends under way, by id
    std::uint64_t next_request_ = 1;             ///< The id of the next send
    std::vector<std::string> violations_;        ///< What went against the contract
};

/******************************************************************************/
// GMPIFakeCalls

inline int GMPIFakeCalls::size() const {
    return static_cast<int>(world_->ranks_.size());
}

inline std::optional<Gem::Courtier::Consumers::GMPIProbed> GMPIFakeCalls::probe() {
    const GMPIFakeWorld::GCallGuard guard(*world_, rank_);
    const std::scoped_lock lock(world_->mutex_);
    auto &me = world_->ranks_.at(static_cast<std::size_t>(rank_));
    ++me.counts.probes;
    GMPIFakeWorld::strike_(me, GMPIFakeWorld::GFault::PROBE);
    if(me.probed && me.prober != std::this_thread::get_id()) {
        world_->violation_("a second receiver probed on rank " + std::to_string(rank_));
    }
    me.probed.reset();
    if(me.mailbox.empty()) {
        return std::nullopt;
    }
    // In turn: the first source at or after next_source that has something waiting.
    auto it = me.mailbox.lower_bound(me.next_source);
    if(it == me.mailbox.end()) {
        it = me.mailbox.begin();
    }
    me.next_source = it->first + 1;
    const auto &message = it->second.front();
    me.probed = Gem::Courtier::Consumers::GMPIProbed{
        .source = it->first,
        .tag = message.tag,
        .size = message.view.size()
    };
    me.prober = std::this_thread::get_id();
    return me.probed;
}

inline std::string GMPIFakeCalls::receive(const Gem::Courtier::Consumers::GMPIProbed &probed) {
    const GMPIFakeWorld::GCallGuard guard(*world_, rank_);
    const std::scoped_lock lock(world_->mutex_);
    auto &me = world_->ranks_.at(static_cast<std::size_t>(rank_));
    if(not me.probed || me.prober != std::this_thread::get_id() ||
       me.probed->source != probed.source || me.probed->tag != probed.tag ||
       me.probed->size != probed.size) {
        world_->violation_(
            "a receive on rank " + std::to_string(rank_) +
            " that is not of the message its probe reported"
        );
        throw std::logic_error("GMPIFakeCalls::receive(): no matching probe");
    }
    me.probed.reset();
    GMPIFakeWorld::strike_(me, GMPIFakeWorld::GFault::RECEIVE);
    auto queue = me.mailbox.find(probed.source);
    if(queue == me.mailbox.end()) { // the sender gave the message up between the probe and now
        world_->violation_(
            "the message probed on rank " + std::to_string(rank_) + " was taken back"
        );
        throw std::logic_error("GMPIFakeCalls::receive(): the message is gone");
    }
    GMPIFakeWorld::GMessage message = std::move(queue->second.front());
    queue->second.pop_front();
    if(queue->second.empty()) {
        me.mailbox.erase(queue);
    }
    ++me.counts.receives;
    if(message.request != 0) {
        if(auto request = world_->requests_.find(message.request);
           request != world_->requests_.end()) {
            request->second.done = true;
        }
    }
    return std::string{message.view};
}

inline Gem::Courtier::Consumers::GMPIRequest
GMPIFakeCalls::startSend(int rank, int tag, std::string_view bytes) {
    const std::scoped_lock lock(world_->mutex_);
    const std::uint64_t id = world_->next_request_++;
    world_->requests_[id] =
        GMPIFakeWorld::GRequest{.owner = rank_, .destination = rank, .done = false};
    ++world_->ranks_.at(static_cast<std::size_t>(rank_)).counts.sends;
    auto &destination = world_->ranks_.at(static_cast<std::size_t>(rank));
    if(not destination.swallowing) {
        destination.mailbox[rank_].push_back(
            GMPIFakeWorld::GMessage{.tag = tag, .view = bytes, .owned = nullptr, .request = id}
        );
    }
    return static_cast<Gem::Courtier::Consumers::GMPIRequest>(id);
}

inline std::size_t
GMPIFakeCalls::testSome(std::span<Gem::Courtier::Consumers::GMPIRequest> requests) {
    const GMPIFakeWorld::GCallGuard guard(*world_, rank_);
    const std::scoped_lock lock(world_->mutex_);
    GMPIFakeWorld::strike_(
        world_->ranks_.at(static_cast<std::size_t>(rank_)),
        GMPIFakeWorld::GFault::TEST_SOME
    );
    std::size_t n_completed = 0;
    for(auto &handle : requests) {
        if(handle == Gem::Courtier::Consumers::GMPIRequest::NONE) {
            continue;
        }
        const auto request = world_->requests_.find(static_cast<std::uint64_t>(handle));
        if(request == world_->requests_.end() || request->second.owner != rank_) {
            world_->violation_(
                "rank " + std::to_string(rank_) + " tested a request that is not its own"
            );
            continue;
        }
        if(request->second.done) {
            world_->requests_.erase(request);
            handle = Gem::Courtier::Consumers::GMPIRequest::NONE;
            ++n_completed;
        }
    }
    return n_completed;
}

inline void GMPIFakeCalls::giveUp(
    Gem::Courtier::Consumers::GMPIRequest handle,
    std::unique_ptr<std::string> bytes
) {
    const GMPIFakeWorld::GCallGuard guard(*world_, rank_);
    const std::scoped_lock lock(world_->mutex_);
    const auto id = static_cast<std::uint64_t>(handle);
    const auto request = world_->requests_.find(id);
    if(request == world_->requests_.end() || request->second.owner != rank_ || not bytes) {
        world_->violation_(
            "rank " + std::to_string(rank_) + " gave up a request that is not its own"
        );
        return;
    }
    // The cancel succeeds: a message that has not been received leaves the mailbox before its bytes
    // go.
    auto &destination = world_->ranks_.at(static_cast<std::size_t>(request->second.destination));
    if(auto queue = destination.mailbox.find(rank_); queue != destination.mailbox.end()) {
        std::erase_if(queue->second, [id](const GMPIFakeWorld::GMessage &m) {
            return m.request == id;
        });
        if(queue->second.empty()) {
            destination.mailbox.erase(queue);
        }
    }
    world_->requests_.erase(request);
    ++world_->ranks_.at(static_cast<std::size_t>(rank_)).counts.givenUp;
}

/******************************************************************************/

} /* namespace Gem::Courtier::Tests */
