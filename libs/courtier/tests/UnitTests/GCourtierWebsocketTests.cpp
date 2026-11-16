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
 * Tier-2 loopback tests for the courtier websocket consumer: a real
 * websocket server (the courtier GWebsocketConsumerT) driven by the span+policy executor and
 * served by one or more unmodified existing courtier websocket clients over loopback.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <string>
#include <cstdint>
#include <filesystem>
#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <tuple>
#include <vector>

#include <unistd.h> // getpid()

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/GWireCodec.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp" // the (reused) client

#include "GTcpRelay.hpp"

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
    c2::GWebsocketConsumerConfig config{
        .ip = "127.0.0.1",
        .serializationMode = BIN,
        .nThreads = 2,
        .clientPrefetchDepth = prefetch_depth,
    };
    auto consumer = std::make_shared<c2::GWebsocketConsumerT<GFaultyContainer>>(config);
    consumer->startServer();
    config.port = consumer->getPort(); // the server chose a free port; its clients are told

    std::vector<std::shared_ptr<ccons::GWebsocketClientT<GFaultyContainer>>> clients;
    std::vector<std::jthread> client_threads;
    std::atomic<bool> any_threw{false};
    for(std::size_t c = 0; c < n_clients; ++c) {
        auto client = std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(config);
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
// A client holds n items and evaluates m of them at a time; m is 1 unless it is set. A client of
// a default configuration with a depth of 3 therefore never evaluates two items at once; with
// m = 2 it evaluates two at once, never three.

namespace {

/** @brief How many evaluations ran at once, at most */
struct AtOnce {
    std::mutex mutex;
    std::size_t now = 0;
    std::size_t most = 0;
};

/** @brief Runs a batch through one websocket client of @p config whose evaluations take a while,
 *  and counts how many ran at once @param config The family's configuration (depth and m)
 *  @return The largest number of evaluations at once */
std::size_t mostAtOnce(c2::GWebsocketConsumerConfig config) {
    constexpr std::size_t ITEMS = 18;
    auto consumer = std::make_shared<c2::GWebsocketConsumerT<GFaultyContainer>>(config);
    consumer->startServer();
    config.port = consumer->getPort();
    auto at_once = std::make_shared<AtOnce>();
    auto client = std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(config);
    client->setItemProcessor([at_once](GFaultyContainer &item) {
        {
            const std::scoped_lock lock(at_once->mutex);
            at_once->most = std::max(at_once->most, ++at_once->now);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        {
            const std::scoped_lock lock(at_once->mutex);
            --at_once->now;
        }
        item.markAsProcessedWith({true});
    });
    std::jthread runner([client] { client->run(); });
    auto items = make_batch(ITEMS);
    consumer->processBatch(
        std::span<item_ptr>(items.data(), items.size()),
        c2::GSubmissionPolicy::full_success_or_fatal()
    );
    client->flagCloseRequested();
    runner.join();
    consumer->stopServer();
    CHECK(count_processed(items) == ITEMS);
    const std::scoped_lock lock(at_once->mutex);
    return at_once->most;
}

} /* anonymous namespace */

TEST_CASE(
    "courtier(websocket): a client evaluates one item at a time unless told otherwise",
    "[courtier][websocket][net]"
) {
    const c2::GWebsocketConsumerConfig defaults;
    CHECK(defaults.clientParallelEvaluations == 1);
    c2::GWebsocketConsumerConfig config{
        .ip = "127.0.0.1",
        .serializationMode = BIN,
        .nThreads = 2,
        .clientPrefetchDepth = 3,
    };
    CHECK(mostAtOnce(config) == 1);

    config.clientParallelEvaluations = 2;
    CHECK(mostAtOnce(config) == 2);
}

/******************************************************************************/

TEST_CASE(
    "courtier(websocket): a client whose server vanishes ends at its reconnect limit",
    "[courtier][websocket][net]"
) {
    // Regression (2026-07-18): every fatal client transport path (read/write error, decode
    // failure, invalid command) set close_code_ but never tore the connection down, so the
    // 1-second halt-poll timer kept io_context::run() alive forever -- the client hung instead
    // of terminating. A minimal one-shot server stands in for a crashed server process: it
    // completes the websocket handshake, answers the client's first pull with NO_WORK (contact),
    // closes its acceptor and hard-closes the TCP socket without a websocket close handshake. The
    // client connects again, is refused, and its run() must return BY ITSELF at its reconnect
    // limit (no flagCloseRequested from the outside).
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
        const c2::GCommandContainerT<GFaultyContainer> no_work{c2::GFrameKind::NO_WORK};
        ws.write(boost::asio::buffer(c2::wireEncode(no_work, nullptr, BIN)), ec);
        std::ignore = acceptor.close(ec); // the server process is gone: connects are refused
        // No close frame: slam the TCP connection shut like a killed server process.
        ws.next_layer().close(ec);
    });

    auto client =
        std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(c2::GWebsocketConsumerConfig{
            .ip = "127.0.0.1",
            .port = port,
            .serializationMode = BIN,
            .clientReconnectLimit = 2
        });
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
    CHECK(client->losses() == 1);
    CHECK(client->terminalErrorFlagged()); // the limit, not a halt from outside

    if(not returned.load()) {
        // Rescue a hung client (the pre-fix behaviour) so the jthread can join and the test
        // FAILS instead of deadlocking the suite.
        client->flagCloseRequested();
    }
}

/******************************************************************************/
// A client halts with one evaluation running; its result goes out as a RESULT (never answered) and
// then its close. The server's end of the run (RUN_ENDED) crosses that close: the server's close
// frame arrives after the client's has gone out, so Beast's close operation of the client reads it
// (not the peer's own read, and without the control callback). The client must hear the end of
// the run and save nothing (it used to report a plain close and save the result). The server is
// written by hand from the handshake on, so that the test decides when its close goes out. (The
// in-process pair has no second reader of a close and cannot show this.)
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario with a hand-written server; Catch2's macros count
TEST_CASE(
    "courtier(websocket): a halting client whose close crosses the end of the run saves nothing",
    "[courtier][websocket][net]"
) {
    using boost::asio::ip::tcp;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("geneva-crossing-run-end-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    boost::asio::io_context server_ioc;
    tcp::acceptor acceptor(server_ioc, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    std::atomic<bool> server_ok{false};
    std::jthread server_thread([&acceptor, &server_ok] {
        boost::system::error_code ec;
        tcp::socket sock = acceptor.accept(ec);
        if(ec) {
            return;
        }
        boost::beast::websocket::stream<tcp::socket> ws(std::move(sock));
        ws.binary(true);
        ws.accept(ec);
        if(ec) {
            return;
        }
        // Reads one frame of the client by hand (masked, as a client's are) and returns its opcode
        auto &raw = ws.next_layer();
        const auto read_frame = [&raw, &ec]() -> int {
            std::array<unsigned char, 8> head{};
            boost::asio::read(raw, boost::asio::buffer(head.data(), 2), ec);
            const auto opcode = static_cast<int>(head[0] & 0x0FU);
            std::uint64_t length = head[1] & 0x7FU;
            if(length == 126) {
                boost::asio::read(raw, boost::asio::buffer(head.data(), 2), ec);
                length = (std::uint64_t{head[0]} << 8U) | head[1];
            }
            else if(length == 127) {
                boost::asio::read(raw, boost::asio::buffer(head.data(), 8), ec);
                length = 0;
                for(const unsigned char byte : head) {
                    length = (length << 8U) | byte;
                }
            }
            std::vector<unsigned char> rest(length + 4); // a client's frame is masked: the mask first
            boost::asio::read(raw, boost::asio::buffer(rest.data(), rest.size()), ec);
            return ec ? -1 : opcode;
        };
        if(read_frame() != 2) { // the client's PULL (binary)
            return;
        }
        auto item = std::make_unique<GFaultyContainer>(7);
        item->mark_as_due_for_processing();
        const c2::GCommandContainerT<GFaultyContainer> work{c2::GFrameKind::WORK, std::move(item)};
        ws.write(boost::asio::buffer(c2::wireEncode(work, nullptr, BIN)), ec);
        const int result = read_frame(); // the RESULT
        const int close = read_frame();  // the client's close: it has gone out
        // The end of the run (4000), written by hand: it crosses the client's close
        const std::array<unsigned char, 4> run_ended{0x88, 0x02, 0x0F, 0xA0};
        boost::asio::write(raw, boost::asio::buffer(run_ended), ec);
        server_ok.store(result == 2 && close == 8 && not ec);
        std::ignore = raw.shutdown(tcp::socket::shutdown_both, ec);
        std::ignore = raw.close(ec);
    });

    auto evaluating = std::make_shared<std::atomic<bool>>(false);
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto client = std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(
        c2::GWebsocketConsumerConfig{
            .ip = "127.0.0.1",
            .port = port,
            .serializationMode = BIN,
            .clientUndeliveredDir = dir.string()
        }
    );
    client->setItemProcessor([evaluating, release](GFaultyContainer &item) {
        evaluating->store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while(not release->load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        item.markAsProcessedWith({true});
    });
    std::atomic<bool> returned{false};
    const std::jthread client_thread([client, &returned] {
        try {
            client->run();
        }
        catch(...) { // NOLINT(bugprone-empty-catch) -- a throw still counts as returning
        }
        returned.store(true);
    });
    const auto until = [](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while(not condition() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return condition();
    };
    REQUIRE(until([&] { return evaluating->load(); }));
    client->flagCloseRequested(); // the halt: the result goes out, then the close
    release->store(true);
    const bool ended = until([&] { return returned.load(); });
    server_thread.join();
    CHECK(ended);
    CHECK(server_ok.load());
    CHECK(client->resultsSaved() == 0);
    std::size_t n_files = 0;
    for([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator(dir)) {
        ++n_files;
    }
    CHECK(n_files == 0);
    std::filesystem::remove_all(dir);
}

/******************************************************************************/
// A TCP connection whose websocket handshake has not completed is a session of the server, but not
// a connected client (connectedClients(), what a benchmark waits for): only a client that has
// spoken is.
TEST_CASE(
    "courtier(websocket): a connection without a handshake is not a connected client",
    "[courtier][websocket][net]"
) {
    using boost::asio::ip::tcp;
    c2::GWebsocketConsumerConfig config{.ip = "127.0.0.1", .serializationMode = BIN, .nThreads = 2};
    auto consumer = std::make_shared<c2::GWebsocketConsumerT<GFaultyContainer>>(config);
    consumer->startServer();
    config.port = consumer->getPort();

    boost::asio::io_context own;
    tcp::socket raw(own);
    raw.connect({boost::asio::ip::address_v4::loopback(), config.port});
    const auto patience = std::chrono::seconds(10);
    const auto until = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while(not condition() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return condition();
    };
    REQUIRE(until([&] { return consumer->getNActiveSessions() == 1; }));
    CHECK(consumer->connectedClients() == std::optional<std::size_t>{0});

    auto client = std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(config);
    std::jthread client_thread([client] { client->run(); });
    REQUIRE(until([&] { return consumer->connectedClients() == 1U; }));
    CHECK(consumer->getNActiveSessions() == 2);

    client->flagCloseRequested();
    client_thread.join();
    boost::system::error_code ignored;
    std::ignore = raw.close(ignored);
    consumer->stopServer();
}

/******************************************************************************/
// A client for which there is no work is not sent away with NO_WORK to ask again after a pause:
// its session is parked with the consumer and answers as soon as there is an item. So an idle
// client causes no traffic, and none of the pauses that used to add to every iteration.
//
// Checked without timing: after an idle phase every client is parked and the server has not
// answered a single request with NO_WORK; a batch is evaluated completely; afterwards every
// client is parked again. With a prefetch depth above one a session owes several answers but is
// parked once.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario, run for two prefetch depths
TEST_CASE(
    "courtier(websocket): idle clients are parked and not sent away",
    "[courtier][websocket][net][park]"
) {
    constexpr std::size_t CLIENTS = 4;
    constexpr std::size_t ITEMS = 40;

    for(const std::size_t prefetch_depth : {std::size_t{1}, std::size_t{3}}) {
        c2::GWebsocketConsumerConfig config{
            .ip = "127.0.0.1",
            .serializationMode = BIN,
            .nThreads = 2,
            .clientPrefetchDepth = prefetch_depth,
        };
        auto consumer = std::make_shared<c2::GWebsocketConsumerT<GFaultyContainer>>(config);
        consumer->startServer();
        config.port = consumer->getPort(); // the server chose a free port; its clients are told

        std::vector<std::shared_ptr<ccons::GWebsocketClientT<GFaultyContainer>>> clients;
        std::vector<std::jthread> client_threads;
        for(std::size_t c = 0; c < CLIENTS; ++c) {
            clients.push_back(std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(config));
            client_threads.emplace_back([client = clients.back()] { client->run(); });
        }

        const auto all_parked = [&consumer] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while(consumer->parkedCount() != CLIENTS &&
                  std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return consumer->parkedCount() == CLIENTS;
        };

        CHECK(all_parked());
        CHECK(consumer->emptyCheckoutCount() == 0);

        // On a thread of its own and with a limit: if the parked sessions were not woken, the
        // batch would wait forever, and the test is to fail, not to hang.
        auto items = make_batch(ITEMS);
        auto batch = std::async(std::launch::async, [&] {
            consumer->processBatch(
                std::span<item_ptr>(items.data(), items.size()),
                c2::GSubmissionPolicy::full_success_or_fatal()
            );
        });
        if(batch.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
            consumer->stopServer(); // releases the waiting batch
            batch.wait();
            for(auto &client : clients) {
                client->flagCloseRequested();
            }
            FAIL("the batch was not served");
        }
        CHECK(count_processed(items) == ITEMS);

        CHECK(all_parked()); // each client's last result was a request for more
        CHECK(consumer->emptyCheckoutCount() == 0);

        for(auto &client : clients) {
            client->flagCloseRequested();
        }
        client_threads.clear(); // joins

        // The clients are gone while their sessions were parked. The list of parked callers must
        // not keep a session alive: each one ends with its connection.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while(consumer->getNActiveSessions() != 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(consumer->getNActiveSessions() == 0);

        // Their entries are still in the list of parked callers, ahead of the client that comes
        // now. One item means one wake-up, and it must not be spent on a session that has ended.
        CHECK(consumer->parkedCount() == CLIENTS);
        auto late_client = std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(config);
        std::jthread late_thread([late_client] { late_client->run(); });
        const auto late_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while(consumer->parkedCount() != CLIENTS + 1 &&
              std::chrono::steady_clock::now() < late_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(consumer->parkedCount() == CLIENTS + 1);

        // A batch that makes no progress is given up after a while and submitted again, which
        // announces its item anew; that would paper over a wake-up that went astray. Not here.
        consumer->setTimeoutTreatment(c2::timeoutTreatment::wait_indefinitely);
        auto one_item = make_batch(1);
        auto one_batch = std::async(std::launch::async, [&] {
            consumer->processBatch(
                std::span<item_ptr>(one_item.data(), one_item.size()),
                c2::GSubmissionPolicy::full_success_or_fatal()
            );
        });
        if(one_batch.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
            consumer->stopServer(); // releases the waiting batch
            one_batch.wait();
            late_client->flagCloseRequested();
            FAIL("the wake-up for the item did not reach the client that is there");
        }
        CHECK(count_processed(one_item) == 1);

        // The late client ends before the server stops: a server that stops first leaves the
        // client's close unanswered, and the client waits for its idle limit (30 s by default).
        late_client->flagCloseRequested();
        late_thread.join();
        consumer->stopServer();
    }
}

/******************************************************************************/
// Connecting over sockets: a halt during a handshake the server never answers ends the client at
// once; a client started while nothing accepts connects once something does.

TEST_CASE(
    "courtier(websocket): a halt during an unanswered handshake ends the client at once",
    "[courtier][websocket][net]"
) {
    using boost::asio::ip::tcp;
    // The kernel completes the TCP connect (the backlog); nobody ever answers the handshake.
    boost::asio::io_context server_ioc;
    const tcp::acceptor acceptor(
        server_ioc,
        tcp::endpoint(boost::asio::ip::address_v4::loopback(), /*port=*/0)
    );
    auto client =
        std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(c2::GWebsocketConsumerConfig{
            .ip = "127.0.0.1",
            .port = acceptor.local_endpoint().port(),
            .serializationMode = BIN
        });
    std::atomic<bool> returned{false};
    std::jthread const client_thread([client, &returned] {
        client->run();
        returned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto asked = std::chrono::steady_clock::now();
    client->flagCloseRequested();
    const auto deadline = asked + std::chrono::seconds(40); // beyond the handshake limit (30 s)
    while(not returned.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto took = std::chrono::steady_clock::now() - asked;
    REQUIRE(returned.load());
    CHECK(took < std::chrono::seconds(3));
}

TEST_CASE(
    "courtier(websocket): a client started while nothing accepts connects later",
    "[courtier][websocket][net]"
) {
    c2::GWebsocketConsumerConfig config{.ip = "127.0.0.1", .serializationMode = BIN, .nThreads = 2};
    auto consumer = std::make_shared<c2::GWebsocketConsumerT<GFaultyContainer>>(config);
    consumer->startServer();
    Gem::Courtier::Tests::GTcpRelay relay(consumer->getPort());
    relay.setAccepting(false);
    config.port = relay.port();

    auto client = std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(config);
    std::atomic<bool> returned{false};
    std::jthread client_thread([client, &returned] {
        client->run();
        returned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // refused meanwhile
    CHECK_FALSE(returned.load());
    CHECK(relay.nConnections() == 0);

    relay.setAccepting(true);
    auto items = make_batch(5);
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<item_ptr>(items.data(), items.size()),
            c2::GSubmissionPolicy::full_success_or_fatal()
        );
    });
    const bool done = batch.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
    if(not done) {
        consumer->stopServer();
    }
    batch.get();
    CHECK(done);
    CHECK(count_processed(items) == 5);
    client->flagCloseRequested();
    client_thread.join();
}
