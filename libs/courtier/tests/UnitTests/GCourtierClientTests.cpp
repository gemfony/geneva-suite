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
 * Tests of the client pipeline shared by the socket clients (GPrefetchingClientT) that need no
 * server: a probe client stands in for a transport and drives the pipeline's timers by hand.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <thread>

#include <boost/asio.hpp>

#include "courtier/GCommandContainerT.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/transport/GPrefetchingClientT.hpp"

using Gem::Courtier::GCommandContainerT;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::Consumers::GPrefetchingClientT;
using namespace std::chrono_literals;

namespace {

/** @brief A client without a transport. It supplies the hooks the pipeline calls and lets a test
 *  arrange the order in which the pipeline's timer handlers run. */
class ProbeClient final
  : public GPrefetchingClientT<ProbeClient, GFaultyContainer>
  , public std::enable_shared_from_this<ProbeClient> {
    friend class GPrefetchingClientT<ProbeClient, GFaultyContainer>;
    using pipeline_base = GPrefetchingClientT<ProbeClient, GFaultyContainer>;

public:
    ProbeClient()
      : pipeline_base("ProbeClient", /*prefetch_depth=*/1) { /* nothing */
    }

    /**
     * @brief Arranges for the client to stop its timers at a moment when the halt-poll timer has
     * already expired and its handler is queued.
     *
     * The halt poll expires after one second. A second timer, which stops the pipeline's timers,
     * expires just before it. The calling thread then sleeps past both, so that the io context,
     * once it runs, finds both expired and queues their handlers together, the stop first. The stop
     * therefore runs while the halt-poll handler is queued: too late to cancel it.
     */
    void stopWhileHaltPollIsQueued() {
        this->start_halt_timer();
        stop_timer_.expires_after(900ms);
        stop_timer_.async_wait([this]([[maybe_unused]] boost::system::error_code ec) {
            this->stopPipelineTimers_();
        });
        std::this_thread::sleep_for(1100ms);
    }

    /** @brief Runs the io context for at most @p limit.
     *  @return Whether it ran out of work, i.e. whether a client's run() would have returned */
    [[nodiscard]] bool runsOutOfWork(std::chrono::milliseconds limit) {
        io_context_.run_for(limit);
        return io_context_.stopped();
    }

private:
    void run_() override { /* the tests drive the pipeline directly */
    }
    void refill_() { /* no transport to pull from */
    }
    void sendResultAndRefill_(GCommandContainerT<GFaultyContainer> /*container*/) { /* nothing */
    }
    void haltShutdown_() { /* no transport to close */
    }

    boost::asio::steady_timer stop_timer_{io_context_};
};

} /* anonymous namespace */

TEST_CASE(
    "courtier(client): a timer that had expired when the client stopped does not arm itself again",
    "[courtier][client]"
) {
    // A client stops its timers when it tears its transport down, so that its run() can return.
    // The handler of a timer that has already expired cannot be cancelled any more; it used to
    // re-arm the halt poll, and the client then never terminated.
    auto client = std::make_shared<ProbeClient>();
    client->stopWhileHaltPollIsQueued();
    CHECK(client->runsOutOfWork(1500ms));
}
