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
 * Multi-process exerciser for the courtier MPI consumer with LARGE work items, to cover the path
 * that the former fixed 20 KB MPI message-size cap (GMPICONSUMERMAXMESSAGESIZE, since removed) would
 * have rejected. Each work item carries thousands of doubles, so its serialized wire message far
 * exceeds 20 KB; the consumer must still deliver it both ways (master -> worker -> master) now that
 * receives are sized dynamically via MPI_Iprobe + MPI_Get_count.
 *
 * Like GCourtierMpiExerciser, MPI fixes the rank layout at launch, so this is launched by CTest via
 * `mpirun -np N`: rank 0 is the master (serves work and drives the batch), every other rank is a
 * worker. The master prints OK / FAIL and returns 0 only when every (large) item made the full
 * round-trip AND a single item genuinely serializes to more than the old cap (so the test is proven
 * to exercise the formerly-rejected size).
 */

#include "common/GGlobalDefines.hpp"
#include "weft/GBinaryArchive.hpp" // Gem::Weft::GBinary[IO]Archive

#ifdef GENEVA_BUILD_WITH_MPI_CONSUMER

#include <cstddef>
#include <memory>
#include <print>
#include <span>
#include <sstream>
#include <vector>


#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GMPIConsumerT.hpp"

namespace c2 = Gem::Courtier;
using Gem::Courtier::GRandomNumberContainer;

namespace {

// The former fixed MPI message-size cap. A payload above this is exactly what could not be delivered
// before the cap was removed.
constexpr std::size_t OLD_MPI_CAP_BYTES = 1024 * 20;

// Each item holds this many doubles. At ~8 bytes per double in a binary archive this is ~40 KB --
// comfortably above OLD_MPI_CAP_BYTES even before the command-container / archive overhead.
constexpr std::size_t DOUBLES_PER_ITEM = 5000;

// A small number of items keeps the test quick while still spreading work across the worker ranks.
constexpr std::size_t N_ITEMS = 8;

/** @brief Serialized size (binary) of one work item, used to prove the payload exceeds the old cap. */
std::size_t serialized_size_of_one_item() {
    GRandomNumberContainer probe(DOUBLES_PER_ITEM);
    Gem::Weft::GBinaryOArchive oa;
    oa &Gem::Weft::make_nvp("item", probe);
    return oa.str().size();
}

} // namespace

int main(int argc, char **argv) {
    auto consumer = std::make_shared<c2::GMPIConsumerT<GRandomNumberContainer>>(&argc, &argv);

    if(consumer->isMasterNode()) {
        // Confirm the payload really is larger than the old cap, so a pass genuinely exercises the
        // path that the 20 KB cap would have rejected (rather than silently using tiny items).
        const std::size_t item_bytes = serialized_size_of_one_item();
        if(item_bytes <= OLD_MPI_CAP_BYTES) {
            std::println("FAIL: per-item payload is only {} bytes, not above the old cap of {}; "
                         "the test would not exercise the large-message path",
                         item_bytes, OLD_MPI_CAP_BYTES);
            consumer->startServer();
            consumer->stopServer();
            return 1;
        }

        consumer->startServer();

        std::vector<std::unique_ptr<GRandomNumberContainer>> items;
        items.reserve(N_ITEMS);
        for(std::size_t i = 0; i < N_ITEMS; ++i) {
            items.push_back(std::make_unique<GRandomNumberContainer>(DOUBLES_PER_ITEM));
        }

        consumer->processBatch(
            std::span<std::unique_ptr<GRandomNumberContainer>>(items.data(), items.size()),
            c2::GSubmissionPolicy::full_success_or_fatal()
        );

        consumer->stopServer();

        std::size_t processed = 0;
        for(const auto &it : items) {
            if(it && it->is_processed()) {
                ++processed;
            }
        }

        if(processed == N_ITEMS) {
            std::println("OK: {}/{} large items ({} bytes each, > old cap {}) processed over MPI ({} ranks)",
                         processed, N_ITEMS, item_bytes, OLD_MPI_CAP_BYTES, consumer->getCommSize());
            return 0;
        }
        std::println("FAIL: only {}/{} large items processed", processed, N_ITEMS);
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
