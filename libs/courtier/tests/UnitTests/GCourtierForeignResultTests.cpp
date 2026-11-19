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

// A result that a client carries from one server process to another -- it lost its connection, and
// the server it reaches next is another process -- is dropped there as foreign, and that server's
// batch completes with its own values. The first server runs in a process of its own
// (GCourtierHelperServer): a server tells its items from those of another process by a salt that is
// drawn once per process.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "GDemoProcessingContainers.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp"

#include "GTcpRelay.hpp"

// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)
extern char **environ; // posix_spawn wants it

using Gem::Courtier::GFaultyContainer;
using namespace std::chrono_literals;

namespace {

constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

/** @brief Polls @p condition for at most @p patience @return Whether it held */
template <typename condition_type>
bool eventually(condition_type condition, std::chrono::milliseconds patience = 10s) {
    const auto give_up_at = std::chrono::steady_clock::now() + patience;
    while(std::chrono::steady_clock::now() < give_up_at) {
        if(condition()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

/** @brief GCourtierHelperServer in a process of its own; it ends when this object goes */
class ChildServer {
public:
    /** @brief Starts it and reads its port @param stored The stored number of its one item */
    explicit ChildServer(std::size_t stored) {
        std::array<int, 2> to_child{-1, -1};
        std::array<int, 2> from_child{-1, -1};
        if(::pipe(to_child.data()) != 0 || ::pipe(from_child.data()) != 0) {
            return;
        }
        posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_addclose(&actions, to_child[1]);
        ::posix_spawn_file_actions_addclose(&actions, from_child[0]);
        std::string program = GCOURTIER_HELPER_SERVER;
        std::string argument = std::to_string(stored);
        std::array<char *, 3> argv{program.data(), argument.data(), nullptr};
        const int spawned =
            ::posix_spawn(&pid_, program.c_str(), &actions, nullptr, argv.data(), environ);
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(to_child[0]);
        ::close(from_child[1]);
        stdin_ = to_child[1];
        if(spawned != 0) {
            pid_ = -1;
            ::close(from_child[0]);
            return;
        }
        // Its first line is the port; at most 10 s
        std::string line;
        pollfd waiting{.fd = from_child[0], .events = POLLIN, .revents = 0};
        const auto give_up_at = std::chrono::steady_clock::now() + 10s;
        char c = 0;
        while(std::chrono::steady_clock::now() < give_up_at && ::poll(&waiting, 1, 100) >= 0) {
            if((static_cast<unsigned>(waiting.revents) & static_cast<unsigned>(POLLIN)) == 0U) {
                continue;
            }
            if(::read(from_child[0], &c, 1) != 1 || c == '\n') {
                break;
            }
            line.push_back(c);
        }
        ::close(from_child[0]);
        if(not line.empty()) {
            port_ = static_cast<unsigned short>(std::stoul(line));
        }
    }
    /** @brief Closes its stdin, so that it ends; kills it if it does not within 10 s */
    ~ChildServer() {
        ::close(stdin_);
        if(pid_ <= 0) {
            return;
        }
        int status = 0;
        const auto give_up_at = std::chrono::steady_clock::now() + 10s;
        while(::waitpid(pid_, &status, WNOHANG) == 0) {
            if(std::chrono::steady_clock::now() > give_up_at) {
                ::kill(pid_, SIGKILL);
                ::waitpid(pid_, &status, 0);
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
    }
    ChildServer(const ChildServer &) = delete;
    ChildServer(ChildServer &&) = delete;
    ChildServer &operator=(const ChildServer &) = delete;
    ChildServer &operator=(ChildServer &&) = delete;

    /** @brief @return Its port; 0 if it did not start */
    [[nodiscard]] unsigned short port() const {
        return port_;
    }

private:
    pid_t pid_ = -1;          ///< The child
    int stdin_ = -1;          ///< Its stdin
    unsigned short port_ = 0; ///< Where it listens
};

} /* anonymous namespace */

TEST_CASE(
    "courtier(websocket): a result carried to another server process is dropped there",
    "[courtier][websocket][net]"
) {
    constexpr std::size_t FIRST_ITEM = 100;
    constexpr std::size_t SECOND_ITEM = 200;
    const ChildServer first(FIRST_ITEM);
    REQUIRE(first.port() != 0);

    auto second = std::make_shared<Gem::Courtier::GWebsocketConsumerT<GFaultyContainer>>(
        Gem::Courtier::GWebsocketConsumerConfig{
            .ip = "127.0.0.1",
            .serializationMode = BIN,
            .nThreads = 2
        }
    );
    second->startServer();
    Gem::Courtier::Tests::GTcpRelay relay(first.port());

    // The client evaluates in this process; it counts per item, and its first evaluation waits
    std::mutex mutex;
    std::map<std::size_t, int> evaluated;
    const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    auto client = std::make_shared<Gem::Courtier::Consumers::GWebsocketClientT<GFaultyContainer>>(
        Gem::Courtier::GWebsocketConsumerConfig{
            .ip = "127.0.0.1",
            .port = relay.port(),
            .serializationMode = BIN,
            // No keep-alive on either side (GCourtierHelperServer neither): the next byte towards
            // the server after cutAfter() is the result's, never a ping or a pong
            .pingInterval = 0
        }
    );
    client->setItemProcessor([&mutex, &evaluated, release](GFaultyContainer &item) {
        {
            const std::scoped_lock lock(mutex);
            ++evaluated[item.get_stored_number()];
        }
        if(item.get_stored_number() == FIRST_ITEM) {
            release->wait_for(10s);
        }
        item.markAsProcessedWith({true});
    });
    const std::jthread client_thread([client] { client->run(); });
    auto count = [&](std::size_t item) {
        const std::scoped_lock lock(mutex);
        return evaluated[item];
    };
    REQUIRE(eventually([&] { return count(FIRST_ITEM) == 1; }));

    // The result is sent and lost with the connection (the relay cuts at its first byte); the next
    // connection reaches the server of this process
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(SECOND_ITEM));
    auto batch = std::async(std::launch::async, [&] {
        second->processBatch(
            std::span(items.data(), items.size()),
            Gem::Courtier::GSubmissionPolicy::full_success_or_fatal()
        );
    });
    relay.setUpstream(second->getPort());
    relay.cutAfter(Gem::Courtier::Tests::relayDirection::TO_SERVER, 0);
    release->count_down();

    const bool done = batch.wait_for(15s) == std::future_status::ready;
    CHECK(done);
    if(not done) {
        second->stopServer();
    }
    batch.get();
    CHECK(count(SECOND_ITEM) == 1); // its own item was evaluated ...
    CHECK(items.front()->is_processed());
    CHECK(second->foreignReturnCount() == 1); // ... and the result from the other process dropped
    CHECK(client->resultsResent() == 1);
    client->flagCloseRequested();
}
