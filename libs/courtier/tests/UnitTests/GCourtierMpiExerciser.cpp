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
 * the submitter (its consumer serves work and drives the batch through the span+policy executor);
 * every other rank is a client and runs a GMPIConsumerWorkerNodeT. The master prints OK / FAIL and
 * returns 0 only when every item made the full master->client->master round-trip.
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
 *   idle-session   -- a master session that has not sent anything (single rank). Asking it
 *                     whether its send has completed must be harmless.
 *   undecodable    -- each worker fails to load the first such item it is sent. The workers must
 *                     survive, and the items come back to the master through the lease.
 *   refusal        -- (two ranks) the client tries to build the consumer and the submitter tries
 *                     to build the client; each must be refused.
 *   client-limits  -- (two ranks) a time limit of 1 ms is set on the client. It must warn that the
 *                     limit has no effect, and serve until every item is processed.
 *   item-processor -- (three ranks) each client is given an item processor that marks every item
 *                     with the result false (an item's own processing stores true). Every item
 *                     must come back processed by a processor.
 */

#include "common/GGlobalDefines.hpp"

#ifdef GENEVA_BUILD_WITH_MPI_CONSUMER

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <mpi.h>

#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierEnums.hpp" // processRole
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GMPIConsumerT.hpp"
#include "courtier/mpi/GMPIEnvironment.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::Consumers::GMPIEnvironment;
namespace c2 = Gem::Courtier;
using client_type = c2::Consumers::GMPIConsumerWorkerNodeT<GFaultyContainer>;

// The configuration of the MPI family is an aggregate: it takes designated initializers.
static_assert(std::is_aggregate_v<c2::Consumers::MPIConsumerConfig>);

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
        const GMPIEnvironment environment(&argc, &argv);
    }
    catch(const geneva_exception &) {
        refused = true;
    }
    MPI_Finalize(); // this program initialized MPI, so it finalizes it
    if(provided == MPI_THREAD_MULTIPLE) {
        std::println("SKIPPED: this MPI library grants MPI_THREAD_MULTIPLE when less is asked for");
        return 77;
    }
    return verdict(
        refused,
        "an MPI environment was refused on MPI initialized with a lower thread level"
    );
}

/** A cleanup interval of zero must be refused. */
int zeroCleanInterval(int argc, char **argv) {
    // The environment finalizes MPI when it goes out of scope, whether the consumer was built or
    // not.
    const auto environment = std::make_shared<GMPIEnvironment>(&argc, &argv);
    const c2::Consumers::MPIConsumerConfig config{.masterCleanSessIntervalMSec = 0};
    bool refused = false;
    try {
        const c2::GMPIConsumerT<GFaultyContainer> consumer(environment, config);
    }
    catch(const geneva_exception &) {
        refused = true;
    }
    return verdict(refused, "a consumer with a cleanup interval of 0 ms was refused");
}

/** A session that has not sent anything may be asked whether its send has completed. */
int idleSession(int argc, char **argv) {
    const GMPIEnvironment environment(&argc, &argv); // finalizes MPI when it goes out of scope
    bool completed = false;
    {
        c2::Consumers::GMPIConsumerSessionT<GFaultyContainer> session(
            environment.communicator(),
            MPI_Status{},
            std::string{},
            [] { return std::unique_ptr<GFaultyContainer>{}; },
            [](c2::GCommandContainerT<GFaultyContainer> &&) { /* nothing */ },
            Gem::Common::serializationMode::GEM_BINARY,
            false
        );
        completed = session.isCompleted();
    }
    return verdict(completed, "a session that has not sent anything counts as completed");
}

/** The client is refused the consumer, and the submitter the client. */
int refusal(int argc, char **argv) {
    const auto environment = std::make_shared<GMPIEnvironment>(&argc, &argv);
    bool refused = false;
    if(environment->role() == c2::processRole::SUBMITTER) {
        try {
            const client_type client(environment, c2::Consumers::MPIConsumerConfig{});
        }
        catch(const geneva_exception &) {
            refused = true;
        }
        return verdict(refused, "the submitter was refused the MPI client");
    }
    try {
        const c2::GMPIConsumerT<GFaultyContainer> consumer(environment);
    }
    catch(const geneva_exception &) {
        refused = true;
    }
    return verdict(refused, "a client was refused the MPI consumer");
}

/** A log target that notes whether a message contained a given text, and passes every message on
 *  to the console. */
class GTextSpotter final : public Gem::Common::GBaseLogTarget {
public:
    explicit GTextSpotter(std::string text)
      : text_(std::move(text)) {
    }

    void log(std::string const &message) const override {
        this->note_(message);
        console_.log(message);
    }
    void logWithSource(std::string const &message, std::string const &extension) const override {
        this->note_(message);
        console_.logWithSource(message, extension);
    }

    /** Whether a message contained the text. */
    [[nodiscard]] bool seen() const {
        return seen_.load();
    }

private:
    void note_(std::string const &message) const {
        if(message.contains(text_)) {
            seen_.store(true);
        }
    }

    std::string text_;
    Gem::Common::GConsoleLogger console_;
    mutable std::atomic<bool> seen_{false};
};

/** The client with a time limit of 1 ms: it must say that the limit has no effect, and it must
 *  not end before the master tells it to (the master then finds every item processed). */
int clientWithLimits(const std::shared_ptr<GMPIEnvironment> &environment) {
    const auto spotter = std::make_shared<GTextSpotter>("has no effect for the MPI transport");
    glogger.addLogTarget(spotter);
    {
        client_type client(environment, c2::Consumers::MPIConsumerConfig{});
        client.setMaxTime(std::chrono::milliseconds(1));
        client.run();
    }
    glogger.resetLogTargets();
    return verdict(spotter->seen(), "the client warned that its time limit has no effect");
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
        " | idle-session"
        " | undecodable"
        " | refusal"
        " | client-limits"
        " | item-processor"
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
    if(scenario == "idle-session") {
        return idleSession(argc, argv);
    }
    if(scenario == "refusal") {
        return refusal(argc, argv);
    }
    if(scenario != "clean" && scenario != "throwing" && scenario != "never-started" &&
       scenario != "undecodable" && scenario != "client-limits" && scenario != "item-processor") {
        std::println(stderr, "Unknown scenario: {}", scenario);
        return 2;
    }

    // Declared before the consumer and the client, so that it outlives them; it finalizes MPI at
    // the end of main.
    const auto environment = std::make_shared<GMPIEnvironment>(&argc, &argv);

    // Clients: serve the master until it tells them to stop.
    if(environment->role() == c2::processRole::CLIENT) {
        if(scenario == "client-limits") {
            return clientWithLimits(environment);
        }
        if(scenario == "undecodable") {
            // the first such item this rank is sent fails to load
            GFaultyContainer::armLoadFaults(1);
        }
        client_type client(environment, c2::Consumers::MPIConsumerConfig{});
        if(scenario == "item-processor") {
            // an item's own processing stores true; this processor marks it with false
            client.setItemProcessor([](GFaultyContainer &item) {
                item.markAsProcessedWith({false});
            });
        }
        client.run();
        return 0;
    }

    auto consumer = std::make_shared<c2::GMPIConsumerT<GFaultyContainer>>(environment);

    if(scenario == "never-started") {
        // The consumer's destructor stops the server. With a server that never ran there is nothing
        // to join, and the destructor must not try.
        consumer.reset();
        std::println("OK: a master that was never started was destroyed");
        return 0;
    }

    if(scenario == "item-processor") {
        // Nothing may be resubmitted behind the test's back: every item that comes back was
        // processed by the client it was sent to.
        consumer->setTimeoutTreatment(c2::timeoutTreatment::wait_indefinitely);
    }
    consumer->startServer();
    if(scenario == "client-limits") {
        // Let the client's time limit of 1 ms pass before it gets any work.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const bool throwing = (scenario == "throwing");
    const bool undecodable = (scenario == "undecodable");
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.reserve(N);
    for(std::size_t i = 0; i < N; ++i) {
        fault_mode mode = fault_mode::NONE;
        if(throwing && (i % 5 == 0)) {
            mode = fault_mode::THROW_PROCESSING;
        }
        else if(undecodable && (i % 10 == 0)) {
            mode = fault_mode::THROW_LOADING;
        }
        items.push_back(std::make_unique<GFaultyContainer>(i, mode));
    }

    // A throwing item is flagged by the worker and returned; the tolerant policy then refills
    // its slot with a clone of a processed sibling. The clean scenario needs every evaluation.
    consumer->processBatch(
        std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
        throwing ? c2::GSubmissionPolicy::clone_on_partial_return()
                 : c2::GSubmissionPolicy::full_success_or_fatal()
    );

    // The items the workers could not load came back through the lease. If none did, the
    // scenario has not exercised what it is for.
    const bool faults_seen = not undecodable || consumer->requeuedCount() > 0;
    consumer->stopServer();

    std::size_t processed = 0;
    for(const auto &it : items) {
        if(it && it->is_processed()) {
            ++processed;
        }
    }

    if(not faults_seen) {
        std::println("FAIL: no item was put back, so no worker failed to load one");
        return 1;
    }
    if(scenario == "item-processor") {
        const auto own = std::ranges::count_if(items, [](const auto &it) {
            return not it || not it->is_processed() || it->getStoredResult();
        });
        if(own != 0) {
            std::println("FAIL: {}/{} items carry the result of their own process()", own, N);
            return 1;
        }
    }
    if(processed == N) {
        std::println(
            "OK: {}/{} items processed over MPI ({} ranks)",
            processed,
            N,
            environment->size()
        );
        return 0;
    }
    std::println("FAIL: only {}/{} items processed", processed, N);
    return 1;
}

#else

int main() {
    // MPI consumer not built -- nothing to exercise.
    return 0;
}

#endif /* GENEVA_BUILD_WITH_MPI_CONSUMER */
