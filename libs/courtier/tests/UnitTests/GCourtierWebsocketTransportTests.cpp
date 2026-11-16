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

// The two halves of the websocket transport that make connections: the listener (the server's
// side, GListenerI) and the connector (the client's side, GConnectorI).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/asio.hpp>

#include "common/GLogger.hpp"
#include "courtier/transport/GPeerI.hpp"
#include "courtier/transport/GWebsocketPeerTransport.hpp"

using boost::asio::ip::tcp;
using Gem::Courtier::GWirePeerId;
using Gem::Courtier::Consumers::GPeerClose;
using Gem::Courtier::Consumers::GPeerEnd;
using Gem::Courtier::Consumers::GPeerHandlers;
using Gem::Courtier::Consumers::GPeerI;
using Gem::Courtier::Consumers::GWebsocketConnector;
using Gem::Courtier::Consumers::GWebsocketListener;
using Gem::Courtier::Consumers::GWebsocketPeer;
using namespace std::chrono_literals;

namespace {

/** @brief An io context with one thread; stops and joins it when it goes */
class OneThread {
public:
    OneThread()
      : thread_([this] { ctx_.run(); }) { /* nothing */
    }
    ~OneThread() {
        guard_.reset();
        ctx_.stop();
        thread_.join();
    }
    OneThread(const OneThread &) = delete;
    OneThread(OneThread &&) = delete;
    OneThread &operator=(const OneThread &) = delete;
    OneThread &operator=(OneThread &&) = delete;

    /** @brief @return The executor of the context */
    boost::asio::any_io_executor executor() {
        return ctx_.get_executor();
    }

private:
    boost::asio::io_context ctx_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_ =
        boost::asio::make_work_guard(ctx_);
    std::thread thread_;
};

/** @brief Polls @p condition for at most five seconds @return Whether it held */
template <typename condition_type>
bool eventually(condition_type condition) {
    const auto give_up_at = std::chrono::steady_clock::now() + 5s;
    while(not condition() && std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

/** @brief A port that refuses connections for as long as this object lives: it is bound and does
 *  not listen, so no other process can take it meanwhile */
class ClosedPort {
public:
    ClosedPort()
      : socket_(ctx_) {
        socket_.open(tcp::v4());
        socket_.bind(tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    }
    /** @brief @return The port */
    [[nodiscard]] unsigned short port() const {
        return socket_.local_endpoint().port();
    }

private:
    boost::asio::io_context ctx_;
    tcp::socket socket_;
};

/** @brief @param ping The ping interval @return The options of a peer with it */
GWebsocketPeer::GOptions withPing(std::chrono::milliseconds ping) {
    return GWebsocketPeer::GOptions{.name = "transport test", .pingInterval = ping};
}

} /* anonymous namespace */

/******************************************************************************/
/** A port that cannot be bound makes start() throw, with the port in the message; the listener
 *  is then as if it had not been started. */
TEST_CASE(
    "GWebsocketListener: a port that is taken makes start throw and names the port",
    "[courtier][websocket][transport]"
) {
    boost::asio::io_context ctx;
    tcp::acceptor taken(ctx, tcp::endpoint(tcp::v4(), 0));
    taken.listen();
    const unsigned short port = taken.local_endpoint().port();

    OneThread sessions;
    GWebsocketListener listener("listener test", port, withPing(1s), 1);
    std::string message;
    try {
        listener.start(sessions.executor(), [](const std::shared_ptr<GPeerI> &, GWirePeerId) {});
    }
    catch(const std::exception &e) {
        message = e.what();
    }
    CHECK(message.contains(std::to_string(port)));
}

/******************************************************************************/
/** Port 0 lets the system choose; the listener tells which port it got, and a connector reaches it:
 *  the listener hands the server's end to on_peer, on the shell's executor, and both ends open. */
TEST_CASE(
    "GWebsocketListener: port 0 yields a port that a connector reaches",
    "[courtier][websocket][transport]"
) {
    OneThread sessions;
    OneThread client;
    GWebsocketListener listener("listener test", 0, withPing(1s), 1);
    std::atomic<int> n_open{0};
    std::atomic<int> n_closed{0};
    std::atomic<GWirePeerId> seen_id{0};
    const auto counting = [&] {
        return GPeerHandlers{
            .onOpen = [&] { n_open.fetch_add(1); },
            .onMessage = {},
            .onClosed = [&](GPeerEnd, GPeerClose, std::string_view) { n_closed.fetch_add(1); }
        };
    };
    std::shared_ptr<GPeerI> server_end;
    std::mutex mutex;
    listener.start(sessions.executor(), [&](std::shared_ptr<GPeerI> peer, GWirePeerId id) {
        seen_id.store(id);
        peer->start(counting());
        const std::scoped_lock lock(mutex);
        server_end = std::move(peer);
    });
    REQUIRE(listener.getPort() != 0);

    GWebsocketConnector connector("localhost", listener.getPort(), withPing(1s));
    boost::asio::post(client.executor(), [&] {
        connector.connect(
            client.executor(),
            [&](const std::shared_ptr<GPeerI> &peer, std::string_view) {
                if(peer) {
                    peer->start(counting()); // from here on its pending work owns it
                }
            }
        );
    });
    REQUIRE(eventually([&] { return n_open.load() == 2; }));
    CHECK(seen_id.load() == 1);
    {
        const std::scoped_lock lock(mutex);
        server_end->close(GPeerClose::NORMAL);
    }
    REQUIRE(eventually([&] { return n_closed.load() == 2; }));
    listener.stop();
    listener.endWork();
}

/******************************************************************************/
/** A connect to a port nothing listens on reports an error and no peer. */
TEST_CASE(
    "GWebsocketConnector: a closed port reports an error and no peer",
    "[courtier][websocket][transport]"
) {
    const ClosedPort closed;
    OneThread client;
    GWebsocketConnector connector("localhost", closed.port(), withPing(1s));
    std::atomic<bool> done{false};
    std::atomic<bool> got_peer{true};
    std::string error;
    boost::asio::post(client.executor(), [&] {
        connector.connect(
            client.executor(),
            [&](const std::shared_ptr<GPeerI> &peer, std::string_view why) {
                got_peer.store(peer != nullptr);
                error = std::string(why);
                done.store(true);
            }
        );
    });
    REQUIRE(eventually([&] { return done.load(); }));
    CHECK_FALSE(got_peer.load());
    CHECK_FALSE(error.empty());
}

/******************************************************************************/
/** An attempt that is given up does not report. */
TEST_CASE(
    "GWebsocketConnector: an attempt given up with cancel does not report",
    "[courtier][websocket][transport]"
) {
    const ClosedPort closed;
    OneThread client;
    GWebsocketConnector connector("localhost", closed.port(), withPing(1s));
    std::atomic<bool> called{false};
    std::atomic<bool> posted{false};
    boost::asio::post(client.executor(), [&] {
        connector.connect(
            client.executor(),
            [&](const std::shared_ptr<GPeerI> &, std::string_view) { called.store(true); }
        );
        connector.cancel();
        posted.store(true);
    });
    REQUIRE(eventually([&] { return posted.load(); }));
    std::this_thread::sleep_for(300ms);
    CHECK_FALSE(called.load());
}

/******************************************************************************/
/** Without the keep-alive a lost client is not noticed by the transport: the consumer then needs
 *  the time lease. */
TEST_CASE(
    "GWebsocketListener: detectsLoss follows the keep-alive",
    "[courtier][websocket][transport]"
) {
    const GWebsocketListener with_ping("listener test", 0, withPing(1s), 1);
    const GWebsocketListener without_ping("listener test", 0, withPing(0ms), 1);
    CHECK(with_ping.detectsLoss());
    CHECK_FALSE(without_ping.detectsLoss());
}

/******************************************************************************/
/** After stop() the listener makes no peer: a client that connects is refused, and on_peer is not
 *  called. Three connections before it get the ids 1, 2 and 3. */
TEST_CASE(
    "GWebsocketListener: the ids count from 1 and no peer comes after stop",
    "[courtier][websocket][transport]"
) {
    OneThread sessions;
    GWebsocketListener listener("listener test", 0, withPing(1s), 1);
    std::mutex mutex;
    std::vector<GWirePeerId> ids;
    std::vector<std::shared_ptr<GPeerI>> ends; // never started: the client sockets just connect
    listener.start(sessions.executor(), [&](std::shared_ptr<GPeerI> peer, GWirePeerId id) {
        const std::scoped_lock lock(mutex);
        ids.push_back(id);
        ends.push_back(std::move(peer));
    });

    boost::asio::io_context ctx;
    const tcp::endpoint server{boost::asio::ip::address_v4::loopback(), listener.getPort()};
    std::vector<tcp::socket> sockets;
    for(int c = 0; c < 3; ++c) {
        sockets.emplace_back(ctx).connect(server);
    }
    REQUIRE(eventually([&] {
        const std::scoped_lock lock(mutex);
        return ids.size() == 3;
    }));
    {
        const std::scoped_lock lock(mutex);
        CHECK(ids == std::vector<GWirePeerId>{1, 2, 3});
    }

    listener.stop();
    tcp::socket late(ctx);
    boost::system::error_code ec;
    std::ignore = late.connect(server, ec);
    CHECK(ec); // refused: the acceptor is closed
    std::this_thread::sleep_for(200ms);
    {
        const std::scoped_lock lock(mutex);
        CHECK(ids.size() == 3);
        ends.clear(); // before the I/O context goes (endWork())
    }
    listener.endWork();
}

/******************************************************************************/
/** endWork() lets the open connections go without onClosed (GListenerI::endWork(), rule 5): the
 *  handlers are dropped, and with them what they hold. */
TEST_CASE(
    "GWebsocketListener: endWork lets open connections go without onClosed",
    "[courtier][websocket][transport]"
) {
    // The shell's context, as GPeerConsumerT keeps it: its thread is joined before endWork(), the
    // context itself goes after it (the peers' handler strands are on it)
    boost::asio::io_context sessions_ctx;
    auto sessions_work = std::make_optional(boost::asio::make_work_guard(sessions_ctx));
    std::thread sessions_thread([&sessions_ctx] { sessions_ctx.run(); });
    OneThread client;
    GWebsocketListener listener("listener test", 0, withPing(1s), 1);
    std::atomic<int> n_open{0};
    std::atomic<int> n_closed_server{0};
    auto token = std::make_shared<int>(0); // held by the server end's handlers
    const std::weak_ptr<int> token_seen = token;
    listener.start(
        sessions_ctx.get_executor(),
        [&n_open, &n_closed_server, token](std::shared_ptr<GPeerI> peer, GWirePeerId) {
            peer->start(GPeerHandlers{
                .onOpen = [&n_open] { n_open.fetch_add(1); },
                .onMessage = {},
                .onClosed = [&n_closed_server, token](GPeerEnd, GPeerClose, std::string_view) {
                    n_closed_server.fetch_add(1);
                }
            });
        }
    );
    token.reset();

    GWebsocketConnector connector("localhost", listener.getPort(), withPing(1s));
    boost::asio::post(client.executor(), [&] {
        connector.connect(
            client.executor(),
            [&n_open](const std::shared_ptr<GPeerI> &peer, std::string_view) {
                if(peer) {
                    peer->start(GPeerHandlers{
                        .onOpen = [&n_open] { n_open.fetch_add(1); },
                        .onMessage = {},
                        .onClosed = {}
                    });
                }
            }
        );
    });
    REQUIRE(eventually([&] { return n_open.load() == 2; }));

    listener.stop();
    sessions_work.reset();
    sessions_ctx.stop();
    sessions_thread.join(); // no handler runs any more
    listener.endWork();
    CHECK(n_closed_server.load() == 0);
    CHECK(token_seen.expired());
}

/******************************************************************************/
/**
 * A start() whose I/O threads cannot all be created leaves the listener as if it had not been
 * started (GListenerI::start()): it throws, no thread is left running, and its destructor returns.
 * The threads are made to fail half way in a child process: every new thread gets a stack of
 * 128 MiB, and the address space is limited to what the child uses and room for two and a half
 * such stacks, so the third thread cannot be made (its stack cannot be mapped). The parent waits
 * for the child, at most ten seconds.
 */
TEST_CASE(
    "GWebsocketListener: a start whose threads cannot all be created leaves nothing running",
    "[courtier][websocket][transport]"
) {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if(child == 0) {
        // The child: large stacks, and room for two and a half of them
        constexpr std::size_t STACK_BYTES = std::size_t{128} << 20U;
        pthread_attr_t attributes;
        if(::pthread_attr_init(&attributes) != 0 ||
           ::pthread_attr_setstacksize(&attributes, STACK_BYTES) != 0 ||
           ::pthread_setattr_default_np(&attributes) != 0) {
            ::_exit(4);
        }
        std::size_t pages = 0;
        if(std::FILE *statm = std::fopen("/proc/self/statm", "r")) {
            if(std::fscanf(statm, "%zu", &pages) != 1) {
                pages = 0;
            }
            std::fclose(statm);
        }
        const auto used = static_cast<rlim_t>(pages) * static_cast<rlim_t>(::sysconf(_SC_PAGESIZE));
        const rlimit limit{
            .rlim_cur = used + static_cast<rlim_t>(STACK_BYTES) * 5U / 2U,
            .rlim_max = RLIM_INFINITY
        };
        if(pages == 0 || ::setrlimit(RLIMIT_AS, &limit) != 0) {
            ::_exit(3);
        }
        int code = 2; // start() did not throw
        {
            boost::asio::io_context sessions; // never runs: no peer is made here
            GWebsocketListener listener("listener test", 0, withPing(1s), 64);
            try {
                listener.start(sessions.get_executor(), [](const std::shared_ptr<GPeerI> &, GWirePeerId) {});
            }
            catch(...) {
                code = 0;
            }
        } // the destructor must return
        ::_exit(code);
    }

    int status = 0;
    const auto give_up_at = std::chrono::steady_clock::now() + 10s;
    pid_t done = 0;
    while((done = ::waitpid(child, &status, WNOHANG)) == 0 &&
          std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(10ms);
    }
    if(done == 0) {
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
    }
    INFO("the child " << (done == 0 ? "hung and was killed" : "ended") << ", status " << status);
    CHECK(done == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

/******************************************************************************/
/**
 * An accept that fails for want of a descriptor is retried after a pause, not at once and not
 * never. In a child process the client's socket is made, and then every descriptor up to a low
 * limit is taken, so that the listener's accept has none for the connection: the listener must
 * retry (a second failure comes), neither spin (it warns once per failed accept; at most 30 in a
 * second, the pause being 100 ms) nor stop accepting (once descriptors are free again, the waiting connection reaches on_peer). The
 * parent waits for the child, at most twenty seconds; the child's exit code says what went wrong.
 */
TEST_CASE(
    "GWebsocketListener pauses after an accept that fails for want of descriptors and goes on",
    "[courtier][websocket][transport]"
) {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if(child == 0) {
        // Counts the warnings of failed accepts
        struct GAcceptFailures : Gem::Common::GBaseLogTarget {
            void log(const std::string &message) const override {
                if(message.find("pausing before the next accept") != std::string::npos) {
                    n.fetch_add(1);
                }
            }
            void logWithSource(const std::string &message, const std::string &) const override {
                log(message);
            }
            mutable std::atomic<int> n{0};
        };
        auto failures = std::make_shared<GAcceptFailures>();
        glogger.addLogTarget(failures);

        OneThread sessions;
        std::atomic<int> n_peers{0};
        GWebsocketListener listener("listener test", 0, withPing(1s), 1);
        listener.start(
            sessions.executor(),
            [&n_peers](const std::shared_ptr<GPeerI> &, GWirePeerId) { n_peers.fetch_add(1); }
        );

        // A low limit, and every descriptor below it taken
        const int probe_fd = ::open("/dev/null", O_RDONLY);
        if(probe_fd < 0) {
            ::_exit(3);
        }
        ::close(probe_fd);
        rlimit limit{};
        if(::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
            ::_exit(3);
        }
        limit.rlim_cur = static_cast<rlim_t>(probe_fd) + 64U;
        if(::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
            ::_exit(3);
        }
        // The client's socket is made before, so that it has its descriptor
        boost::asio::io_context client_ctx;
        tcp::socket client(client_ctx);
        client.open(tcp::v4());
        std::vector<int> fillers;
        for(int fd = ::open("/dev/null", O_RDONLY); fd >= 0; fd = ::open("/dev/null", O_RDONLY)) {
            fillers.push_back(fd);
        }
        if(fillers.empty()) {
            ::_exit(3);
        }

        // The connection waits in the backlog; the listener has no descriptor to accept it with
        boost::system::error_code ec;
        std::ignore =
            client.connect({boost::asio::ip::address_v4::loopback(), listener.getPort()}, ec);
        if(ec) {
            ::_exit(4);
        }

        // The accept is retried: a second failure comes (at most within the five seconds of
        // eventually(), which only keep a failing child from hanging). Then the failures of one
        // second are counted: a pause of 100 ms allows about ten, a retry without a pause
        // thousands. A slow host only lowers the count, so neither check depends on how busy the
        // host is.
        int code = 0;
        if(not eventually([&failures] { return failures->n.load() >= 2; })) {
            code = 5; // the accept was not retried
        }
        else {
            const int n_before = failures->n.load();
            std::this_thread::sleep_for(1s);
            if(failures->n.load() - n_before > 30) {
                code = 6; // the accept was retried without a pause
            }
        }

        for(const int fd : fillers) {
            ::close(fd);
        }
        if(code == 0 && not eventually([&n_peers] { return n_peers.load() == 1; })) {
            code = 7; // the listener did not accept again
        }
        listener.stop();
        listener.endWork();
        glogger.resetLogTargets();
        ::_exit(code);
    }

    int status = 0;
    const auto give_up_at = std::chrono::steady_clock::now() + 20s;
    pid_t done = 0;
    while((done = ::waitpid(child, &status, WNOHANG)) == 0 &&
          std::chrono::steady_clock::now() < give_up_at) {
        std::this_thread::sleep_for(10ms);
    }
    if(done == 0) {
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
    }
    INFO(
        "the child " << (done == 0 ? "hung and was killed" : "ended") << ", status " << status
                     << " (exit code 5: no retry, 6: retried without a pause, 7: did not accept "
                        "again)"
    );
    CHECK(done == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
