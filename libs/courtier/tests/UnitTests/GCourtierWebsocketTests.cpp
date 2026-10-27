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
 * Tier-2 loopback tests for the courtier websocket consumer, mirroring the ASIO tests: a real
 * websocket server (the courtier GWebsocketConsumerT) driven by the span+policy executor and
 * served by one or more unmodified existing courtier websocket clients over loopback.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>
#include <span>

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp" // the (reused) client
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
namespace c2 = Gem::Courtier;
namespace ccons = Gem::Courtier::Consumers;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;
// The wire codec exercised over the real socket: the GArchive flat-binary format (the networked
// default). This carries a GEM-encoded work item end-to-end through the beast consumer/session path.
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

void run_over_sockets(std::vector<item_ptr> &items, const c2::GSubmissionPolicy &policy,
                      std::size_t n_clients = 1, std::size_t prefetch_depth = 1) {
    auto consumer = std::make_shared<c2::GWebsocketConsumerT<GFaultyContainer>>(
        /*port=*/0, /*threads=*/2, BIN
    );
    consumer->startServer();
    const unsigned short port = consumer->getPort();

    std::vector<std::shared_ptr<ccons::GWebsocketClientT<GFaultyContainer>>> clients;
    std::vector<std::jthread> client_threads;
    std::atomic<bool> any_threw{false};
    for(std::size_t c = 0; c < n_clients; ++c) {
        auto client = std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(
            "127.0.0.1", port, BIN, /*verbose_control_frames=*/false, prefetch_depth
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

TEST_CASE("courtier(websocket): a clean batch is fully evaluated over real sockets",
          "[courtier][websocket][net]") {
    auto items = make_batch(120);
    run_over_sockets(items, c2::GSubmissionPolicy::full_success_or_fatal());
    CHECK(items.size() == 120);
    CHECK(count_processed(items) == 120);
}

TEST_CASE("courtier(websocket): several clients share the batch", "[courtier][websocket][net]") {
    auto items = make_batch(200);
    run_over_sockets(items, c2::GSubmissionPolicy::full_success_or_fatal(), /*n_clients=*/4);
    CHECK(count_processed(items) == 200);
}

TEST_CASE("courtier(websocket): throwing items are refilled under clone-on-partial-return",
          "[courtier][websocket][net]") {
    auto items = make_batch(60, {3, 11, 27, 48}, fault_mode::THROW_PROCESSING);
    run_over_sockets(items, c2::GSubmissionPolicy::clone_on_partial_return(), /*n_clients=*/2);
    CHECK(items.size() == 60);
    CHECK(count_processed(items) == 60);
}

TEST_CASE("courtier(websocket): prefetching clients evaluate the whole batch",
          "[courtier][websocket][net]") {
    // Each client holds several items in flight at once (overlapping fetch/compute/return); every slot
    // must still come back exactly once. Fewer clients than the prefetch depth, so the depth genuinely
    // multiplexes work onto each connection.
    auto items = make_batch(200);
    run_over_sockets(items, c2::GSubmissionPolicy::full_success_or_fatal(),
                     /*n_clients=*/2, /*prefetch_depth=*/8);
    CHECK(items.size() == 200);
    CHECK(count_processed(items) == 200);
}

TEST_CASE("courtier(websocket): prefetch + throwing items refilled under clone-on-partial-return",
          "[courtier][websocket][net]") {
    // Combine prefetch with faulty items: the multi-item CheckoutLease and the reconciliation must
    // cooperate so failed slots are refilled and the batch comes back full.
    auto items = make_batch(120, {5, 17, 39, 64, 88, 103}, fault_mode::THROW_PROCESSING);
    run_over_sockets(items, c2::GSubmissionPolicy::clone_on_partial_return(),
                     /*n_clients=*/3, /*prefetch_depth=*/4);
    CHECK(items.size() == 120);
    CHECK(count_processed(items) == 120);
}

/******************************************************************************/

TEST_CASE("courtier(websocket): a client whose server vanishes terminates on its own",
          "[courtier][websocket][net]") {
    // Regression (2026-07-18): every fatal client transport path (read/write error, decode
    // failure, invalid command) set close_code_ but never tore the connection down, so the
    // 1-second halt-poll timer kept io_context::run() alive forever -- the client hung instead
    // of terminating. A minimal one-shot server stands in for a crashed server process: it
    // completes the websocket handshake, reads the client's first pull, then hard-closes the
    // TCP socket without a websocket close handshake. The client's outstanding read errors and
    // its run() must return BY ITSELF (no flagCloseRequested from the outside).
    namespace bws = boost::beast::websocket;
    using boost::asio::ip::tcp;

    boost::asio::io_context server_ioc;
    tcp::acceptor acceptor(
        server_ioc, tcp::endpoint(boost::asio::ip::address_v4::loopback(), /*port=*/0)
    );
    const unsigned short port = acceptor.local_endpoint().port();

    std::jthread const server_thread([&acceptor] {
        boost::system::error_code ec;
        tcp::socket sock = acceptor.accept(ec);
        if(ec) {
            return;
        }
        bws::stream<tcp::socket> ws(std::move(sock));
        ws.accept(ec); // the server side of the websocket handshake
        if(ec) {
            return;
        }
        boost::beast::flat_buffer buf;
        ws.read(buf, ec); // the client's first PULL frame
        // No reply, no close frame: slam the TCP connection shut like a killed server process.
        ws.next_layer().close(ec);
    });

    auto client = std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(
        "127.0.0.1", port, BIN, /*verbose_control_frames=*/false, /*prefetch_depth=*/1
    );
    std::atomic<bool> returned{false};
    std::jthread const client_thread([client, &returned] {
        try {
            client->run();
        }
        catch(...) { /* a throw still counts as returning */
        }
        returned.store(true);
    });

    // The client must come back on its own within a generous bound.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while(not returned.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(returned.load());

    if(not returned.load()) {
        // Rescue a hung client (the pre-fix behaviour) so the jthread can join and the test
        // FAILS instead of deadlocking the suite.
        client->flagCloseRequested();
    }
}
