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
 * Tests of what keeps a websocket connection of the courtier honest: the keep-alive that notices a
 * peer which has fallen silent (on either end), the limit on the handshake, the absence of a limit
 * on the message size, and the answer to a request the server cannot handle. The time limits all
 * derive from the ping interval, which is set to one second here, so each of these tests takes a
 * few seconds.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include "GDemoProcessingContainers.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GCompletionLatch.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GProcessable.hpp" // detail::lineageSalt (a foreign result)
#include "courtier/GProcessingContainerT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp"

#include "GCapturedLog.hpp"
#include "GTcpRelay.hpp"

namespace {
/// As many check-ins of a GSimpleContainer throw as this says (see the seam below).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<int> checkInsToFail{0};
/// Whether such a check-in throws a std::exception (true) or an int, which is not one
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> checkInsThrowStandard{true};

/** @brief Which of the four codec runs of an item's round trip is slow (see GSlowCodecItem). */
enum class CodecRun : int {
    NONE = 0,
    SERVER_ENCODES = 1, ///< the server writes the WORK frame
    CLIENT_DECODES = 2, ///< the client reads the WORK frame
    CLIENT_ENCODES = 3, ///< the client writes the RESULT frame
    SERVER_DECODES = 4  ///< the server reads the RESULT frame
};

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<CodecRun> slowRun{CodecRun::NONE}; ///< The run of every item that takes SLOW_CODEC
std::atomic<int> slowRunsDone{0};              ///< How many slow runs have happened
std::atomic<int> evaluations{0};               ///< How many items have been evaluated
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// How long the slow run takes: three ping intervals (see PING_INTERVAL_S below), more than the
/// idle limit of two. It is a sleep, so it does not depend on how busy the host is.
constexpr auto SLOW_CODEC = std::chrono::seconds(3);

/**
 * A work item whose (de)serialization can be made slow, separately for each of the four codec runs
 * of a round trip. The run is told apart by the direction of the archive and by the item's state:
 * an item is unprocessed on its way out and processed on its way back.
 */
class GSlowCodecItem final : public Gem::Courtier::GProcessingContainerT<GSlowCodecItem, bool> {
    friend struct Gem::Weft::access;

    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<Gem::Courtier::GProcessingContainerT<GSlowCodecItem, bool>>(
            ar,
            "GProcessingContainerT_GSlowCodecItem",
            *this
        );
        Gem::Common::archive_named(ar, "id_", id_);
        const bool back = this->is_processed();
        CodecRun run = CodecRun::NONE;
        if constexpr(Gem::Common::archive_is_saving_v<archive_type>) {
            run = back ? CodecRun::CLIENT_ENCODES : CodecRun::SERVER_ENCODES;
        }
        else {
            run = back ? CodecRun::SERVER_DECODES : CodecRun::CLIENT_DECODES;
        }
        if(run == slowRun.load()) {
            slowRunsDone.fetch_add(1);
            std::this_thread::sleep_for(SLOW_CODEC);
        }
    }

public:
    /** @brief An item with an id. @param id Tells the items apart */
    explicit GSlowCodecItem(std::size_t id)
      : Gem::Courtier::GProcessingContainerT<GSlowCodecItem, bool>(1)
      , id_(id) { /* nothing */
    }

private:
    /** @brief For loading only */
    GSlowCodecItem()
      : Gem::Courtier::GProcessingContainerT<GSlowCodecItem, bool>(1) { /* nothing */
    }
    /** @brief Counts the evaluation */
    void process_([[maybe_unused]] const std::vector<bool> &res_vec) override {
        evaluations.fetch_add(1);
    }

    std::size_t id_ = 0; ///< Tells the items apart
};
} /* anonymous namespace */

namespace Gem::Courtier {
/** @brief The wire seam of GSimpleContainer in this file: the default one, except that applying a
 *  return can be made to throw. That is how a test makes the consumer's check-in fail. */
template <>
struct GWireProtocolT<GSimpleContainer> {
    using payload_type = GNoLibraryPayload;
    static std::uint32_t workTag() {
        return 0;
    }
    static bool buildReturn(
        [[maybe_unused]] const GSimpleContainer &item,
        std::uint32_t &tag,
        [[maybe_unused]] payload_type &payload
    ) {
        tag = 0;
        return true;
    }
    static void applyReturn(
        GSimpleContainer &slot,
        [[maybe_unused]] std::uint32_t tag,
        [[maybe_unused]] const payload_type *payload,
        const GSimpleContainer *item
    ) {
        int left = checkInsToFail.load();
        while(left > 0 && not checkInsToFail.compare_exchange_weak(left, left - 1)) {
        }
        if(left > 0) {
            if(checkInsThrowStandard.load()) {
                throw std::runtime_error("this test makes the check-in fail");
            }
            throw 42; // NOLINT(bugprone-std-exception-baseclass) -- not a std::exception, on purpose
        }
        if(item != nullptr) {
            slot.absorbResultsFrom(*item);
        }
    }
};
} /* namespace Gem::Courtier */

using boost::asio::ip::tcp;
using Gem::Courtier::fault_mode;
using Gem::Courtier::GCommandContainerT;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::GFrameKind;
using Gem::Courtier::GRandomNumberContainer;
using Gem::Courtier::GSimpleContainer;
using Gem::Courtier::GSubmissionPolicy;
using Gem::Courtier::GWebsocketConsumerT;
using Gem::Courtier::Consumers::GWebsocketClientT;
using namespace std::chrono_literals;
using Gem::Courtier::Tests::CapturedLog;
namespace Websocket = boost::beast::websocket;

namespace {

constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;
constexpr std::size_t PING_INTERVAL_S =
    1;                        // a ping after 1 s of silence, the connection closed after 2 s
constexpr auto PATIENCE = 8s; // how long a test waits for something due after about 2 s

/** @brief A started websocket server for work items of type @p item_type. */
template <typename item_type>
std::shared_ptr<GWebsocketConsumerT<item_type>> startedServer(std::size_t n_threads) {
    auto consumer =
        std::make_shared<GWebsocketConsumerT<item_type>>(Gem::Courtier::GWebsocketConsumerConfig{
            .serializationMode = BIN,
            .nThreads = n_threads,
            .pingInterval = PING_INTERVAL_S,
        });
    consumer->startServer();
    return consumer;
}

/** @brief Asks a client to close when it goes, however the test leaves its scope: a client that
 *  is not asked goes on (it connects again) and the thread that runs it is never joined. Declared
 *  after that thread, so that it goes first. */
template <typename client_type>
class ClosesAtEnd {
public:
    /** @brief @param client The client */
    explicit ClosesAtEnd(std::shared_ptr<client_type> client)
      : client_(std::move(client)) { /* nothing */
    }
    /** @brief Asks the client to close */
    ~ClosesAtEnd() {
        client_->flagCloseRequested();
    }
    ClosesAtEnd(const ClosesAtEnd &) = delete;
    ClosesAtEnd(ClosesAtEnd &&) = delete;
    ClosesAtEnd &operator=(const ClosesAtEnd &) = delete;
    ClosesAtEnd &operator=(ClosesAtEnd &&) = delete;

private:
    std::shared_ptr<client_type> client_; ///< The client
};

/** @brief A client for @p port with the same keep-alive as the servers of these tests. */
template <typename item_type>
std::shared_ptr<GWebsocketClientT<item_type>> matchingClient(unsigned short port) {
    return std::make_shared<GWebsocketClientT<item_type>>(Gem::Courtier::GWebsocketConsumerConfig{
        .ip = "127.0.0.1",
        .port = port,
        .serializationMode = BIN,
        .pingInterval = PING_INTERVAL_S,
    });
}

/** @brief Polls @p condition until it holds or @p patience has passed.
 *  @return Whether the condition held in time */
template <typename condition_type>
bool eventually(condition_type condition, std::chrono::milliseconds patience = PATIENCE) {
    const auto give_up_at = std::chrono::steady_clock::now() + patience;
    while(std::chrono::steady_clock::now() < give_up_at) {
        if(condition()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return condition();
}

/** @brief A websocket client played by hand: it connects and completes the handshake, and from then
 *  on does only what a test tells it to. Left alone it is a peer that has fallen silent: it reads
 *  nothing, so it answers no ping. */
struct HandDrivenPeer {
    boost::asio::io_context ctx;
    Websocket::stream<tcp::socket> ws{ctx};

    explicit HandDrivenPeer(unsigned short port) {
        ws.next_layer().connect({boost::asio::ip::address_v4::loopback(), port});
        ws.handshake("127.0.0.1", "/");
        ws.binary(true);
    }
};

} /* anonymous namespace */

TEST_CASE(
    "courtier(websocket): a client that falls silent is dropped and nothing waits for it",
    "[courtier][websocket][liveness][net]"
) {
    // One io thread only: a session that blocked it while closing would stop the whole server.
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/1);

    HandDrivenPeer silent(consumer->getPort());
    REQUIRE(eventually([&] { return consumer->getNActiveSessions() == 1; }));

    // No pong comes back, so the session must end after two ping intervals.
    CHECK(eventually([&] { return consumer->getNActiveSessions() == 0; }));

    // The server must be able to serve a real client now.
    auto client = matchingClient<GFaultyContainer>(consumer->getPort());
    const std::jthread client_thread([client] { client->run(); });

    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.reserve(8);
    for(std::size_t i = 0; i < 8; ++i) {
        items.push_back(std::make_unique<GFaultyContainer>(i, fault_mode::NONE));
    }
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });
    const bool served = batch.wait_for(PATIENCE) == std::future_status::ready;
    CHECK(served);

    // Let go of the silent connection: a server that still waits for it can then go on, so the
    // test ends instead of hanging.
    boost::system::error_code ec;
    std::ignore = silent.ws.next_layer().close(ec);
    batch.wait();
    client->flagCloseRequested();
}

TEST_CASE(
    "courtier(websocket): a client whose server falls silent ends at its first-contact limit",
    "[courtier][websocket][liveness][net]"
) {
    // A "server" that completes the handshake and then does nothing: no reply, no pong. It accepts
    // one connection; later connects are refused. The client had no contact (no answer, and the
    // connection was open for less than the contact time): its first-contact limit ends it.
    boost::asio::io_context server_ctx;
    tcp::acceptor acceptor(server_ctx, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatch>(1);
    const std::jthread server_thread([&acceptor, held = release]() {
        boost::system::error_code ec;
        Websocket::stream<tcp::socket> ws(acceptor.accept(ec));
        std::ignore = acceptor.close(ec);
        if(not ec) {
            ws.accept(ec);
        }
        held->wait(); // hold the connection open, silently, until the test is over
    });

    // What the client logs goes here for the duration of this test.
    auto log = std::make_shared<CapturedLog>();
    GEM_LOGGER.addLogTarget(log);

    auto client = std::make_shared<GWebsocketClientT<GFaultyContainer>>(
        Gem::Courtier::GWebsocketConsumerConfig{
            .ip = "127.0.0.1",
            .port = port,
            .serializationMode = BIN,
            .pingInterval = PING_INTERVAL_S,
            .clientFirstContactLimit = 3,
        }
    );
    std::atomic<bool> returned{false};
    const std::jthread client_thread([client, &returned] {
        client->run();
        returned.store(true);
    });

    // Nothing arrives, not even a pong: after two ping intervals the connection ends, and the
    // client, refused from then on, ends at its limit.
    CHECK(eventually([&] { return returned.load(); }));
    CHECK(client->losses() == 1);
    CHECK(client->terminalErrorFlagged());

    // The log says which end gave up and how long the other end had been silent: what is needed to
    // tell a dead peer from a busy one.
    CHECK(log->hasLineWith(
        {"GWebsocketClientT<> client: the idle limit of 2000 ms has expired", "arrived", "ms ago"}
    ));
    GEM_LOGGER.resetLogTargets();

    client->flagCloseRequested(); // ends a client that is still waiting, so the test can finish
    release->count_down();
}

TEST_CASE(
    "courtier(websocket): a connection that never starts the handshake is dropped",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);

    boost::asio::io_context ctx;
    tcp::socket socket(ctx);
    socket.connect({boost::asio::ip::address_v4::loopback(), consumer->getPort()});
    REQUIRE(eventually([&] { return consumer->getNActiveSessions() == 1; }));

    // The handshake has the same limit as the idle time. Without one the session waits forever.
    CHECK(eventually([&] { return consumer->getNActiveSessions() == 0; }));
}

TEST_CASE(
    "courtier(websocket): an evaluation much longer than the idle limit keeps its connection",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);
    auto client = matchingClient<GFaultyContainer>(consumer->getPort());
    const std::jthread client_thread([client] { client->run(); });

    // Five seconds of evaluation against an idle limit of two: the client's io thread answers the
    // server's pings while the item is computed elsewhere, so the connection never looks idle.
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(0, fault_mode::SLEEP, /*sleep_ms=*/5000));
    consumer->processBatch(
        std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
        GSubmissionPolicy::full_success_or_fatal()
    );

    CHECK(items.front()->is_processed());
    CHECK(consumer->getNActiveSessions() == 1); // still the session the item went out on
    CHECK(consumer->requeuedCount() == 0);      // and the item was never taken back
    client->flagCloseRequested();
}

TEST_CASE(
    "courtier(websocket): a work item of more than 16 MiB travels in both directions",
    "[courtier][websocket][liveness][net]"
) {
    constexpr std::size_t N_DOUBLES =
        3'000'000; // 24 MB of payload; Beast's own default limit is 16 MiB

    auto consumer = startedServer<GRandomNumberContainer>(/*n_threads=*/2);
    auto client = matchingClient<GRandomNumberContainer>(consumer->getPort());
    const std::jthread client_thread([client] { client->run(); });

    std::vector<std::unique_ptr<GRandomNumberContainer>> items;
    items.push_back(std::make_unique<GRandomNumberContainer>(N_DOUBLES));
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GRandomNumberContainer>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });

    // With a limit on the message size the client fails to read the item and the batch never ends.
    const bool done = batch.wait_for(4 * PATIENCE) == std::future_status::ready;
    CHECK(done);
    if(done) {
        CHECK(items.front()->is_processed());
    }
    else {
        consumer->stopServer(); // releases the waiting batch, so the test ends
        batch.wait();
    }
    client->flagCloseRequested();
}

namespace {

/** @brief Reads the next message of a hand-driven client @param peer The client
 *  @return It, decoded */
GCommandContainerT<GFaultyContainer> readFrame(HandDrivenPeer &peer) {
    boost::beast::flat_buffer buffer;
    peer.ws.read(buffer);
    GCommandContainerT<GFaultyContainer> frame{GFrameKind::NONE};
    Gem::Courtier::container_from_string(
        boost::beast::buffers_to_string(buffer.data()),
        frame,
        BIN
    );
    return frame;
}

/** @brief A batch of one item that may end without a usable result: its slot is then filled with
 *  a clone of a template, so that the test, not the policy, judges it
 *  @param consumer The server @param items The batch @return It, running */
std::future<void> tolerantBatch(
    const std::shared_ptr<GWebsocketConsumerT<GFaultyContainer>> &consumer,
    std::vector<std::unique_ptr<GFaultyContainer>> &items
) {
    return std::async(std::launch::async, [consumer, &items] {
        auto policy = GSubmissionPolicy::clone_on_partial_return();
        policy.fatal_on_none_returned = false;
        auto clone_template = std::make_unique<GFaultyContainer>(99, fault_mode::NONE);
        clone_template->markAsProcessedWith({true});
        consumer->processBatch(
            std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
            policy,
            std::move(clone_template)
        );
    });
}

} /* anonymous namespace */

/******************************************************************************/
// The head of a message is read first: a result whose payload cannot be read still names its item,
// which then fails, and the connection goes on; a message whose head cannot be read ends the
// connection, and the item the client held is handed out again.

TEST_CASE(
    "courtier(websocket): a result whose payload cannot be read fails its item",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(0, fault_mode::NONE));
    auto batch = tolerantBatch(consumer, items);

    HandDrivenPeer peer(consumer->getPort());
    REQUIRE(readFrame(peer).kind() == GFrameKind::HELLO);
    peer.ws.write(
        boost::asio::buffer(
            Gem::Courtier::container_to_string(
                GCommandContainerT<GFaultyContainer>{GFrameKind::PULL},
                BIN
            )
        )
    );
    auto work = readFrame(peer);
    REQUIRE(work.kind() == GFrameKind::WORK);
    Gem::Courtier::processWorkFrame(work, "hand-driven client");
    Gem::Courtier::makeReturnFrame(work);
    const std::string result = Gem::Courtier::container_to_string(work, BIN);
    peer.ws.write(boost::asio::buffer(result.substr(0, result.size() - 8))); // the payload is cut

    const bool done = batch.wait_for(PATIENCE) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        consumer->stopServer(); // releases the waiting batch, so the test ends
    }
    batch.get();
    CHECK(consumer->requeuedCount() == 0);      // failed, not handed out again
    CHECK(consumer->getNActiveSessions() == 1); // and the connection goes on
}

TEST_CASE(
    "courtier(websocket): a message whose head cannot be read ends the connection",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(0, fault_mode::NONE));
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });

    HandDrivenPeer peer(consumer->getPort());
    REQUIRE(readFrame(peer).kind() == GFrameKind::HELLO);
    peer.ws.write(
        boost::asio::buffer(
            Gem::Courtier::container_to_string(
                GCommandContainerT<GFaultyContainer>{GFrameKind::PULL},
                BIN
            )
        )
    );
    REQUIRE(readFrame(peer).kind() == GFrameKind::WORK); // the hand-driven client holds the item

    peer.ws.write(boost::asio::buffer(std::string("this is not a frame")));
    boost::beast::flat_buffer buffer;
    boost::system::error_code ec;
    peer.ws.read(buffer, ec);
    CHECK(ec == Websocket::error::closed); // the session closed the connection
    CHECK(peer.ws.reason().code == Websocket::close_code::internal_error); // GPeerClose::ERROR
    REQUIRE(eventually([&] { return consumer->requeuedCount() == 1; }));   // the item is back

    // Another client can now evaluate the item.
    auto client = matchingClient<GFaultyContainer>(consumer->getPort());
    const std::jthread client_thread([client] { client->run(); });
    const bool done = batch.wait_for(PATIENCE) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        consumer->stopServer(); // releases the waiting batch, so the test ends
        batch.wait();
    }
    client->flagCloseRequested();
}

/******************************************************************************/
// A prefetching client has several pulls outstanding. While its session is parked the pulls are
// only counted; once there is work, each of them gets its answer -- without the client having to
// say anything more.
TEST_CASE(
    "courtier(websocket): a parked session answers every pull it owes",
    "[courtier][websocket][liveness][net]"
) {
    constexpr std::size_t PULLS = 3;
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);
    HandDrivenPeer peer(consumer->getPort());

    const auto next_frame = [&peer] {
        boost::beast::flat_buffer buffer;
        peer.ws.read(buffer);
        GCommandContainerT<GFaultyContainer> frame{GFrameKind::NONE};
        Gem::Courtier::container_from_string(
            boost::beast::buffers_to_string(buffer.data()),
            frame,
            BIN
        );
        return frame;
    };

    REQUIRE(next_frame().kind() == GFrameKind::HELLO); // the first message of the connection
    const std::string pull = Gem::Courtier::container_to_string(
        GCommandContainerT<GFaultyContainer>{GFrameKind::PULL},
        BIN
    );
    for(std::size_t i = 0; i < PULLS; ++i) {
        peer.ws.write(boost::asio::buffer(pull));
    }
    // A message whose effect the server counts: a result of an item another server process handed
    // out, which is dropped. Messages are handled in order, so once it is counted the session has
    // seen all three pulls: it is parked, owing three.
    GCommandContainerT<GFaultyContainer> foreign{GFrameKind::RESULT};
    Gem::Courtier::GProcessingOutcome outcome;
    outcome.submission_uuid = {Gem::Courtier::detail::lineageSalt() ^ 1U, 1};
    foreign.setOutcome(outcome);
    peer.ws.write(boost::asio::buffer(Gem::Courtier::container_to_string(foreign, BIN)));
    REQUIRE(eventually([&] { return consumer->foreignReturnCount() == 1; }));
    REQUIRE(consumer->parkedCount() == 1);

    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.reserve(PULLS);
    for(std::size_t i = 0; i < PULLS; ++i) {
        items.push_back(std::make_unique<GFaultyContainer>(i, fault_mode::NONE));
    }
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });

    // If an item does not arrive, the read below waits forever. The watchdog then stops the
    // server, and the batch that still waits ends the test process with a fatal error.
    const auto all_arrived = std::make_shared<Gem::Common::Concurrency::GCompletionLatch>(1);
    const std::jthread watchdog([&consumer, arrived = all_arrived] {
        if(not arrived->wait_for(PATIENCE)) {
            consumer->stopServer();
        }
    });

    // All three items arrive before the client returns anything.
    std::vector<GCommandContainerT<GFaultyContainer>> work;
    work.reserve(PULLS);
    for(std::size_t i = 0; i < PULLS; ++i) {
        work.push_back(next_frame());
        CHECK(work.back().kind() == GFrameKind::WORK);
    }
    all_arrived->count_down();
    for(auto &frame : work) {
        Gem::Courtier::processWorkFrame(frame, "hand-driven client");
        Gem::Courtier::makeReturnFrame(frame);
        peer.ws.write(boost::asio::buffer(Gem::Courtier::container_to_string(frame, BIN)));
    }

    const bool done = batch.wait_for(PATIENCE) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        consumer->stopServer(); // releases the waiting batch, so the test ends
        batch.wait();
    }
    CHECK(consumer->emptyCheckoutCount() == 0);
}

/******************************************************************************/
// One caller is woken per item. If that caller's client has died without the server knowing, the
// item goes nowhere -- until the keep-alive ends the connection. The session then gives the item
// back, and that wakes the next parked caller.
TEST_CASE(
    "courtier(websocket): an item handed to a parked client that has died reaches the next one",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);

    // The first to be parked asks for work and then falls silent: it reads nothing any more.
    HandDrivenPeer dead(consumer->getPort());
    dead.ws.write(
        boost::asio::buffer(
            Gem::Courtier::container_to_string(
                GCommandContainerT<GFaultyContainer>{GFrameKind::PULL},
                BIN
            )
        )
    );
    REQUIRE(eventually([&] { return consumer->parkedCount() == 1; }));

    // Behind it, a client that works.
    auto client = matchingClient<GFaultyContainer>(consumer->getPort());
    const std::jthread client_thread([client] { client->run(); });
    REQUIRE(eventually([&] { return consumer->parkedCount() == 2; }));

    // One item: one wake-up, for the caller that has waited longest -- the dead one.
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(0, fault_mode::NONE));
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });

    const bool done = batch.wait_for(PATIENCE) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        consumer->stopServer(); // releases the waiting batch, so the test ends
        batch.wait();
    }
    CHECK(items.front()->is_processed());
    CHECK(consumer->requeuedCount() == 1); // taken back from the dead client, once
    client->flagCloseRequested();
}

/******************************************************************************/
// A result arrives and cannot be applied to its slot (the library's seam throws). The slot may be
// half-written by then, so it must not go out to a client again as it is: it ends as failed, and
// the submission policy deals with it like with an evaluation that failed. The client is not
// refused, and nothing is put back.
TEST_CASE(
    "courtier(websocket): a result that cannot be applied ends its item as failed",
    "[courtier][websocket][liveness][net]"
) {
    const bool standard = GENERATE(true, false);
    INFO((standard ? "the seam throws a std::exception" : "the seam throws an int"));
    checkInsThrowStandard.store(standard);
    auto consumer = startedServer<GSimpleContainer>(/*n_threads=*/2);
    auto client = matchingClient<GSimpleContainer>(consumer->getPort());
    const std::jthread client_thread([client] { client->run(); });

    checkInsToFail.store(1); // the first result that comes back
    std::vector<std::unique_ptr<GSimpleContainer>> items;
    items.push_back(std::make_unique<GSimpleContainer>(0));
    items.push_back(std::make_unique<GSimpleContainer>(1));
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GSimpleContainer>>(items.data(), items.size()),
            GSubmissionPolicy::clone_on_partial_return()
        );
    });

    const bool done = batch.wait_for(PATIENCE) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        consumer->stopServer(); // releases the waiting batch, so the test ends
        batch.wait();
    }
    CHECK(checkInsToFail.load() == 0);          // the failure did happen
    CHECK(consumer->requeuedCount() == 0);      // the failed slot was not handed out again
    CHECK(items.front()->is_processed());       // both slots are filled, the failed one from the
    CHECK(items.back()->is_processed());        // one that succeeded
    CHECK(consumer->getNActiveSessions() == 1); // and the client is still there
    client->flagCloseRequested();
}

/******************************************************************************/
// A message whose encoding or decoding takes longer than the idle limit must not cost its
// connection: while one end works on a message it still has to answer the other end's pings. Each
// case makes one of the four codec runs of every item take three ping intervals and checks that
// no connection is lost, that every item is evaluated once and that every result arrives.

namespace {

/** @brief One case of slow codec work: which run is slow, and the shape of the server. */
struct SlowCodecCase {
    CodecRun run;              ///< The codec run of every item that is slow
    std::size_t serverThreads; ///< The server's io threads (nThreads)
    std::size_t clients;       ///< How many clients take part, each with its own connection
};

/** @brief Runs one batch of one item per client with the given run slow, and checks that every
 *  connection survives it. */
// NOLINTNEXTLINE(readability-function-size) -- one linear scenario; the count comes from the assertion macros
void runSlowCodecCase(const SlowCodecCase &c) {
    slowRun.store(c.run);
    slowRunsDone.store(0);
    evaluations.store(0);
    auto log = std::make_shared<CapturedLog>();
    GEM_LOGGER.addLogTarget(log);

    auto consumer = startedServer<GSlowCodecItem>(c.serverThreads);
    std::vector<std::shared_ptr<GWebsocketClientT<GSlowCodecItem>>> clients;
    std::vector<std::jthread> client_threads;
    clients.reserve(c.clients);
    client_threads.reserve(c.clients);
    for(std::size_t i = 0; i < c.clients; ++i) {
        clients.push_back(matchingClient<GSlowCodecItem>(consumer->getPort()));
        client_threads.emplace_back([client = clients.back()] { client->run(); });
    }
    REQUIRE(eventually([&] { return consumer->getNActiveSessions() == c.clients; }));

    std::vector<std::unique_ptr<GSlowCodecItem>> items;
    items.reserve(c.clients);
    for(std::size_t i = 0; i < c.clients; ++i) {
        items.push_back(std::make_unique<GSlowCodecItem>(i));
    }
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GSlowCodecItem>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });

    // Every slow run may wait for the others (on a server with fewer threads than clients),
    // plus the time the rest of the round trip needs.
    const auto patience = static_cast<int>(c.clients) * SLOW_CODEC + PATIENCE;
    const bool done = batch.wait_for(patience) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        consumer->stopServer(); // releases the waiting batch, so the test ends
        batch.wait();
    }

    CHECK(slowRunsDone.load() >= 1); // the case did exercise the slow run
    for(const auto &item : items) {
        CHECK(item->is_processed());
    }
    CHECK(evaluations.load() == static_cast<int>(c.clients)); // each item evaluated once
    CHECK(consumer->requeuedCount() == 0);                    // nothing was taken back
    CHECK(consumer->getNActiveSessions() == c.clients);       // no connection was lost
    CHECK_FALSE(log->hasLineWith({"the idle limit of"}));     // and no end gave up on the other
    for(const auto &client : clients) {
        CHECK(client->losses() == 0); // not even one that the client made good by connecting again
    }

    slowRun.store(CodecRun::NONE);
    GEM_LOGGER.resetLogTargets();
    for(auto &client : clients) {
        client->flagCloseRequested();
    }
}

} /* anonymous namespace */

TEST_CASE(
    "courtier(websocket): a server that encodes a work item slowly keeps its client",
    "[courtier][websocket][liveness][net][slowcodec]"
) {
    runSlowCodecCase({.run = CodecRun::SERVER_ENCODES, .serverThreads = 2, .clients = 1});
}

TEST_CASE(
    "courtier(websocket): a client that decodes a work item slowly keeps its server",
    "[courtier][websocket][liveness][net][slowcodec]"
) {
    runSlowCodecCase({.run = CodecRun::CLIENT_DECODES, .serverThreads = 2, .clients = 1});
}

TEST_CASE(
    "courtier(websocket): a client that encodes its result slowly keeps its server",
    "[courtier][websocket][liveness][net][slowcodec]"
) {
    runSlowCodecCase({.run = CodecRun::CLIENT_ENCODES, .serverThreads = 2, .clients = 1});
}

TEST_CASE(
    "courtier(websocket): a server that decodes a result slowly keeps its client",
    "[courtier][websocket][liveness][net][slowcodec]"
) {
    runSlowCodecCase({.run = CodecRun::SERVER_DECODES, .serverThreads = 2, .clients = 1});
}

TEST_CASE(
    "courtier(websocket): a server with one io thread that encodes slowly keeps all its clients",
    "[courtier][websocket][liveness][net][slowcodec]"
) {
    runSlowCodecCase({.run = CodecRun::SERVER_ENCODES, .serverThreads = 1, .clients = 2});
}

TEST_CASE(
    "courtier(websocket): more sessions encode slowly than the server has io threads",
    "[courtier][websocket][liveness][net][slowcodec]"
) {
    runSlowCodecCase({.run = CodecRun::SERVER_ENCODES, .serverThreads = 2, .clients = 3});
}

/******************************************************************************/
// A connection that is lost: the client connects again (through a relay that cuts or freezes the
// connection) and the batch is done.

TEST_CASE(
    "courtier(websocket): a result cut in the middle arrives once after the client connects again",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GRandomNumberContainer>(/*n_threads=*/2);
    Gem::Courtier::Tests::GTcpRelay relay(consumer->getPort());
    auto client = matchingClient<GRandomNumberContainer>(relay.port());
    std::atomic<int> n_evaluated{0};
    const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatch>(1);
    client->setItemProcessor([&n_evaluated, release](GRandomNumberContainer &item) {
        n_evaluated.fetch_add(1);
        release->wait_for(PATIENCE);
        item.markAsProcessedWith({true});
    });
    const std::jthread client_thread([client] { client->run(); });

    std::vector<std::unique_ptr<GRandomNumberContainer>> items;
    items.push_back(std::make_unique<GRandomNumberContainer>(100'000)); // a RETURN of about 800 kB
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GRandomNumberContainer>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });
    REQUIRE(eventually([&] { return n_evaluated.load() == 1; }));
    relay.cutAfter(Gem::Courtier::Tests::relayDirection::TO_SERVER, 400'000);
    release->count_down();

    const bool done = batch.wait_for(2 * PATIENCE) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        consumer->stopServer();
        batch.wait();
    }
    CHECK(n_evaluated.load() == 1); // the result was sent again, the item not evaluated again
    CHECK(client->losses() == 1);
    CHECK(client->resultsResent() == 1);
    CHECK(relay.nConnections() == 2);
    client->flagCloseRequested();
}

TEST_CASE(
    "courtier(websocket): a client whose connection goes silent connects again",
    "[courtier][websocket][liveness][net]"
) {
    auto consumer = startedServer<GFaultyContainer>(/*n_threads=*/2);
    Gem::Courtier::Tests::GTcpRelay relay(consumer->getPort());
    auto client = matchingClient<GFaultyContainer>(relay.port());
    const std::jthread client_thread([client] { client->run(); });
    const ClosesAtEnd close_at_end(client); // also when a REQUIRE below fails
    REQUIRE(eventually([&] { return consumer->connectedClients() == 1U; })); // it has asked

    relay.freeze(); // half-open at both ends: nothing passes, nothing is closed
    REQUIRE(eventually([&] { return relay.nConnections() == 2; })); // it connects again
    CHECK(client->losses() == 1);

    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.reserve(3);
    for(std::size_t i = 0; i < 3; ++i) {
        items.push_back(std::make_unique<GFaultyContainer>(i, fault_mode::NONE));
    }
    auto batch = std::async(std::launch::async, [&] {
        consumer->processBatch(
            std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
            GSubmissionPolicy::full_success_or_fatal()
        );
    });
    const bool done = batch.wait_for(PATIENCE) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        consumer->stopServer();
        batch.wait();
    }
}
