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

// A websocket server in a process of its own, for the tests that need a second server process
// (GCourtierForeignResultTests): it serves one batch of one work item, writes its port to stdout
// and runs until its stdin is closed.
//
// Usage: GCourtierHelperServer <stored number of the item>

#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"

// A test helper: an exception ends it, which the test sees
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char **argv) {
    if(argc != 2) {
        std::cerr << "Usage: GCourtierHelperServer <stored number of the item>\n";
        return 2;
    }
    const std::vector<std::string> args(argv, argv + argc);
    const std::size_t stored = std::stoul(args.at(1));

    auto consumer =
        std::make_shared<Gem::Courtier::GWebsocketConsumerT<Gem::Courtier::GFaultyContainer>>(
            Gem::Courtier::GWebsocketConsumerConfig{
                .ip = "127.0.0.1",
                .serializationMode = Gem::Common::serializationMode::GEM_BINARY,
                .nThreads = 2
            }
        );
    consumer->setTimeoutTreatment(Gem::Courtier::timeoutTreatment::wait_indefinitely);
    consumer->startServer();
    std::cout << consumer->getPort() << '\n' << std::flush;

    std::vector<std::unique_ptr<Gem::Courtier::GFaultyContainer>> items;
    items.push_back(std::make_unique<Gem::Courtier::GFaultyContainer>(stored));
    const std::jthread batch([&consumer, &items] {
        auto policy = Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
        policy.fatal_on_none_returned = false;
        auto clone_template = std::make_unique<Gem::Courtier::GFaultyContainer>(0);
        clone_template->markAsProcessedWith({true});
        consumer->processBatch(
            std::span(items.data(), items.size()),
            policy,
            std::move(clone_template)
        );
    });

    std::string line;
    while(std::getline(std::cin, line)) {
        // nothing: the test closes stdin when this server is to end
    }
    consumer->stopServer();
    return 0;
}
