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

// The end of a client's run and the evaluations it has started: run() returns only once no
// evaluation of the client runs any more, also when the client halts while one runs. A program
// that uses something in its item processor that it ends after run() (an MPI cascade releases its
// group then) relies on it.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "GDemoProcessingContainers.hpp"
#include "common/concurrency/GCompletionLatch.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GPeerClientT.hpp"
#include "courtier/transport/GWebsocketPeerTransport.hpp"

#include "GInProcessTestSupport.hpp"

using Gem::Common::Concurrency::GCompletionLatch;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::Tests::eventually;
using namespace std::chrono_literals;

namespace {

/** @brief How long a test waits for something that must happen */
constexpr auto PATIENCE = 30s;

/**
 * A websocket client (as GWebsocketClientT, which is final) that halts when the test says so,
 * through its custom halt condition, and counts how often it has been told to halt since: a count
 * of 1 says that the client has seen the halt and has begun to wind up (its halt timer then stops).
 */
class HaltingClient final : public Gem::Courtier::Consumers::GPeerClientT<GFaultyContainer> {
public:
    /** @brief A client of the server @p config names @param config The server's configuration */
    explicit HaltingClient(const Gem::Courtier::GWebsocketConsumerConfig &config)
      : Gem::Courtier::Consumers::GPeerClientT<GFaultyContainer>(
            std::make_unique<Gem::Courtier::Consumers::GWebsocketConnector>(
                config.ip,
                config.port,
                Gem::Courtier::Consumers::GWebsocketPeer::GOptions{
                    .name = "halting client",
                    .mode = config.serializationMode
                }
            ),
            Gem::Courtier::Consumers::GPeerClientOptions{
                .name = "halting client",
                .mode = config.serializationMode
            }
        ) { /* nothing */
    }

    /** @brief From now on the client halts */
    void haltNow() {
        halt_ = true;
    }
    /** @brief How often the client has been told to halt since haltNow() */
    [[nodiscard]] std::size_t haltsSeen() const {
        return halts_seen_.load();
    }

protected:
    /** @brief Halts after haltNow(), and counts it */
    bool customHalt() override {
        if(not halt_) {
            return false;
        }
        ++halts_seen_;
        return true;
    }

private:
    std::atomic<bool> halt_{false};          ///< Whether to halt
    std::atomic<std::size_t> halts_seen_{0}; ///< How often the client was told to halt
};

} /* anonymous namespace */

/******************************************************************************/
TEST_CASE(
    "GPeerClientT: a client that halts returns from run() only after its running evaluation",
    "[courtier][peer][client]"
) {
    Gem::Courtier::GWebsocketConsumerConfig config{
        .ip = "127.0.0.1",
        .serializationMode = Gem::Common::serializationMode::GEM_BINARY,
        .nThreads = 2,
    };
    auto consumer = std::make_shared<Gem::Courtier::GWebsocketConsumerT<GFaultyContainer>>(config);
    consumer->startServer();
    config.port = consumer->getPort();
    auto client = std::make_shared<HaltingClient>(config);
    // Shared with the processor, which may run after this test has left (see GCompletionLatch)
    auto arrived = std::make_shared<GCompletionLatch>(1); // the evaluation has begun
    auto gate = std::make_shared<GCompletionLatch>(1);    // the test lets it end
    client->setItemProcessor([arrived, gate](GFaultyContainer &item) {
        arrived->count_down();
        gate->wait_for(PATIENCE);
        item.markAsProcessedWith({true});
    });

    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.push_back(std::make_unique<GFaultyContainer>(0));
    std::atomic<bool> returned{false};
    std::atomic<bool> batch_done{false};
    {
        Gem::Common::Concurrency::GThreadGroup threads;
        threads.create_thread([client, &returned] {
            try {
                client->run();
            }
            catch(...) { // NOLINT(bugprone-empty-catch): the checks below say what is wrong
            }
            returned = true;
        });
        threads.create_thread([&consumer, &items, &batch_done] {
            consumer->processBatch(
                std::span(items.data(), items.size()),
                Gem::Courtier::GSubmissionPolicy::full_success_or_fatal()
            );
            batch_done = true;
        });

        CHECK(arrived->wait_for(PATIENCE));
        client->haltNow();
        // The client has seen the halt and wound up as far as it may while the evaluation runs.
        // run() must not return now: a wrong client would within a few io turns, so the bad
        // event is given a second to happen (a negative cannot be waited for otherwise)
        CHECK(eventually([&] { return client->haltsSeen() >= 1; }, PATIENCE));
        CHECK_FALSE(eventually([&] { return returned.load(); }, 1s));

        gate->count_down();
        CHECK(eventually([&] { return returned.load(); }, PATIENCE));
        CHECK(eventually([&] { return batch_done.load(); }, PATIENCE));
        consumer->stopServer(); // ends a batch that would wait for ever
        threads.join_all();
    }
    CHECK(items.front()->is_processed());
}
