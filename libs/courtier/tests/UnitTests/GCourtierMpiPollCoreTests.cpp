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

// The MPI poll (GMPIPollCore) on the in-memory stand-in for MPI of GMPIFakeCalls.hpp: in one
// process, under ThreadSanitizer, without mpirun.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/strand.hpp>

#include "GInProcessTestSupport.hpp"
#include "GMPIFakeCalls.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/transport/GIoContext.hpp"
#include "courtier/transport/GMPIPollCore.hpp"

using Gem::Common::Concurrency::GThreadGroup;
using Gem::Courtier::Consumers::GIoContext;
using Gem::Courtier::Consumers::GMPIPollCore;
using Gem::Courtier::Consumers::GMPIPollReceiverI;
using Gem::Courtier::Consumers::GMPIPollTiming;
using Gem::Courtier::Tests::eventually;
using Gem::Courtier::Tests::GMPIFakeWorld;

using namespace std::chrono_literals;

namespace {

using clock_type = std::chrono::steady_clock;

/******************************************************************************/
/** @brief An io context with threads of its own, which can be stopped and then shut down */
class Threads {
public:
    /** @brief @param n_threads How many threads run the context */
    explicit Threads(std::size_t n_threads) {
        for(std::size_t i = 0; i < n_threads; ++i) {
            threads_.create_thread([this] {
                // An exception that leaves run() is counted, not let end the process
                try {
                    ctx_.run();
                }
                catch(...) {
                    escaped_.fetch_add(1);
                }
            });
        }
    }
    /** @brief Stops and joins, if that has not been done */
    ~Threads() {
        this->join();
    }
    Threads(const Threads &) = delete;
    Threads(Threads &&) = delete;
    Threads &operator=(const Threads &) = delete;
    Threads &operator=(Threads &&) = delete;

    /** @brief @return The executor of the context */
    boost::asio::any_io_executor executor() {
        return ctx_.get_executor();
    }

    /** @brief Stops the context and joins its threads: no handler runs afterwards */
    void join() {
        guard_.reset();
        ctx_.stop();
        threads_.join_all();
    }

    /** @brief Destroys the pending handlers; after join() */
    void shutdown() {
        ctx_.shutdown();
    }

    /** @brief @return How many exceptions have left run() */
    int escaped() const {
        return escaped_.load();
    }

private:
    std::atomic<int> escaped_{0}; ///< Exceptions that have left run()
    GIoContext ctx_; ///< The context
    boost::asio::executor_work_guard<GIoContext::executor_type> guard_ =
        boost::asio::make_work_guard(ctx_);
    GThreadGroup threads_; ///< Its threads
};

/******************************************************************************/
/** @brief Ends a test's poll in the order its owner would: no handler runs, shutdown(), the pending
 *  handlers go, then the poll
 *  @return What shutdown() gave up */
std::size_t finish(std::shared_ptr<GMPIPollCore> &core, std::initializer_list<Threads *> contexts) {
    for(auto *threads : contexts) {
        threads->join();
    }
    const std::size_t given_up = core->shutdown();
    for(auto *threads : contexts) {
        threads->shutdown();
    }
    core.reset();
    return given_up;
}

/******************************************************************************/
/** @brief A receiver that records what it gets, on a strand of the executor it is given */
class Receiver final : public GMPIPollReceiverI {
public:
    using on_message_type = std::function<void(int, const std::string &)>;

    /** @brief @param executor Where the strand is made @param on_message Called for every message
     *  before it is recorded */
    explicit Receiver(const boost::asio::any_io_executor &executor, on_message_type on_message = {})
      : strand_(boost::asio::make_strand(executor))
      , on_message_(std::move(on_message)) { /* nothing */
    }

    boost::asio::any_io_executor executor() override {
        return strand_;
    }

    void received(int tag, std::string &&message) override {
        if(on_message_) {
            on_message_(tag, message);
        }
        const std::scoped_lock lock(mutex_);
        messages_.emplace_back(tag, std::move(message));
    }

    void discard() override {
        discards_.fetch_add(1);
    }

    void pollFailed(const std::string &reason) override {
        const std::scoped_lock lock(mutex_);
        failures_.push_back(reason);
        messages_at_failure_ = messages_.size();
    }

    /** @brief @return The reasons pollFailed() was called with */
    std::vector<std::string> failures() const {
        const std::scoped_lock lock(mutex_);
        return failures_;
    }

    /** @brief @return How many messages had arrived when pollFailed() was called */
    std::size_t messagesAtFailure() const {
        const std::scoped_lock lock(mutex_);
        return messages_at_failure_;
    }

    /** @brief @return What arrived so far, in order */
    std::vector<std::pair<int, std::string>> messages() const {
        const std::scoped_lock lock(mutex_);
        return messages_;
    }

    /** @brief @return How many messages arrived so far */
    std::size_t count() const {
        const std::scoped_lock lock(mutex_);
        return messages_.size();
    }

    /** @brief @return How often discard() was called */
    int discards() const {
        return discards_.load();
    }

private:
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    on_message_type on_message_;
    mutable std::mutex mutex_;
    std::vector<std::pair<int, std::string>> messages_;
    std::vector<std::string> failures_;
    std::size_t messages_at_failure_ = 0;
    std::atomic<int> discards_{0};
};

} /* anonymous namespace */

/******************************************************************************/
TEST_CASE("The defaults of the poll's timing are the constants and an unusable timing is refused") {
    const GMPIPollTiming timing;
    CHECK(timing.pauseMin == Gem::Courtier::GMPIPEERPOLLPAUSEMIN);
    CHECK(timing.pauseMax == Gem::Courtier::GMPIPEERPOLLPAUSEMAX);
    CHECK(timing.rounds == Gem::Courtier::GMPIPEERPOLLROUNDS);
    CHECK(timing.drainGrace == Gem::Courtier::GMPIPEERDRAINGRACE);
    CHECK(timing.pauseBusy == Gem::Courtier::GMPIPEERPOLLPAUSEBUSY);
    CHECK(Gem::Courtier::GMPIPEERPOLLPAUSEBUSY == 50ms);
    CHECK(Gem::Courtier::GMPIPEERPOLLPAUSEMIN == 50us);
    CHECK(Gem::Courtier::GMPIPEERPOLLPAUSEMAX == 1ms);
    CHECK(Gem::Courtier::GMPIPEERPOLLROUNDS == 16);
    CHECK(Gem::Courtier::GMPIPEERDRAINGRACE == 2s);

    auto world = std::make_shared<GMPIFakeWorld>(2);
    Threads threads(1);
    CHECK_THROWS(std::make_shared<GMPIPollCore>(nullptr, threads.executor()));
    CHECK_THROWS(
        std::make_shared<GMPIPollCore>(
            world->calls(0),
            threads.executor(),
            GMPIPollTiming{.pauseMin = 0us}
        )
    );
    CHECK_THROWS(
        std::make_shared<GMPIPollCore>(
            world->calls(0),
            threads.executor(),
            GMPIPollTiming{.pauseMin = 2ms, .pauseMax = 1ms}
        )
    );
    CHECK_THROWS(
        std::make_shared<GMPIPollCore>(
            world->calls(0),
            threads.executor(),
            GMPIPollTiming{.rounds = 0}
        )
    );
    CHECK_THROWS(
        std::make_shared<GMPIPollCore>(
            world->calls(0),
            threads.executor(),
            GMPIPollTiming{.pauseMax = 2ms, .pauseBusy = 1ms}
        )
    );
}

/******************************************************************************/
TEST_CASE("Receivers are added before the start and one per rank of the communicator") {
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Threads threads(1);
    auto core = std::make_shared<GMPIPollCore>(world->calls(0), threads.executor());

    CHECK_THROWS(core->addReceiver(3, std::make_shared<Receiver>(threads.executor())));
    CHECK_THROWS(core->addReceiver(-1, std::make_shared<Receiver>(threads.executor())));
    CHECK_THROWS(core->addReceiver(1, nullptr));
    core->addReceiver(1, std::make_shared<Receiver>(threads.executor()));
    CHECK_THROWS(core->addReceiver(1, std::make_shared<Receiver>(threads.executor())));
    core->start();
    CHECK_THROWS(core->addReceiver(2, std::make_shared<Receiver>(threads.executor())));
    CHECK_THROWS(core->startSend(3, Gem::Courtier::TAG_PEER_MESSAGE, "x"));

    std::ignore = finish(core, {&threads});
    CHECK(world->violations().empty());
}

/******************************************************************************/
TEST_CASE(
    "The messages of one rank reach its receiver in the order they were sent whatever their tags"
) {
    constexpr int N_MESSAGES = 400;
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Threads poll_threads(4);
    Threads receiver_threads(4);
    auto core = std::make_shared<GMPIPollCore>(world->calls(0), poll_threads.executor());
    auto from_1 = std::make_shared<Receiver>(receiver_threads.executor());
    auto from_2 = std::make_shared<Receiver>(receiver_threads.executor());
    core->addReceiver(1, from_1);
    core->addReceiver(2, from_2);
    core->start();

    // Three tags in turn; every seventh message is empty.
    std::vector<std::pair<int, std::string>> expected;
    expected.reserve(N_MESSAGES);
    for(int i = 0; i < N_MESSAGES; ++i) {
        expected.emplace_back(50 + (i % 3), i % 7 == 0 ? std::string{} : std::to_string(i));
    }
    for(int i = 0; i < N_MESSAGES; ++i) {
        const auto &[tag, text] = expected[static_cast<std::size_t>(i)];
        world->inject(1, 0, tag, text);
        world->inject(2, 0, tag, text);
    }

    REQUIRE(eventually(
        [&] { return from_1->count() == N_MESSAGES && from_2->count() == N_MESSAGES; },
        20s
    ));
    std::ignore = finish(core, {&poll_threads, &receiver_threads});
    CHECK(from_1->messages() == expected);
    CHECK(from_2->messages() == expected);
    CHECK(world->violations().empty());
}

/******************************************************************************/
TEST_CASE("A send during the longest pause is followed by a look within a bound") {
    // A pause of 30 s: without the kick of the send, the next look would come 30 s later.
    auto world = std::make_shared<GMPIFakeWorld>(2);
    Threads threads(1);
    auto core = std::make_shared<GMPIPollCore>(
        world->calls(1),
        threads.executor(),
        GMPIPollTiming{.pauseMin = 30s, .pauseMax = 30s, .pauseBusy = 30s}
    );
    core->addReceiver(0, std::make_shared<Receiver>(threads.executor()));
    core->start();

    REQUIRE(eventually([&] { return world->counts(1).probes >= 1; }, 5s));
    std::this_thread::sleep_for(100ms); // the chain sleeps now
    const auto probes_before = world->counts(1).probes;
    REQUIRE(probes_before == 1);

    const auto sent_at = clock_type::now();
    core->startSend(0, 50, "a request");
    const bool looked = eventually([&] { return world->counts(1).probes > probes_before; }, 5s);
    const auto took = clock_type::now() - sent_at;
    INFO(
        "the next look came after "
        << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms"
    );
    CHECK(looked);

    std::ignore = finish(core, {&threads});
    CHECK(world->violations().empty());
}

/******************************************************************************/
TEST_CASE("Without traffic the pause grows to its upper bound and the poll keeps looking") {
    auto world = std::make_shared<GMPIFakeWorld>(2);
    Threads threads(1);
    auto core = std::make_shared<GMPIPollCore>(world->calls(1), threads.executor());
    core->addReceiver(0, std::make_shared<Receiver>(threads.executor()));
    core->start();

    REQUIRE(eventually([&] { return world->counts(1).probes >= 1; }, 5s));
    const auto probes_before = world->counts(1).probes;
    std::this_thread::sleep_for(300ms);
    const auto probes = world->counts(1).probes - probes_before;
    std::ignore = finish(core, {&threads});

    // One look per millisecond at most once the pause has grown (it takes five looks to get there);
    // a pause that stays at 50 us would look about 6000 times.
    INFO("looks in 300 ms: " << probes);
    CHECK(probes <= 450);
    CHECK(probes >= 10);
}

/******************************************************************************/
TEST_CASE(
    "A send that never completes ends the chain the drain grace after the last receiver and is "
    "given up"
) {
    constexpr auto GRACE = 300ms;
    auto world = std::make_shared<GMPIFakeWorld>(2);
    world->swallow(0); // rank 0 receives nothing
    GIoContext io;
    auto core = std::make_shared<GMPIPollCore>(
        world->calls(1),
        io.get_executor(),
        GMPIPollTiming{.drainGrace = GRACE}
    );
    core->addReceiver(0, std::make_shared<Receiver>(io.get_executor()));
    core->start();
    core->startSend(0, 50, std::string(1000, 'x'));
    core->receiverEnded(0);

    // As on a client: run() returns when the chain has ended.
    std::atomic<bool> returned{false};
    const auto started_at = clock_type::now();
    std::atomic<clock_type::duration> took{};
    GThreadGroup thread;
    thread.create_thread([&] {
        io.run();
        took.store(clock_type::now() - started_at);
        returned.store(true);
    });
    const bool ended = eventually([&] { return returned.load(); }, 10s);
    if(not ended) {
        io.stop();
    }
    thread.join_all();
    REQUIRE(ended);
    CHECK(took.load() >= GRACE - 20ms);
    CHECK(core->chainEnded());

    const std::size_t given_up = core->shutdown();
    io.shutdown();
    CHECK(given_up == 1);
    CHECK(core->counts().givenUp == 1);
    CHECK(world->counts(1).givenUp == 1);
    CHECK(world->sendsUnderWay() == 0);
    CHECK(world->violations().empty());
}

/******************************************************************************/
TEST_CASE("Two ends that answer each other drain and their runs return without the grace") {
    // The grace is long, so a run that returns quickly has not waited for it.
    auto world = std::make_shared<GMPIFakeWorld>(2);
    const GMPIPollTiming timing{.drainGrace = 30s};
    GIoContext master_io;
    GIoContext client_io;
    auto master = std::make_shared<GMPIPollCore>(world->calls(0), master_io.get_executor(), timing);
    auto client = std::make_shared<GMPIPollCore>(world->calls(1), client_io.get_executor(), timing);
    const std::weak_ptr<GMPIPollCore> master_weak = master;
    const std::weak_ptr<GMPIPollCore> client_weak = client;

    // The master answers and ends; the client ends when it has the answer.
    master->addReceiver(
        1,
        std::make_shared<Receiver>(
            master_io.get_executor(),
            [master_weak](int, const std::string &) {
                if(auto core = master_weak.lock()) {
                    core->startSend(1, 50, "an answer");
                    core->receiverEnded(1);
                }
            }
        )
    );
    client->addReceiver(
        0,
        std::make_shared<Receiver>(
            client_io.get_executor(),
            [client_weak](int, const std::string &) {
                if(auto core = client_weak.lock()) {
                    core->receiverEnded(0);
                }
            }
        )
    );
    master->start();
    client->start();
    client->startSend(0, 50, "a request");

    std::atomic<int> returned{0};
    GThreadGroup threads;
    threads.create_thread([&] {
        master_io.run();
        returned.fetch_add(1);
    });
    threads.create_thread([&] {
        client_io.run();
        returned.fetch_add(1);
    });
    const bool ended = eventually([&] { return returned.load() == 2; }, 10s);
    if(not ended) {
        master_io.stop();
        client_io.stop();
    }
    threads.join_all();
    REQUIRE(ended);
    CHECK(master->shutdown() == 0);
    CHECK(client->shutdown() == 0);
    master_io.shutdown();
    client_io.shutdown();
    CHECK(master->counts().received == 1);
    CHECK(client->counts().received == 1);
    CHECK(world->sendsUnderWay() == 0);
    CHECK(world->violations().empty());
}

/******************************************************************************/
TEST_CASE("A receiver that takes long does not hold up the poll") {
    // As on a master: the poll on a context of its own, the receivers on another one with a single
    // thread, which the first message blocks.
    constexpr auto BLOCK = 600ms;
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Threads poll_thread(1);
    Threads receiver_thread(1);
    auto core = std::make_shared<GMPIPollCore>(world->calls(0), poll_thread.executor());
    std::atomic<bool> blocking{false};
    core->addReceiver(
        1,
        std::make_shared<Receiver>(
            receiver_thread.executor(),
            [&blocking, BLOCK](int, const std::string &) {
                blocking.store(true);
                std::this_thread::sleep_for(BLOCK);
                blocking.store(false);
            }
        )
    );
    auto from_2 = std::make_shared<Receiver>(receiver_thread.executor());
    core->addReceiver(2, from_2);
    core->start();

    world->inject(1, 0, 50, "a slow one");
    REQUIRE(eventually([&] { return blocking.load(); }, 5s));
    const auto blocked_at = clock_type::now();
    for(int i = 0; i < 10; ++i) {
        world->inject(2, 0, 50, std::to_string(i));
    }
    const bool all_received =
        eventually([&] { return world->counts(0).receives == 11; }, BLOCK / 2);
    const bool still_blocking = blocking.load() && clock_type::now() - blocked_at < BLOCK;
    CHECK(all_received);
    CHECK(still_blocking);

    REQUIRE(eventually([&] { return from_2->count() == 10; }, 5s));
    std::ignore = finish(core, {&poll_thread, &receiver_thread});
    CHECK(world->violations().empty());
}

/******************************************************************************/
TEST_CASE("The fake MPI notices a second receiver") {
    auto world = std::make_shared<GMPIFakeWorld>(2);
    world->inject(1, 0, 50, "one");
    auto calls = world->calls(0);
    GThreadGroup other;
    other.create_thread([&calls] { REQUIRE(calls->probe().has_value()); });
    other.join_all();
    // The other thread's probe has not been followed by its receive: this probe is a second receiver.
    std::ignore = calls->probe();
    CHECK_FALSE(world->violations().empty());
}

/******************************************************************************/
TEST_CASE("Only one chain receives also on an executor with several threads and with many sends") {
    constexpr int N_PER_RANK = 600;
    auto world = std::make_shared<GMPIFakeWorld>(4);
    Threads poll_threads(4);
    Threads receiver_threads(4);
    auto core = std::make_shared<GMPIPollCore>(world->calls(0), poll_threads.executor());
    const std::weak_ptr<GMPIPollCore> weak = core;
    std::vector<std::shared_ptr<Receiver>> receivers;
    for(int rank = 1; rank < 4; ++rank) {
        // Every message is answered: many sends, each with its kick.
        receivers.push_back(
            std::make_shared<Receiver>(
                receiver_threads.executor(),
                [weak, rank](int, const std::string &) {
                    if(auto c = weak.lock()) {
                        c->startSend(rank, 50, "an answer");
                    }
                }
            )
        );
        core->addReceiver(rank, receivers.back());
    }
    core->start();
    for(int i = 0; i < N_PER_RANK; ++i) {
        for(int rank = 1; rank < 4; ++rank) {
            world->inject(rank, 0, 50, std::to_string(i));
        }
    }
    REQUIRE(eventually(
        [&] {
            return receivers[0]->count() == N_PER_RANK && receivers[1]->count() == N_PER_RANK &&
                   receivers[2]->count() == N_PER_RANK;
        },
        20s
    ));
    std::ignore = finish(core, {&poll_threads, &receiver_threads});
    const auto violations = world->violations();
    INFO("first violation: " << (violations.empty() ? std::string{"none"} : violations.front()));
    CHECK(violations.empty());
}

/******************************************************************************/
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; the count comes from the assertion macros
TEST_CASE("shutdown() gives up what has not completed and lets go of every receiver once") {
    auto world = std::make_shared<GMPIFakeWorld>(3);
    world->swallow(2);
    Threads threads(1);
    auto core = std::make_shared<GMPIPollCore>(world->calls(0), threads.executor());
    auto one = std::make_shared<Receiver>(threads.executor());
    auto two = std::make_shared<Receiver>(threads.executor());
    core->addReceiver(1, one);
    core->addReceiver(2, two);
    core->start();
    core->startSend(1, 50, "never read"); // rank 1 has nobody that receives
    core->startSend(2, 50, "swallowed");
    REQUIRE(eventually([&] { return world->counts(0).probes >= 2; }, 5s));

    threads.join();
    CHECK(core->shutdown() == 2);
    CHECK(core->chainEnded());
    CHECK(one->discards() == 1);
    CHECK(two->discards() == 1);
    CHECK(world->counts(0).givenUp == 2);
    CHECK(world->sendsUnderWay() == 0);
    CHECK(world->waiting(1) == 0); // the cancel took the message back

    CHECK(core->shutdown() == 0);
    CHECK(one->discards() == 1);
    core->startSend(1, 50, "too late");
    CHECK(world->counts(0).sends == 2);

    threads.shutdown();
    core.reset();
    CHECK(world->violations().empty());
}

/******************************************************************************/
TEST_CASE("A message from a rank without a receiver is received and dropped") {
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Threads threads(1);
    auto core = std::make_shared<GMPIPollCore>(world->calls(0), threads.executor());
    auto one = std::make_shared<Receiver>(threads.executor());
    core->addReceiver(1, one);
    core->start();
    world->inject(2, 0, 50, "from nobody");
    world->inject(2, 0, 50, "from nobody again");
    world->inject(1, 0, 50, "from one");

    REQUIRE(eventually([&] { return one->count() == 1; }, 5s));
    CHECK(eventually([&] { return world->waiting(0) == 0; }, 5s));
    std::ignore = finish(core, {&threads});
    CHECK(world->counts(0).receives == 3);
    CHECK(world->violations().empty());
}

/******************************************************************************/
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one linear scenario; the count comes from the assertion macros
TEST_CASE("A call of the poll that throws ends the chain and tells every receiver once") {
    using GFault = GMPIFakeWorld::GFault;
    const auto fault = GENERATE(GFault::PROBE, GFault::RECEIVE, GFault::TEST_SOME);
    const bool standard = GENERATE(true, false);
    INFO("fault " << static_cast<int>(fault) << (standard ? ", a std::exception" : ", an int"));

    auto world = std::make_shared<GMPIFakeWorld>(3);
    world->swallow(2);
    Threads poll_threads(1);
    Threads receiver_threads(2);
    auto core = std::make_shared<GMPIPollCore>(world->calls(0), poll_threads.executor());
    auto one = std::make_shared<Receiver>(receiver_threads.executor());
    auto two = std::make_shared<Receiver>(receiver_threads.executor());
    core->addReceiver(1, one);
    core->addReceiver(2, two);
    core->start();

    world->inject(1, 0, 50, "before");
    REQUIRE(eventually([&] { return one->count() == 1; }, 5s));
    core->startSend(2, 50, "swallowed"); // never completes: shutdown() gives it up
    REQUIRE(eventually([&] { return world->counts(0).probes >= 3; }, 5s));
    CHECK(core->failure() == std::nullopt);

    world->throwOnce(0, fault, standard);
    world->inject(1, 0, 51, "the message the fault strikes");

    CHECK(eventually([&] { return core->chainEnded(); }, 5s));
    CHECK(
        eventually([&] { return one->failures().size() == 1 && two->failures().size() == 1; }, 5s)
    );
    CHECK(poll_threads.escaped() == 0);
    CHECK(core->failure().has_value());

    // Nothing is delivered afterwards, and nothing more is looked for
    const auto probes = world->counts(0).probes;
    world->inject(2, 0, 52, "after");
    std::this_thread::sleep_for(100ms);
    CHECK(world->counts(0).probes == probes);
    CHECK(one->count() == one->messagesAtFailure());
    CHECK(two->count() == 0);
    CHECK(one->failures().size() == 1);
    CHECK(two->failures().size() == 1);

    CHECK(finish(core, {&poll_threads, &receiver_threads}) == 1);
    CHECK(world->sendsUnderWay() == 0);
    CHECK(one->discards() == 1);
    CHECK(two->discards() == 1);
}

/******************************************************************************/
// startSend() is allowed from any thread, also while shutdown() runs: a send that has begun is taken
// over by shutdown() and given up, never left with its bytes freed. Each round has a sender that
// sends without pause to a rank that never receives (each send takes 200 us in the fake, between
// the poll's look at its end and the hand-over) while shutdown() runs.
TEST_CASE("A send from another thread during shutdown() is taken over or dropped and never lost") {
    constexpr int N_ROUNDS = 50;
    int n_lost = 0;
    for(int round = 0; round < N_ROUNDS; ++round) {
        auto world = std::make_shared<GMPIFakeWorld>(2);
        world->swallow(1);
        world->delaySends(200us);
        Threads threads(1);
        auto core = std::make_shared<GMPIPollCore>(world->calls(0), threads.executor());
        core->addReceiver(1, std::make_shared<Receiver>(threads.executor()));
        core->start();

        std::atomic<bool> stop{false};
        std::thread sender([&] {
            while(not stop.load()) {
                core->startSend(1, 50, "to nobody");
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(1 + round % 3));
        threads.join();
        std::ignore = core->shutdown();
        stop.store(true);
        sender.join();
        n_lost += static_cast<int>(world->sendsUnderWay());

        threads.shutdown();
        core.reset();
        CHECK(world->violations().empty());
    }
    CHECK(n_lost == 0);
}

/******************************************************************************/
TEST_CASE("A poll whose owner did not call shutdown() gives up its sends when it goes") {
    auto world = std::make_shared<GMPIFakeWorld>(2);
    world->swallow(1);
    Threads threads(1);
    auto core = std::make_shared<GMPIPollCore>(world->calls(0), threads.executor());
    auto one = std::make_shared<Receiver>(threads.executor());
    core->addReceiver(1, one);
    core->start();
    core->startSend(1, 50, "swallowed");
    REQUIRE(eventually([&] { return world->counts(0).probes >= 2; }, 5s));

    threads.join();
    threads.shutdown();
    core.reset(); // without shutdown()
    CHECK(world->sendsUnderWay() == 0);
    CHECK(world->counts(0).givenUp == 1);
    CHECK(one->discards() == 1);
}

/******************************************************************************/
TEST_CASE("While every receiver's owner is busy the poll looks far less often") {
    auto world = std::make_shared<GMPIFakeWorld>(2);
    Threads threads(1);
    auto core = std::make_shared<GMPIPollCore>(world->calls(1), threads.executor());
    core->addReceiver(0, std::make_shared<Receiver>(threads.executor()));
    core->start();
    REQUIRE(eventually([&] { return world->counts(1).probes >= 1; }, 5s));

    // The looks of 500 ms, after the pause has grown to its bound
    const auto looks = [&] {
        std::this_thread::sleep_for(200ms);
        const auto before = world->counts(1).probes;
        std::this_thread::sleep_for(500ms);
        return world->counts(1).probes - before;
    };
    const auto idle = looks();
    core->receiverBusy(true);
    const auto busy = looks();
    core->receiverBusy(false);
    const auto idle_again = looks();
    INFO("looks in 500 ms: idle " << idle << ", busy " << busy << ", idle again " << idle_again);
    CHECK(busy * 10 <= idle);
    CHECK(busy * 10 <= idle_again);

    std::ignore = finish(core, {&threads});
    CHECK(world->violations().empty());
}

/******************************************************************************/
TEST_CASE("A send while every owner is busy and the end of a busy spell are followed by a look") {
    // Every pause is 30 s: a look that comes sooner comes from a kick.
    auto world = std::make_shared<GMPIFakeWorld>(2);
    Threads threads(1);
    auto core = std::make_shared<GMPIPollCore>(
        world->calls(1),
        threads.executor(),
        GMPIPollTiming{.pauseMin = 30s, .pauseMax = 30s, .pauseBusy = 30s}
    );
    core->addReceiver(0, std::make_shared<Receiver>(threads.executor()));
    core->receiverBusy(true);
    core->start();
    REQUIRE(eventually([&] { return world->counts(1).probes >= 1; }, 5s));
    std::this_thread::sleep_for(100ms); // the chain sleeps now
    const auto asleep = world->counts(1).probes;

    core->startSend(0, 50, "a result");
    CHECK(eventually([&] { return world->counts(1).probes > asleep; }, 5s));

    std::this_thread::sleep_for(100ms); // asleep once more
    const auto asleep_again = world->counts(1).probes;
    core->receiverBusy(false);
    CHECK(eventually([&] { return world->counts(1).probes > asleep_again; }, 5s));

    std::ignore = finish(core, {&threads});
    CHECK(world->violations().empty());
}
