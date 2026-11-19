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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard headers go here
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

// Geneva headers go here
#include "common/concurrency/GThreadBudget.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GWireSerializationContext.hpp" // GWirePeerId
#include "courtier/transport/GIoContext.hpp"
#include "courtier/transport/GPeerI.hpp" // the interfaces

/******************************************************************************/
/**
 * @file
 * The MQTT transport of courtier over the transport-neutral interfaces of GPeerI.hpp: the ends of a
 * connection (GMqttPeer) talk through an MQTT broker instead of a socket of their own. Boost.MQTT5
 * is used in GMqttPeerTransport.cpp only; this header includes none of it.
 *
 * **One MQTT session is one courtier connection, and nothing more.** Every end owns its own MQTT
 * client, on both sides: a client process has one per connection to its server, the server one per
 * client and one more for its listener. MQTT's own recovery is left out: an end never connects to
 * the broker again. Boost.MQTT5 reconnects by itself and has no switch for it; the only sign of a
 * lost broker connection is its logger, and every hook that reports one (a transport error, a
 * failed or a repeated TCP connect, a refused CONNACK, the broker's DISCONNECT) ends the end at
 * once, FAILED -- or SILENT when the broker fell silent within its keep-alive -- and stops its
 * client. A new connection is a new end with a new MQTT client and a new id, made by the
 * connector, as the websocket connector makes a new socket. So nothing is sent twice or out of
 * order, no subscription is lost on a reconnect, and an end's will is published only when its
 * connection is really over. Everything travels with QoS 0: on one live path client -> broker ->
 * receiver it arrives in order, and what a disturbance loses ends the connection; courtier's lease
 * and resend cover it as for websocket.
 *
 * **Topics and the handshake.** With the prefix P (GMqttOptions::topic) and a random id I chosen by
 * the connector: the listener's client subscribes to P/join before the listener's start() returns.
 * A connector's end subscribes to P/I/down, waits for the broker's acknowledgement and only then
 * publishes I on P/join. On a join the listener makes the server's end, which subscribes to P/I/up
 * and is handed to the shell only when the broker has acknowledged that; its session greets first.
 * A client's role sends nothing before the server's greeting, so nothing is published to a topic
 * whose reader has not subscribed yet (the broker would drop it). A join for an id already seen is
 * ignored. An end that is not subscribed (and on the client, has not joined) within the handshake
 * limit is given up; a client's end that hears nothing of a server within the limit after its
 * join ends SILENT (nobody took the join), and its client connects again.
 *
 * **What travels.** Every payload on P/I/up and P/I/down begins with one byte: DATA (a message of
 * the role follows), CLOSE (followed by one byte, the GPeerClose), or GONE (the will). A close goes
 * on the same topic as the data, so it arrives after it (GPeerI.hpp, rule 6). After its close has
 * been published an end disconnects in the regular way, which discards its will.
 *
 * **Ends.** Every end leaves a will on the topic its other end reads (payload GONE, no delay): the
 * broker publishes it when it loses that end's connection without a regular DISCONNECT -- at once
 * for a process that dies, after one and a half keep-alive intervals for one that hangs. Reading
 * GONE ends an end SILENT, reading CLOSE ends it CLOSED_THERE with what the close said, its own
 * close CLOSED_HERE. An end that fails for another reason (a message longer than the broker
 * accepts, a broken broker connection) stops its client without a DISCONNECT, so that its will
 * tells the other end. The MQTT keep-alive watches the link to the broker and is the transport's
 * sign of life: the server's ends use the server's interval (GListenerI::keepAlive()), named to the
 * clients in the HELLO; a client's end connects before it has read the HELLO and cannot change its
 * MQTT keep-alive afterwards, so it keeps its own (setKeepAlive() says so in the log when the two
 * differ).
 *
 * **Size.** Every MQTT client accepts packets of up to 2,000,000 bytes (mosquitto 2.1's default
 * limit of a broker; a fixed limit, since Boost.MQTT5 keeps a read buffer of that size for every
 * client) and reads the broker's limit from its CONNACK; maxMessageBytes() is what fits into a
 * packet both accept. A message
 * handed to send() beyond that is not published: the end fails (FAILED) with both sizes in the
 * log. The roles check the length first and fail the item instead (GPeerI::maxMessageBytes()).
 *
 * **Threads.** Every call into an MQTT client runs on its end's I/O executor, which serializes
 * (a strand of the listener's I/O context on the server, the executor given to connect() on a
 * client); send(), close() and setKeepAlive() post there. Messages handed to send() are published
 * one after another. The role's handlers run on the end's handler strand.
 */

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/** @brief Where the ends of the MQTT transport find their broker, and how they name their topics.
 *  The server and its clients must agree on host, port and topic. */
struct GMqttOptions {
    std::string name = "GMqttPeer";         ///< Who uses the transport, for the log
    std::string host = GMQTTDEFAULTBROKER;  ///< The broker's name or address
    unsigned short port = GMQTTDEFAULTPORT; ///< The broker's port
    std::string topic = GMQTTDEFAULTTOPIC;  ///< The prefix of every topic
    /// The MQTT keep-alive of every broker connection of these ends (0: none). The server names
    /// it to its clients; a client's end keeps its own (see the file comment). Twice the interval
    /// (GMQTTHANDSHAKETIMEOUT without one) bounds the handshake and the close.
    std::chrono::seconds keepAlive{GBEASTCONSUMERPINGINTERVAL};
};

/******************************************************************************/
/**
 * One end of a courtier connection over MQTT, as a GPeerI: the same class serves the client and the
 * server's session (see the file comment for what travels and how an end ends). It keeps the rules
 * of GPeerI.hpp.
 *
 * Made by GMqttListener and GMqttConnector only, which hand it out once its MQTT client has
 * connected and subscribed. From then on it reads: what arrives before start()'s work has run waits
 * for it. It opens when start()'s work runs, unless close() was called before (rule 1); a close
 * goes out after what was handed to send() before it, bounded by the handshake limit.
 *
 * Ownership: the pending work of its MQTT client owns the end, as do the handlers posted to its
 * strands; the end owns the role through the handlers until it has reported its end (rule 4), and
 * keeps the handler executor's context busy until then.
 */
class GMqttPeer final
  : public GPeerI
  , public std::enable_shared_from_this<GMqttPeer> {
    friend class GMqttListener;
    friend class GMqttConnector;

public:
    /** @brief Which end of a connection this is */
    enum class GSide : std::uint8_t {
        SERVER, ///< The server's end: reads P/I/up, writes P/I/down
        CLIENT  ///< The client's end: reads P/I/down, writes P/I/up, joins on P/join
    };

    /** @brief Prepares the end; nothing happens before the listener or the connector begins it */
    GMqttPeer(
        GMqttOptions,
        std::string,
        GSide,
        const boost::asio::any_io_executor &,
        const boost::asio::any_io_executor & = {}
    );
    /** @brief The destructor */
    ~GMqttPeer() override;

    GMqttPeer(const GMqttPeer &) = delete;
    GMqttPeer(GMqttPeer &&) = delete;
    GMqttPeer &operator=(const GMqttPeer &) = delete;
    GMqttPeer &operator=(GMqttPeer &&) = delete;

    /** @brief Installs the handlers and opens on the I/O executor */
    void start(GPeerHandlers) override;
    /** @brief Queues a message; published after those handed over before */
    void send(GMessage) override;
    /** @brief Closes after what was handed to send() before. Any thread; idempotent. */
    void close(GPeerClose) override;
    /** @brief Ends at once, for teardown; tells the other end with a close if it still can */
    void abort() override;
    /** @brief The strand all handlers of this end run on. Any thread. */
    [[nodiscard]] boost::asio::any_io_executor executor() override;
    /** @brief The longest message a packet the broker and the other end accept can carry. Any
     *  thread. */
    [[nodiscard]] std::size_t maxMessageBytes() const override;
    /** @brief Notes the interval the server named; the MQTT keep-alive stays as it connected */
    void setKeepAlive(std::chrono::milliseconds) override;

    /** @brief Whether the end has ended. Any thread. @return Whether it has */
    [[nodiscard]] bool ended() const noexcept;
    /** @brief How many messages of the role have arrived from the other end so far (handed to the
     *  handler strand, or waiting for start()). Any thread. @return The count */
    [[nodiscard]] std::uint64_t received() const noexcept;

private:
    struct GLink; ///< The MQTT client (GMqttPeerTransport.cpp)

    /** @brief What the listener or the connector is told once the end is ready to be handed out
     *  (an empty text), or why it cannot be (the end is then over) */
    using ready_type = std::move_only_function<void(std::string_view)>;

    /** @brief How an end lets go of its MQTT client when it ends */
    enum class GLetGo : std::uint8_t {
        DISCONNECT,  ///< A regular DISCONNECT: the close has been exchanged, no will
        CLOSE_FIRST, ///< A close (NORMAL) first, then a regular DISCONNECT (abort())
        CANCEL       ///< Stop the client without a DISCONNECT: the broker publishes the will
    };

    void begin_(ready_type);
    void drop_();
    void subscribed_(const std::string &);
    void becomeReady_();
    void notReady_(const std::string &);
    void linkBroken_(const std::string &, bool);
    void armLimit_();
    void limitExpired_();
    void read_();
    void readDone_(const std::string &, std::string &);
    void started_();
    void deliver_(std::string &);
    void pump_();
    void published_(const std::string &);
    void beginClose_(GPeerClose);
    void sendClose_();
    void finish_(GPeerEnd, GPeerClose, std::string, GLetGo, bool report_inline = false);
    void letGo_(GLetGo);
    void report_(GPeerEnd, GPeerClose, const std::string &);

    GMqttOptions options_;            ///< Broker, prefix, keep-alive
    GSide side_;                      ///< Which end this is
    std::string id_;                  ///< The connection's id (I)
    std::string in_topic_;            ///< What it reads
    std::string out_topic_;           ///< What it writes (and where its will goes)
    std::chrono::milliseconds limit_; ///< The handshake limit

    boost::asio::any_io_executor io_;             ///< Where the MQTT client runs (serialized)
    boost::asio::any_io_executor handler_strand_; ///< Where the role's handlers run
    boost::asio::any_io_executor owner_work_;     ///< Keeps the handler context busy until the end
    std::unique_ptr<GLink> link_;                 ///< The MQTT client (I/O executor)
    boost::asio::steady_timer
        limit_timer_; ///< The handshake limit, the wait for the server, the close

    GPeerHandlers handlers_; ///< Set by start(); used on the handler strand; dropped at the end
    ready_type on_ready_;    ///< Who waits for the end to be ready (until it is, or is not)
    std::size_t max_message_bytes_ = 0; ///< See maxMessageBytes(); set before the end is handed out
    std::deque<std::string> waiting_;   ///< Arrived before start()'s work ran (I/O executor)
    std::deque<GMessage> send_queue_;   ///< Waiting to be published (I/O executor)

    // The state below is read and written on the I/O executor only
    bool is_ready_ = false;     ///< Subscribed (and joined): the end may be handed out
    bool started_flag_ = false; ///< start()'s work has run
    bool opened_ = false;       ///< The role has been told of the open
    bool heard_ = false;        ///< Something of the other end has arrived
    bool publishing_ = false;   ///< A publish is outstanding
    bool closing_ = false;      ///< A close has begun: nothing is queued any more
    bool close_sent_ = false;   ///< The close is being published
    bool finished_ = false;     ///< The end has been decided
    GPeerClose close_how_ = GPeerClose::NORMAL; ///< What this end's close says
    GPeerEnd end_ = GPeerEnd::FAILED;           ///< The end decided by finish_()
    GPeerClose end_how_ = GPeerClose::NORMAL;   ///< What the other end's close said
    std::string end_detail_;                    ///< Words about the end

    std::atomic<bool> close_requested_{false}; ///< close() was called (any thread)
    std::atomic<bool> ended_{false};           ///< See ended()
    std::atomic<std::uint64_t> received_{0};   ///< See received()
};

/******************************************************************************/
/**
 * The server side of the MQTT transport (a GListenerI): its own MQTT client, subscribed to P/join,
 * makes a server's end for every client that joins (see the file comment).
 *
 * Threads. The listener owns an I/O context with one thread of its own (accounted in the thread
 * budget as "<name>:io"); its MQTT client and every end's MQTT client run on strands of it, and so
 * do the calls of on_peer, on the listener's strand. The ends' handlers -- the sessions -- run on
 * strands of the executor the shell hands to start() (GPeerI.hpp, rule 2).
 *
 * start() connects and subscribes before it returns, and throws, naming the broker, if that fails
 * within the handshake limit. stop() takes no join any more and hands no end out from then on;
 * endWork() joins the thread, stops the MQTT clients that still run and lets their operations
 * complete, and shuts the I/O context down: the ends that are still open go without onClosed, and
 * without a DISCONNECT, so their clients hear their wills. A listener whose own broker connection
 * fails takes no new client; it says so in the log.
 */
class GMqttListener final : public GListenerI {
public:
    /** @brief Prepares the listener; nothing happens before start(). */
    GMqttListener(std::string, GMqttOptions, std::chrono::milliseconds = GMQTTCONSUMERSTOPGRACE);
    /** @brief Ends the I/O thread if endWork() has not done so */
    ~GMqttListener() override;

    GMqttListener(const GMqttListener &) = delete;
    GMqttListener(GMqttListener &&) = delete;
    GMqttListener &operator=(const GMqttListener &) = delete;
    GMqttListener &operator=(GMqttListener &&) = delete;

    /** @brief Starts the I/O thread, connects to the broker and subscribes to the joins; throws
     *  (and is then as if not started) if the broker cannot be reached. */
    void start(boost::asio::any_io_executor, on_peer_type) override;
    /** @brief No joins and no new ends from here on. Bounded; idempotent. */
    void stop() override;
    /** @brief Joins the I/O thread and shuts the I/O context down. */
    void endWork() override;
    /** @brief How long the shell waits for its clients at the end of a run. */
    [[nodiscard]] std::chrono::milliseconds stopGrace() const override;
    /** @brief The MQTT keep-alive of the server's ends, which their clients are told in the HELLO */
    [[nodiscard]] std::chrono::milliseconds keepAlive() const override;

private:
    struct GJoins; ///< The listener's MQTT client (GMqttPeerTransport.cpp)

    void connect_();
    void joinsBroken_(const std::string &);
    void receiveJoin_();
    void joined_(std::string);
    void handOver_(const std::shared_ptr<GMqttPeer> &, const std::string &);
    void joinIo_();
    void letClientsGo_();

    std::string name_;                     ///< The owner's name, for the log and the thread budget
    GMqttOptions options_;                 ///< Broker, prefix, keep-alive
    std::chrono::milliseconds stop_grace_; ///< What stopGrace() answers

    GIoContext io_context_; ///< Where the MQTT clients run
    std::optional<boost::asio::executor_work_guard<GIoContext::executor_type>> work_guard_;
    boost::asio::strand<GIoContext::executor_type> strand_; ///< The joins and the calls of on_peer
    std::unique_ptr<GJoins> joins_;                         ///< The listener's MQTT client

    boost::asio::any_io_executor handler_executor_; ///< Where the ends' handlers run
    on_peer_type on_peer_;         ///< Set by start(), called on strand_, dropped by endWork()
    GWirePeerId next_peer_id_ = 1; ///< strand_ only
    /// The connections that have joined, by id, with their server's ends, held weakly (strand_
    /// only; endWork() stops those that have not ended)
    std::unordered_map<std::string, std::weak_ptr<GMqttPeer>> ends_;

    /// The I/O thread's reservation in the process-wide thread budget
    Gem::Common::Concurrency::GThreadBudget::Reservation budget_;
    Gem::Common::Concurrency::GThreadGroup threads_; ///< The I/O thread
    std::atomic<bool> running_{false};               ///< The I/O thread runs
    std::atomic<bool> stopped_{false};               ///< stop() was called
};

/******************************************************************************/
/**
 * The client side of the MQTT transport (a GConnectorI): every connect() makes a new client's end
 * with a new id and a new MQTT client, which connects, subscribes and joins (see the file comment);
 * it is reported once it has joined. Its handlers and its I/O run on the executor given to
 * connect(). reconnectable() is true.
 */
class GMqttConnector final : public GConnectorI {
public:
    /** @brief Prepares the connector; nothing happens before connect(). */
    explicit GMqttConnector(GMqttOptions);
    /** @brief Gives up an attempt in progress. */
    ~GMqttConnector() override;

    GMqttConnector(const GMqttConnector &) = delete;
    GMqttConnector(GMqttConnector &&) = delete;
    GMqttConnector &operator=(const GMqttConnector &) = delete;
    GMqttConnector &operator=(GMqttConnector &&) = delete;

    /** @brief Makes a new end, which connects to the broker, subscribes and joins. */
    void connect(boost::asio::any_io_executor, done_type) override;
    /** @brief Gives up the attempt in progress; its done is not called. */
    void cancel() override;
    /** @brief @return true: every connect() is an attempt of its own */
    [[nodiscard]] bool reconnectable() const override;

private:
    struct GAttempt; ///< The end of one attempt and its callback

    GMqttOptions options_;            ///< Broker, prefix, keep-alive
    std::weak_ptr<GAttempt> attempt_; ///< The attempt in progress, if any
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
