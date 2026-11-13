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

/**
 * Fan-in (multi-submitter) tests for the courtier consumers. Several optimization algorithms can
 * submit to the SAME consumer concurrently -- the use case being a meta-optimization over a
 * population of inner algorithms, each submitting its individuals in parallel to one shared client
 * pool. Each submitter's processBatch() must complete, and -- crucially -- every result must return
 * to EXACTLY its own batch's slot (no cross-batch corruption), which the (batch_id, slot) correlation
 * token guarantees.
 *
 * The networked consumer is exercised deterministically through a SimNetConsumer (no real socket): N
 * driver threads play the shared client pool (checkout / process / checkin) against M concurrent
 * processBatch() submitters. The local thread-pool consumer is exercised directly (concurrent
 * processBatch() onto one shared pool).
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <ranges>
#include <span>
#include <thread>
#include <vector>

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GNetworkedConsumerT.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
namespace c2 = Gem::Courtier;
using namespace std::chrono_literals;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;

// A unique, decodable stored number per (batch, slot), so we can verify each result came home to its
// own batch's slot and was not mis-routed to another concurrent batch.
constexpr std::size_t kBatchStride = 100'000;
std::size_t tag(std::size_t batch, std::size_t slot) { return batch * kBatchStride + slot; }

std::vector<item_ptr> make_tagged_batch(std::size_t batch, std::size_t n,
                                        const std::vector<std::size_t> &faulty = {}) {
    std::vector<item_ptr> v;
    v.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        const bool f = std::ranges::contains(faulty, i);
        v.push_back(std::make_unique<GFaultyContainer>(
            tag(batch, i), f ? fault_mode::THROW_PROCESSING : fault_mode::NONE));
    }
    return v;
}

std::size_t count_processed(const std::vector<item_ptr> &v) {
    std::size_t c = 0;
    for(const auto &it : v) {
        if(it && it->is_processed()) {
            ++c;
        }
    }
    return c;
}

/** @brief A GNetworkedConsumerT with no real transport: exposes checkout/checkin so a pool of test
 *  driver threads can play "the shared client pool" against many concurrent dispatch_ submitters. */
class SimNetConsumer : public c2::GNetworkedConsumerT<GFaultyContainer> {
public:
    using c2::GNetworkedConsumerT<GFaultyContainer>::checkout;
    using c2::GNetworkedConsumerT<GFaultyContainer>::checkin;
};

} /* anonymous namespace */

/******************************************************************************/

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent fan-in scenario: spawns a shared client pool plus per-batch submitter threads against one networked consumer, then verifies each batch's own slots come back correctly routed; splitting would scatter the capturing lambdas
TEST_CASE("courtier(fanin): concurrent submitters to one networked consumer, results not cross-routed",
          "[courtier][concurrency][fanin]") {
    constexpr std::size_t M = 6;  // concurrent submitters (inner algorithms)
    constexpr std::size_t N = 25; // items per batch
    constexpr std::size_t CLIENTS = 4;

    SimNetConsumer consumer;

    // Each submitter owns a batch (the span borrows it, so it must outlive processBatch()).
    std::vector<std::vector<item_ptr>> batches;
    batches.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        batches.push_back(make_tagged_batch(b, N));
    }

    std::atomic<std::size_t> active{M};

    // The shared "client pool": pull any pending item from any batch, process it, hand it back.
    std::vector<std::jthread> clients;
    clients.reserve(CLIENTS);
    for(std::size_t c = 0; c < CLIENTS; ++c) {
        clients.emplace_back([&] {
            while(active.load() > 0) {
                auto p = consumer.checkout();
                if(not p) {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                try {
                    p->process();
                }
                catch(...) {
                    // process() sets the item's status (here always PROCESSED) before any re-throw;
                    // a real client swallows the re-throw and ships the item back. Mirror that.
                }
                consumer.checkin(std::move(p));
            }
        });
    }

    // The submitters: each blocks in processBatch() until its own batch is whole again.
    std::vector<std::jthread> submitters;
    submitters.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        submitters.emplace_back([&, b] {
            consumer.processBatch(
                std::span<item_ptr>(batches[b].data(), batches[b].size()),
                c2::GSubmissionPolicy::full_success_or_fatal());
            --active;
        });
    }

    for(auto &s : submitters) {
        s.join();
    }
    for(auto &c : clients) {
        c.join();
    }

    // Every batch fully evaluated, and every slot still holds ITS OWN item (correct (batch,slot)
    // routing -- a broken router would land batch B's result in batch A's slot, changing its tag).
    for(auto const& [b, batch] : batches | std::views::enumerate) {
        CHECK(count_processed(batch) == N);
        for(auto const& [i, item] : batch | std::views::enumerate) {
            CHECK(item->get_stored_number() == tag(b, i));
            CHECK(item->is_processed());
        }
    }
}

/******************************************************************************/

TEST_CASE("courtier(fanin): concurrent submitters with failures stay size-preserved per batch",
          "[courtier][concurrency][fanin]") {
    // Concurrent batches, each with a couple of throwing items, under clone-on-partial-return: each
    // batch must come back full (failed slots refilled from a successful sibling) and independent.
    constexpr std::size_t M = 5;
    constexpr std::size_t N = 20;

    SimNetConsumer consumer;

    std::vector<std::vector<item_ptr>> batches;
    batches.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        batches.push_back(make_tagged_batch(b, N, {3, 11})); // two throwing slots per batch
    }

    std::atomic<std::size_t> active{M};
    std::vector<std::jthread> clients;
    for(std::size_t c = 0; c < 4; ++c) {
        clients.emplace_back([&] {
            while(active.load() > 0) {
                auto p = consumer.checkout();
                if(not p) {
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                try {
                    p->process(); // a faulty item sets itself EXCEPTION_CAUGHT, then re-throws
                }
                catch(...) {
                    // Swallow the re-throw (as a real client does); the EXCEPTION_CAUGHT status
                    // is already on the item, so checkin reports it as FAILED for the policy loop.
                }
                consumer.checkin(std::move(p));
            }
        });
    }

    std::vector<std::jthread> submitters;
    for(std::size_t b = 0; b < M; ++b) {
        submitters.emplace_back([&, b] {
            consumer.processBatch(
                std::span<item_ptr>(batches[b].data(), batches[b].size()),
                c2::GSubmissionPolicy::clone_on_partial_return());
            --active;
        });
    }
    for(auto &s : submitters) {
        s.join();
    }
    for(auto &c : clients) {
        c.join();
    }

    for(std::size_t b = 0; b < M; ++b) {
        CHECK(batches[b].size() == N);       // size preserved
        CHECK(count_processed(batches[b]) == N); // every slot ends up evaluated (clones refill failures)
    }
}

/******************************************************************************/

TEST_CASE("courtier(fanin): concurrent submitters to one local thread consumer",
          "[courtier][concurrency][fanin]") {
    // The local thread-pool consumer shares one pool across submitters; concurrent processBatch()
    // calls must each wait for only their OWN items (per-batch wait), not the whole pool.
    constexpr std::size_t M = 8;
    constexpr std::size_t N = 30;

    auto consumer = std::make_shared<c2::GStdThreadConsumerT<GFaultyContainer>>(4);

    std::vector<std::vector<item_ptr>> batches;
    batches.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        batches.push_back(make_tagged_batch(b, N));
    }

    std::vector<std::jthread> submitters;
    submitters.reserve(M);
    for(std::size_t b = 0; b < M; ++b) {
        submitters.emplace_back([&, b] {
            consumer->processBatch(std::span<item_ptr>(batches[b].data(), batches[b].size()),
                                   c2::GSubmissionPolicy::full_success_or_fatal());
        });
    }
    for(auto &s : submitters) {
        s.join();
    }

    for(auto const& [b, batch] : batches | std::views::enumerate) {
        CHECK(count_processed(batch) == N);
        for(auto const& [i, item] : batch | std::views::enumerate) {
            CHECK(item->get_stored_number() == tag(b, i));
        }
    }
}

/******************************************************************************/
