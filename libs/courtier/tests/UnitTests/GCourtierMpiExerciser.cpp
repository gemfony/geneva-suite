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
 * Multi-process exerciser for the courtier MPI consumer. It cannot be an in-process Catch2 test
 * (MPI fixes the rank layout at launch), so it is launched by CTest via `mpirun -np N`: rank 0 is
 * the master (it registers the consumer, serves work and drives the batch through the span+policy
 * executor); every other rank is a worker. The master prints OK / FAIL and returns 0 only when
 * every item made the full master->worker->master round-trip.
 */

#include "common/GGlobalDefines.hpp"

#ifdef GENEVA_BUILD_WITH_MPI_CONSUMER

#include <cstddef>
#include <memory>
#include <print>
#include <vector>
#include <span>

#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GMPIConsumerT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
namespace c2 = Gem::Courtier;

int main(int argc, char **argv) {
    constexpr std::size_t N = 120;

    auto consumer = std::make_shared<c2::GMPIConsumerT<GFaultyContainer>>(&argc, &argv);

    if(consumer->isMasterNode()) {
        consumer->startServer();

        std::vector<std::unique_ptr<GFaultyContainer>> items;
        items.reserve(N);
        for(std::size_t i = 0; i < N; ++i) {
            items.push_back(std::make_unique<GFaultyContainer>(i, fault_mode::NONE));
        }

        consumer->processBatch(std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
                               c2::GSubmissionPolicy::full_success_or_fatal());

        consumer->stopServer();

        std::size_t processed = 0;
        for(const auto &it : items) {
            if(it && it->is_processed()) {
                ++processed;
            }
        }

        if(processed == N) {
            std::println("OK: {}/{} items processed over MPI ({} ranks)",
                         processed, N, consumer->getCommSize());
            return 0;
        }
        std::println("FAIL: only {}/{} items processed", processed, N);
        return 1;
    }

    // Worker ranks: serve the master until it broadcasts the stop signal.
    consumer->runWorker();
    return 0;
}

#else

int main() {
    // MPI consumer not built -- nothing to exercise.
    return 0;
}

#endif /* GENEVA_BUILD_WITH_MPI_CONSUMER */
