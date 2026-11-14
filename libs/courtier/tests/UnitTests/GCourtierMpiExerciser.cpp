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
 *
 *   GCourtierMpiExerciser [--scenario=<scenario>]
 *
 * Scenarios:
 *   clean          -- (the default) every item evaluates normally.
 *   throwing       -- every fifth item throws from its evaluation. The workers must survive, and
 *                     the clone-on-partial-return policy must leave every slot processed.
 *   never-started  -- the master builds its consumer and destroys it without starting the server
 *                     (run with a single rank). The process must end normally.
 *   low-thread-level -- the program initializes MPI itself, below MPI_THREAD_MULTIPLE (single
 *                     rank). The consumer must refuse to be built.
 *   zero-clean-interval -- a cleanup interval of 0 ms (single rank). The consumer must refuse to
 *                     be built: the cleanup thread would occupy a core.
 */

#include "common/GGlobalDefines.hpp"

#ifdef GENEVA_BUILD_WITH_MPI_CONSUMER

#include <cstddef>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <vector>

#include <mpi.h>

#include "common/GExceptions.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GMPIConsumerT.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
namespace c2 = Gem::Courtier;

namespace {

/** Prints the outcome of a single-rank check and turns it into the exit code. */
int verdict(bool ok, const char *what) {
    std::println("{}: {}", ok ? "OK" : "FAIL", what);
    return ok ? 0 : 1;
}

/** A program that initialized MPI below MPI_THREAD_MULTIPLE must be refused. Returns 77 (skipped)
 *  if the MPI library grants MPI_THREAD_MULTIPLE whatever is asked for. */
int lowThreadLevel(int argc, char **argv) {
    int provided = 0;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_SINGLE, &provided);
    bool refused = false;
    try {
        const c2::GMPIConsumerT<GFaultyContainer> consumer(&argc, &argv);
    }
    catch(const geneva_exception &) {
        refused = true;
    }
    MPI_Finalize();
    if(provided == MPI_THREAD_MULTIPLE) {
        std::println("SKIPPED: this MPI library grants MPI_THREAD_MULTIPLE when less is asked for");
        return 77;
    }
    return verdict(refused, "a consumer was refused on MPI initialized with a lower thread level");
}

/** A cleanup interval of zero must be refused. */
int zeroCleanInterval(int argc, char **argv) {
    c2::Consumers::MPIConsumerConfig config;
    config.masterCleanSessIntervalMSec = 0;
    bool refused = false;
    try {
        const c2::GMPIConsumerT<GFaultyContainer> consumer(&argc, &argv, config);
    }
    catch(const geneva_exception &) {
        refused = true;
    }
    // A constructor that throws leaves MPI initialized; a consumer that was built has finalized it.
    int finalized = 0;
    MPI_Finalized(&finalized);
    if(finalized == 0) {
        MPI_Finalize();
    }
    return verdict(refused, "a consumer with a cleanup interval of 0 ms was refused");
}

} // namespace

int main(int argc, char **argv) {
    constexpr std::size_t N = 120;

    std::string scenario;
    Gem::Common::GParserBuilder gpb;
    gpb.registerCLParameter(
        "scenario",
        scenario,
        std::string("clean"),
        "what to exercise: clean | throwing | never-started"
        " | low-thread-level"
        " | zero-clean-interval"
    );
    if(gpb.parseCommandLine(argc, argv) == Gem::Common::GCL_HELP_REQUESTED) {
        return 0;
    }
    if(scenario == "low-thread-level") {
        return lowThreadLevel(argc, argv);
    }
    if(scenario == "zero-clean-interval") {
        return zeroCleanInterval(argc, argv);
    }
    if(scenario != "clean" && scenario != "throwing" && scenario != "never-started") {
        std::println(stderr, "Unknown scenario: {}", scenario);
        return 2;
    }

    auto consumer = std::make_shared<c2::GMPIConsumerT<GFaultyContainer>>(&argc, &argv);

    if(scenario == "never-started") {
        // The consumer's destructor stops the server. With a server that never ran there is nothing
        // to join, and the destructor must not try.
        consumer.reset();
        std::println("OK: a master that was never started was destroyed");
        return 0;
    }

    if(consumer->isMasterNode()) {
        consumer->startServer();

        const bool throwing = (scenario == "throwing");
        std::vector<std::unique_ptr<GFaultyContainer>> items;
        items.reserve(N);
        for(std::size_t i = 0; i < N; ++i) {
            const bool faulty = throwing && (i % 5 == 0);
            items.push_back(
                std::make_unique<GFaultyContainer>(
                    i,
                    faulty ? fault_mode::THROW_PROCESSING : fault_mode::NONE
                )
            );
        }

        // A throwing item is flagged by the worker and returned; the tolerant policy then refills
        // its slot with a clone of a processed sibling. The clean scenario needs every evaluation.
        consumer->processBatch(
            std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
            throwing ? c2::GSubmissionPolicy::clone_on_partial_return()
                     : c2::GSubmissionPolicy::full_success_or_fatal()
        );

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
