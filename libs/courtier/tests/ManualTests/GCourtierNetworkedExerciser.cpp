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
 * Standalone, multi-PROCESS exerciser for the courtier socket consumers. Unlike the in-process
 * loopback unit tests, this runs the server and the clients as separate OS processes (typically
 * launched together by startCourtierJobs.sh), so it validates the real cross-process behaviour.
 *
 *   server : GCourtierNetworkedExerciser [-c asio|beast] [--asio_port P] [--beast_port P] [--nWorkItems N] [--faultEvery K]
 *   client : GCourtierNetworkedExerciser  --client  -c asio|beast [--asio_ip HOST] [--beast_ip HOST] [--asio_port P] [--beast_port P]
 *
 * The server submits a batch through the courtier span+policy executor and prints OK/FAIL plus the
 * processed count; with --faultEvery K, every K-th item throws and the clone-on-partial-return
 * policy is used (so every slot must still end up processed).
 */

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <span>

#include "common/GParserBuilder.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/transport/GAsioTransportT.hpp"
#include "courtier/transport/GWebsocketTransportT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GAsioConsumerT.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
namespace c2 = Gem::Courtier;
namespace ccons = Gem::Courtier::Consumers;

namespace {

constexpr auto BIN = Gem::Common::serializationMode::GEM_BINARY;

int run_server(const std::string &consumer, unsigned short port, std::size_t n, std::size_t fault_every) {
    // Build the chosen courtier consumer behind the common base type.
    std::shared_ptr<c2::GBaseConsumerT<GFaultyContainer>> base;
    std::shared_ptr<c2::GAsioConsumerT<GFaultyContainer>> asio;
    std::shared_ptr<c2::GWebsocketConsumerT<GFaultyContainer>> beast;
    if(consumer == "beast") {
        beast = std::make_shared<c2::GWebsocketConsumerT<GFaultyContainer>>(port, 0, BIN);
        base = beast;
    }
    else {
        asio = std::make_shared<c2::GAsioConsumerT<GFaultyContainer>>(port, 0, BIN);
        base = asio;
    }

    if(asio) {
        asio->startServer();
        port = asio->getPort();
    }
    else {
        beast->startServer();
        port = beast->getPort();
    }
    std::cout << "[server] courtier " << consumer << " consumer listening on port " << port
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

    base->processBatch(std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()), policy);

    if(asio) {
        asio->stopServer();
    }
    else {
        beast->stopServer();
    }

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

int run_client(const std::string &consumer, const std::string &ip, unsigned short port) {
    std::cout << "[client] connecting to " << ip << ":" << port << " (" << consumer << ")\n"
              << std::flush;
    if(consumer == "beast") {
        auto client =
            std::make_shared<ccons::GWebsocketClientT<GFaultyContainer>>(ip, port, BIN, false);
        client->run();
    }
    else {
        auto client =
            std::make_shared<ccons::GAsioConsumerClientT<GFaultyContainer>>(ip, port, BIN, 50);
        client->run();
    }
    return 0;
}

} /* anonymous namespace */

int main(int argc, char **argv) {
    std::string consumer;
    bool is_client = false;
    std::string asio_ip;
    std::string beast_ip;
    unsigned short asio_port = 0;
    unsigned short beast_port = 0;
    std::size_t n = 0;
    std::size_t fault_every = 0;

    // Use Geneva's own command-line parser. The consumer-specific option names (--asio_*/--beast_*)
    // match what real Geneva programs expose, so this exerciser is drop-in with startLocalJobs.sh.
    Gem::Common::GParserBuilder gpb;
    gpb.registerCLParameter("consumer,c", consumer, std::string("asio"),
                            "the consumer to exercise: asio or beast");
    gpb.registerCLParameter("client", is_client, false,
                            "run as a client (default: server)",
                            Gem::Common::GCL_IMPLICIT_ALLOWED, true);
    gpb.registerCLParameter("asio_ip", asio_ip, std::string("127.0.0.1"),
                            "server ip/host the asio client connects to");
    gpb.registerCLParameter("asio_port", asio_port, static_cast<unsigned short>(10000),
                            "the asio server's TCP port");
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

    const bool beast = (consumer == "beast");
    const unsigned short port = beast ? beast_port : asio_port;
    const std::string ip = beast ? beast_ip : asio_ip;

    if(is_client) {
        return run_client(consumer, ip, port);
    }
    return run_server(consumer, port, n, fault_every);
}
