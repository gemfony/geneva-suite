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
// evaluation of the client runs any more, also when the client is asked to close while one runs.
// A program that uses something in its item processor that it ends after run() (an MPI cascade
// releases its group then) relies on it.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp"

#include "GInProcessTestSupport.hpp"

using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::Tests::eventually;
using namespace std::chrono_literals;

namespace {

/** @brief How long a test waits for something that must happen */
constexpr auto PATIENCE = 30s;

/** @brief A gate an item processor waits at until the test opens it */
struct Gate {
    std::promise<void> opened;                                     ///< Set to open it
    std::shared_future<void> waited = opened.get_future().share(); ///< Where processors wait
    std::atomic<bool> arrived{false};                              ///< An evaluation waits
};

} /* anonymous namespace */

/******************************************************************************/
TEST_CASE(
    "GPeerClientT: a client asked to close returns from run() only after its running evaluation",
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
    auto client =
        std::make_shared<Gem::Courtier::Consumers::GWebsocketClientT<GFaultyContainer>>(config);
    auto gate = std::make_shared<Gate>();
    client->setItemProcessor([gate](GFaultyContainer &item) {
        gate->arrived = true;
        gate->waited.wait_for(PATIENCE);
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

        const bool arrived = eventually([&] { return gate->arrived.load(); }, PATIENCE);
        CHECK(arrived);
        client->flagCloseRequested();
        // The client looks for a halt every GCLIENTHALTPOLLINTERVAL: after that and a margin it has
        // seen the request and would have ended, were it not for the evaluation at the gate
        std::this_thread::sleep_for(Gem::Courtier::GCLIENTHALTPOLLINTERVAL + 500ms);
        CHECK_FALSE(returned.load());

        gate->opened.set_value();
        CHECK(eventually([&] { return returned.load(); }, PATIENCE));
        CHECK(eventually([&] { return batch_done.load(); }, PATIENCE));
        consumer->stopServer(); // ends a batch that would wait for ever
        threads.join_all();
    }
    CHECK(items.front()->is_processed());
}
