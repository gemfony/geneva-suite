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

// The rules of GPeerI.hpp, checked on every transport that can run in one process: the in-process
// pair of GInProcessPeer.hpp -- the reference the roles are tested against -- and the MPI
// transport's ends (GMPIPeer) on the in-memory stand-in for MPI of GMPIFakeCalls.hpp. The cases
// that need what one transport alone can do (a fault of the in-process pair) run on that one; the
// websocket peer has rule tests of its own (GCourtierWebsocketPeerTests).

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include "GInProcessPeer.hpp"
#include "GInProcessTestSupport.hpp"
#include "GMPIFakeCalls.hpp"
#include "GPeerTestRole.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/transport/GIoContext.hpp"
#include "courtier/transport/GMPIPeerTransport.hpp"
#include "courtier/transport/GMPIPollCore.hpp"
#include "courtier/transport/GPeerI.hpp"

using Gem::Common::Concurrency::GThreadGroup;
using Gem::Courtier::GWirePeerId;
using Gem::Courtier::Consumers::GIoContext;
using Gem::Courtier::Consumers::GMPIPeer;
using Gem::Courtier::Consumers::GMPIPeerOptions;
using Gem::Courtier::Consumers::GMPIPollCore;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerEnd;
using Gem::Courtier::Consumers::GPeerHandlers;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Tests::GInProcessConnector;
using Gem::Courtier::Tests::GInProcessFaults;
using Gem::Courtier::Tests::GInProcessListener;
using Gem::Courtier::Tests::GInProcessPeer;
using Gem::Courtier::Tests::GInProcessRoute;
using Gem::Courtier::Tests::GMPIFakeWorld;
using Gem::Courtier::Tests::IoThreads;
using Gem::Courtier::Tests::makeRole;
using Gem::Courtier::Tests::Role;
using namespace std::chrono_literals;

namespace {

constexpr auto PATIENCE = 5s;       // how long a test waits for something that is due at once
constexpr auto CLOSE_GRACE = 300ms; // the ends' wait for an acknowledgement, where it matters

/** @brief Waits, at most PATIENCE, for @p condition @return Whether it held in time */
template <typename condition_type>
bool eventually(condition_type condition) {
    return Gem::Courtier::Tests::eventually(condition, PATIENCE);
}

/** @brief Two ends on one executor, with the given faults */
auto pairOn(
    const boost::asio::any_io_executor &executor,
    GInProcessFaults first = {},
    GInProcessFaults second = {},
    std::chrono::milliseconds grace = CLOSE_GRACE
) {
    return GInProcessPeer::makePair(executor, first, executor, second, grace);
}

/******************************************************************************/
/**
 * The transports the cases of the rules are run on (one set of rule tests for every transport).
 * Each makes the two ends of a connection on threads of its own, and says when what the first end
 * has handed over has reached the second end's executor (arrived()).
 */

/** @brief The in-process pair of GInProcessPeer.hpp */
class InProcessTransport {
public:
    using peer_type = GInProcessPeer;

    /** @brief @return The executor of the threads */
    boost::asio::any_io_executor executor() {
        return io_.executor();
    }

    /** @brief @param grace How long a close waits for its answer @return The two ends */
    std::pair<std::shared_ptr<GInProcessPeer>, std::shared_ptr<GInProcessPeer>>
    pair(std::chrono::milliseconds grace = CLOSE_GRACE) {
        auto [a, b] = pairOn(io_.executor(), {}, {}, grace);
        auto handed_over = std::make_shared<std::atomic<std::size_t>>(0);
        handed_over_ = handed_over;
        a->setTransmitHook([handed_over](std::string_view) { handed_over->fetch_add(1); });
        return {a, b};
    }

    /** @brief @param n A number @return Whether @p n messages (closes included) of the first end of
     *  the last pair have been posted to the second end's strand */
    [[nodiscard]] bool arrived(std::size_t n) const {
        return handed_over_ && handed_over_->load() >= n;
    }

private:
    IoThreads io_;                                          ///< The threads
    std::shared_ptr<std::atomic<std::size_t>> handed_over_; ///< See arrived()
};

/**
 * The MPI transport's ends (GMPIPeer) on the in-memory stand-in for MPI: every pair is a
 * communicator of two ranks, each with its poll. The threads are joined before the polls are ended
 * (GMPIPollCore::shutdown()), and the context goes after them.
 */
class MpiTransport {
public:
    using peer_type = GMPIPeer;

    MpiTransport() {
        for(std::size_t i = 0; i < 4; ++i) {
            threads_.create_thread([this] { ctx_.run(); });
        }
    }
    ~MpiTransport() {
        guard_.reset();
        ctx_.stop();
        threads_.join_all();
        for(auto &core : cores_) {
            std::ignore = core->shutdown();
        }
        ctx_.shutdown();
    }
    MpiTransport(const MpiTransport &) = delete;
    MpiTransport(MpiTransport &&) = delete;
    MpiTransport &operator=(const MpiTransport &) = delete;
    MpiTransport &operator=(MpiTransport &&) = delete;

    /** @brief @return The executor of the threads */
    boost::asio::any_io_executor executor() {
        return ctx_.get_executor();
    }

    /** @brief @param grace How long a close waits for its answer @return The two ends: rank 0's
     *  end of the connection to rank 1, and rank 1's end of the connection to rank 0 */
    std::pair<std::shared_ptr<GMPIPeer>, std::shared_ptr<GMPIPeer>>
    pair(std::chrono::milliseconds grace = CLOSE_GRACE) {
        world_ = std::make_shared<GMPIFakeWorld>(2);
        const GMPIPeerOptions options{.name = "MPI pair", .closeGrace = grace};
        auto first_core = std::make_shared<GMPIPollCore>(world_->calls(0), ctx_.get_executor());
        auto second_core = std::make_shared<GMPIPollCore>(world_->calls(1), ctx_.get_executor());
        auto a = std::make_shared<GMPIPeer>(
            first_core,
            ctx_.get_executor(),
            1,
            GMPIPeer::GOpening::AT_START,
            options
        );
        auto b = std::make_shared<GMPIPeer>(
            second_core,
            ctx_.get_executor(),
            0,
            GMPIPeer::GOpening::AT_START,
            options
        );
        first_core->addReceiver(1, a);
        second_core->addReceiver(0, b);
        first_core->start();
        second_core->start();
        cores_.push_back(std::move(first_core));
        cores_.push_back(std::move(second_core));
        return {a, b};
    }

    /** @brief @param n A number @return Whether @p n messages (closes included) of the first end of
     *  the last pair have been received by the second rank's poll (and posted to its end's strand) */
    [[nodiscard]] bool arrived(std::size_t n) const {
        return world_ && world_->counts(1).receives >= n;
    }

private:
    GIoContext ctx_; ///< The context of the polls and the ends
    boost::asio::executor_work_guard<GIoContext::executor_type> guard_ =
        boost::asio::make_work_guard(ctx_);
    GThreadGroup threads_;                             ///< Its threads
    std::vector<std::shared_ptr<GMPIPollCore>> cores_; ///< Every poll made, to be ended
    std::shared_ptr<GMPIFakeWorld> world_;             ///< The communicator of the last pair
};

/** @brief Holds the strand of @p executor until the latch it returns is counted down
 *  @param executor A strand @return The latch */
std::shared_ptr<Gem::Common::Concurrency::GCompletionLatchT>
holdStrand(const boost::asio::any_io_executor &executor) {
    auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    boost::asio::post(executor, [held = release]() { held->wait(); });
    return release;
}

/** @brief Waits, at most PATIENCE, until every handler posted to @p executor before this call has
 *  run (a strand runs its handlers in the order they were posted)
 *  @param executor A strand @return Whether they ran in time */
bool flushed(const boost::asio::any_io_executor &executor) {
    const auto done = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    boost::asio::post(executor, [done] { done->count_down(); });
    return done->wait_for(PATIENCE);
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * Rule 1: nothing is reported before start(), and onOpen comes before any message. Messages that
 * reach an end before it is started are kept, not lost. After close() onOpen is not delivered, nor
 * is any message that arrived before.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEMPLATE_TEST_CASE(
    "Peer rules: rule 1 -- two phases",
    "[courtier][peer]",
    InProcessTransport,
    MpiTransport
) {
    TestType transport;

    SECTION("onOpen precedes the messages that arrived before start()") {
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->whenOpen = [](Role &role) {
            role.send("m1");
            role.send("m2");
            role.send("m3");
        };
        role_a->start(a);
        REQUIRE(eventually([&] { return role_a->events().size() == 1; }));
        REQUIRE(eventually([&] { return transport.arrived(3); }));
        REQUIRE(flushed(b->executor())); // what arrived has been handled by b
        CHECK(role_b->events().empty());

        role_b->start(b);
        REQUIRE(eventually([&] { return role_b->events().size() == 4; }));
        CHECK(role_b->events() == std::vector<std::string>{"open", "msg:m1", "msg:m2", "msg:m3"});
        CHECK(role_a->nNested() == 0);
        CHECK(role_b->nNested() == 0);
    }

    SECTION("after close() onOpen is not delivered, also when close() precedes the open; nor is "
            "any message then") {
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        std::atomic<bool> sent{false};
        role_b->whenOpen = [&sent](Role &role) {
            role.send("early");
            sent.store(true);
        };
        // The strand of a is held, so that the message of b arrives at a before a is opened, and
        // start()'s work runs only after close() was called.
        const auto release = holdStrand(a->executor());
        role_b->start(b);
        REQUIRE(eventually([&] { return sent.load(); }));
        role_a->start(a);
        a->close(GPeerClose::NORMAL);
        release->count_down();
        REQUIRE(eventually([&] { return role_a->nClosed() == 1; }));
        CHECK(role_a->events() == std::vector<std::string>{"closed:CLOSED_HERE:NORMAL"});
    }
}

/******************************************************************************/
/**
 * Rules 2 and 3: a close() from another thread right after start() is ordered after start()'s
 * work (the end is opened, then closed; the role is told of the open only if its close() came
 * after it, rule 6), and no handler runs inside a call of GPeerI -- also not when a role sends and
 * closes from inside its handlers.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEMPLATE_TEST_CASE(
    "Peer rules: rules 2 and 3 -- one executor and no handler inside a call",
    "[courtier][peer]",
    InProcessTransport,
    MpiTransport
) {
    TestType transport;
    constexpr int ROUNDS = 100;
    for(int round = 0; round < ROUNDS; ++round) {
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->whenOpen = [](Role &role) { role.send("ping"); };
        role_b->whenMessage = [](Role &role, const std::string &message) {
            role.send("echo " + message);
            role.close(GPeerClose::NORMAL);
        };
        role_b->start(b);
        role_a->start(a);
        if(round % 2 == 0) {
            role_a->close(GPeerClose::NORMAL); // from this thread, right after start()
        }

        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
        const auto events_a = role_a->events();
        REQUIRE_FALSE(events_a.empty());
        // Closed right after start(): onOpen only if the open came before the close (rule 6)
        CHECK((events_a.front() == "open" || round % 2 == 0));
        CHECK(events_a.back().starts_with("closed:"));
        CHECK(role_a->nNested() == 0);
        CHECK(role_b->nNested() == 0);
    }
}

/******************************************************************************/
/**
 * Rule 5: onClosed is called exactly once while the executor runs -- when one end closes, when
 * both close at once from two threads, and when close() is called twice.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEMPLATE_TEST_CASE(
    "Peer rules: rule 5 -- onClosed exactly once",
    "[courtier][peer]",
    InProcessTransport,
    MpiTransport
) {
    TestType transport;
    constexpr int ROUNDS = 100;
    for(int round = 0; round < ROUNDS; ++round) {
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] {
            return role_a->events().size() == 1 && role_b->events().size() == 1;
        }));

        {
            std::jthread const closer_a([&] {
                role_a->close(GPeerClose::NORMAL);
                role_a->close(GPeerClose::ERROR); // idempotent
            });
            std::jthread const closer_b([&] { role_b->close(GPeerClose::RUN_ENDED); });
        }

        // An end reports its end after it has ended: wait for the reports. Once both ends have
        // ended nothing is on its way between them any more (each end ends on the other's last
        // message), so a second report could only come from a handler already on one of the two
        // strands: when those have run, the count is final.
        REQUIRE(eventually([&] { return role_a->nClosed() >= 1 && role_b->nClosed() >= 1; }));
        REQUIRE(eventually([&] { return a->ended() && b->ended(); }));
        REQUIRE(flushed(a->executor()));
        REQUIRE(flushed(b->executor()));
        CHECK(role_a->nClosed() == 1);
        CHECK(role_b->nClosed() == 1);
    }
}

/******************************************************************************/
/**
 * Rule 6: a close sends what was handed to send() before it, and what the close says reaches the
 * other end; a send() after close() is dropped; what arrives after close() is delivered until this
 * end's close has gone out, and not after; only an answer confirms a close, the other end's own
 * close crossing it does not; an end whose role closed before it was told of the open confirms
 * nothing.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEMPLATE_TEST_CASE(
    "Peer rules: rule 6 -- what a close sends and what it stops",
    "[courtier][peer]",
    InProcessTransport,
    MpiTransport
) {
    TestType transport;

    SECTION("everything sent before the close arrives, in order, before the close") {
        constexpr int N_MESSAGES = 50;
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->whenOpen = [](Role &role) {
            for(int i = 0; i < N_MESSAGES; ++i) {
                role.send(std::to_string(i));
            }
            role.close(GPeerClose::RUN_ENDED);
            role.send("late"); // after close(): dropped
        };
        role_b->start(b);
        role_a->start(a);
        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));

        std::vector<std::string> expected{"open"};
        for(int i = 0; i < N_MESSAGES; ++i) {
            expected.push_back("msg:" + std::to_string(i));
        }
        expected.emplace_back("closed:CLOSED_THERE:RUN_ENDED");
        CHECK(role_b->events() == expected);
        // The other end answered the close: confirmed (rule 6)
        CHECK(role_a->events().back() == "closed:CLOSED_HERE:RUN_ENDED");
    }

    SECTION("what arrives after close() and before this end's close has gone out is delivered") {
        constexpr std::size_t N_MESSAGES = 100;
        constexpr std::size_t CLOSE_AT = 10;
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->whenOpen = [](Role &role) {
            for(std::size_t i = 0; i < N_MESSAGES; ++i) {
                role.send(std::to_string(i));
            }
        };
        role_b->whenMessage = [](Role &role, const std::string &) {
            if(role.nMessages() == CLOSE_AT) {
                role.close(GPeerClose::NORMAL);
            }
        };
        role_b->start(b);
        // The strand of b is held until everything a sent has reached it: all of it arrives at b's
        // role before b's close goes out, which b's role asks for at the tenth message.
        const auto release = holdStrand(b->executor());
        role_a->start(a);
        REQUIRE(eventually([&] { return transport.arrived(N_MESSAGES); }));
        release->count_down();
        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
        CHECK(role_b->nMessages() == N_MESSAGES);
        CHECK(role_b->events().back() == "closed:CLOSED_HERE:NORMAL");
        CHECK(role_a->events().back() == "closed:CLOSED_THERE:NORMAL");
    }

    SECTION("what arrives after this end's close has gone out is not delivered") {
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] {
            return role_a->events().size() == 1 && role_b->events().size() == 1;
        }));
        // The strand of a is held; a sends one more message before it hears of b's close, which
        // has gone out by then.
        const auto release = holdStrand(a->executor());
        boost::asio::post(a->executor(), [&role_a] { role_a->send("late"); });
        role_b->close(GPeerClose::NORMAL);
        release->count_down();
        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
        CHECK(role_b->events() ==
              std::vector<std::string>{"open", "closed:CLOSED_HERE:NORMAL"});
    }

    SECTION("an end whose role closed before it was told of the open confirms nothing") {
        // a sends D and closes before b is started: both wait at b. b's role then asks for the
        // close before it is told of the open. b's role never sees D, so a's close must not be
        // confirmed: b's close goes out before b reads what has arrived, and the two cross.
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->whenOpen = [](Role &role) {
            role.send("D");
            role.close(GPeerClose::NORMAL);
        };
        role_a->start(a);
        REQUIRE(eventually([&] { return transport.arrived(2); })); // D and the close
        // what a sent has reached b (b's strand has run it)
        const auto arrived = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
        boost::asio::post(b->executor(), [arrived] { arrived->count_down(); });
        REQUIRE(arrived->wait_for(PATIENCE));

        const auto release = holdStrand(b->executor());
        role_b->start(b);
        role_b->close(GPeerClose::NORMAL); // before start()'s work has run
        release->count_down();
        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
        CHECK(role_a->events().back() == "closed:CLOSED_HERE_UNCONFIRMED:NORMAL");
        CHECK(role_b->events() ==
              std::vector<std::string>{"closed:CLOSED_HERE_UNCONFIRMED:NORMAL"});
    }

    SECTION("two closes that cross confirm neither") {
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] {
            return role_a->events().size() == 1 && role_b->events().size() == 1;
        }));
        // Both strands are held, so that each end's close has gone out before the other's
        // arrives.
        const auto release_a = holdStrand(a->executor());
        const auto release_b = holdStrand(b->executor());
        role_a->close(GPeerClose::NORMAL);
        role_b->close(GPeerClose::ERROR);
        release_a->count_down();
        release_b->count_down();
        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
        CHECK(role_a->events().back() == "closed:CLOSED_HERE_UNCONFIRMED:ERROR");
        CHECK(role_b->events().back() == "closed:CLOSED_HERE_UNCONFIRMED:NORMAL");
    }

    SECTION("abort() reports at once; the other end hears a close") {
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] {
            return role_a->events().size() == 1 && role_b->events().size() == 1;
        }));

        std::promise<int> closed_in_abort;
        boost::asio::post(a->executor(), [&, peer = a] {
            peer->abort();
            closed_in_abort.set_value(role_a->nClosed());
        });
        CHECK(closed_in_abort.get_future().get() == 1);
        CHECK(role_a->events().back() == "closed:CLOSED_HERE:NORMAL");
        REQUIRE(eventually([&] { return role_b->nClosed() == 1; }));
        CHECK(role_b->events().back() == "closed:CLOSED_THERE:NORMAL");
    }

    SECTION("the first close decides what the other end is told") {
        auto [a, b] = transport.pair();
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] { return role_b->events().size() == 1; }));
        a->close(GPeerClose::ERROR);
        a->close(GPeerClose::RUN_ENDED); // changes nothing
        REQUIRE(eventually([&] { return role_b->nClosed() == 1; }));
        CHECK(role_b->events().back() == "closed:CLOSED_THERE:ERROR");
    }

    SECTION("setOwnerBusy() reaches the end") {
        auto [a, b] = transport.pair();
        std::promise<bool> busy;
        boost::asio::post(a->executor(), [&, peer = a] {
            peer->setOwnerBusy(true);
            busy.set_value(peer->ownerBusy());
        });
        CHECK(busy.get_future().get());
    }
}
/******************************************************************************/
/** Rule 1, for the in-process pair: a peer that cannot be opened reports FAILED without onOpen. */
TEST_CASE("Peer rules (in-process): rule 1 -- a peer that cannot be opened", "[courtier][peer]") {
    IoThreads io;
    auto [a, b] = pairOn(io.executor(), GInProcessFaults{.failStart = true});
    auto role_a = makeRole();
    role_a->start(a);
    REQUIRE(eventually([&] { return role_a->nClosed() == 1; }));
    CHECK(role_a->events() == std::vector<std::string>{"closed:FAILED:NORMAL"});
}

/******************************************************************************/
/**
 * Rule 4: the peer owns the role through its handlers and drops them after onClosed (the role,
 * which nobody else holds, then goes); the role holds the peer weakly, so the ends go too. And
 * an open end keeps its executor's context running. (Two linear scenarios; the size and the
 * complexity the linter counts come from Catch2's macros and the local stopper class.)
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size)
TEST_CASE("Peer rules (in-process): rule 4 -- ownership", "[courtier][peer]") {
    SECTION("roles and peers go when the connection has ended") {
        IoThreads io;
        std::weak_ptr<Role> weak_role_a;
        std::weak_ptr<Role> weak_role_b;
        std::weak_ptr<GPeerI> weak_a;
        std::weak_ptr<GPeerI> weak_b;
        {
            auto [a, b] = pairOn(io.executor());
            auto role_a = makeRole();
            auto role_b = makeRole();
            role_a->start(a);
            role_b->start(b);
            weak_role_a = role_a;
            weak_role_b = role_b;
            weak_a = a;
            weak_b = b;
            REQUIRE(eventually([&] { return role_a->events().size() == 1; }));
        }
        // Only the peers hold the roles now, and only their pending work holds the peers.
        CHECK_FALSE(weak_role_a.expired());
        CHECK_FALSE(weak_a.expired());

        if(auto a = weak_a.lock()) {
            a->close(GPeerClose::NORMAL);
        }
        CHECK(eventually([&] {
            return weak_role_a.expired() && weak_role_b.expired() && weak_a.expired() &&
                   weak_b.expired();
        }));
    }

    SECTION("an open end keeps the context running; it stops when the end has ended") {
        boost::asio::io_context ctx; // no work guard
        auto [a, b] = pairOn(ctx.get_executor());
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);

        std::atomic<bool> returned{false};
        std::jthread const runner([&] {
            ctx.run();
            returned.store(true);
        });
        /** Stops the context before the runner is joined, whatever the checks found */
        class Stopper {
        public:
            explicit Stopper(boost::asio::io_context &ctx)
              : ctx_(ctx) { /* nothing */
            }
            Stopper(const Stopper &) = delete;
            Stopper(Stopper &&) = delete;
            Stopper &operator=(const Stopper &) = delete;
            Stopper &operator=(Stopper &&) = delete;
            ~Stopper() {
                ctx_.stop();
            }

        private:
            boost::asio::io_context &ctx_;
        };
        const Stopper stopper{ctx};

        CHECK(eventually([&] { return role_b->events().size() == 1; }));
        // The context still runs handlers after both ends have opened: it has not run out of work
        const auto ran = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
        boost::asio::post(ctx, [ran] { ran->count_down(); });
        CHECK(ran->wait_for(PATIENCE));
        CHECK_FALSE(returned.load());

        role_a->close(GPeerClose::NORMAL);
        CHECK(eventually([&] { return returned.load(); }));
    }
}

/******************************************************************************/
/**
 * The close protocol of the in-process pair: a close that is never acknowledged ends at the grace,
 * and is not confirmed (rule 6); abort() of an end whose close was under way reports
 * CLOSED_HERE_UNCONFIRMED; an end that loses messages after a number of them still hears the close.
 * (abort() of an end that had not been closed, and the first close's word, are cases of rule 6 for
 * every transport.)
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE("Peer rules (in-process): the close protocol and the switches", "[courtier][peer]") {
    IoThreads io;

    SECTION("a close that is never acknowledged ends at the grace") {
        auto [a, b] = pairOn(io.executor(), {}, GInProcessFaults{.acknowledgeClose = false});
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] { return role_b->events().size() == 1; }));

        const auto begin = std::chrono::steady_clock::now();
        role_a->close(GPeerClose::RUN_ENDED);
        REQUIRE(eventually([&] { return role_a->nClosed() == 1; }));
        const auto took = std::chrono::steady_clock::now() - begin;
        CHECK(took >= CLOSE_GRACE);
        CHECK(took < CLOSE_GRACE + 2s);
        CHECK(role_a->events().back() == "closed:CLOSED_HERE_UNCONFIRMED:NORMAL");
        CHECK_FALSE(b->ended()); // it is let go with the context, without onClosed (rule 5)
    }

    SECTION("abort() of an end whose close is not confirmed reports it so") {
        auto [a, b] = pairOn(io.executor(), {}, GInProcessFaults{.acknowledgeClose = false});
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] { return role_b->events().size() == 1; }));

        std::promise<int> closed_in_abort;
        boost::asio::post(a->executor(), [&, peer = a] {
            peer->close(GPeerClose::NORMAL);
            boost::asio::post(peer->executor(), [&, peer] { // after the close has gone out
                peer->abort();
                closed_in_abort.set_value(role_a->nClosed());
            });
        });
        CHECK(closed_in_abort.get_future().get() == 1);
        CHECK(role_a->events().back() == "closed:CLOSED_HERE_UNCONFIRMED:NORMAL");
    }

    SECTION("an end that loses what comes after three messages") {
        auto [a, b] = pairOn(io.executor(), {}, GInProcessFaults{.deliverAtMost = 3});
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->whenOpen = [](Role &role) {
            for(int i = 0; i < 10; ++i) {
                role.send(std::to_string(i));
            }
            role.close(GPeerClose::NORMAL);
        };
        role_b->start(b);
        role_a->start(a);
        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
        CHECK(role_b->nMessages() == 3);
    }

    SECTION("GPeerI's default of setOwnerBusy() ignores it") {
        /** A peer that does not override setOwnerBusy() */
        struct Minimal final : GPeerI {
            void start(GPeerHandlers) override {
            }
            void send(Gem::Courtier::Consumers::GMessage) override {
            }
            void close(GPeerClose) override {
            }
            void abort() override {
            }
            boost::asio::any_io_executor executor() override {
                return {};
            }
        };
        Minimal minimal;
        minimal.setOwnerBusy(true); // compiles and does nothing
    }
}

/******************************************************************************/
/**
 * The in-process listener and connector: a connection per connect(), the server end handed to
 * on_peer with a fresh id, none after stop(); done is not called after cancel(); endWork().
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE("Peer rules (in-process): listener and connector", "[courtier][peer]") {
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{
        .stopGrace = 1500ms,
        .detectsLoss = false,
    });
    CHECK(listener->stopGrace() == 1500ms);
    CHECK_FALSE(listener->detectsLoss());
    // The connector goes after the threads its ends run on, the ends kept here before them
    GInProcessConnector connector(std::make_shared<GInProcessRoute>(listener));
    IoThreads io;

    std::mutex mutex;
    std::vector<std::pair<std::shared_ptr<GPeerI>, GWirePeerId>> server_ends;
    listener->start(io.executor(), [&](std::shared_ptr<GPeerI> peer, GWirePeerId id) {
        const std::scoped_lock lock(mutex);
        server_ends.emplace_back(std::move(peer), id);
    });

    auto strand = boost::asio::make_strand(io.executor());
    CHECK(connector.reconnectable());

    // done runs on the strand; the futures carry its result to this thread.
    auto connect = [&] {
        auto outcome = std::make_shared<std::promise<std::shared_ptr<GPeerI>>>();
        auto future = outcome->get_future();
        boost::asio::post(strand, [&, outcome] {
            connector.connect(strand, [outcome](std::shared_ptr<GPeerI> peer, std::string_view) {
                outcome->set_value(std::move(peer));
            });
        });
        return future;
    };

    auto client_end = connect().get();
    REQUIRE(client_end != nullptr);
    REQUIRE(eventually([&] {
        const std::scoped_lock lock(mutex);
        return server_ends.size() == 1;
    }));

    // The two ends are connected.
    auto server_role = makeRole();
    auto client_role = makeRole();
    client_role->whenOpen = [](Role &role) { role.send("hello"); };
    {
        const std::scoped_lock lock(mutex);
        CHECK(server_ends.front().second == 1);
        server_role->start(server_ends.front().first);
    }
    client_role->start(client_end);
    CHECK(eventually([&] { return server_role->nMessages() == 1; }));

    CHECK(connect().get() != nullptr);
    REQUIRE(eventually([&] {
        const std::scoped_lock lock(mutex);
        return server_ends.size() == 2 && server_ends.back().second == 2;
    }));

    // cancel(): done is not called.
    std::atomic<bool> called{false};
    const auto issued = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    boost::asio::post(strand, [&] {
        connector.connect(strand, [&called](const std::shared_ptr<GPeerI> &, std::string_view) {
            called.store(true);
        });
        connector.cancel();
        issued->count_down();
    });
    issued->wait();
    REQUIRE(flushed(strand)); // done would have been posted to the strand before this
    CHECK_FALSE(called.load());

    // After stop() there are no new connections.
    listener->stop();
    CHECK(connect().get() == nullptr);

    client_role->close(GPeerClose::NORMAL);
    CHECK(eventually([&] { return client_role->nClosed() == 1 && server_role->nClosed() == 1; }));

    // endWork() is due when no handler of the listener runs: every on_peer call has been made
    // (two connections, none after stop()).
    {
        const std::scoped_lock lock(mutex);
        server_ends.clear();
    }
    listener->endWork();
    CHECK(listener->workEnded());
}

/******************************************************************************/
/**
 * An end of the pair never outlives the io context its strand lives on: while the server's end hands
 * its close to the client's end (held there, on a server thread), the client's context is
 * destroyed. The client's end goes with it, not later on the server's thread, and the server's end
 * does not post into the context that has gone.
 */
TEST_CASE(
    "Peer rules (in-process): an end does not outlive its io context",
    "[courtier][peer]"
) {
    IoThreads server;
    auto client_ctx = std::make_unique<boost::asio::io_context>();
    auto [server_end, client_end] =
        GInProcessPeer::makePair(server.executor(), {}, client_ctx->get_executor(), {}, CLOSE_GRACE);

    const auto in_hook = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    bool hooked = false; // on the server end's strand only
    server_end->setTransmitHook([&in_hook, release, &hooked](std::string_view) {
        if(hooked) {
            return;
        }
        hooked = true;
        in_hook->count_down();
        release->wait_for(PATIENCE);
    });
    server_end->start(GPeerHandlers{});
    client_end->start(GPeerHandlers{});
    client_ctx->poll(); // the client's end opens: it is owned by the wait pending on its strand
    const std::weak_ptr<GInProcessPeer> client_weak = client_end;
    client_end.reset();
    REQUIRE_FALSE(client_weak.expired());

    // The server's end hands its CLOSE over, and is held there.
    server_end->close(GPeerClose::NORMAL);
    REQUIRE(in_hook->wait_for(PATIENCE));

    // The client's context goes, and with it everything that owns the client's end.
    client_ctx.reset();
    CHECK(client_weak.expired());

    release->count_down();
    REQUIRE(eventually([&] { return server_end->ended(); }));
}

/******************************************************************************/
/**
 * abort() at teardown, before the work of start() has run (its context never ran): the end does not
 * open afterwards, and the role hears the end once, when that work runs.
 */
TEST_CASE(
    "Peer rules (in-process): an end aborted before its start has run does not open",
    "[courtier][peer]"
) {
    boost::asio::io_context ctx; // no thread runs it yet
    auto [end, other] = GInProcessPeer::makePair(
        ctx.get_executor(),
        {},
        ctx.get_executor(),
        {},
        CLOSE_GRACE
    );
    std::size_t n_opened = 0; // on ctx's thread, which is this one
    std::size_t n_closed = 0;
    end->start(
        GPeerHandlers{
            .onOpen = [&n_opened] { ++n_opened; },
            .onMessage = {},
            .onClosed = [&n_closed](GPeerEnd, GPeerClose, std::string_view) { ++n_closed; },
        }
    );
    other->start(GPeerHandlers{});
    end->abort();
    ctx.run_for(300ms);
    CHECK(n_opened == 0);
    CHECK(n_closed == 1);
    CHECK(end->ended());
}

/******************************************************************************/
/**
 * An on_peer that throws: as with the websocket listener, the exception does not leave the thread
 * that runs it (the listener's own; GListenerI::start()), which goes on calling on_peer for the next
 * connection, and the call is counted out, so that stop() does not wait for it until its bound.
 */
TEST_CASE("Peer rules (in-process): an on_peer that throws is counted out", "[courtier][peer]") {
    const bool standard = GENERATE(true, false);
    INFO((standard ? "on_peer throws a std::exception" : "on_peer throws an int"));
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{
        .stopBound = 5s,
    });
    IoThreads io(1); // the server ends' handlers

    std::atomic<int> calls{0};
    listener->start(
        io.executor(),
        [&calls, standard](const std::shared_ptr<GPeerI> &, GWirePeerId) {
            calls.fetch_add(1);
            if(standard) {
                throw std::runtime_error("on_peer refused by the test");
            }
            throw 42; // NOLINT(hicpp-exception-baseclass,cert-err60-cpp) -- not a std::exception, on purpose
        }
    );
    auto first_end = listener->connect_(io.executor());
    REQUIRE(first_end != nullptr);
    REQUIRE(eventually([&] { return calls.load() == 1; }));
    auto second_end = listener->connect_(io.executor());
    REQUIRE(second_end != nullptr);
    CHECK(eventually([&] { return calls.load() == 2; })); // the listener's thread goes on

    const auto stop_began = std::chrono::steady_clock::now();
    listener->stop();
    const auto took = std::chrono::steady_clock::now() - stop_began;
    CHECK(listener->nStopBoundsHit() == 0);
    CHECK(took < 2s);

    first_end.reset();
    second_end.reset();
    listener->endWork();
}

/******************************************************************************/
/**
 * A cut connection ends as a reset: both ends report FAILED, no close travels, and what was handed
 * over just before the cut and has not been delivered yet is not delivered.
 */
TEST_CASE("Peer rules (in-process): a severed connection fails at both ends", "[courtier][peer]") {
    IoThreads io;
    auto [a, b] = pairOn(io.executor());
    auto role_a = makeRole();
    auto role_b = makeRole();
    role_a->start(a);
    role_b->start(b);
    REQUIRE(eventually([&] {
        return not role_a->events().empty() && not role_b->events().empty();
    }));
    boost::asio::post(a->executor(), [role_a] { role_a->send("delivered"); });
    REQUIRE(eventually([&] { return role_b->nMessages() == 1; }));

    // The message is on its way when the connection is cut: the other end's strand is held until
    // the cut has been made, so that it cannot have been delivered before.
    const auto cut = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    boost::asio::post(b->executor(), [cut] { cut->wait(); });
    boost::asio::post(a->executor(), [role_a, a, &cut] {
        role_a->send("lost");
        a->sever();
        cut->count_down();
    });
    REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
    REQUIRE(flushed(a->executor())); // what was on its way has been handled
    REQUIRE(flushed(b->executor()));
    CHECK(role_a->events().back() == "closed:FAILED:NORMAL");
    CHECK(role_b->events().back() == "closed:FAILED:NORMAL");
    CHECK(role_b->nMessages() == 1);
    CHECK(role_a->nClosed() == 1);
    CHECK(role_b->nClosed() == 1);
}

/******************************************************************************/
/** A connector connects to the listener its route points at when it connects, and a route without
 *  one refuses. */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE("Peer rules (in-process): a route can be switched and cleared", "[courtier][peer]") {
    auto first = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    auto second = std::make_shared<GInProcessListener>(GInProcessListener::Options{});
    auto route = std::make_shared<GInProcessRoute>(first);
    // The connector goes after the threads its ends run on, the ends kept here before them
    GInProcessConnector connector(route);
    IoThreads io;
    std::mutex mutex;
    std::vector<std::shared_ptr<GPeerI>> server_ends;
    auto keep = [&](std::shared_ptr<GPeerI> peer, GWirePeerId) {
        const std::scoped_lock lock(mutex);
        server_ends.push_back(std::move(peer));
    };
    first->start(io.executor(), keep);
    second->start(io.executor(), keep);

    auto strand = boost::asio::make_strand(io.executor());
    auto connect = [&] {
        auto outcome = std::make_shared<std::promise<std::shared_ptr<GPeerI>>>();
        auto future = outcome->get_future();
        boost::asio::post(strand, [&, outcome] {
            connector.connect(strand, [outcome](std::shared_ptr<GPeerI> peer, std::string_view) {
                outcome->set_value(std::move(peer));
            });
        });
        return future.get();
    };

    std::vector<std::shared_ptr<GPeerI>> client_ends;
    client_ends.push_back(connect());
    CHECK(client_ends.back() != nullptr);
    CHECK(first->nConnections() == 1);

    route->set(second);
    client_ends.push_back(connect());
    CHECK(client_ends.back() != nullptr);
    CHECK(first->nConnections() == 1);
    CHECK(second->nConnections() == 1);

    route->clear();
    CHECK(connect() == nullptr);
    CHECK(first->nConnections() == 1);
    CHECK(second->nConnections() == 1);

    first->stop();
    second->stop();
    client_ends.clear();
    {
        const std::scoped_lock lock(mutex);
        server_ends.clear();
    }
    first->endWork();
    second->endWork();
}
