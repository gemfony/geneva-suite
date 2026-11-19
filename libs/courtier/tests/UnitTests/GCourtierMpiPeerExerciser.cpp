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
 * The MPI transport's ends over real MPI (GMPICalls), under mpirun: what a client rank is told when
 * the submitter's end for it ends without a close of the role. Rank 0 runs a GMPIListener with a
 * role for every rank, the other ranks a GMPIConnector with a role each. Every rank prints OK / FAIL
 * and returns 0 only when its check holds; a client that is not told waits for ever, and ctest's
 * time limit ends the entry.
 *
 *   GCourtierMpiPeerExerciser --scenario=<scenario>
 *
 * Scenarios:
 *   end-of-work    -- every client has opened; rank 0 joins its session threads and ends the
 *                     listener's work without closing an end: every client is told the end of
 *                     the run.
 *   session-fails  -- the session of rank 1 cannot be made: rank 1 is told at once (ERROR); the
 *                     other clients are told the end of the run when the work ends.
 */

#include "common/GGlobalDefines.hpp"

#ifdef GEM_BUILD_WITH_MPI_CONSUMER

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>

#include <mpi.h>

#include "GInProcessTestSupport.hpp"
#include "GPeerTestRole.hpp"
#include "common/GParserBuilder.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/mpi/GMPICalls.hpp"
#include "courtier/mpi/GMPIEnvironment.hpp"
#include "courtier/transport/GIoContext.hpp"
#include "courtier/transport/GMPIPeerTransport.hpp"

using Gem::Courtier::GWirePeerId;
using Gem::Courtier::Consumers::GIoContext;
using Gem::Courtier::Consumers::GMPICalls;
using Gem::Courtier::Consumers::GMPIConnector;
using Gem::Courtier::Consumers::GMPIEnvironment;
using Gem::Courtier::Consumers::GMPIListener;
using Gem::Courtier::Consumers::GMPIPeerOptions;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Tests::makeRole;
using Gem::Courtier::Tests::Role;
using namespace std::chrono_literals;

namespace {

constexpr auto PATIENCE = 20s; // how long a rank waits for what is due

/** @brief An io context with threads of its own, which can be stopped and joined */
class Pool {
public:
    explicit Pool(std::size_t n_threads) {
        for(std::size_t i = 0; i < n_threads; ++i) {
            threads_.create_thread([this] { ctx_.run(); });
        }
    }
    ~Pool() {
        this->join();
    }
    Pool(const Pool &) = delete;
    Pool(Pool &&) = delete;
    Pool &operator=(const Pool &) = delete;
    Pool &operator=(Pool &&) = delete;
    boost::asio::any_io_executor executor() {
        return ctx_.get_executor();
    }
    void join() {
        guard_.reset();
        ctx_.stop();
        threads_.join_all();
    }

private:
    GIoContext ctx_;
    boost::asio::executor_work_guard<GIoContext::executor_type> guard_ =
        boost::asio::make_work_guard(ctx_);
    Gem::Common::Concurrency::GThreadGroup threads_;
};

/** @brief Prints the outcome of a check and turns it into the exit code */
int verdict(bool ok, const std::string &what) {
    std::println("{}: {}", ok ? "OK" : "FAIL", what);
    return ok ? 0 : 1;
}

/** @brief Rank 0: a listener with a role for every rank; rank 1's session fails if asked to */
int submitter(const std::shared_ptr<GMPIEnvironment> &environment, bool session_fails) {
    const auto n_clients = static_cast<std::size_t>(environment->size() - 1);
    auto listener = std::make_unique<GMPIListener>(
        std::make_shared<GMPICalls>(environment),
        GMPIPeerOptions{.name = "exerciser"}
    );
    std::mutex mutex;
    std::vector<std::shared_ptr<Role>> roles;
    bool ok = true;
    {
        Pool pool(2);
        listener->start(pool.executor(), [&](const std::shared_ptr<GPeerI> &peer, GWirePeerId id) {
            if(session_fails && id == 1) {
                throw std::runtime_error("the exerciser cannot make this session");
            }
            auto role = makeRole();
            {
                const std::scoped_lock lock(mutex);
                roles.push_back(role);
            }
            role->start(peer);
        });
        // Every client that is served says something: here, it opens and sends "hello"
        const std::size_t expected = session_fails ? n_clients - 1 : n_clients;
        ok = Gem::Courtier::Tests::eventually(
            [&] {
                const std::scoped_lock lock(mutex);
                std::size_t n = 0;
                for(const auto &role : roles) {
                    n += role->nMessages();
                }
                return n == expected;
            },
            PATIENCE
        );
        pool.join(); // no session runs any more: nobody closes an end
        listener->endWork();
    }
    listener.reset();
    return verdict(ok, "every served client said hello before the work ended");
}

/** @brief A client rank: its end opens on the greeting, says hello, and waits to be told */
int client(const std::shared_ptr<GMPIEnvironment> &environment, bool session_fails) {
    const int rank = environment->rank();
    auto role = makeRole();
    role->whenOpen = [](Role &self) { self.send("hello"); };
    {
        Pool pool(1);
        auto connector = std::make_unique<GMPIConnector>(
            std::make_shared<GMPICalls>(environment),
            GMPIPeerOptions{.name = "exerciser client"}
        );
        connector->connect(
            pool.executor(),
            [role](const std::shared_ptr<GPeerI> &peer, std::string_view) {
                if(peer) {
                    role->start(peer);
                }
            }
        );
        // No limit here: a client that is not told waits until ctest ends the entry
        while(role->nClosed() == 0) {
            std::this_thread::sleep_for(10ms);
        }
        pool.join();
        connector.reset();
    }
    const std::string expected =
        session_fails && rank == 1 ? "closed:CLOSED_THERE:ERROR" : "closed:CLOSED_THERE:RUN_ENDED";
    const auto events = role->events();
    return verdict(
        events.back() == expected,
        "rank " + std::to_string(rank) + " was told: " + events.back()
    );
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) -- only the printing in the handlers could throw
int main(int argc, char **argv) {
    try {
        std::string scenario;
        Gem::Common::GParserBuilder gpb;
        gpb.registerCLParameter(
            "scenario",
            scenario,
            std::string("end-of-work"),
            "what to exercise: end-of-work | session-fails"
        );
        if(gpb.parseCommandLine(argc, argv) == Gem::Common::GCL_HELP_REQUESTED) {
            return 0;
        }
        if(scenario != "end-of-work" && scenario != "session-fails") {
            std::println(stderr, "Unknown scenario: {}", scenario);
            return 2;
        }
        const auto environment = std::make_shared<GMPIEnvironment>(&argc, &argv);
        const bool session_fails = scenario == "session-fails";
        if(environment->role() == Gem::Courtier::processRole::SUBMITTER) {
            return submitter(environment, session_fails);
        }
        return client(environment, session_fails);
    }
    catch(const std::exception &e) {
        std::println(stderr, "FAIL: {}", e.what());
        return 1;
    }
    catch(...) {
        std::println(stderr, "FAIL: an exception that is not a std::exception");
        return 1;
    }
}

#else

int main() {
    // MPI consumer not built -- nothing to exercise.
    return 0;
}

#endif /* GEM_BUILD_WITH_MPI_CONSUMER */
