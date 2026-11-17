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

// The MPI transport's ends, listener and connector (GMPIPeerTransport.hpp) on the in-memory
// stand-in for MPI of GMPIFakeCalls.hpp: what the MPI transport does beyond the rules every
// transport keeps (those are run on it by GCourtierPeerRulesTests): the greeting, a close before
// it, the limits of a message and of a close, a failed poll, the listener's stop and end, the
// connector. In one process, under ThreadSanitizer, without mpirun.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
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

#include "GInProcessTestSupport.hpp"
#include "GMPIFakeCalls.hpp"
#include "GPeerTestRole.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GNetworkedTimeoutConfig.hpp"
#include "courtier/consumers/GPeerConsumerT.hpp"
#include "courtier/transport/GIoContext.hpp"
#include "courtier/transport/GMPIPeerTransport.hpp"
#include "courtier/transport/GPeerClientT.hpp"
#include "courtier/transport/GPeerI.hpp"

using Gem::Common::Concurrency::GThreadGroup;
using Gem::Courtier::GWirePeerId;
using Gem::Courtier::Consumers::GIoContext;
using Gem::Courtier::Consumers::GMPIConnector;
using Gem::Courtier::Consumers::GMPIListener;
using Gem::Courtier::Consumers::GMPIPeerOptions;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Tests::GMPIFakeWorld;
using Gem::Courtier::Tests::makeRole;
using Gem::Courtier::Tests::Role;

using namespace std::chrono_literals;

namespace {

constexpr auto PATIENCE = 5s;       // how long a test waits for something that is due at once
constexpr auto CLOSE_GRACE = 300ms; // the ends' wait for an answer, where it matters

/** @brief Waits, at most PATIENCE, for @p condition @return Whether it held in time */
template <typename condition_type>
bool eventually(condition_type condition) {
    return Gem::Courtier::Tests::eventually(condition, PATIENCE);
}

/** @brief @param grace The close grace @return The options of the tests' ends: the drain at the
 *  end of the work is short too (some tests have ranks that never receive) */
GMPIPeerOptions optionsWith(std::chrono::milliseconds grace = CLOSE_GRACE) {
    GMPIPeerOptions options{.name = "test", .closeGrace = grace};
    options.poll.drainGrace = 300ms;
    return options;
}

/** @brief Waits, at most PATIENCE, until every handler posted to @p executor before this call has
 *  run (an executor with one thread, or a strand, runs them in the order they were posted)
 *  @param executor The executor @return Whether they ran in time */
bool flushed(const boost::asio::any_io_executor &executor) {
    const auto done = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    boost::asio::post(executor, [done] { done->count_down(); });
    return done->wait_for(PATIENCE);
}

/******************************************************************************/
/** @brief An io context with threads of its own, which can be stopped, joined and shut down */
class Pool {
public:
    /** @brief @param n_threads How many threads run the context */
    explicit Pool(std::size_t n_threads) {
        for(std::size_t i = 0; i < n_threads; ++i) {
            threads_.create_thread([this] { ctx_.run(); });
        }
    }
    /** @brief Stops and joins, if that has not been done */
    ~Pool() {
        this->join();
    }
    Pool(const Pool &) = delete;
    Pool(Pool &&) = delete;
    Pool &operator=(const Pool &) = delete;
    Pool &operator=(Pool &&) = delete;

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

private:
    GIoContext ctx_; ///< The context
    boost::asio::executor_work_guard<GIoContext::executor_type> guard_ =
        boost::asio::make_work_guard(ctx_);
    GThreadGroup threads_; ///< Its threads
};

/******************************************************************************/
/**
 * The submitter's side as a shell has it: a listener on rank 0 and a pool its peers' handlers run
 * on; every peer is given a Role. At the end the pool is joined, then the listener ends its work,
 * then the pool's context goes, then the listener (GPeerI.hpp, rule 7).
 */
class Master {
public:
    /** @brief @param world The communicator @param options How the ends are set up */
    explicit Master(
        const std::shared_ptr<GMPIFakeWorld> &world,
        GMPIPeerOptions options = optionsWith(),
        std::size_t n_threads = 4
    )
      : listener_(std::make_unique<GMPIListener>(world->calls(0), std::move(options)))
      , pool_(n_threads) { /* nothing */
    }
    ~Master() {
        pool_.join();
        if(started_) {
            listener_->endWork();
        }
    }
    Master(const Master &) = delete;
    Master(Master &&) = delete;
    Master &operator=(const Master &) = delete;
    Master &operator=(Master &&) = delete;

    /** @brief Starts the listener; every peer gets a role that @p prepare may set up
     *  @param prepare Called for every new role, with the rank, before its peer is started */
    void start(std::function<void(Role &, GWirePeerId)> prepare = {}) {
        listener_->start(
            pool_.executor(),
            [this,
             prepare = std::move(prepare)](const std::shared_ptr<GPeerI> &peer, GWirePeerId id) {
                auto role = makeRole();
                if(prepare) {
                    prepare(*role, id);
                }
                {
                    const std::scoped_lock lock(mutex_);
                    roles_.emplace_back(id, role);
                }
                role->start(peer);
            }
        );
        started_ = true;
    }

    /** @brief @param id A rank @return Its role, once it has been made */
    std::shared_ptr<Role> role(GWirePeerId id) const {
        const std::scoped_lock lock(mutex_);
        for(const auto &[rank, role] : roles_) {
            if(rank == id) {
                return role;
            }
        }
        return nullptr;
    }
    /** @brief @return The ranks whose roles have been made */
    std::set<GWirePeerId> ranks() const {
        const std::scoped_lock lock(mutex_);
        std::set<GWirePeerId> ranks;
        for(const auto &[rank, role] : roles_) {
            ranks.insert(rank);
        }
        return ranks;
    }
    /** @brief @return Whether no peer holds a role any more (only this object does). When no
     *  handler runs. */
    bool rolesLetGo() const {
        const std::scoped_lock lock(mutex_);
        return std::ranges::all_of(roles_, [](const auto &entry) {
            return entry.second.use_count() == 1;
        });
    }

    GMPIListener &listener() {
        return *listener_;
    }
    Pool &pool() {
        return pool_;
    }

private:
    std::unique_ptr<GMPIListener> listener_; ///< Destroyed after the pool's context
    Pool pool_;                              ///< Where the peers' handlers run
    bool started_ = false;                   ///< start() was called
    mutable std::mutex mutex_;               ///< Guards roles_
    /// The roles made (their peers hold them too, until they let go)
    std::vector<std::pair<GWirePeerId, std::shared_ptr<Role>>> roles_;
};

/******************************************************************************/
/**
 * A client's side: a connector on rank @p rank and one thread for its poll and its end; the end the
 * connector makes is started for a Role. At the end the thread is joined, then the connector goes
 * (it ends the poll), then the context (GPeerI.hpp, rule 7).
 */
class Client {
public:
    /** @brief @param world The communicator @param rank This client's rank
     *  @param options How the end is set up */
    Client(
        const std::shared_ptr<GMPIFakeWorld> &world,
        int rank,
        GMPIPeerOptions options = optionsWith()
    )
      : connector_(std::make_unique<GMPIConnector>(world->calls(rank), std::move(options))) {
        connector_->connect(
            pool_.executor(),
            [role = role_](const std::shared_ptr<GPeerI> &peer, std::string_view) {
                if(peer) {
                    role->start(peer);
                }
            }
        );
    }
    ~Client() {
        pool_.join();
        connector_.reset();
    }
    Client(const Client &) = delete;
    Client(Client &&) = delete;
    Client &operator=(const Client &) = delete;
    Client &operator=(Client &&) = delete;

    /** @brief @return The role of this client's end */
    Role &role() {
        return *role_;
    }
    /** @brief @return The connector */
    GMPIConnector &connector() {
        return *connector_;
    }
    /** @brief @return The client's io thread (its end and its poll run there) */
    boost::asio::any_io_executor executor() {
        return pool_.executor();
    }

private:
    std::shared_ptr<Role> role_ = makeRole();  ///< The role of the end
    std::unique_ptr<GMPIConnector> connector_; ///< Destroyed after the thread is joined
    Pool pool_{1};                             ///< The client's io thread
};

} /* anonymous namespace */

/******************************************************************************/
/**
 * The greeting: a client's end opens when the listener of the submitter has started, not before;
 * the listener makes a peer for every other rank, with the rank as its id.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE(
    "MPI transport: a client's end opens on the greeting of the listener",
    "[courtier][mpi]"
) {
    auto world = std::make_shared<GMPIFakeWorld>(4);
    Master master(world);
    Client one(world, 1);
    Client two(world, 2);
    Client three(world, 3);

    // The clients' polls have looked a number of times and found no greeting: nothing is opened
    REQUIRE(eventually([&] {
        return world->counts(1).probes > 20 && world->counts(3).probes > 20;
    }));
    REQUIRE(flushed(one.executor()));
    CHECK(one.role().events().empty());

    master.start();
    CHECK(master.listener().nPeers() == 3);
    REQUIRE(eventually([&] {
        return one.role().events().size() == 1 && two.role().events().size() == 1 &&
               three.role().events().size() == 1;
    }));
    CHECK(one.role().events().front() == "open");
    REQUIRE(eventually([&] { return master.ranks().size() == 3; }));
    CHECK(master.ranks() == std::set<GWirePeerId>{1, 2, 3});
    const auto master_one = master.role(1);
    REQUIRE(master_one != nullptr);
    CHECK(master_one->events() == std::vector<std::string>{"open"});

    one.role().close(GPeerClose::NORMAL);
    REQUIRE(eventually([&] { return one.role().nClosed() == 1; }));
    CHECK(one.role().events().back() == "closed:CLOSED_HERE:NORMAL");
    REQUIRE(eventually([&] { return master_one->nClosed() == 1; }));
    CHECK(master_one->events().back() == "closed:CLOSED_THERE:NORMAL");
}

/******************************************************************************/
/**
 * A client whose role closes before the greeting has arrived: the close is answered by a master
 * that starts within the grace (CLOSED_HERE, without onOpen); without one it is unconfirmed at the
 * grace.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a close before the greeting", "[courtier][mpi]") {
    auto world = std::make_shared<GMPIFakeWorld>(2);

    SECTION("a master that starts answers it") {
        Master master(world, optionsWith(5s));
        Client client(world, 1, optionsWith(5s));
        REQUIRE(eventually([&] { return world->counts(1).probes > 0; })); // the poll runs
        client.role().close(GPeerClose::NORMAL);
        REQUIRE(eventually([&] { return world->counts(1).sends == 1; })); // the close is out
        master.start();
        REQUIRE(eventually([&] { return client.role().nClosed() == 1; }));
        CHECK(client.role().events() == std::vector<std::string>{"closed:CLOSED_HERE:NORMAL"});
        REQUIRE(eventually([&] {
            const auto role = master.role(1);
            REQUIRE(role != nullptr);
            return role != nullptr && role->nClosed() == 1;
        }));
    }

    SECTION("without a master it is not confirmed") {
        Client client(world, 1);
        REQUIRE(eventually([&] { return world->counts(1).probes > 0; }));
        const auto begin = std::chrono::steady_clock::now();
        client.role().close(GPeerClose::NORMAL);
        REQUIRE(eventually([&] { return client.role().nClosed() == 1; }));
        const auto took = std::chrono::steady_clock::now() - begin;
        CHECK(took >= CLOSE_GRACE);
        CHECK(
            client.role().events() ==
            std::vector<std::string>{"closed:CLOSED_HERE_UNCONFIRMED:NORMAL"}
        );
    }
}

/******************************************************************************/
/** A close that the other end never answers ends unconfirmed at the grace (rule 6). */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a close that is never answered", "[courtier][mpi]") {
    auto world = std::make_shared<GMPIFakeWorld>(2); // rank 1 never receives: nobody polls there
    Master master(world);
    master.start();
    REQUIRE(eventually([&] { return master.role(1) != nullptr; }));
    const auto role = master.role(1);
    REQUIRE(role != nullptr);
    REQUIRE(eventually([&] { return role->events().size() == 1; }));

    const auto begin = std::chrono::steady_clock::now();
    role->close(GPeerClose::RUN_ENDED);
    REQUIRE(eventually([&] { return role->nClosed() == 1; }));
    const auto took = std::chrono::steady_clock::now() - begin;
    CHECK(took >= CLOSE_GRACE);
    CHECK(took < CLOSE_GRACE + 2s);
    CHECK(role->events().back() == "closed:CLOSED_HERE_UNCONFIRMED:NORMAL");
    CHECK(master.listener().stopGrace() == CLOSE_GRACE + 1s);
    CHECK_FALSE(master.listener().detectsLoss());
}

/******************************************************************************/
/**
 * A message longer than the limit is not sent: the end fails, reported on its executor and not
 * inside send() (rule 3), and the other end hears a close that says ERROR.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a message longer than the limit", "[courtier][mpi]") {
    auto world = std::make_shared<GMPIFakeWorld>(2);
    GMPIPeerOptions options = optionsWith();
    options.maxMessageBytes = 8;
    Master master(world, options);
    Client client(world, 1, options);
    master.start([](Role &role, GWirePeerId) {
        role.whenOpen = [](Role &self) { self.send("123456789"); }; // nine bytes
    });
    REQUIRE(eventually([&] { return master.role(1) != nullptr; }));
    const auto role = master.role(1);
    REQUIRE(role != nullptr);
    REQUIRE(eventually([&] { return role->nClosed() == 1 && client.role().nClosed() == 1; }));
    CHECK(role->events() == std::vector<std::string>{"open", "closed:FAILED:NORMAL"});
    CHECK(role->nNested() == 0);
    CHECK(client.role().events() == std::vector<std::string>{"open", "closed:CLOSED_THERE:ERROR"});
}

/******************************************************************************/
/**
 * A poll that fails ends every end of the process as FAILED, also one not started yet -- and every
 * client rank is told (a close that says ERROR): MPI has no keep-alive, and a client that is not
 * told waits for ever.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a failed poll ends every end", "[courtier][mpi]") {
    const bool before_start = GENERATE(true, false);
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Master master(world);
    Client client_one(world, 1);
    Client client_two(world, 2);
    if(before_start) {
        world->throwOnce(0, GMPIFakeWorld::GFault::PROBE);
    }
    master.start();
    REQUIRE(eventually([&] { return master.ranks().size() == 2; }));
    const auto one = master.role(1);
    const auto two = master.role(2);
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    if(not before_start) {
        REQUIRE(eventually([&] {
            return one->events().size() == 1 && two->events().size() == 1 &&
                   client_one.role().events().size() == 1 && client_two.role().events().size() == 1;
        }));
        world->throwOnce(0, GMPIFakeWorld::GFault::PROBE);
    }
    REQUIRE(eventually([&] { return one->nClosed() == 1 && two->nClosed() == 1; }));
    CHECK(one->events().back() == "closed:FAILED:NORMAL");
    CHECK(two->events().back() == "closed:FAILED:NORMAL");
    REQUIRE(eventually([&] {
        return client_one.role().nClosed() == 1 && client_two.role().nClosed() == 1;
    }));
    CHECK(client_one.role().events().back() == "closed:CLOSED_THERE:ERROR");
    CHECK(client_two.role().events().back() == "closed:CLOSED_THERE:ERROR");
}

/******************************************************************************/
/**
 * The listener's endWork() lets go of the ends that have not ended (the shell's threads were stuck
 * past the grace, say): each tells its client rank that the run has ended, before the poll ends.
 * The server's end of rank 1 is started late on purpose, so that a test that joined the pool once
 * the clients are open, without waiting for the server's ends, fails every time.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: the end of the listener's work tells the clients", "[courtier][mpi]") {
    constexpr auto SERVER_START_HOLD = 500ms;
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Master master(world);
    Client client_one(world, 1);
    Client client_two(world, 2);
    // The clients' ends open on the greeting, which goes out before the listener hands the peers
    // over; the server's end of rank 1 is started only after the hold (its open handler is then
    // posted to the pool), long after the clients are open.
    master.start([&](Role &, GWirePeerId rank) {
        if(rank == 1) {
            std::this_thread::sleep_for(SERVER_START_HOLD);
        }
    });
    // The server's ends are open too: a handler still queued on the pool when it is joined never
    // runs, and the peer it holds would keep its role (rolesLetGo() below). The roles are looked
    // up inside the wait, so that the test holds none of them afterwards.
    const auto open_here = [&](GWirePeerId rank) {
        const auto role = master.role(rank);
        return role != nullptr && role->events().size() == 1;
    };
    REQUIRE(eventually([&] {
        return open_here(1) && open_here(2) && client_one.role().events().size() == 1 &&
               client_two.role().events().size() == 1;
    }));
    master.pool().join(); // no session runs: nobody closes the ends
    master.listener().endWork();
    REQUIRE(eventually([&] {
        return client_one.role().nClosed() == 1 && client_two.role().nClosed() == 1;
    }));
    CHECK(client_one.role().events().back() == "closed:CLOSED_THERE:RUN_ENDED");
    CHECK(client_two.role().events().back() == "closed:CLOSED_THERE:RUN_ENDED");
    CHECK(master.rolesLetGo());
}

/******************************************************************************/
/**
 * A stop whose bound is hit, then the end of the listener's work: the calls of on_peer not yet
 * made are dropped (the shell's threads have gone), and every client rank is still told that the
 * run has ended. Rank 1's on_peer is held past the stop's bound and past the start of endWork(),
 * so rank 2's call is the one that must not be made.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE(
    "MPI transport: the end of the listener's work after a stop whose bound was hit",
    "[courtier][mpi]"
) {
    // Longer than the stop's bound of one second, with a second's margin for endWork() to stop
    // the poll's context while rank 1's call still runs
    constexpr auto ON_PEER_HOLD = 2s;
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Master master(world);
    Client client_one(world, 1);
    Client client_two(world, 2);
    master.start([&](Role &, GWirePeerId rank) {
        if(rank == 1) {
            std::this_thread::sleep_for(ON_PEER_HOLD);
        }
    });
    master.listener().stop(); // the bound is hit: rank 1's call has not returned
    master.pool().join();
    master.listener().endWork(); // joins the poll's thread once rank 1's call has returned
    CHECK(master.ranks() == std::set<GWirePeerId>{1});
    REQUIRE(eventually([&] {
        return client_one.role().nClosed() == 1 && client_two.role().nClosed() == 1;
    }));
    CHECK(client_one.role().events().back() == "closed:CLOSED_THERE:RUN_ENDED");
    CHECK(client_two.role().events().back() == "closed:CLOSED_THERE:RUN_ENDED");
}

/******************************************************************************/
/**
 * A rank whose session cannot be made (an exception in on_peer) is closed with an error at once,
 * also before its end was started; the other ranks are served.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a rank whose session cannot be made is told", "[courtier][mpi]") {
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Master master(world);
    Client client_one(world, 1);
    Client client_two(world, 2);
    master.start([](Role &, GWirePeerId rank) {
        if(rank == 1) {
            throw std::runtime_error("the test cannot make this session");
        }
    });
    REQUIRE(eventually([&] { return client_one.role().nClosed() == 1; }));
    CHECK(client_one.role().events().back() == "closed:CLOSED_THERE:ERROR");
    REQUIRE(eventually([&] { return client_two.role().events().size() == 1; }));
    CHECK(client_two.role().events().front() == "open");
    CHECK(client_two.role().nClosed() == 0);
}

/******************************************************************************/
/**
 * The listener's stop() returns when every rank's peer has reached the shell (on_peer has
 * returned for each), so that a stop right after the start tells every rank; endWork() lets the
 * peers that have not ended go without onClosed.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: the listener's stop and end", "[courtier][mpi]") {
    constexpr int N_RANKS = 9;
    auto world = std::make_shared<GMPIFakeWorld>(N_RANKS);
    Master master(world);
    master.start([](Role &, GWirePeerId) { std::this_thread::sleep_for(20ms); });
    master.listener().stop();
    CHECK(master.ranks().size() == static_cast<std::size_t>(N_RANKS - 1));

    const auto all_open = [&] {
        for(GWirePeerId rank = 1; rank < static_cast<GWirePeerId>(N_RANKS); ++rank) {
            const auto role = master.role(rank);
            if(role == nullptr || role->events().size() != 1) {
                return false;
            }
        }
        return true;
    };
    REQUIRE(eventually(all_open));
    master.pool().join();
    master.listener().endWork();
    CHECK(master.rolesLetGo());
}

/******************************************************************************/
/**
 * The listener hands its peers over (on_peer) on the poll's thread, not on the shell's executor
 * (GListenerI::start()): while the one session thread is busy, every rank's session is made all
 * the same. The roles are told of the open on the session thread, once it is free again.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE(
    "MPI transport: the peers reach the shell while every session thread is busy",
    "[courtier][mpi]"
) {
    auto world = std::make_shared<GMPIFakeWorld>(4);
    Master master(world, optionsWith(), 1);
    Client one(world, 1);
    Client two(world, 2);
    Client three(world, 3);
    std::atomic<bool> holding{false};
    const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    boost::asio::post(master.pool().executor(), [&holding, release] {
        holding.store(true);
        std::ignore = release->wait_for(3 * PATIENCE); // longer than the check waits
    });
    REQUIRE(eventually([&] { return holding.load(); }));

    master.start();
    CHECK(eventually([&] { return master.ranks().size() == 3; }));
    const auto role = master.role(1);
    REQUIRE(role != nullptr);
    CHECK(role->events().empty()); // its handlers wait for the session thread
    release->count_down();

    REQUIRE(eventually([&] { return role->events().size() == 1; }));
    CHECK(role->events().front() == "open");
    REQUIRE(eventually([&] { return one.role().events().size() == 1; }));
    CHECK(one.role().events().front() == "open");
}

/******************************************************************************/
/** An MPI client has one connection for the whole run; cancel() keeps done from being called. */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: the connector", "[courtier][mpi]") {
    auto world = std::make_shared<GMPIFakeWorld>(2);
    Pool pool(1);
    auto connector = std::make_unique<GMPIConnector>(world->calls(1), optionsWith());
    CHECK_FALSE(connector->reconnectable());

    SECTION("a second connect() reports no connection") {
        std::atomic<int> peers{0};
        std::atomic<int> refusals{0};
        const auto done = [&](const std::shared_ptr<GPeerI> &peer, std::string_view why) {
            if(peer) {
                peers.fetch_add(1);
                peer->start({});
            }
            else if(not why.empty()) {
                refusals.fetch_add(1);
            }
        };
        connector->connect(pool.executor(), done);
        connector->connect(pool.executor(), done);
        REQUIRE(eventually([&] { return peers.load() + refusals.load() == 2; }));
        CHECK(peers.load() == 1);
        CHECK(refusals.load() == 1);
    }

    SECTION("cancel() before the attempt is reported") {
        std::atomic<bool> called{false};
        const auto held = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
        boost::asio::post(pool.executor(), [held] { held->wait(); });
        connector->connect(pool.executor(), [&](const std::shared_ptr<GPeerI> &, std::string_view) {
            called.store(true);
        });
        connector->cancel();
        held->count_down();
        REQUIRE(flushed(pool.executor())); // the report would have been posted before this
        CHECK_FALSE(called.load());
    }

    SECTION("the submitter's rank has no connector") {
        CHECK_THROWS(GMPIConnector(world->calls(0), optionsWith()));
    }

    pool.join();
    connector.reset();
}

/******************************************************************************/
/**
 * The poll of the submitter runs on a thread of its own: a session that takes longer over a message
 * than the poll's drain grace changes nothing for its client, whose close is answered (not ended by
 * its timer).
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a slow session does not hold up the poll", "[courtier][mpi]") {
    // One session thread, held by rank 1's session while it handles a message: the poll has a
    // thread of its own, so rank 2's message is received meanwhile, and rank 1's close is answered.
    auto world = std::make_shared<GMPIFakeWorld>(3);
    GMPIPeerOptions options = optionsWith(5s);
    Master master(world, options, 1);
    Client one(world, 1, options);
    Client two(world, 2, options);
    one.role().whenOpen = [](Role &role) {
        role.send("M");
        role.close(GPeerClose::NORMAL);
    };
    std::atomic<bool> holding{false};
    const auto release = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
    master.start([&holding, release](Role &role, GWirePeerId rank) {
        if(rank == 1) {
            role.whenMessage = [&holding, release](Role &, const std::string &) {
                holding.store(true);
                std::ignore = release->wait_for(3 * PATIENCE); // longer than the check waits
            };
        }
    });
    REQUIRE(eventually([&] { return holding.load(); }));
    REQUIRE(eventually([&] { return two.role().events().size() == 1; })); // open
    const auto received = world->counts(0).receives;
    boost::asio::post(two.executor(), [&two] { two.role().send("N"); });
    CHECK(eventually([&] { return world->counts(0).receives > received; }));
    release->count_down();

    REQUIRE(eventually([&] { return one.role().nClosed() == 1; }));
    CHECK(one.role().events().back() == "closed:CLOSED_HERE:NORMAL");
    const auto role = master.role(1);
    REQUIRE(role != nullptr);
    REQUIRE(eventually([&] { return role->nClosed() == 1; }));
    CHECK(
        role->events() == std::vector<std::string>{"open", "msg:M", "closed:CLOSED_THERE:NORMAL"}
    );
}

/******************************************************************************/
/**
 * A client's io context runs out of work once its end has ended and its sends have completed: the
 * poll's chain ends by itself (GPeerI.hpp, rule 7), and the role goes.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE(
    "MPI transport: a client's context runs out of work when its end has ended",
    "[courtier][mpi]"
) {
    auto world = std::make_shared<GMPIFakeWorld>(2);
    Master master(world);
    master.start();

    GIoContext ctx; // no work guard
    auto connector = std::make_unique<GMPIConnector>(world->calls(1), optionsWith());
    auto role = makeRole();
    const std::weak_ptr<Role> weak_role = role;
    connector->connect(
        ctx.get_executor(),
        [role](const std::shared_ptr<GPeerI> &peer, std::string_view) {
            role->whenOpen = [](Role &self) { self.close(GPeerClose::NORMAL); };
            role->start(peer);
        }
    );
    role.reset();
    std::atomic<bool> returned{false};
    std::thread runner([&] {
        ctx.run();
        returned.store(true);
    });
    const bool ran_out = eventually([&] { return returned.load(); });
    if(not ran_out) {
        ctx.stop();
    }
    runner.join();
    CHECK(ran_out);
    CHECK(weak_role.expired());
    connector.reset();
}

/******************************************************************************/
/**
 * A client's end whose owner is busy makes its poll look far less often (it has no use for an
 * answer while it evaluates); a send of the end still goes out and is looked after at once.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a busy owner makes its poll look less often", "[courtier][mpi]") {
    auto world = std::make_shared<GMPIFakeWorld>(2);
    Master master(world);
    Client client(world, 1);
    master.start();
    REQUIRE(eventually([&] { return client.role().events().size() == 1; }));

    const auto looks = [&] {
        std::this_thread::sleep_for(300ms);
        const auto before = world->counts(1).probes;
        std::this_thread::sleep_for(500ms);
        return world->counts(1).probes - before;
    };
    const auto idle = looks();
    boost::asio::post(client.executor(), [&client] { client.role().setOwnerBusy(true); });
    const auto busy = looks();
    INFO("looks in 500 ms: idle " << idle << ", busy " << busy);
    CHECK(busy * 10 <= idle);

    // The end of the run reaches a busy client all the same
    client.role().close(GPeerClose::NORMAL);
    REQUIRE(eventually([&] { return client.role().nClosed() == 1; }));
    CHECK(client.role().events().back() == "closed:CLOSED_HERE:NORMAL");
}

/******************************************************************************/
/**
 * A submitter that never starts tells the clients that wait for its greeting that the run has ended;
 * they end without having been opened. A rank that does not answer is waited for at most the stop
 * grace.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a listener that never starts announces the end", "[courtier][mpi]") {
    const bool all_there = GENERATE(true, false);
    auto world = std::make_shared<GMPIFakeWorld>(3);
    auto listener = std::make_unique<GMPIListener>(world->calls(0), optionsWith());
    Client one(world, 1);
    std::unique_ptr<Client> two;
    if(all_there) {
        two = std::make_unique<Client>(world, 2);
    }
    REQUIRE(eventually([&] { return world->counts(1).probes > 0; })); // waits for the greeting

    const auto begin = std::chrono::steady_clock::now();
    listener->announceEndWithoutStart();
    const auto took = std::chrono::steady_clock::now() - begin;
    REQUIRE(eventually([&] { return one.role().nClosed() == 1; }));
    CHECK(one.role().events() == std::vector<std::string>{"closed:CLOSED_THERE:RUN_ENDED"});
    if(all_there) {
        REQUIRE(eventually([&] { return two->role().nClosed() == 1; }));
        CHECK(two->role().events() == std::vector<std::string>{"closed:CLOSED_THERE:RUN_ENDED"});
        CHECK(took < listener->stopGrace());
    }
    else {
        // rank 2 never receives: its answer does not come; its end gives up at the close grace
        CHECK(took >= CLOSE_GRACE);
        CHECK(took < listener->stopGrace() + 2s);
    }
}

/******************************************************************************/
/**
 * A result longer than the client's MPI end can carry is returned as failed (the item comes back
 * flagged and the policy refills its slot), and the client keeps its connection: handed to the end,
 * it would end the connection. Rank 1's end carries at most 2000 bytes, an item of 1000 numbers
 * about 8000; rank 2 serves normally.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE(
    "MPI transport: a result longer than the end can carry is returned as failed",
    "[courtier][mpi]"
) {
    using item_type = Gem::Courtier::GRandomNumberContainer;
    using client_type = Gem::Courtier::Consumers::GPeerClientT<item_type>;
    using Gem::Courtier::Consumers::GPeerClientOptions;
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Gem::Courtier::GPeerConsumerT<item_type> shell(
        "limit shell",
        std::make_unique<GMPIListener>(world->calls(0), optionsWith()),
        2,
        Gem::Common::serializationMode::GEM_BINARY
    );
    Gem::Courtier::GNetworkedTimeoutConfig timeouts;
    timeouts.treatment = "wait_indefinitely";
    shell.applyTimeoutConfig(timeouts);
    shell.startServer();

    GMPIPeerOptions limited_options = optionsWith();
    limited_options.maxMessageBytes = 2000;
    auto limited = std::make_shared<client_type>(
        std::make_unique<GMPIConnector>(world->calls(1), limited_options),
        GPeerClientOptions{.name = "limited"}
    );
    auto unlimited = std::make_shared<client_type>(
        std::make_unique<GMPIConnector>(world->calls(2), optionsWith()),
        GPeerClientOptions{.name = "unlimited"}
    );
    std::jthread run_limited([&limited] { limited->run(); });
    std::jthread run_unlimited([&unlimited] { unlimited->run(); });

    std::vector<std::unique_ptr<item_type>> items;
    items.reserve(20);
    for(int i = 0; i < 20; ++i) {
        items.push_back(std::make_unique<item_type>(1000));
    }
    shell.processBatch(
        std::span(items.data(), items.size()),
        Gem::Courtier::GSubmissionPolicy::clone_on_partial_return()
    );
    shell.stopServer();
    run_limited.join();
    run_unlimited.join();
    CHECK(limited->getNProcessed() > 0); // it evaluated items and returned them as failed
    CHECK(
        limited->connectionEnd() == Gem::Courtier::Consumers::GPeerEnd::CLOSED_THERE
    ); // told the end
    CHECK(limited->resultsSaved() == 0);
    CHECK_FALSE(limited->terminalErrorFlagged());
    CHECK(unlimited->getNProcessed() > 0);
}

/******************************************************************************/
/**
 * A client's end waits for the greeting: without a submitter it never opens, and the client gives
 * up at its first-contact limit (--mpi_firstContactLimit); once greeted it is open, and the limit
 * no longer applies, however long the submitter has no work for it.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE(
    "MPI transport: a client waits for the greeting up to its first-contact limit",
    "[courtier][mpi]"
) {
    using client_type = Gem::Courtier::Consumers::GPeerClientT<Gem::Courtier::GFaultyContainer>;
    using Gem::Courtier::Consumers::GPeerClientOptions;
    auto world = std::make_shared<GMPIFakeWorld>(2);
    const auto make_client = [&world] {
        return std::make_shared<client_type>(
            std::make_unique<GMPIConnector>(world->calls(1), optionsWith()),
            GPeerClientOptions{.name = "greeting test", .firstContactLimit = 1s}
        );
    };

    SECTION("no submitter: the client gives up at the limit") {
        auto client = make_client();
        std::atomic<bool> returned{false};
        std::jthread runner([&] {
            client->run();
            returned.store(true);
        });
        CHECK(eventually([&] { return returned.load(); }));
        runner.join();
        CHECK(client->connections() == 0);
        CHECK(client->terminalErrorFlagged());
    }

    SECTION("greeted: the limit no longer applies") {
        Gem::Courtier::GPeerConsumerT<Gem::Courtier::GFaultyContainer> shell(
            "greeting shell",
            std::make_unique<GMPIListener>(world->calls(0), optionsWith()),
            1,
            Gem::Common::serializationMode::GEM_BINARY
        );
        shell.startServer();
        auto client = make_client();
        std::atomic<bool> returned{false};
        std::jthread runner([&] {
            client->run();
            returned.store(true);
        });
        REQUIRE(eventually([&] { return client->connections() == 1; }));
        // Past the limit, and past the halt poll that looks at it, the client still waits
        const auto polls = client->haltPolls();
        REQUIRE(eventually([&] { return client->haltPolls() >= polls + 2; }));
        CHECK_FALSE(returned.load());
        shell.stopServer();
        CHECK(eventually([&] { return returned.load(); }));
        runner.join();
        CHECK(client->connectionEnd() == Gem::Courtier::Consumers::GPeerEnd::CLOSED_THERE);
        CHECK_FALSE(client->terminalErrorFlagged());
    }
}

/******************************************************************************/
/**
 * A communicator of the submitter's rank alone has no client rank, and an MPI job keeps its ranks:
 * the listener refuses to start, with a text that says what is missing and how the program is
 * started, instead of waiting for ever; the shell's startServer() passes the refusal on, and the
 * shell stops without anything to tell.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a communicator of one rank is refused at the start", "[courtier][mpi]") {
    auto world = std::make_shared<GMPIFakeWorld>(1);
    const auto refusal = [](const std::function<void()> &start) {
        try {
            start();
        }
        catch(const std::exception &e) {
            return std::string{e.what()};
        }
        return std::string{};
    };

    SECTION("the listener") {
        GMPIListener listener(world->calls(0), optionsWith());
        Pool pool(1);
        const std::string what = refusal([&] {
            listener.start(pool.executor(), [](const std::shared_ptr<GPeerI> &, GWirePeerId) {});
        });
        CHECK(what.find("has no client rank") != std::string::npos);
        CHECK(what.find("at least two processes") != std::string::npos);
        CHECK(listener.nPeers() == 0);
        CHECK_FALSE(listener.pollCounts().has_value()); // no poll was made
        pool.join();
    }

    SECTION("the shell") {
        Gem::Courtier::GPeerConsumerT<Gem::Courtier::GFaultyContainer> shell(
            "one-rank shell",
            std::make_unique<GMPIListener>(world->calls(0), optionsWith()),
            1,
            Gem::Common::serializationMode::GEM_BINARY
        );
        const auto begin = std::chrono::steady_clock::now();
        const std::string what = refusal([&] { shell.startServer(); });
        CHECK(what.find("has no client rank") != std::string::npos);
        shell.stopServer();
        CHECK(std::chrono::steady_clock::now() - begin < PATIENCE);
    }
}

/******************************************************************************/
/**
 * A start() that throws after the poll was set up (here: the first greeting cannot be sent) ends
 * the work as endWork() does: the clients that wait are told, and the closes are drained (every
 * receive takes a while here, so a close given up at once would be gone before its client read it).
 * The shell counts such a listener as not started and destroys its context before the listener,
 * as the pool here is: the poll must have let go of that context by then.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size) -- linear; Catch2 macros count
TEST_CASE("MPI transport: a start that throws tells the clients", "[courtier][mpi]") {
    auto world = std::make_shared<GMPIFakeWorld>(3);
    Client one(world, 1);
    Client two(world, 2);
    REQUIRE(eventually([&] {
        return world->counts(1).probes > 0 && world->counts(2).probes > 0;
    })); // they wait for the greeting
    world->delayReceives(50ms);
    {
        GMPIListener listener(world->calls(0), optionsWith());
        Pool pool(1);
        world->throwOnce(0, GMPIFakeWorld::GFault::SEND);
        CHECK_THROWS(
            listener.start(pool.executor(), [](const std::shared_ptr<GPeerI> &, GWirePeerId) {})
        );
        CHECK(listener.pollCounts().has_value()); // the poll had been set up
        pool.join();
    }
    REQUIRE(eventually([&] { return one.role().nClosed() == 1 && two.role().nClosed() == 1; }));
    CHECK(one.role().events() == std::vector<std::string>{"closed:CLOSED_THERE:RUN_ENDED"});
    CHECK(two.role().events() == std::vector<std::string>{"closed:CLOSED_THERE:RUN_ENDED"});
    world->delayReceives(0ms);
}
