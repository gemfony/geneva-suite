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

// The rules of GPeerI.hpp, checked on the in-process pair of GInProcessPeer.hpp. The pair is the
// reference the roles are tested against later, so it must keep the rules itself; a transport
// that is added later can be run through the same cases.

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
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>

#include "GInProcessPeer.hpp"
#include "GInProcessTestSupport.hpp"
#include "courtier/transport/GPeerI.hpp"

using Gem::Courtier::GWirePeerId;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerEnd;
using Gem::Courtier::Consumers::GPeerHandlers;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Tests::closeName;
using Gem::Courtier::Tests::endName;
using Gem::Courtier::Tests::GInProcessConnector;
using Gem::Courtier::Tests::GInProcessFaults;
using Gem::Courtier::Tests::GInProcessListener;
using Gem::Courtier::Tests::GInProcessPeer;
using Gem::Courtier::Tests::GInProcessRoute;
using Gem::Courtier::Tests::IoThreads;
using namespace std::chrono_literals;

namespace {

constexpr auto PATIENCE = 5s;       // how long a test waits for something that is due at once
constexpr auto CLOSE_GRACE = 300ms; // the ends' wait for an acknowledgement, where it matters

/** @brief Waits, at most PATIENCE, for @p condition @return Whether it held in time */
template <typename condition_type>
bool eventually(condition_type condition) {
    return Gem::Courtier::Tests::eventually(condition, PATIENCE);
}

/** @brief @return How deep this thread is inside calls of GPeerI made by a role (rule 3) */
int &inPeerCall() {
    thread_local int depth = 0;
    return depth;
}

/** @brief Marks the span of a call of GPeerI, so that a handler run inside it is seen (rule 3) */
struct InPeerCall {
    InPeerCall() {
        ++inPeerCall();
    }
    ~InPeerCall() {
        --inPeerCall();
    }
    InPeerCall(const InPeerCall &) = delete;
    InPeerCall(InPeerCall &&) = delete;
    InPeerCall &operator=(const InPeerCall &) = delete;
    InPeerCall &operator=(InPeerCall &&) = delete;
};

/**
 * A role for the tests: records what its peer tells it ("open", "msg:<text>",
 * "closed:<end>:<how>") and whether a handler ran inside one of its calls of GPeerI. What it does
 * when it is opened or gets a message is up to the test. Like the real roles it is owned by its
 * peer's handlers and refers to the peer weakly.
 */
class Role : public std::enable_shared_from_this<Role> {
public:
    std::function<void(Role &)> whenOpen;                         ///< Done on onOpen
    std::function<void(Role &, const std::string &)> whenMessage; ///< Done on onMessage

    /** @brief @return Handlers that report to this role (and own it) */
    GPeerHandlers handlers() {
        auto self = this->shared_from_this();
        return GPeerHandlers{
            .onOpen =
                [self] {
                    self->note_("open");
                    if(self->whenOpen) {
                        self->whenOpen(*self);
                    }
                },
            .onMessage =
                [self](std::string &&message) {
                    const std::string text{std::move(message)};
                    self->note_("msg:" + text);
                    if(self->whenMessage) {
                        self->whenMessage(*self, text);
                    }
                },
            .onClosed =
                [self](GPeerEnd end, GPeerClose how, std::string_view) {
                    self->note_("closed:" + endName(end) + ":" + closeName(how));
                    self->n_closed_.fetch_add(1);
                },
        };
    }

    /** @brief Starts @p peer for this role, which then holds it weakly @param peer The peer */
    void start(const std::shared_ptr<GPeerI> &peer) {
        peer_ = peer;
        const InPeerCall call;
        peer->start(this->handlers());
    }

    /** @brief Sends through the peer (on its executor) @param message What is sent */
    void send(std::string message) {
        if(auto peer = peer_.lock()) {
            const InPeerCall call;
            peer->send(Gem::Courtier::Consumers::makeMessage(std::move(message)));
        }
    }

    /** @brief Closes the peer (any thread) @param how What the other end is told */
    void close(GPeerClose how) {
        if(auto peer = peer_.lock()) {
            const InPeerCall call;
            peer->close(how);
        }
    }

    /** @brief @return A copy of what has been recorded */
    std::vector<std::string> events() const {
        const std::scoped_lock lock(mutex_);
        return events_;
    }

    /** @brief @return How many messages have been recorded */
    std::size_t nMessages() const {
        const std::scoped_lock lock(mutex_);
        std::size_t n = 0;
        for(const auto &event : events_) {
            n += event.starts_with("msg:") ? 1 : 0;
        }
        return n;
    }

    /** @brief @return How often onClosed was called */
    int nClosed() const {
        return n_closed_.load();
    }
    /** @brief @return How often a handler ran inside a call of GPeerI made by this role */
    int nNested() const {
        return n_nested_.load();
    }

private:
    /** @brief Records one event @param event What happened */
    void note_(std::string event) {
        if(inPeerCall() > 0) {
            n_nested_.fetch_add(1);
        }
        const std::scoped_lock lock(mutex_);
        events_.push_back(std::move(event));
    }

    mutable std::mutex mutex_;        ///< Guards events_ (the test reads them on its own thread)
    std::vector<std::string> events_; ///< What happened, in order
    std::atomic<int> n_closed_{0};    ///< onClosed calls
    std::atomic<int> n_nested_{0};    ///< Handlers run inside a call of GPeerI
    std::weak_ptr<GPeerI> peer_;      ///< The peer, weakly (rule 4)
};

/** @brief Two ends on one executor, with the given faults */
auto pairOn(
    const boost::asio::any_io_executor &executor,
    GInProcessFaults first = {},
    GInProcessFaults second = {},
    std::chrono::milliseconds grace = CLOSE_GRACE
) {
    return GInProcessPeer::makePair(executor, first, executor, second, grace);
}

/** @brief @return A role, owned by the caller until its peer takes it over */
std::shared_ptr<Role> makeRole() {
    return std::make_shared<Role>();
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * Rule 1: nothing is reported before start(), and onOpen comes before any message. Messages that
 * reach an end before it is started are kept, not lost, by this transport.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE("Peer rules (in-process): rule 1 -- two phases", "[courtier][peer]") {
    IoThreads io;

    SECTION("onOpen precedes the messages that arrived before start()") {
        auto [a, b] = pairOn(io.executor());
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->whenOpen = [](Role &role) {
            role.send("m1");
            role.send("m2");
            role.send("m3");
        };
        role_a->start(a);
        REQUIRE(eventually([&] { return b->nDelivered() == 0 && role_a->events().size() == 1; }));
        std::this_thread::sleep_for(50ms); // the messages are on their way to b
        CHECK(role_b->events().empty());

        role_b->start(b);
        REQUIRE(eventually([&] { return role_b->events().size() == 4; }));
        CHECK(role_b->events() == std::vector<std::string>{"open", "msg:m1", "msg:m2", "msg:m3"});
        CHECK(role_a->nNested() == 0);
        CHECK(role_b->nNested() == 0);
    }

    SECTION("after close() onOpen is not delivered, also when close() precedes the open; nor is "
            "any message then") {
        auto [a, b] = pairOn(io.executor());
        auto role_a = makeRole();
        auto role_b = makeRole();
        std::atomic<bool> sent{false};
        role_b->whenOpen = [&sent](Role &role) {
            role.send("early");
            sent.store(true);
        };
        // The strand of a is held, so that the message of b arrives at a before a is opened, and
        // start()'s work runs only after close() was called.
        std::promise<void> release;
        boost::asio::post(a->executor(), [held = release.get_future()]() mutable { held.wait(); });
        role_b->start(b);
        REQUIRE(eventually([&] { return sent.load(); }));
        role_a->start(a);
        a->close(GPeerClose::NORMAL);
        release.set_value();
        REQUIRE(eventually([&] { return role_a->nClosed() == 1; }));
        CHECK(role_a->events() == std::vector<std::string>{"closed:CLOSED_HERE:NORMAL"});
    }

    SECTION("a peer that cannot be opened reports FAILED without onOpen") {
        auto [a, b] = pairOn(io.executor(), GInProcessFaults{.failStart = true});
        auto role_a = makeRole();
        role_a->start(a);
        REQUIRE(eventually([&] { return role_a->nClosed() == 1; }));
        CHECK(role_a->events() == std::vector<std::string>{"closed:FAILED:NORMAL"});
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
TEST_CASE(
    "Peer rules (in-process): rules 2 and 3 -- one executor and no handler inside a call",
    "[courtier][peer]"
) {
    IoThreads io;
    constexpr int ROUNDS = 100;
    for(int round = 0; round < ROUNDS; ++round) {
        auto [a, b] = pairOn(io.executor());
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
TEST_CASE("Peer rules (in-process): rule 5 -- onClosed exactly once", "[courtier][peer]") {
    IoThreads io;
    constexpr int ROUNDS = 100;
    for(int round = 0; round < ROUNDS; ++round) {
        auto [a, b] = pairOn(io.executor());
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

        REQUIRE(eventually([&] { return a->ended() && b->ended(); }));
        std::this_thread::sleep_for(1ms);
        CHECK(role_a->nClosed() == 1);
        CHECK(role_b->nClosed() == 1);
    }
}

/******************************************************************************/
/**
 * Rule 6: a close sends what was handed to send() before it, and what the close says reaches the
 * other end; a send() after close() is dropped; what arrives after close() is delivered until this
 * end's close has gone out, and not after; only an acknowledgement confirms a close, the other
 * end's own close crossing it does not.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- linear; Catch2 macros count
TEST_CASE(
    "Peer rules (in-process): rule 6 -- what a close sends and what it stops",
    "[courtier][peer]"
) {
    IoThreads io;

    SECTION("everything sent before the close arrives, in order, before the close") {
        constexpr int N_MESSAGES = 50;
        auto [a, b] = pairOn(io.executor());
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
        // The other end acknowledged the close: confirmed (rule 6)
        CHECK(role_a->events().back() == "closed:CLOSED_HERE:RUN_ENDED");
    }

    SECTION("what arrives after close() and before this end's close has gone out is delivered") {
        constexpr std::size_t N_MESSAGES = 100;
        constexpr std::size_t CLOSE_AT = 10;
        auto [a, b] = pairOn(io.executor());
        auto role_a = makeRole();
        auto role_b = makeRole();
        std::atomic<bool> all_sent{false};
        role_a->whenOpen = [&all_sent](Role &role) {
            for(std::size_t i = 0; i < N_MESSAGES; ++i) {
                role.send(std::to_string(i));
            }
            all_sent.store(true);
        };
        role_b->whenMessage = [](Role &role, const std::string &) {
            if(role.nMessages() == CLOSE_AT) {
                role.close(GPeerClose::NORMAL);
            }
        };
        role_b->start(b);
        // The strand of b is held until a has sent everything: all of it arrives at b before b's
        // close goes out, which b's role asks for at the tenth message.
        std::promise<void> release;
        boost::asio::post(b->executor(), [held = release.get_future()]() mutable { held.wait(); });
        role_a->start(a);
        REQUIRE(eventually([&] { return all_sent.load(); }));
        release.set_value();
        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
        CHECK(role_b->nMessages() == N_MESSAGES);
        CHECK(role_b->events().back() == "closed:CLOSED_HERE:NORMAL");
        CHECK(role_a->events().back() == "closed:CLOSED_THERE:NORMAL");
    }

    SECTION("what arrives after this end's close has gone out is not delivered") {
        auto [a, b] = pairOn(io.executor());
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] {
            return role_a->events().size() == 1 && role_b->events().size() == 1;
        }));
        // The strand of a is held; a sends one more message before it hears of b's close, which
        // has gone out by then.
        std::promise<void> release;
        boost::asio::post(a->executor(), [held = release.get_future()]() mutable { held.wait(); });
        boost::asio::post(a->executor(), [&role_a] { role_a->send("late"); });
        role_b->close(GPeerClose::NORMAL);
        release.set_value();
        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
        CHECK(role_b->events() ==
              std::vector<std::string>{"open", "closed:CLOSED_HERE:NORMAL"});
    }

    SECTION("two closes that cross confirm neither") {
        auto [a, b] = pairOn(io.executor());
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] {
            return role_a->events().size() == 1 && role_b->events().size() == 1;
        }));
        // Both strands are held, so that each end's close has gone out before the other's
        // arrives.
        std::promise<void> release_a;
        std::promise<void> release_b;
        boost::asio::post(a->executor(), [held = release_a.get_future()]() mutable { held.wait(); });
        boost::asio::post(b->executor(), [held = release_b.get_future()]() mutable { held.wait(); });
        role_a->close(GPeerClose::NORMAL);
        role_b->close(GPeerClose::ERROR);
        release_a.set_value();
        release_b.set_value();
        REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
        CHECK(role_a->events().back() == "closed:CLOSED_HERE_UNCONFIRMED:ERROR");
        CHECK(role_b->events().back() == "closed:CLOSED_HERE_UNCONFIRMED:NORMAL");
    }
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
        std::this_thread::sleep_for(50ms);
        CHECK_FALSE(returned.load());

        role_a->close(GPeerClose::NORMAL);
        CHECK(eventually([&] { return returned.load(); }));
    }
}

/******************************************************************************/
/**
 * The close protocol of the pair: a close that is never acknowledged ends at the grace, and is
 * not confirmed (rule 6); abort() ends at once and the other end hears a close -- CLOSED_HERE for
 * an end that had not been closed, CLOSED_HERE_UNCONFIRMED for one whose close was under way; an
 * end that loses messages after a number of them still hears the close.
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

    SECTION("abort() reports at once; the other end hears a close") {
        auto [a, b] = pairOn(io.executor());
        auto role_a = makeRole();
        auto role_b = makeRole();
        role_a->start(a);
        role_b->start(b);
        REQUIRE(eventually([&] { return role_a->events().size() == 1; }));

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

    SECTION("the first close decides what the other end is told") {
        auto [a, b] = pairOn(io.executor());
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

    SECTION("setOwnerBusy() reaches the end; GPeerI's default ignores it") {
        auto [a, b] = pairOn(io.executor());
        std::promise<bool> busy;
        boost::asio::post(a->executor(), [&, peer = a] {
            peer->setOwnerBusy(true);
            busy.set_value(peer->ownerBusy());
        });
        CHECK(busy.get_future().get());

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
    std::promise<void> issued;
    boost::asio::post(strand, [&] {
        connector.connect(strand, [&called](const std::shared_ptr<GPeerI> &, std::string_view) {
            called.store(true);
        });
        connector.cancel();
        issued.set_value();
    });
    issued.get_future().get();
    std::this_thread::sleep_for(50ms);
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

    std::promise<void> in_hook;
    std::promise<void> release;
    auto released = release.get_future().share();
    bool hooked = false; // on the server end's strand only
    server_end->setTransmitHook([&in_hook, released, &hooked](std::string_view) {
        if(hooked) {
            return;
        }
        hooked = true;
        in_hook.set_value();
        released.wait_for(PATIENCE);
    });
    server_end->start(GPeerHandlers{});
    client_end->start(GPeerHandlers{});
    client_ctx->poll(); // the client's end opens: it is owned by the wait pending on its strand
    const std::weak_ptr<GInProcessPeer> client_weak = client_end;
    client_end.reset();
    REQUIRE_FALSE(client_weak.expired());

    // The server's end hands its CLOSE over, and is held there.
    server_end->close(GPeerClose::NORMAL);
    REQUIRE(in_hook.get_future().wait_for(PATIENCE) == std::future_status::ready);

    // The client's context goes, and with it everything that owns the client's end.
    client_ctx.reset();
    CHECK(client_weak.expired());

    release.set_value();
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
 * that runs it, and the call is counted out, so that stop() does not wait for it until its bound.
 */
TEST_CASE("Peer rules (in-process): an on_peer that throws is counted out", "[courtier][peer]") {
    const bool standard = GENERATE(true, false);
    INFO((standard ? "on_peer throws a std::exception" : "on_peer throws an int"));
    auto listener = std::make_shared<GInProcessListener>(GInProcessListener::Options{
        .stopBound = 5s,
    });
    boost::asio::io_context ctx;
    auto guard = boost::asio::make_work_guard(ctx);
    std::atomic<int> escaped{0};
    std::thread runner([&ctx, &escaped] {
        while(true) {
            try {
                ctx.run();
                return;
            }
            catch(...) {
                escaped.fetch_add(1);
            }
        }
    });

    std::atomic<int> calls{0};
    listener->start(
        ctx.get_executor(),
        [&calls, standard](const std::shared_ptr<GPeerI> &, GWirePeerId) {
            calls.fetch_add(1);
            if(standard) {
                throw std::runtime_error("on_peer refused by the test");
            }
            throw 42; // NOLINT(hicpp-exception-baseclass,cert-err60-cpp) -- not a std::exception, on purpose
        }
    );
    auto client_end = listener->connect_(ctx.get_executor());
    REQUIRE(client_end != nullptr);
    REQUIRE(eventually([&] { return calls.load() == 1; }));

    const auto stop_began = std::chrono::steady_clock::now();
    listener->stop();
    const auto took = std::chrono::steady_clock::now() - stop_began;
    CHECK(listener->nStopBoundsHit() == 0);
    CHECK(took < 2s);
    CHECK(escaped.load() == 0);

    client_end.reset();
    guard.reset();
    ctx.stop();
    runner.join();
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
    std::promise<void> cut;
    auto cut_made = cut.get_future().share();
    boost::asio::post(b->executor(), [cut_made] { cut_made.wait(); });
    boost::asio::post(a->executor(), [role_a, a, &cut] {
        role_a->send("lost");
        a->sever();
        cut.set_value();
    });
    REQUIRE(eventually([&] { return role_a->nClosed() == 1 && role_b->nClosed() == 1; }));
    std::this_thread::sleep_for(50ms);
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
