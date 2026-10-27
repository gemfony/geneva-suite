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
 * Tier-2 loopback tests for the courtier ASIO consumer: a real TCP server (the courtier
 * GAsioConsumerT) driven by the span+policy executor, served by one or more unmodified existing
 * courtier ASIO clients over loopback sockets. Verifies that every item makes the full
 * server->client->server round-trip and is reconciled into the batch, that several clients can
 * share the load, and that throwing items are refilled under the clone-on-partial-return policy.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>
#include <span>

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/transport/GAsioTransportT.hpp" // the (reused) client
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GAsioConsumerT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
namespace c2 = Gem::Courtier;
namespace ccons = Gem::Courtier::Consumers;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;
// The wire codec exercised over the real socket: the GArchive flat-binary format (the networked
// default). This carries a GEM-encoded work item end-to-end through the asio consumer/session path.
constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

std::vector<item_ptr> make_batch(
    std::size_t n,
    const std::vector<std::size_t> &faulty = {},
    fault_mode fm = fault_mode::THROW_PROCESSING
) {
    std::vector<item_ptr> v;
    v.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        const bool f = std::ranges::contains(faulty, i);
        v.push_back(std::make_unique<GFaultyContainer>(i, f ? fm : fault_mode::NONE));
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

/** @brief Runs @p n_clients reused courtier ASIO clients against a courtier server, drives the
 *  batch through the executor, then tears everything down cleanly. */
void run_over_sockets(std::vector<item_ptr> &items, const c2::GSubmissionPolicy &policy,
                      std::size_t n_clients = 1, std::size_t prefetch_depth = 1) {
    auto consumer = std::make_shared<c2::GAsioConsumerT<GFaultyContainer>>(/*port=*/0, /*threads=*/2, BIN);
    consumer->startServer();
    const unsigned short port = consumer->getPort();

    std::vector<std::shared_ptr<ccons::GAsioConsumerClientT<GFaultyContainer>>> clients;
    std::vector<std::jthread> client_threads;
    std::atomic<bool> any_threw{false};
    for(std::size_t c = 0; c < n_clients; ++c) {
        auto client = std::make_shared<ccons::GAsioConsumerClientT<GFaultyContainer>>(
            "127.0.0.1", port, BIN, /*max_reconnects=*/50, prefetch_depth
        );
        clients.push_back(client);
        client_threads.emplace_back([client, &any_threw] {
            try {
                client->run();
            }
            catch(...) {
                any_threw.store(true);
            }
        });
    }

    consumer->processBatch(std::span<item_ptr>(items.data(), items.size()), policy);

    for(auto &client : clients) {
        client->flagCloseRequested();
    }
    for(auto &t : client_threads) {
        if(t.joinable()) {
            t.join();
        }
    }
    consumer->stopServer();

    CHECK_FALSE(any_threw.load());
}

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("courtier(asio): a clean batch is fully evaluated over real sockets",
          "[courtier][asio][net]") {
    auto items = make_batch(120);
    run_over_sockets(items, c2::GSubmissionPolicy::full_success_or_fatal());
    CHECK(items.size() == 120);
    CHECK(count_processed(items) == 120);
}

TEST_CASE("courtier(asio): several clients share the batch", "[courtier][asio][net]") {
    auto items = make_batch(200);
    run_over_sockets(items, c2::GSubmissionPolicy::full_success_or_fatal(), /*n_clients=*/4);
    CHECK(count_processed(items) == 200);
}

TEST_CASE("courtier(asio): throwing items are refilled under clone-on-partial-return",
          "[courtier][asio][net]") {
    auto items = make_batch(60, {3, 11, 27, 48}, fault_mode::THROW_PROCESSING);
    run_over_sockets(items, c2::GSubmissionPolicy::clone_on_partial_return(), /*n_clients=*/2);
    CHECK(items.size() == 60);
    CHECK(count_processed(items) == 60);
}

TEST_CASE("courtier(asio): prefetching clients evaluate the whole batch",
          "[courtier][asio][net]") {
    // Each client keeps several items in flight at once (a spare is fetched while another computes,
    // each connection returning a finished item and fetching the next); every slot must come back once.
    auto items = make_batch(200);
    run_over_sockets(items, c2::GSubmissionPolicy::full_success_or_fatal(),
                     /*n_clients=*/2, /*prefetch_depth=*/8);
    CHECK(items.size() == 200);
    CHECK(count_processed(items) == 200);
}

TEST_CASE("courtier(asio): prefetch + throwing items refilled under clone-on-partial-return",
          "[courtier][asio][net]") {
    // Combine prefetch with faulty items: the per-item time-lease reclaim and the reconciliation must
    // cooperate so failed slots are refilled and the batch comes back full.
    auto items = make_batch(120, {5, 17, 39, 64, 88, 103}, fault_mode::THROW_PROCESSING);
    run_over_sockets(items, c2::GSubmissionPolicy::clone_on_partial_return(),
                     /*n_clients=*/3, /*prefetch_depth=*/4);
    CHECK(items.size() == 120);
    CHECK(count_processed(items) == 120);
}

/******************************************************************************/
