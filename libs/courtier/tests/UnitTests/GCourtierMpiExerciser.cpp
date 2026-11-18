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
 * every other rank is a client and runs a GMPIConsumerWorkerNodeT. Each rank prints OK / FAIL and
 * returns 0 only when its checks hold.
 *
 *   GCourtierMpiExerciser [--scenario=<scenario>]
 *
 * Scenarios:
 *   clean          -- (the default) every item evaluates normally. The master starts its batch
 *                     when every client has asked for work (connectedClients()).
 *   throwing       -- every fifth item throws from its evaluation. The workers must survive, and
 *                     the clone-on-partial-return policy must leave every slot processed.
 *   never-started  -- the master builds its consumer and destroys it without starting the server.
 *                     Its clients, which wait for its greeting, are told that the run has ended
 *                     and end at once; so does the master.
 *   low-thread-level -- the program initializes MPI itself, below MPI_THREAD_MULTIPLE (single
 *                     rank). The consumer must refuse to be built.
 *   undecodable    -- each worker fails to load the first such item it is sent. The workers must
 *                     survive: each returns the item as failed (the tolerant policy refills its
 *                     slot), and no item is put back.
 *   refusal        -- (two ranks) the client tries to build the consumer and the submitter tries
 *                     to build the client; each must be refused.
 *   client-limits  -- (three ranks) rank 1 has an item limit of 5 and a depth of 1: it processes
 *                     exactly 5 items, sends the fifth result and leaves; rank 2 serves the rest,
 *                     and no item is put back.
 *   client-time-limit -- (three ranks) rank 1 has a time limit of 1 s: it ends by itself while the
 *                     master goes on with rank 2 (connectedClients() falls to 1 during the batch);
 *                     what rank 1 held is handed out again, and every item is processed.
 *   item-processor -- (three ranks) each client is given an item processor that marks every item
 *                     with the result false (an item's own processing stores true). Every item
 *                     must come back processed by a processor.
 *   soak           -- (five ranks) many batches of changing size from three submitters, with idle
 *                     phases; give-up window and time lease off. Nothing may be put back, dropped
 *                     or late; at the end every client waits at the master; the stop is quick.
 *   stop-parked    -- the master stops while every client's request waits at it.
 *   stop-evaluating -- the master stops while every client evaluates an item of 3 s.
 *   stop-late-rank -- the client with the highest rank builds its client --lateMs milliseconds
 *                     late; the master has stopped by then, and the late client is told at once.
 *   stop-client-left -- rank 1 leaves after --leaveAfter items (an item limit); the batches
 *                     complete without it, then the master stops.
 *   client-stops   -- (three ranks, a keep-alive of 1 s) rank 1 stops itself (SIGSTOP) in the
 *                     middle of the batch: the master finds it silent, puts back what it held and
 *                     finishes the batch with rank 2; the job is then ended (MPI_Finalize would
 *                     wait for the stopped rank), with a non-zero exit code.
 *   submitter-stops -- (three ranks, a keep-alive of 1 s) the master stops itself in the middle of
 *                     the batch: every client finds it silent and ends, and the job is ended.
 */

#include "common/GGlobalDefines.hpp"

#ifdef GENEVA_BUILD_WITH_MPI_CONSUMER

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <limits>
#include <memory>
#include <print>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <mpi.h>

#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierEnums.hpp" // processRole
#include "GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GMPIConsumerT.hpp"
#include "courtier/mpi/GMPIEnvironment.hpp"

using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;
using Gem::Courtier::Consumers::GMPIEnvironment;
namespace c2 = Gem::Courtier;
using client_type = c2::Consumers::GMPIConsumerWorkerNodeT<GFaultyContainer>;
using consumer_type = c2::GMPIConsumerT<GFaultyContainer>;
using clock_type = std::chrono::steady_clock;

// The configuration of the MPI family is an aggregate: it takes designated initializers.
static_assert(std::is_aggregate_v<c2::Consumers::MPIConsumerConfig>);

namespace {

/** Prints the outcome of a check and turns it into the exit code. */
int verdict(bool ok, const std::string &what) {
    std::println("{}: {}", ok ? "OK" : "FAIL", what);
    return ok ? 0 : 1;
}

/** Seconds since @p since */
double secondsSince(clock_type::time_point since) {
    return std::chrono::duration<double>(clock_type::now() - since).count();
}

/** Polls @p condition every 5 ms until it holds or @p limit has passed; whether it held. */
template <typename condition_type>
bool waitFor(const condition_type &condition, std::chrono::milliseconds limit) {
    const auto deadline = clock_type::now() + limit;
    while(not condition()) {
        if(clock_type::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
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

/** The client is refused the consumer, and the submitter the client. */
int refusal(int argc, char **argv) {
    const auto environment = std::make_shared<GMPIEnvironment>(&argc, &argv);
    bool refused = false;
    if(environment->role() == c2::processRole::SUBMITTER) {
        try {
            std::ignore =
                std::make_shared<client_type>(environment, c2::Consumers::MPIConsumerConfig{});
        }
        catch(const geneva_exception &) {
            refused = true;
        }
        return verdict(refused, "the submitter was refused the MPI client");
    }
    try {
        const consumer_type consumer(environment);
    }
    catch(const geneva_exception &) {
        refused = true;
    }
    return verdict(refused, "a client was refused the MPI consumer");
}

/** Makes @p n items, of which every @p every -th (if not 0) has the fault @p mode, and each sleeps
 *  @p ms milliseconds (0: none). */
std::vector<std::unique_ptr<GFaultyContainer>> makeItems(
    std::size_t n,
    unsigned int ms = 0,
    std::size_t every = 0,
    fault_mode mode = fault_mode::NONE
) {
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    items.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        fault_mode this_mode = ms > 0 ? fault_mode::SLEEP : fault_mode::NONE;
        if(every != 0 && i % every == 0) {
            this_mode = mode;
        }
        items.push_back(std::make_unique<GFaultyContainer>(i, this_mode, ms));
    }
    return items;
}

/** Submits @p items and returns how many came back processed. */
std::size_t runBatch(
    consumer_type &consumer,
    std::vector<std::unique_ptr<GFaultyContainer>> &items,
    const c2::GSubmissionPolicy &policy = c2::GSubmissionPolicy::full_success_or_fatal()
) {
    consumer.processBatch(
        std::span<std::unique_ptr<GFaultyContainer>>(items.data(), items.size()),
        policy
    );
    return static_cast<std::size_t>(std::ranges::count_if(items, [](const auto &it) {
        return it && it->is_processed();
    }));
}

/** Many batches of changing size from three submitters, with idle phases in between. The give-up
 *  window and the time lease are off, so a lost item or a lost result makes the run hang (ctest's
 *  limit ends it), and nothing may be put back or dropped. */
int soak(consumer_type &consumer, std::size_t workers) {
    constexpr int ROUNDS = 80;
    consumer.setTimeoutTreatment(c2::timeoutTreatment::wait_indefinitely);
    consumer.startServer();
    std::atomic<std::size_t> total{0};
    std::atomic<std::size_t> bad{0};
    auto submit = [&](unsigned int seed) {
        std::mt19937 rng(seed);
        for(int round = 0; round < ROUNDS; ++round) {
            const std::size_t n = 1 + (rng() % ((3 * workers) + 1));
            auto items = makeItems(n, rng() % 4);
            const std::size_t processed = runBatch(consumer, items);
            total += n;
            bad += n - processed;
            if(round % 20 == 19) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100 + (50 * seed)));
            }
        }
    };
    const auto t0 = clock_type::now();
    {
        const std::jthread a(submit, 1U);
        const std::jthread b(submit, 2U);
        const std::jthread c(submit, 3U);
    }
    const double dt = secondsSince(t0);
    // When all batches are done every client is idle, and an idle client has exactly one request
    // with the master. A client whose wake-up or whose request was lost is missing here.
    const bool all_parked =
        waitFor([&] { return consumer.parkedCount() == workers; }, std::chrono::milliseconds(5000));
    const auto t1 = clock_type::now();
    consumer.stopServer();
    const double stop = secondsSince(t1);
    const bool ok = bad.load() == 0 && consumer.requeuedCount() == 0 &&
                    consumer.droppedReturnCount() == 0 && consumer.lateReturnDroppedCount() == 0 &&
                    stop < 2.0 && all_parked;
    return verdict(
        ok,
        std::format(
            "soak: {} items in {} batches, {} not processed, requeued={} dropped={} late={} "
            "parked at the end={} time={:.2f}s stop={:.2f}s ({} ranks)",
            total.load(),
            3 * ROUNDS,
            bad.load(),
            consumer.requeuedCount(),
            consumer.droppedReturnCount(),
            consumer.lateReturnDroppedCount(),
            consumer.parkedCount(),
            dt,
            stop,
            workers + 1
        )
    );
}

/** The stop cases: what the master does before it stops differs, the check is the same --
 *  stopServer() returns within a bound (every client is told the end of the run: runClient()). */
int stopCase(consumer_type &consumer, const std::string &scenario, std::size_t workers) {
    consumer.setTimeoutTreatment(c2::timeoutTreatment::wait_indefinitely);
    consumer.startServer();
    bool ok = true;
    double stop_bound = 2.0; // seconds
    std::jthread submitter;

    if(scenario == "stop-parked") {
        // Every client has exactly one request with the master, and there is no work.
        ok = waitFor([&] { return consumer.parkedCount() == workers; }, std::chrono::seconds(30));
        std::println("stop-parked: {} of {} requests parked", consumer.parkedCount(), workers);
    }
    else if(scenario == "stop-evaluating") {
        // One fast item per client, then slow ones: when the master stops, every client is in
        // an evaluation of 3 s (and holds a second item, at depth 2).
        submitter = std::jthread([&consumer, workers] {
            std::vector<std::unique_ptr<GFaultyContainer>> items;
            for(std::size_t i = 0; i < 3 * workers; ++i) {
                const bool slow = i >= workers;
                items.push_back(
                    std::make_unique<GFaultyContainer>(
                        i,
                        slow ? fault_mode::SLEEP : fault_mode::NONE,
                        slow ? 3000U : 0U
                    )
                );
            }
            const auto processed =
                runBatch(consumer, items, c2::GSubmissionPolicy::clone_on_partial_return());
            std::println("stop-evaluating: the batch came back with {} items processed", processed);
        });
        ok = waitFor(
            [&] { return consumer.connectedClients().value_or(0) == workers; },
            std::chrono::seconds(10)
        );
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    else if(scenario == "stop-late-rank") {
        // The other ranks serve a batch; the last one is not there yet.
        auto items = makeItems(40);
        const auto processed = runBatch(consumer, items);
        ok = processed == 40;
        std::println("stop-late-rank: {}/40 items processed without the late rank", processed);
        // The late rank hears the end when it has built its client.
        stop_bound = 13.0;
    }
    else if(scenario == "stop-client-left") {
        auto first_items = makeItems(60, 5);
        const auto first = runBatch(consumer, first_items);
        const bool left = waitFor(
            [&] { return consumer.getNActiveSessions() == workers - 1; },
            std::chrono::seconds(20)
        );
        auto second_items = makeItems(60, 5);
        const auto second = runBatch(consumer, second_items);
        ok = first == 60 && second == 60 && left;
        std::println(
            "stop-client-left: {}/60 and {}/60 items processed, {} of {} sessions left, "
            "requeued={}",
            first,
            second,
            consumer.getNActiveSessions(),
            workers,
            consumer.requeuedCount()
        );
    }

    const auto t1 = clock_type::now();
    consumer.stopServer();
    const double stop = secondsSince(t1);
    if(submitter.joinable()) {
        submitter.join();
    }
    ok = ok && stop < stop_bound;
    return verdict(
        ok,
        std::format("{}: stopServer() took {:.3f} s (bound {:.1f} s)", scenario, stop, stop_bound)
    );
}

/** What a client does, by scenario; its exit code */
int runClient(
    const std::shared_ptr<GMPIEnvironment> &environment,
    const std::string &scenario,
    unsigned int late_ms,
    std::uint32_t leave_after
) {
    const int rank = environment->rank();
    c2::Consumers::MPIConsumerConfig config{};

    if(scenario == "client-limits" && rank == 1) {
        config.clientPrefetchDepth = 1;
        const auto client = std::make_shared<client_type>(environment, config);
        client->setProcessMax(5);
        client->run();
        return verdict(
            client->getNProcessed() == 5 &&
                client->connectionEnd() == c2::Consumers::GPeerEnd::CLOSED_HERE,
            std::format(
                "rank 1 processed {} items with an item limit of 5",
                client->getNProcessed()
            )
        );
    }
    if(scenario == "client-time-limit" && rank == 1) {
        const auto client = std::make_shared<client_type>(environment, config);
        client->setMaxTime(std::chrono::seconds(1));
        client->run();
        // It closed the connection itself (CLOSED_HERE), and was not told
        // the end of the run (CLOSED_THERE): an event, not a duration
        return verdict(
            client->connectionEnd() == c2::Consumers::GPeerEnd::CLOSED_HERE &&
                client->getNProcessed() > 0,
            std::format(
                "rank 1 ended by its time limit (its own close: {}), having processed {} "
                "items",
                client->connectionEnd() == c2::Consumers::GPeerEnd::CLOSED_HERE,
                client->getNProcessed()
            )
        );
    }
    if(scenario == "stop-client-left" && rank == 1) {
        const auto client = std::make_shared<client_type>(environment, config);
        client->setProcessMax(leave_after);
        client->run();
        return 0;
    }
    if(scenario == "stop-late-rank" && rank == environment->size() - 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(late_ms));
    }
    if(scenario == "client-stops" && rank == 1) {
        // The process stops in the middle of the batch. It runs again only while the launcher ends
        // the job (Open MPI continues a stopped process before it ends it): it then reads the
        // master's close and returns; what it does then is not judged.
        const auto client = std::make_shared<client_type>(environment, config);
        const std::jthread stopper([] {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::ignore = std::raise(SIGSTOP);
        });
        client->run();
        return 0;
    }

    if(scenario == "undecodable") {
        // the first such item this rank is sent fails to load
        GFaultyContainer::armLoadFaults(1);
    }
    const auto client = std::make_shared<client_type>(environment, config);
    if(scenario == "item-processor") {
        // an item's own processing stores true; this processor marks it with false
        client->setItemProcessor([](GFaultyContainer &item) { item.markAsProcessedWith({false}); });
    }
    client->run();
    if(scenario == "submitter-stops") {
        return verdict(
            client->connectionEnd() == c2::Consumers::GPeerEnd::SILENT,
            std::format("rank {} found its submitter silent", rank)
        );
    }
    if(scenario.starts_with("stop-") || scenario == "soak" || scenario == "never-started") {
        // Every client of these is told the end of the run
        return verdict(
            client->connectionEnd() == c2::Consumers::GPeerEnd::CLOSED_THERE,
            std::format("rank {} was told the end of the run", rank)
        );
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    constexpr std::size_t N = 120;

    std::string scenario;
    unsigned int late_ms = 3000;
    std::uint32_t leave_after = 20;
    Gem::Common::GParserBuilder gpb;
    gpb.registerCLParameter(
        "scenario",
        scenario,
        std::string("clean"),
        "what to exercise: clean | throwing | never-started | low-thread-level | undecodable"
        " | refusal | client-limits | client-time-limit | item-processor | soak | stop-parked"
        " | stop-evaluating | stop-late-rank | stop-client-left | client-stops | submitter-stops"
    );
    gpb.registerCLParameter(
        "lateMs",
        late_ms,
        3000U,
        "stop-late-rank: how late the last rank builds its client, in milliseconds"
    );
    gpb.registerCLParameter(
        "leaveAfter",
        leave_after,
        std::uint32_t{20},
        "stop-client-left: the number of items after which rank 1 leaves"
    );
    if(gpb.parseCommandLine(argc, argv) == Gem::Common::GCL_HELP_REQUESTED) {
        return 0;
    }
    if(scenario == "low-thread-level") {
        return lowThreadLevel(argc, argv);
    }
    if(scenario == "refusal") {
        return refusal(argc, argv);
    }
    static const std::vector<std::string> SCENARIOS{
        "clean",
        "throwing",
        "never-started",
        "undecodable",
        "client-limits",
        "client-time-limit",
        "item-processor",
        "soak",
        "stop-parked",
        "stop-evaluating",
        "stop-late-rank",
        "stop-client-left",
        "client-stops",
        "submitter-stops"
    };
    if(std::ranges::find(SCENARIOS, scenario) == SCENARIOS.end()) {
        std::println(stderr, "Unknown scenario: {}", scenario);
        return 2;
    }

    // Declared before the consumer and the client, so that it outlives them; it finalizes MPI at
    // the end of main.
    const auto environment = std::make_shared<GMPIEnvironment>(&argc, &argv);
    const auto workers = static_cast<std::size_t>(environment->size() - 1);

    if(environment->role() == c2::processRole::CLIENT) {
        return runClient(environment, scenario, late_ms, leave_after);
    }

    // The keep-alive of the scenarios of a lost rank is short, so that the loss is found at once
    c2::Consumers::MPIConsumerConfig consumer_config{};
    if(scenario == "client-stops" || scenario == "submitter-stops") {
        consumer_config.pingInterval = 1;
    }
    auto consumer = std::make_shared<consumer_type>(environment, consumer_config);

    if(scenario == "never-started") {
        // The consumer's destructor stops the server, which never ran: it tells the clients that
        // wait for it, at once, and they end
        const auto t0 = clock_type::now();
        consumer.reset();
        const double took = secondsSince(t0);
        return verdict(
            took < 5.0,
            std::format("a master that was never started was destroyed after {:.2f} s", took)
        );
    }
    if(scenario == "soak") {
        return soak(*consumer, workers);
    }
    if(scenario.starts_with("stop-")) {
        return stopCase(*consumer, scenario, workers);
    }

    // Nothing may be resubmitted behind the test's back: every item that comes back was processed
    // by the client it was sent to, and an item comes back to the master only when a client
    // gives it back.
    consumer->setTimeoutTreatment(c2::timeoutTreatment::wait_indefinitely);
    consumer->startServer();
    // Every client has asked for work before the batch starts
    if(not waitFor(
           [&] { return consumer->connectedClients().value_or(0) == workers; },
           std::chrono::seconds(30)
       )) {
        return verdict(
            false,
            std::format(
                "only {} of {} clients asked for work",
                consumer->connectedClients().value_or(0),
                workers
            )
        );
    }

    std::size_t fewest_connected = workers;
    std::atomic<bool> batch_done{false};
    std::jthread sampler;
    std::size_t n_items = N;
    unsigned int ms = 0;
    if(scenario == "client-limits") {
        n_items = 60;
        ms = 5;
    }
    else if(scenario == "client-stops" || scenario == "submitter-stops") {
        // A batch of about three seconds; a rank stops itself after one
        n_items = 120;
        ms = 50;
        if(scenario == "submitter-stops") {
            sampler = std::jthread([] {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::ignore = std::raise(SIGSTOP);
            });
        }
    }
    else if(scenario == "client-time-limit") {
        // Rank 1 leaves after one to two seconds of a batch of about five.
        n_items = 120;
        ms = 50;
        sampler = std::jthread([&] {
            while(not batch_done.load()) {
                fewest_connected =
                    std::min(fewest_connected, consumer->connectedClients().value_or(workers));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    std::vector<std::unique_ptr<GFaultyContainer>> items;
    if(scenario == "throwing") {
        items = makeItems(n_items, ms, 5, fault_mode::THROW_PROCESSING);
    }
    else if(scenario == "undecodable") {
        items = makeItems(n_items, ms, 10, fault_mode::THROW_LOADING);
    }
    else {
        items = makeItems(n_items, ms);
    }

    // A throwing item is flagged by the worker and returned; the tolerant policy then refills
    // its slot with a clone of a processed sibling. The others need every evaluation.
    const std::size_t processed = runBatch(
        *consumer,
        items,
        scenario == "throwing" || scenario == "undecodable"
            ? c2::GSubmissionPolicy::clone_on_partial_return()
            : c2::GSubmissionPolicy::full_success_or_fatal()
    );
    batch_done.store(true);
    if(sampler.joinable()) {
        sampler.join();
    }
    const auto requeued = consumer->requeuedCount();
    consumer->stopServer();

    if(scenario == "undecodable" && requeued != 0) {
        return verdict(false, std::format("{} item(s) were put back, not failed", requeued));
    }
    if(scenario == "client-limits" && requeued != 0) {
        return verdict(false, std::format("{} item(s) were put back: a result was lost", requeued));
    }
    if(scenario == "submitter-stops") {
        return verdict(false, "the submitter ran on after it had stopped itself");
    }
    if(scenario == "client-stops" && requeued == 0) {
        return verdict(false, "nothing of the stopped rank was put back");
    }
    if(scenario == "client-time-limit" && (fewest_connected != 1 || requeued == 0)) {
        return verdict(
            false,
            std::format(
                "the master saw at least {} clients during the batch and put {} item(s) back",
                fewest_connected,
                requeued
            )
        );
    }
    if(scenario == "item-processor") {
        const auto own = std::ranges::count_if(items, [](const auto &it) {
            return not it || not it->is_processed() || it->getStoredResult();
        });
        if(own != 0) {
            return verdict(
                false,
                std::format("{}/{} items carry the result of their own process()", own, n_items)
            );
        }
    }
    return verdict(
        processed == n_items,
        std::format(
            "{}/{} items processed over MPI ({} ranks)",
            processed,
            n_items,
            environment->size()
        )
    );
}

#else

int main() {
    // MPI consumer not built -- nothing to exercise.
    return 0;
}

#endif /* GENEVA_BUILD_WITH_MPI_CONSUMER */
