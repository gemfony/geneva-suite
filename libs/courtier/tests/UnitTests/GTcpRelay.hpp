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

#pragma once

// Standard headers go here
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// Boost headers go here
#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

// Geneva headers go here
#include "common/concurrency/GCompletionLatchT.hpp"
#include "common/concurrency/GThreadGroup.hpp"

/******************************************************************************/
/**
 * @file
 * A TCP relay for the tests of a client that loses its connection: it stands between a client and
 * its server, copies the bytes both ways, and can cut, freeze or refuse connections when a test
 * says so. It knows nothing of websocket frames: it moves and cuts bytes.
 */

namespace Gem::Courtier::Tests {

/******************************************************************************/
/** @brief The two directions in which a relay copies bytes */
enum class relayDirection : std::uint8_t {
    TO_SERVER = 0, ///< From the client to the server
    TO_CLIENT = 1  ///< From the server to the client
};

/******************************************************************************/
/**
 * Listens on 127.0.0.1 (a port of its own, chosen by the system) and, for every connection it
 * accepts, opens one to the upstream port and copies bytes both ways. One io context, run by one
 * thread of a GThreadGroup: every operation runs on that thread. Every public call posts its work
 * there and waits for it, so a call has taken effect when it returns. Not to be called from the
 * relay's own thread.
 */
class GTcpRelay {
    using tcp = boost::asio::ip::tcp;
    using error_code = boost::system::error_code;

public:
    /** @brief Starts the relay @param upstream The port of the server (0: see setUpstream()) */
    explicit GTcpRelay(unsigned short upstream)
      : upstream_(upstream) {
        this->open_(0);
        port_ = acceptor_.local_endpoint().port();
        this->accept_();
        threads_.create_thread([this] { ctx_.run(); });
    }

    /** @brief Closes every connection and the acceptor, then joins its thread */
    ~GTcpRelay() {
        this->call_([this] {
            error_code ignored;
            std::ignore = acceptor_.close(ignored);
            for(const auto &pair : pairs_) {
                pair->close();
            }
            pairs_.clear();
        });
        guard_.reset();
        threads_.join_all();
    }

    GTcpRelay(const GTcpRelay &) = delete;
    GTcpRelay(GTcpRelay &&) = delete;
    GTcpRelay &operator=(const GTcpRelay &) = delete;
    GTcpRelay &operator=(GTcpRelay &&) = delete;

    /** @brief @return The port clients connect to */
    [[nodiscard]] unsigned short port() const {
        return port_;
    }

    /** @brief Where connections made from now on are relayed to; 0: they are accepted and closed
     *  at once (a websocket handshake then fails) @param upstream The server's port */
    void setUpstream(unsigned short upstream) {
        this->call_([this, upstream] { upstream_ = upstream; });
    }

    /** @brief false: the acceptor is closed, a connect is refused; true: it is opened again on the
     *  same port @param accepting Whether connections are accepted */
    void setAccepting(bool accepting) {
        this->call_([this, accepting] {
            if(accepting == acceptor_.is_open()) {
                return;
            }
            if(accepting) {
                this->open_(port_);
                this->accept_();
            }
            else {
                error_code ignored;
                std::ignore = acceptor_.close(ignored);
            }
        });
    }

    /** @brief Every open connection is cut: both sockets are closed with a linger of 0 (a reset at
     *  both ends) */
    void cutAll() {
        this->call_([this] {
            for(const auto &pair : pairs_) {
                pair->cut();
            }
        });
    }

    /** @brief The open connections are cut (as by cutAll()) after @p n more bytes in @p direction:
     *  exactly n of them are forwarded @param direction Which direction counts @param n How many */
    void cutAfter(relayDirection direction, std::size_t n) {
        this->call_([this, direction, n] {
            for(const auto &pair : pairs_) {
                pair->cut_after.at(index_(direction)) = n;
            }
        });
    }

    /** @brief The open connections stop forwarding both ways and stay open (half-open at both
     *  ends); connections made later forward as usual */
    void freeze() {
        this->call_([this] {
            for(const auto &pair : pairs_) {
                pair->frozen = true;
            }
        });
    }

    /** @brief Frozen connections forward again */
    void thaw() {
        this->call_([this] {
            for(const auto &pair : pairs_) {
                pair->thaw();
            }
        });
    }

    /** @brief @return How many connections were made since the relay was started */
    [[nodiscard]] std::size_t nConnections() const {
        return n_connections_.load();
    }
    /** @brief @return How many connections are open now */
    [[nodiscard]] std::size_t nOpen() const {
        return n_open_->load();
    }
    /** @brief @param direction Which direction @return Bytes forwarded in it since the start */
    [[nodiscard]] std::uint64_t bytes(relayDirection direction) const {
        return bytes_->at(index_(direction)).load();
    }

private:
    /** @brief @param direction A direction @return Its index */
    static std::size_t index_(relayDirection direction) {
        return static_cast<std::size_t>(direction);
    }

    /// Per direction: bytes forwarded (shared with the connections, read from any thread)
    using byte_counts = std::array<std::atomic<std::uint64_t>, 2>;

    /**
     * One relayed connection: the client's socket ("down") and the server's ("up"). Everything
     * happens on the relay's thread; pending operations own it.
     */
    struct Pair : std::enable_shared_from_this<Pair> {
        /** @brief @param ctx The relay's context @param down The accepted socket
         *  @param counts Where forwarded bytes are counted @param n_open Open connections */
        Pair(
            boost::asio::io_context &ctx,
            tcp::socket down_socket,
            std::shared_ptr<byte_counts> counts,
            std::shared_ptr<std::atomic<std::size_t>> n_open
        )
          : down(std::move(down_socket))
          , up(ctx)
          , counts_(std::move(counts))
          , n_open_(std::move(n_open)) {
            n_open_->fetch_add(1);
        }

        /** @brief Starts copying both ways */
        void begin() {
            this->read(0);
            this->read(1);
        }

        /** @brief Reads in one direction (0: down to up, 1: up to down) @param dir The direction */
        void read(std::size_t dir) {
            if(not open) {
                return;
            }
            if(frozen) {
                suspended.at(dir) = true;
                return;
            }
            tcp::socket &from = dir == 0 ? down : up;
            from.async_read_some(
                boost::asio::buffer(buffers.at(dir)),
                [self = this->shared_from_this(), dir](error_code ec, std::size_t n) {
                    if(not self->open) {
                        return;
                    }
                    if(ec) {
                        self->close();
                        return;
                    }
                    self->forward(dir, n);
                }
            );
        }

        /** @brief Forwards @p n bytes read in @p dir, unless frozen; cuts where it was asked to
         *  @param dir The direction @param n How many bytes were read */
        void forward(std::size_t dir, std::size_t n) {
            if(frozen) {
                held.at(dir) = n;
                suspended.at(dir) = true;
                return;
            }
            std::size_t k = n;
            bool cut_now = false;
            if(auto &limit = cut_after.at(dir)) {
                if(n >= *limit) {
                    k = *limit;
                    cut_now = true;
                }
                else {
                    *limit -= n;
                }
            }
            if(k == 0) {
                if(cut_now) {
                    this->cut();
                }
                return;
            }
            tcp::socket &to = dir == 0 ? up : down;
            boost::asio::async_write(
                to,
                boost::asio::buffer(buffers.at(dir).data(), k),
                [self = this->shared_from_this(),
                 dir,
                 cut_now](error_code ec, std::size_t written) {
                    if(not self->open) {
                        return;
                    }
                    if(ec) {
                        self->close();
                        return;
                    }
                    self->counts_->at(dir).fetch_add(written);
                    if(cut_now) {
                        self->cut();
                        return;
                    }
                    self->read(dir);
                }
            );
        }

        /** @brief Forwards again what a freeze held back, and reads on */
        void thaw() {
            frozen = false;
            for(std::size_t dir = 0; dir < 2; ++dir) {
                if(not suspended.at(dir)) {
                    continue;
                }
                suspended.at(dir) = false;
                const std::size_t n = std::exchange(held.at(dir), 0);
                if(n > 0) {
                    this->forward(dir, n);
                }
                else {
                    this->read(dir);
                }
            }
        }

        /** @brief Closes both sockets with a linger of 0: both ends see a reset */
        void cut() {
            if(not open) {
                return;
            }
            error_code ignored;
            std::ignore = down.set_option(tcp::socket::linger(true, 0), ignored);
            std::ignore = up.set_option(tcp::socket::linger(true, 0), ignored);
            this->close();
        }

        /** @brief Closes both sockets */
        void close() {
            if(not open) {
                return;
            }
            open = false;
            error_code ignored;
            std::ignore = down.close(ignored);
            std::ignore = up.close(ignored);
            n_open_->fetch_sub(1);
        }

        tcp::socket down;                                 ///< The client's connection
        tcp::socket up;                                   ///< The server's connection
        std::array<std::array<char, 65536>, 2> buffers{}; ///< Per direction: what was read
        std::array<std::size_t, 2> held{}; ///< Per direction: read while frozen, not sent
        std::array<bool, 2> suspended{};   ///< Per direction: reading waits for a thaw
        std::array<std::optional<std::size_t>, 2> cut_after; ///< Per direction: bytes before a cut
        bool frozen = false;                                 ///< Forwards nothing while set
        bool open = true;                                    ///< Not closed yet

    private:
        std::shared_ptr<byte_counts> counts_;              ///< Bytes forwarded, per direction
        std::shared_ptr<std::atomic<std::size_t>> n_open_; ///< Open connections
    };

    /** @brief Runs @p work on the relay's thread and waits for it @param work What runs */
    template <typename work_type>
    void call_(work_type work) {
        auto latch = std::make_shared<Gem::Common::Concurrency::GCompletionLatchT>(1);
        boost::asio::post(ctx_, [latch, work = std::move(work)]() mutable {
            work();
            latch->count_down();
        });
        latch->wait();
    }

    /** @brief Opens, binds and listens on 127.0.0.1 @param port The port (0: any) */
    void open_(unsigned short port) {
        const tcp::endpoint endpoint(boost::asio::ip::address_v4::loopback(), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
    }

    /** @brief Accepts the next connection and relays it */
    void accept_() {
        acceptor_.async_accept([this](error_code ec, tcp::socket socket) {
            if(ec) {
                return; // the acceptor was closed
            }
            n_connections_.fetch_add(1);
            if(upstream_ == 0) {
                error_code ignored;
                std::ignore = socket.close(ignored);
            }
            else {
                auto pair = std::make_shared<Pair>(ctx_, std::move(socket), bytes_, n_open_);
                pairs_.push_back(pair);
                pair->up.async_connect(
                    tcp::endpoint(boost::asio::ip::address_v4::loopback(), upstream_),
                    [pair](error_code cec) {
                        if(cec) {
                            pair->close();
                            return;
                        }
                        pair->begin();
                    }
                );
            }
            this->accept_();
        });
    }

    boost::asio::io_context ctx_; ///< Where everything runs
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_ =
        boost::asio::make_work_guard(ctx_);     ///< Keeps the thread in run() until the destructor
    tcp::acceptor acceptor_{ctx_};              ///< Where clients connect
    unsigned short port_ = 0;                   ///< The acceptor's port
    unsigned short upstream_ = 0;               ///< The server's port (relay thread)
    std::vector<std::shared_ptr<Pair>> pairs_;  ///< Every connection made (relay thread)
    std::atomic<std::size_t> n_connections_{0}; ///< Connections accepted
    std::shared_ptr<std::atomic<std::size_t>> n_open_ =
        std::make_shared<std::atomic<std::size_t>>(0);                     ///< Connections open
    std::shared_ptr<byte_counts> bytes_ = std::make_shared<byte_counts>(); ///< Bytes forwarded
    Gem::Common::Concurrency::GThreadGroup threads_; ///< The relay's thread (declared last: joined
                                                     ///< before the rest goes)
};

/******************************************************************************/

} /* namespace Gem::Courtier::Tests */
