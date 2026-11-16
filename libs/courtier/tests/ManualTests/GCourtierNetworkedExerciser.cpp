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
 * Standalone, multi-PROCESS exerciser for the courtier websocket consumer. Unlike the in-process
 * loopback unit tests, this runs the server and the clients as separate OS processes (typically
 * launched together by startLocalJobs.sh), so it validates the real cross-process behaviour.
 *
 *   server : GCourtierNetworkedExerciser [-c beast] [--beast_port P] [--nWorkItems N] [--faultEvery K]
 *   client : GCourtierNetworkedExerciser  --client [-c beast] [--beast_ip HOST] [--beast_port P]
 *
 * The server submits a batch through the courtier span+policy executor and prints OK/FAIL plus the
 * processed count; with --faultEvery K, every K-th item throws and the clone-on-partial-return
 * policy is used (so every slot must still end up processed).
 */

#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/GParserBuilder.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
namespace c2 = Gem::Courtier;
namespace ccons = Gem::Courtier::Consumers;

namespace {

constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

int runServer(unsigned short port, std::size_t n, std::size_t fault_every) {
    auto consumer = std::make_shared<c2::GWebsocketConsumerT<GFaultyContainer>>(
        c2::GWebsocketConsumerConfig{.port = port, .serializationMode = BIN}
    );
    consumer->startServer();
    port = consumer->getPort();
    std::cout << "[server] courtier websocket consumer listening on port " << port
              << "; submitting " << n << " items (faultEvery=" << fault_every << ")\n"
              << std::flush;

    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        const bool faulty = fault_every > 0 && (i % fault_every == 0);
        items.push_back(std::make_unique<GFaultyContainer>(
            i, faulty ? fault_mode::THROW_PROCESSING : fault_mode::NONE
        ));
    }

    const auto policy = fault_every > 0
        ? c2::GSubmissionPolicy::clone_on_partial_return()
        : c2::GSubmissionPolicy::full_success_or_fatal();

    consumer->processBatch(
        std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
        policy
    );
    consumer->stopServer();

    std::size_t processed = 0;
    for(const auto &it : items) {
        if(it && it->is_processed()) {
            ++processed;
        }
    }

    if(processed == n) {
        std::cout << "[server] OK: " << processed << "/" << n << " items processed\n" << std::flush;
        return 0;
    }
    std::cout << "[server] FAIL: only " << processed << "/" << n << " items processed\n" << std::flush;
    return 1;
}

int runClient(const std::string &ip, unsigned short port) {
    std::cout << "[client] connecting to " << ip << ":" << port << " (beast)\n" << std::flush;
    auto client = std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(
        c2::GWebsocketConsumerConfig{.ip = ip, .port = port, .serializationMode = BIN}
    );
    client->run();
    return 0;
}

} /* anonymous namespace */

int main(int argc, char **argv) {
    std::string consumer;
    bool is_client = false;
    std::string beast_ip;
    unsigned short beast_port = 0;
    std::size_t n = 0;
    std::size_t fault_every = 0;

    // Use Geneva's own command-line parser. The option names (--consumer, --beast_*) match what
    // real Geneva programs expose, so this exerciser is drop-in with startLocalJobs.sh.
    Gem::Common::GParserBuilder gpb;
    gpb.registerCLParameter(
        "consumer,c",
        consumer,
        std::string("beast"),
        "the consumer to exercise: beast (the websocket consumer)"
    );
    gpb.registerCLParameter(
        "client",
        is_client,
        false,
        "run as a client (default: server)",
        Gem::Common::GCL_IMPLICIT_ALLOWED,
        true
    );
    gpb.registerCLParameter("beast_ip", beast_ip, std::string("127.0.0.1"),
                            "server ip/host the beast client connects to");
    gpb.registerCLParameter("beast_port", beast_port, static_cast<unsigned short>(10000),
                            "the beast server's TCP port");
    gpb.registerCLParameter("nWorkItems", n, 200uz,
                            "number of work items the server submits");
    gpb.registerCLParameter("faultEvery", fault_every, 0uz,
                            "every K-th item throws (0 = none); switches to clone-on-partial-return");

    if(gpb.parseCommandLine(argc, argv) == Gem::Common::GCL_HELP_REQUESTED) {
        return 0;
    }

    if(consumer != "beast") {
        std::cerr << "Unknown consumer \"" << consumer
                  << "\"; the consumer that exists is: beast\n";
        return 1;
    }

    if(is_client) {
        return runClient(beast_ip, beast_port);
    }
    return runServer(beast_port, n, fault_every);
}
