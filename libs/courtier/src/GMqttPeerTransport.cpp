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

#include "courtier/transport/GMqttPeerTransport.hpp"

// Standard headers go here
#include <algorithm>
#include <format>
#include <limits>
#include <utility>
#include <variant>
#include <vector>

// Boost headers go here
#include <boost/asio/dispatch.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>
#include <boost/mqtt5/error.hpp>
#include <boost/mqtt5/mqtt_client.hpp>
#include <boost/mqtt5/reason_codes.hpp>
#include <boost/mqtt5/types.hpp>

// Geneva headers go here
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GCompletionLatch.hpp"

namespace Gem::Courtier::Consumers {

namespace {

using error_code = boost::system::error_code;
namespace mqtt = boost::mqtt5;

/// The first byte of every payload on a connection's topics
constexpr char DATA = 0;  ///< A message of the role follows
constexpr char CLOSE = 1; ///< One byte follows: the GPeerClose
constexpr char GONE = 2;  ///< The will: the other end's broker connection was lost

/// The largest packet every MQTT client of the transport accepts (its maximum_packet_size), which
/// bounds what it sends too: mosquitto 2.1's default limit of a broker. Boost.MQTT5 (1.92) gives
/// every client a read buffer of this size and fills it when it connects (impl/assemble_op.hpp), so
/// MQTT's own maximum (268,435,455 bytes) would cost every client 256 MiB and 0.1 s.
constexpr std::uint32_t MQTT_MAX_PACKET = 2'000'000;
/// What a PUBLISH of QoS 0 adds to its topic and to the role's message: the fixed header (1), the
/// remaining length (at most 4), the topic's length (2), the properties' length (1) and the first
/// byte of the payload (1)
constexpr std::size_t PUBLISH_OVERHEAD = 9;
/// How long stop() waits for the listener's strand, after the call of on_peer under way
constexpr std::chrono::milliseconds LISTENER_STOP_BOUND{1000};

/******************************************************************************/
/**
 * The logger of an MQTT client, used as what it is in Boost.MQTT5: the only place where the client
 * tells of a lost broker connection, which it would then re-establish by itself. Every hook that
 * reports one tells the owner (on the client's executor, inside the client's operation: the owner
 * posts what it does). The first round of connecting may try every address the broker's name
 * resolves to; a second round, or any connect after a CONNACK, is a reconnection.
 */
class GLinkWatch {
public:
    /// What is told: what happened, and whether the broker fell silent (its keep-alive ran out)
    using broken_type = std::function<void(std::string, bool)>;

    /** @brief @param broken What is told */
    explicit GLinkWatch(broken_type broken)
      : broken_(std::move(broken)) { /* nothing */
    }

    /** @brief A lookup of the broker's name has completed */
    void at_resolve(
        error_code ec,
        std::string_view host,
        std::string_view port,
        const boost::asio::ip::tcp::resolver::results_type &endpoints
    ) {
        if(++resolves_ > 1) {
            broken_("the MQTT client began to connect to the broker again", false);
        }
        else if(ec) {
            broken_(
                std::format(
                    "the broker {}:{} could not be looked up: {}",
                    host,
                    port,
                    ec.message()
                ),
                false
            );
        }
        n_endpoints_ = endpoints.size();
    }

    /** @brief A TCP connect has completed */
    void at_tcp_connect(error_code ec, const boost::asio::ip::tcp::endpoint &endpoint) {
        if(connected_) {
            broken_("the MQTT client began to connect to the broker again", false);
        }
        else if(ec && ++n_failed_ >= n_endpoints_) {
            broken_(
                std::format(
                    "no connection to the broker at {}:{}: {}",
                    endpoint.address().to_string(),
                    endpoint.port(),
                    ec.message()
                ),
                false
            );
        }
    }

    /** @brief The broker has answered the CONNECT */
    void at_connack(mqtt::reason_code code, bool, const mqtt::connack_props &) {
        if(code) {
            broken_("the broker refused the connection: " + code.message(), false);
        }
        connected_ = true;
    }

    /** @brief The broker has ended the connection */
    void at_disconnect(mqtt::reason_code code, const mqtt::disconnect_props &) {
        broken_("the broker ended the connection: " + code.message(), false);
    }

    /** @brief A read or a write failed, or the broker fell silent */
    void at_transport_error(error_code ec) {
        broken_(
            "the connection to the broker failed: " + ec.message(),
            ec == boost::asio::error::timed_out
        );
    }

private:
    broken_type broken_;          ///< Who is told
    std::size_t resolves_ = 0;    ///< Lookups of the broker's name
    std::size_t n_endpoints_ = 0; ///< The addresses the first lookup found
    std::size_t n_failed_ = 0;    ///< TCP connects that failed in the first round
    bool connected_ = false;      ///< A CONNACK has arrived
};

/// The stream of an MQTT client's broker connection: TCP. The one place where the transport names
/// it: an encrypted connection would put a TLS stream here (and a TLS context in place of
/// std::monostate below, with its settings in GMqttOptions).
using stream_type = boost::asio::ip::tcp::socket;
using client_type = mqtt::mqtt_client<stream_type, std::monostate, GLinkWatch>;

/** @brief An MQTT client with its broker, keep-alive and largest packet set (before it runs)
 *  @param client The client @param options Broker and keep-alive */
void configure(client_type &client, const GMqttOptions &options) {
    client.brokers(options.host, options.port)
        .keep_alive(
            static_cast<std::uint16_t>(std::min<std::chrono::seconds::rep>(
                options.keepAlive.count(),
                std::numeric_limits<std::uint16_t>::max()
            ))
        )
        .connect_property(mqtt::prop::maximum_packet_size, MQTT_MAX_PACKET);
}

/** @brief @param topic A topic @return One subscription to it, QoS 0 */
std::vector<mqtt::subscribe_topic> subscription(std::string topic) {
    return {mqtt::subscribe_topic{
        std::move(topic),
        mqtt::subscribe_options{.max_qos = mqtt::qos_e::at_most_once}
    }};
}

/** @brief @param ec The outcome of a subscription @param codes Its reason codes
 *  @return Why it failed; empty if it did not */
std::string subscriptionError(const error_code &ec, const std::vector<mqtt::reason_code> &codes) {
    if(ec) {
        return ec.message();
    }
    if(codes.empty()) {
        return "no reason code";
    }
    return codes.front() ? codes.front().message() : std::string{};
}

/** @brief @param keep_alive An MQTT keep-alive @return The handshake limit that goes with it */
std::chrono::milliseconds limitFor(std::chrono::seconds keep_alive) {
    return keep_alive.count() > 0 ? std::chrono::milliseconds(2 * keep_alive)
                                  : std::chrono::milliseconds(GMQTTHANDSHAKETIMEOUT);
}

/** @brief @param options What a user asked for @return The same, if its topic prefix can be
 *  used @throws gem_exception if it is empty or holds a wildcard */
GMqttOptions checked(GMqttOptions options) {
    if(options.topic.empty() || options.topic.find_first_of("+#") != std::string::npos) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "In " << options.name << ": \"" << options.topic
            << "\" cannot be the prefix of the MQTT"
            << " topics (it must not be empty and must hold no wildcard)" << '\n'
        );
    }
    return options;
}

/** @brief @param id What a join named @return Whether it is an id a connector chooses */
bool isId(std::string_view id) {
    return not id.empty() && id.size() <= 32 && std::ranges::all_of(id, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

/** @brief @return A new id for a connection: 64 random bits, in hex */
std::string newId() {
    return std::format("{:016x}", Gem::Common::randomBits64());
}

/** @brief @param byte The second byte of a CLOSE @return What the close says */
GPeerClose closeFor(char byte) {
    switch(static_cast<GPeerClose>(byte)) {
    case GPeerClose::NORMAL:
        return GPeerClose::NORMAL;
    case GPeerClose::RUN_ENDED:
        return GPeerClose::RUN_ENDED;
    case GPeerClose::ERROR:
        break;
    }
    return GPeerClose::ERROR;
}

} /* anonymous namespace */

/******************************************************************************/
/** The MQTT client of an end */
struct GMqttPeer::GLink {
    GLink(const boost::asio::any_io_executor &io, GLinkWatch watch)
      : client(io, std::monostate{}, std::move(watch)) { /* nothing */
    }
    client_type client; ///< The client
};

/******************************************************************************/
/**
 * Prepares the end; nothing happens before the listener or the connector begins it.
 *
 * @param options Broker, prefix and keep-alive
 * @param id The connection's id (I)
 * @param side Which end this is
 * @param io Where the MQTT client runs; must serialize its handlers
 * @param handler_executor Where the role's handlers run (on a strand of it); empty: on @p io
 */
GMqttPeer::GMqttPeer(
    GMqttOptions options,
    std::string id,
    GSide side,
    const boost::asio::any_io_executor &io,
    const boost::asio::any_io_executor &handler_executor
)
  : options_(std::move(options))
  , side_(side)
  , id_(std::move(id))
  , in_topic_(options_.topic + "/" + id_ + (side_ == GSide::SERVER ? "/up" : "/down"))
  , out_topic_(options_.topic + "/" + id_ + (side_ == GSide::SERVER ? "/down" : "/up"))
  , limit_(limitFor(options_.keepAlive))
  , io_(io)
  , handler_strand_(
        handler_executor ? boost::asio::any_io_executor(boost::asio::make_strand(handler_executor))
                         : io_
    )
  , owner_work_(
        boost::asio::prefer(handler_strand_, boost::asio::execution::outstanding_work_t::tracked)
    )
  , limit_timer_(io_) { /* nothing */
}

/******************************************************************************/
/** The destructor: an MQTT client that still runs is stopped by its own destructor. */
GMqttPeer::~GMqttPeer() = default;

/******************************************************************************/
/**
 * Begins the end on its I/O executor: the MQTT client connects, with the end's will, subscribes to
 * what the end reads and, on a client, joins; @p ready is told the outcome there. The handlers
 * given to the MQTT client hold the end weakly where the client keeps them itself (the receive and
 * the subscription wait inside it, and would otherwise keep the end alive for ever); the client's
 * run owns the end until the client stops.
 *
 * @param ready Who is told that the end is ready, or why it is not
 */
void GMqttPeer::begin_(ready_type ready) {
    boost::asio::post(io_, [self = this->shared_from_this(), ready = std::move(ready)]() mutable {
        self->on_ready_ = std::move(ready);
        const std::weak_ptr<GMqttPeer> weak = self;
        self->link_ = std::make_unique<GLink>(
            self->io_,
            GLinkWatch{[weak, io = self->io_](std::string what, bool silent) {
                boost::asio::post(io, [weak, what = std::move(what), silent] {
                    if(auto peer = weak.lock()) {
                        peer->linkBroken_(what, silent);
                    }
                });
            }}
        );
        auto &client = self->link_->client;
        configure(client, self->options_);
        client.will(mqtt::will(self->out_topic_, std::string(1, GONE)));
        client.async_run([self](error_code ec) {
            self->linkBroken_("the MQTT client stopped: " + ec.message(), false);
        });
        self->armLimit_();
        client.async_subscribe(
            subscription(self->in_topic_),
            mqtt::subscribe_props{},
            [weak](error_code ec, std::vector<mqtt::reason_code> codes, mqtt::suback_props) {
                if(auto peer = weak.lock()) {
                    peer->subscribed_(subscriptionError(ec, codes));
                }
            }
        );
    });
}

/******************************************************************************/
/** Gives the end up before it was handed out, or after it was not taken: its MQTT client stops
 *  (its will tells a client's server, a server's end its client). On the I/O executor, or when
 *  nothing runs it any more (a connector that is destroyed). */
void GMqttPeer::drop_() {
    boost::asio::dispatch(io_, [self = this->shared_from_this()] {
        self->on_ready_ = nullptr;
        if(not self->finished_) {
            self->finished_ = true;
            self->ended_.store(true);
            self->limit_timer_.cancel();
            self->letGo_(GLetGo::CANCEL);
        }
    });
}

/******************************************************************************/
/**
 * The broker has answered the subscription: the end can carry what fits into a packet both the
 * broker and the other end accept, reads from now on, and is ready -- a client's end once it has joined.
 *
 * @param error Why the subscription failed; empty if it did not
 */
void GMqttPeer::subscribed_(const std::string &error) {
    if(is_ready_ || finished_) {
        return;
    }
    if(not error.empty()) {
        this->notReady_("the broker refused the subscription to " + in_topic_ + ": " + error);
        return;
    }
    // The other end accepts MQTT_MAX_PACKET, the broker what its CONNACK named
    const std::size_t packet_max = std::min<std::size_t>(
        MQTT_MAX_PACKET,
        link_->client.connack_property(mqtt::prop::maximum_packet_size).value_or(MQTT_MAX_PACKET)
    );
    const std::size_t overhead = PUBLISH_OVERHEAD + out_topic_.size();
    max_message_bytes_ = packet_max > overhead ? packet_max - overhead : 0;
    this->read_();
    if(side_ == GSide::SERVER) {
        this->becomeReady_();
        return;
    }
    link_->client.async_publish<mqtt::qos_e::at_most_once>(
        options_.topic + "/join",
        id_,
        mqtt::retain_e::no,
        mqtt::publish_props{},
        [self = this->shared_from_this()](error_code ec) {
            if(self->is_ready_ || self->finished_) {
                return;
            }
            if(ec) {
                self->notReady_("the join could not be published: " + ec.message());
                return;
            }
            self->becomeReady_();
        }
    );
}

/******************************************************************************/
/** The end may be handed out. A client's end waits, from now on, at most the handshake limit for
 *  the first message of a server. */
void GMqttPeer::becomeReady_() {
    is_ready_ = true;
    if(side_ == GSide::CLIENT) {
        this->armLimit_();
    }
    else {
        limit_timer_.cancel();
    }
    ready_type ready = std::move(on_ready_);
    on_ready_ = nullptr;
    if(ready) {
        ready(std::string_view{});
    }
}

/******************************************************************************/
/** The end cannot be handed out: it is over, without a report (no role has started it), and its
 *  MQTT client stops. @param why Why */
void GMqttPeer::notReady_(const std::string &why) {
    finished_ = true;
    ended_.store(true);
    limit_timer_.cancel();
    this->letGo_(GLetGo::CANCEL);
    ready_type ready = std::move(on_ready_);
    on_ready_ = nullptr;
    if(ready) {
        ready(why);
    }
}

/******************************************************************************/
/**
 * The MQTT client has lost its broker connection, or has begun to connect again (GLinkWatch): the
 * end is over -- SILENT if the broker fell silent, else FAILED -- and the client stops, so that it
 * does not reconnect; its will tells the other end.
 *
 * @param what What happened @param silent Whether the broker fell silent
 */
void GMqttPeer::linkBroken_(const std::string &what, bool silent) {
    if(finished_) {
        return;
    }
    if(not is_ready_) {
        this->notReady_(what);
        return;
    }
    this->finish_(
        silent ? GPeerEnd::SILENT : GPeerEnd::FAILED,
        GPeerClose::NORMAL,
        what,
        GLetGo::CANCEL
    );
}

/******************************************************************************/
/** Arms the limit of what the end waits for now: the broker's answers before it is ready, a
 *  server's first message on a client, the close's way out once it closes. */
void GMqttPeer::armLimit_() {
    limit_timer_.expires_after(limit_);
    limit_timer_.async_wait([self = this->shared_from_this()](const error_code &ec) {
        if(not ec) {
            self->limitExpired_();
        }
    });
}

/******************************************************************************/
/** The limit armed last has run out: what the end waited for has not happened. */
void GMqttPeer::limitExpired_() {
    if(finished_) {
        return;
    }
    const std::string limit = std::to_string(limit_.count()) + " ms";
    if(not is_ready_) {
        this->notReady_("the broker did not answer within " + limit);
    }
    else if(closing_) {
        this->finish_(
            GPeerEnd::CLOSED_HERE,
            GPeerClose::NORMAL,
            "closed by this end (what was queued could not be sent within " + limit + ")",
            GLetGo::CANCEL
        );
    }
    else if(side_ == GSide::CLIENT && not heard_) {
        this->finish_(
            GPeerEnd::SILENT,
            GPeerClose::NORMAL,
            "no server answered the join on " + options_.topic + "/join within " + limit,
            GLetGo::CANCEL
        );
    }
}

/******************************************************************************/
/** Waits for the next message; one wait is outstanding from the subscription to the end. */
void GMqttPeer::read_() {
    link_->client.async_receive([weak = this->weak_from_this()](
                                    error_code ec,
                                    std::string,
                                    std::string payload,
                                    mqtt::publish_props
                                ) {
        if(auto self = weak.lock()) {
            self->readDone_(ec ? ec.message() : std::string{}, payload);
        }
    });
}

/******************************************************************************/
/**
 * A message has arrived: a message of the role is handed to the role's strand (or waits for
 * start()'s work), and the end reads on; a close or the will ends the end. What arrives after the
 * end was decided -- after this end's close has gone out, too -- is dropped (GPeerI.hpp, rule 6).
 *
 * @param error Why the wait failed; empty if it did not @param payload What arrived
 */
void GMqttPeer::readDone_(const std::string &error, std::string &payload) {
    if(finished_) {
        return;
    }
    if(not error.empty()) {
        this->finish_(
            GPeerEnd::FAILED,
            GPeerClose::NORMAL,
            "receiving failed: " + error,
            GLetGo::CANCEL
        );
        return;
    }
    if(side_ == GSide::CLIENT && not heard_ && not closing_) {
        limit_timer_.cancel(); // a server has answered the join
    }
    heard_ = true;
    const char kind = payload.empty() ? CLOSE : payload.front();
    if(kind == DATA) {
        payload.erase(0, 1);
        received_.fetch_add(1);
        if(not started_flag_) {
            waiting_.push_back(std::move(payload));
        }
        else if(opened_) {
            boost::asio::post(
                handler_strand_,
                [self = this->shared_from_this(), message = std::move(payload)]() mutable {
                    self->deliver_(message);
                }
            );
        }
        this->read_();
    }
    else if(kind == CLOSE && payload.size() == 2) {
        this->finish_(
            GPeerEnd::CLOSED_THERE,
            closeFor(payload[1]),
            "closed by the other end",
            GLetGo::DISCONNECT
        );
    }
    else if(kind == GONE && payload.size() == 1) {
        this->finish_(
            GPeerEnd::SILENT,
            GPeerClose::NORMAL,
            "the other end's connection to the broker was lost",
            GLetGo::DISCONNECT
        );
    }
    else {
        this->finish_(
            GPeerEnd::FAILED,
            GPeerClose::NORMAL,
            "a message this transport does not know arrived",
            GLetGo::CANCEL
        );
    }
}

/******************************************************************************/
/**
 * Installs the handlers; the end opens on its I/O executor. A close() after this call is ordered
 * after the open (both are posted there).
 *
 * @param handlers What the role wants to be told
 */
void GMqttPeer::start(GPeerHandlers handlers) {
    handlers_ = std::move(handlers);
    boost::asio::post(io_, [self = this->shared_from_this()] { self->started_(); });
}

/******************************************************************************/
/**
 * start()'s work: the role is told of the open, then of what has arrived so far, on its strand --
 * unless the end has ended before (it is told only the end, rule 1) or close() was called before
 * (it is told only the end, once the close has gone out).
 */
void GMqttPeer::started_() {
    started_flag_ = true;
    if(finished_) {
        waiting_.clear();
        boost::asio::post(
            handler_strand_,
            [self = this->shared_from_this(), end = end_, how = end_how_, detail = end_detail_] {
                self->report_(end, how, detail);
            }
        );
        return;
    }
    if(close_requested_.load()) {
        waiting_.clear();
        return;
    }
    opened_ = true;
    auto self = this->shared_from_this();
    boost::asio::post(handler_strand_, [self] {
        if(self->handlers_.onOpen) {
            self->handlers_.onOpen();
        }
    });
    for(auto &message : waiting_) {
        boost::asio::post(handler_strand_, [self, message = std::move(message)]() mutable {
            self->deliver_(message);
        });
    }
    waiting_.clear();
}

/******************************************************************************/
/** On the handler strand: hands one message to the role. @param message The message */
void GMqttPeer::deliver_(std::string &message) {
    if(handlers_.onMessage) {
        handlers_.onMessage(std::move(message));
    }
}

/******************************************************************************/
/**
 * Queues a message on the I/O executor. One handed over before the open or after close() is
 * dropped. One longer than maxMessageBytes() is not published: the end fails (FAILED, reported on
 * the handler strand) and its will tells the other end.
 *
 * @param message The complete message
 */
void GMqttPeer::send(GMessage message) {
    if(close_requested_.load()) {
        return;
    }
    boost::asio::post(
        io_,
        [self = this->shared_from_this(), message = std::move(message)]() mutable {
            if(self->closing_ || self->finished_ || not self->opened_ || not message) {
                return;
            }
            if(message->size() > self->max_message_bytes_) {
                self->finish_(
                    GPeerEnd::FAILED,
                    GPeerClose::NORMAL,
                    std::format(
                        "a message of {} bytes was not sent: a packet the broker and the other "
                        "end accept carries at most {} bytes",
                        message->size(),
                        self->max_message_bytes_
                    ),
                    GLetGo::CANCEL
                );
                return;
            }
            self->send_queue_.push_back(std::move(message));
            self->pump_();
        }
    );
}

/******************************************************************************/
/** Publishes the next queued message unless a publish is outstanding; when the queue has run
 *  empty during a close, the close goes out. */
void GMqttPeer::pump_() {
    if(publishing_ || finished_ || close_sent_) {
        return;
    }
    if(send_queue_.empty()) {
        if(closing_) {
            this->sendClose_();
        }
        return;
    }
    publishing_ = true;
    const GMessage message = std::move(send_queue_.front());
    send_queue_.pop_front();
    std::string payload;
    payload.reserve(message->size() + 1);
    payload.push_back(DATA);
    payload.append(*message);
    link_->client.async_publish<mqtt::qos_e::at_most_once>(
        out_topic_,
        std::move(payload),
        mqtt::retain_e::no,
        mqtt::publish_props{},
        [self = this->shared_from_this()](error_code ec) {
            self->published_(ec ? ec.message() : std::string{});
        }
    );
}

/******************************************************************************/
/** A publish has been written: the next one follows. @param error Why it failed; empty if not */
void GMqttPeer::published_(const std::string &error) {
    publishing_ = false;
    if(finished_) {
        return;
    }
    if(not error.empty()) {
        this->finish_(
            GPeerEnd::FAILED,
            GPeerClose::NORMAL,
            "a message could not be sent: " + error,
            GLetGo::CANCEL
        );
        return;
    }
    this->pump_();
}

/******************************************************************************/
/**
 * Closes in the regular way, after what was handed to send() before (posted to the I/O executor
 * after it). From this call on no message is accepted; what arrives until this end's close has gone
 * out is still delivered.
 *
 * @param how What the other end is told
 */
void GMqttPeer::close(GPeerClose how) {
    if(close_requested_.exchange(true)) {
        return;
    }
    boost::asio::post(io_, [self = this->shared_from_this(), how] { self->beginClose_(how); });
}

/******************************************************************************/
/** Nothing is queued any more; the close goes out when the queue is empty, within the handshake
 *  limit. @param how What the close says */
void GMqttPeer::beginClose_(GPeerClose how) {
    if(closing_ || finished_) {
        return;
    }
    closing_ = true;
    close_how_ = how;
    this->armLimit_();
    this->pump_();
}

/******************************************************************************/
/** Publishes the close; the end has ended (CLOSED_HERE) when it has been written, and disconnects
 *  in the regular way, which discards its will. */
void GMqttPeer::sendClose_() {
    close_sent_ = true;
    std::string payload{CLOSE, static_cast<char>(close_how_)};
    link_->client.async_publish<mqtt::qos_e::at_most_once>(
        out_topic_,
        std::move(payload),
        mqtt::retain_e::no,
        mqtt::publish_props{},
        [self = this->shared_from_this()](error_code ec) {
            self->finish_(
                GPeerEnd::CLOSED_HERE,
                GPeerClose::NORMAL,
                ec ? "closed by this end (" + ec.message() + ")" : "closed by this end",
                ec ? GLetGo::CANCEL : GLetGo::DISCONNECT
            );
        }
    );
}

/******************************************************************************/
/**
 * Ends at once and reports CLOSED_HERE here (GPeerI::abort()); the other end is told with a close
 * (NORMAL) if this end has not closed, which goes out only if the I/O executor still runs. For
 * teardown, when no handler of the end runs and nothing else touches its MQTT client.
 */
void GMqttPeer::abort() {
    close_requested_.store(true);
    this->finish_(
        GPeerEnd::CLOSED_HERE,
        GPeerClose::NORMAL,
        "closed by this end (aborted)",
        close_sent_ ? GLetGo::DISCONNECT : GLetGo::CLOSE_FIRST,
        /*report_inline=*/true
    );
}

/******************************************************************************/
/** @return The strand all handlers of this end run on */
boost::asio::any_io_executor GMqttPeer::executor() {
    return handler_strand_;
}

/******************************************************************************/
/** @return The longest message of the role that fits into a packet the broker and the other end
 *  accept */
std::size_t GMqttPeer::maxMessageBytes() const {
    return max_message_bytes_;
}

/******************************************************************************/
/**
 * The interval the server named in its HELLO. The MQTT keep-alive of this end was fixed when it
 * connected to the broker and stays; the log says so when the two differ.
 *
 * @param interval The server's keep-alive interval
 */
void GMqttPeer::setKeepAlive(std::chrono::milliseconds interval) {
    if(interval != std::chrono::milliseconds(options_.keepAlive)) {
        glogger << options_.name << ": the server's keep-alive is " << interval.count()
                << " ms; this end's MQTT keep-alive stays "
                << std::chrono::milliseconds(options_.keepAlive).count()
                << " ms, as it was when it connected to the broker" << '\n'
                << GLOGGING;
    }
}

/******************************************************************************/
/** @return Whether the end has ended */
bool GMqttPeer::ended() const noexcept {
    return ended_.load();
}

/******************************************************************************/
/** @return How many messages of the role have arrived */
std::uint64_t GMqttPeer::received() const noexcept {
    return received_.load();
}

/******************************************************************************/
/**
 * The end of the end, whatever led to it; runs its body once: nothing more is sent or delivered,
 * the MQTT client is let go as @p let_go says, and the role is told on its strand after the
 * messages handed to it before -- if start()'s work has run, else when it runs. abort() has the
 * role told here and now (the report found before, if the end had ended).
 *
 * @param end Why it ended @param how What the other end's close said @param detail Words about it
 * @param let_go How the MQTT client is let go @param report_inline Whether the role is told here
 */
void GMqttPeer::finish_(
    GPeerEnd end,
    GPeerClose how,
    std::string detail,
    GLetGo let_go,
    bool report_inline
) {
    if(finished_) {
        if(report_inline) {
            this->report_(end_, end_how_, end_detail_);
        }
        return;
    }
    finished_ = true;
    ended_.store(true);
    closing_ = true;
    limit_timer_.cancel();
    send_queue_.clear();
    waiting_.clear();
    end_ = end;
    end_how_ = how;
    end_detail_ = std::move(detail);
    this->letGo_(let_go);
    if(report_inline) {
        this->report_(end_, end_how_, end_detail_);
    }
    else if(started_flag_) {
        boost::asio::post(
            handler_strand_,
            [self = this->shared_from_this(), end, how, detail = end_detail_] {
                self->report_(end, how, detail);
            }
        );
    }
}

/******************************************************************************/
/** Lets the MQTT client go: a regular DISCONNECT (no will), a close first and then one, or a stop
 *  without one (the broker publishes the will). @param let_go How */
void GMqttPeer::letGo_(GLetGo let_go) {
    if(not link_) {
        return;
    }
    auto &client = link_->client;
    switch(let_go) {
    case GLetGo::DISCONNECT:
        client.async_disconnect(
            mqtt::disconnect_rc_e::normal_disconnection,
            mqtt::disconnect_props{},
            [](error_code) { /* nothing */ }
        );
        break;
    case GLetGo::CLOSE_FIRST:
        client.async_publish<mqtt::qos_e::at_most_once>(
            out_topic_,
            std::string{CLOSE, static_cast<char>(GPeerClose::NORMAL)},
            mqtt::retain_e::no,
            mqtt::publish_props{},
            [self = this->shared_from_this()](error_code) {
                self->link_->client.async_disconnect(
                    mqtt::disconnect_rc_e::normal_disconnection,
                    mqtt::disconnect_props{},
                    [](error_code) { /* nothing */ }
                );
            }
        );
        break;
    case GLetGo::CANCEL:
        client.cancel();
        break;
    }
}

/******************************************************************************/
/**
 * On the handler strand (or, from abort(), where no handler runs): tells the role that the
 * connection is over, once, drops the handlers and with them the end's hold on the role, and lets
 * go of the handler context.
 *
 * @param end Why it ended @param how What the other end's close said @param detail Words about it
 */
void GMqttPeer::report_(GPeerEnd end, GPeerClose how, const std::string &detail) {
    GPeerHandlers handlers = std::move(handlers_);
    handlers_ = GPeerHandlers{};
    if(handlers.onClosed) {
        handlers.onClosed(end, how, detail);
    }
    owner_work_ = {};
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/** The listener's MQTT client, and what start() waits for */
struct GMqttListener::GJoins {
    explicit GJoins(const boost::asio::any_io_executor &io)
      : retry(io) { /* nothing */
    }
    /// The client; made anew when it has lost the broker (connect_())
    std::unique_ptr<client_type> client;
    /// Counts the clients made and given up: a hook of one that is not the current one is ignored
    std::uint64_t generation = 0;
    boost::asio::steady_timer retry; ///< The pause before the next client is made
    std::chrono::milliseconds pause = GCLIENTRECONNECTPAUSEMIN; ///< Its next length
    bool down = false;    ///< The broker was lost and has not answered again (said once in the log)
    bool decided = false; ///< start() has its answer (strand only)
    std::string why;      ///< Why the client could not subscribe; empty if it did
    Gem::Common::Concurrency::GCompletionLatch answered{1}; ///< start()'s answer is there
};

/******************************************************************************/
/**
 * Prepares the listener; nothing happens before start().
 *
 * @param name The owner's name, for the log and for the thread budget ("<name>:io")
 * @param options Broker, prefix, and the keep-alive of the server's ends
 * @param stop_grace How long the shell waits for the connections to end at the end of a run
 * @throws gem_exception if the topic prefix cannot be used
 */
GMqttListener::GMqttListener(
    std::string name,
    GMqttOptions options,
    std::chrono::milliseconds stop_grace
)
  : name_(std::move(name))
  , options_(checked(std::move(options)))
  , stop_grace_(stop_grace)
  , strand_(io_context_.get_executor()) { /* nothing */
}

/******************************************************************************/
/** Ends the I/O thread if it still runs: endWork() was not called. */
GMqttListener::~GMqttListener() {
    try {
        stopped_.store(true);
        this->joinIo_();
    }
    catch(...) { // NOLINT(bugprone-empty-catch) -- deliberate best-effort teardown
        // never let an exception escape a destructor
    }
}

/******************************************************************************/
/**
 * Starts the I/O thread, connects to the broker and subscribes to the joins, and waits for that,
 * at most the handshake limit. If the broker cannot be reached, or the thread cannot be made, the
 * thread is stopped and nothing of @p executor and @p on_peer is kept (GListenerI::start()).
 *
 * @param executor Where the ends' handlers run (the shell's session pool)
 * @param on_peer Called for every client that has joined, on the listener's strand
 * @throws gem_exception if the broker cannot be reached or the thread cannot be started
 */
void GMqttListener::start(boost::asio::any_io_executor executor, on_peer_type on_peer) {
    handler_executor_ = std::move(executor);
    on_peer_ = std::move(on_peer);
    joins_ = std::make_unique<GJoins>(strand_);

    std::string why;
    work_guard_.emplace(io_context_.get_executor());
    try {
        budget_ = Gem::Common::Concurrency::threadBudget().reserve(
            name_ + ":io",
            1,
            Gem::Common::Concurrency::ThreadElasticity::Elastic
        );
        threads_.create_thread([this] { io_context_.run(); });
        running_.store(true);
    }
    catch(...) {
        why = "could not start the I/O thread: " +
              Gem::Common::describeException(std::current_exception());
    }
    if(why.empty()) {
        boost::asio::post(strand_, [this] { this->connect_(); });
        const auto limit = limitFor(options_.keepAlive);
        if(joins_->answered.wait_for(limit)) {
            why = joins_->why; // written on the strand before the latch was counted down
        }
        else {
            why = "the broker did not answer within " + std::to_string(limit.count()) + " ms";
        }
    }
    if(why.empty()) {
        return;
    }

    // As if not started: the thread is stopped and joined, and the client's operations complete
    running_.store(true);
    this->joinIo_();
    this->letClientsGo_();
    joins_.reset();
    on_peer_ = nullptr;
    handler_executor_ = boost::asio::any_io_executor{};
    throw Gem::Common::gem_exception(
        Gem::Common::g_error_streamer()
        << "In " << name_ << ": no MQTT broker could be reached at " << options_.host << ":"
        << options_.port << ": " << why << '\n'
    );
}

/******************************************************************************/
/** On the strand: a new MQTT client of the listener (a new number, and a new client id from the
 *  broker) connects and subscribes to P/join, and the joins are read from then on. The first one
 *  tells start() the outcome; a later one, made after the broker was lost, resets the pause and
 *  says once in the log that clients can join again. */
void GMqttListener::connect_() {
    const std::uint64_t generation = ++joins_->generation;
    joins_->client = std::make_unique<client_type>(
        strand_,
        std::monostate{},
        GLinkWatch{[this, generation](std::string what, bool) {
            boost::asio::post(strand_, [this, generation, what = std::move(what)] {
                this->joinsBroken_(generation, what);
            });
        }}
    );
    auto &client = *joins_->client;
    configure(client, options_);
    client.async_run([](error_code) { /* the watch tells what ended it */ });
    client.async_subscribe(
        subscription(options_.topic + "/join"),
        mqtt::subscribe_props{},
        [this,
         generation](error_code ec, std::vector<mqtt::reason_code> codes, mqtt::suback_props) {
            if(generation != joins_->generation) {
                return;
            }
            const std::string why = subscriptionError(ec, codes);
            if(not joins_->decided) {
                joins_->decided = true;
                joins_->why = why;
                if(why.empty()) {
                    this->receiveJoin_();
                }
                joins_->answered.count_down();
                return;
            }
            if(not why.empty()) {
                this->joinsBroken_(generation, "the broker refused the subscription: " + why);
                return;
            }
            joins_->pause = GCLIENTRECONNECTPAUSEMIN;
            if(joins_->down) {
                joins_->down = false;
                glogger << "In " << name_
                        << ": the listener reaches the MQTT broker again: clients can join" << '\n'
                        << GLOGGING;
            }
            this->receiveJoin_();
        }
    );
}

/******************************************************************************/
/** On the strand: the listener's client has lost the broker (GLinkWatch), or the broker refused it
 *  the subscription. The client stops. Before the first subscription start() is told why;
 *  afterwards the log says once that no client can join, and a new client is made after a pause
 *  that doubles from GCLIENTRECONNECTPAUSEMIN to GCLIENTRECONNECTPAUSEMAX, as a client's does
 *  (without its random spread: there is one listener), until the broker answers or the listener
 *  stops. @param generation The number of the client @param what What happened */
void GMqttListener::joinsBroken_(std::uint64_t generation, const std::string &what) {
    if(generation != joins_->generation) {
        return; // a hook of a client given up before
    }
    ++joins_->generation; // what this client reports from now on (its own stop) is ignored
    joins_->client->cancel();
    if(not joins_->decided) {
        joins_->decided = true;
        joins_->why = what;
        joins_->answered.count_down();
        return;
    }
    if(stopped_.load()) {
        return;
    }
    if(not joins_->down) {
        joins_->down = true;
        glogger << "In " << name_ << ": the listener's connection to the MQTT broker has ended ("
                << what << "): no client can join until it has connected again" << '\n'
                << GWARNING;
    }
    joins_->retry.expires_after(joins_->pause);
    joins_->pause = std::min(2 * joins_->pause, GCLIENTRECONNECTPAUSEMAX);
    joins_->retry.async_wait([this](const error_code &ec) {
        if(not ec && not stopped_.load()) {
            this->connect_();
        }
    });
}

/******************************************************************************/
/** Waits for the next join; on the strand. The wait ends with the listener's client, and a wait of
 *  a client that has been replaced reads nothing more. */
void GMqttListener::receiveJoin_() {
    joins_->client->async_receive([this, generation = joins_->generation](
                                      error_code ec,
                                      std::string,
                                      std::string payload,
                                      mqtt::publish_props
                                  ) {
        if(ec || generation != joins_->generation) {
            return; // the client has stopped, or a newer one reads
        }
        this->joined_(std::move(payload));
        this->receiveJoin_();
    });
}

/******************************************************************************/
/**
 * A client has joined: unless the listener has stopped, or the id is not one or has joined before,
 * the server's end of its connection is made on a strand of its own and begun; it is handed to the
 * shell when it is ready (handOver_()).
 *
 * @param id What the join named
 */
void GMqttListener::joined_(std::string id) {
    if(stopped_.load() || not on_peer_) {
        return;
    }
    if(not isId(id)) {
        glogger << "In " << name_ << ": a join on " << options_.topic
                << "/join that names no connection was ignored" << '\n'
                << GWARNING;
        return;
    }
    auto [known, inserted] = ends_.try_emplace(id);
    if(not inserted) {
        return; // this connection has joined before
    }
    auto peer = std::make_shared<GMqttPeer>(
        options_,
        std::move(id),
        GMqttPeer::GSide::SERVER,
        boost::asio::make_strand(io_context_),
        handler_executor_
    );
    known->second = peer;
    peer->begin_([this, weak = std::weak_ptr<GMqttPeer>(peer)](std::string_view why) {
        // On the end's strand, inside its own handler: it is alive
        boost::asio::post(strand_, [this, peer = weak.lock(), why = std::string(why)] {
            this->handOver_(peer, why);
        });
    });
}

/******************************************************************************/
/**
 * On the strand: a server's end is ready, and is handed to the shell -- unless the listener has
 * stopped (the end is dropped; its client hears its will). Since the calls of on_peer run on the
 * strand, stop() returns only after the call under way, and none comes after it.
 *
 * @param peer The end @param why Why it is not ready; empty if it is
 */
void GMqttListener::handOver_(const std::shared_ptr<GMqttPeer> &peer, const std::string &why) {
    if(not peer) {
        return;
    }
    if(not why.empty()) {
        glogger << "In " << name_ << ": a client that joined could not be taken on: " << why << '\n'
                << GWARNING;
        return;
    }
    if(stopped_.load() || not on_peer_) {
        peer->drop_();
        return;
    }
    try {
        on_peer_(peer, next_peer_id_++);
    }
    catch(const std::exception &e) {
        glogger << "In " << name_ << ": a new client could not be taken on: " << e.what() << '\n'
                << GWARNING;
    }
    catch(...) {
        glogger << "In " << name_ << ": a new client could not be taken on" << '\n' << GWARNING;
    }
}

/******************************************************************************/
/**
 * No joins and no new ends from here on (GListenerI::stop()): the listener's client stops on the
 * strand, after the call of on_peer under way, and this call waits for that, at most
 * LISTENER_STOP_BOUND. Not on the I/O thread.
 */
void GMqttListener::stop() {
    if(not running_.load() || stopped_.exchange(true)) {
        return;
    }
    auto done = std::make_shared<Gem::Common::Concurrency::GCompletionLatch>(1);
    boost::asio::post(strand_, [this, done] {
        joins_->client->cancel(); // it has no will: the broker forgets it
        joins_->retry.cancel();
        done->count_down();
    });
    if(not done->wait_for(LISTENER_STOP_BOUND)) {
        glogger << "In " << name_
                << ": the listener, with a new client it takes on, did not stop within "
                << LISTENER_STOP_BOUND.count() << " ms" << '\n'
                << GWARNING;
    }
}

/******************************************************************************/
/**
 * Ends the transport's own work (GPeerI.hpp, rule 7): the I/O thread is stopped and joined, the MQTT
 * clients that still run are stopped and their operations complete (letClientsGo_()), and the I/O
 * context is shut down. The ends that had not ended go without onClosed and without a DISCONNECT
 * (their clients hear their wills). Afterwards nothing here refers to the shell's executor.
 */
void GMqttListener::endWork() {
    stopped_.store(true);
    this->joinIo_();
    this->letClientsGo_();
    io_context_.shutdown();
    joins_.reset();
    ends_.clear();
    on_peer_ = nullptr;
    handler_executor_ = boost::asio::any_io_executor{};
}

/******************************************************************************/
/**
 * When no thread runs the I/O context: the listener's client and every end that has not ended stop
 * their MQTT clients, and the context runs here until their operations have completed, at most
 * LISTENER_STOP_BOUND. No operation of an MQTT client may be destroyed unfinished: Boost.MQTT5 (Boost
 * 1.92) reads memory it has just freed when an operation of a client that connects, or has been
 * stopped, goes with its context (ThreadSanitizer: heap-use-after-free in the nested handlers of
 * its reconnect operation; GCourtierMqttTransportTests, a listener without a broker).
 */
void GMqttListener::letClientsGo_() {
    io_context_.restart();
    if(joins_) {
        if(joins_->client) {
            joins_->client->cancel();
        }
        joins_->retry.cancel();
    }
    for(const auto &known : ends_) {
        if(auto end = known.second.lock()) {
            end->drop_();
        }
    }
    io_context_.run_for(LISTENER_STOP_BOUND);
}

/******************************************************************************/
/** Stops the I/O context, joins its thread and gives back its reservation. */
void GMqttListener::joinIo_() {
    if(not running_.exchange(false)) {
        return;
    }
    work_guard_.reset();
    io_context_.stop();
    threads_.join_all();
    budget_.release();
}

/******************************************************************************/
/** @return How long the shell waits for its clients at the end of a run */
std::chrono::milliseconds GMqttListener::stopGrace() const {
    return stop_grace_;
}

/******************************************************************************/
/** @return The MQTT keep-alive of the server's ends, named to their clients in the HELLO */
std::chrono::milliseconds GMqttListener::keepAlive() const {
    return options_.keepAlive;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/** One attempt: who waits for it, and its end until it has been reported */
struct GMqttConnector::GAttempt {
    done_type done;               ///< Who wants to know; dropped when called or given up
    std::weak_ptr<GMqttPeer> end; ///< The end being made; dropped once reported
    bool cancelled = false;       ///< cancel() was called
};

/******************************************************************************/
/**
 * Prepares the connector; nothing happens before connect().
 *
 * @param options Broker, prefix and keep-alive of the client's ends
 * @throws gem_exception if the topic prefix cannot be used
 */
GMqttConnector::GMqttConnector(GMqttOptions options)
  : options_(checked(std::move(options))) { /* nothing */
}

/******************************************************************************/
/** Gives up an attempt in progress. */
GMqttConnector::~GMqttConnector() {
    this->cancel();
}

/******************************************************************************/
/**
 * Makes a new client's end with a new id; it connects to the broker, subscribes and joins on
 * @p executor, and is reported there when it has joined -- or not, with the reason.
 *
 * @param executor Where the end's MQTT client and its handlers run; runs one handler at a time
 * @param done Called with the end, or with no end and the reason
 */
void GMqttConnector::connect(boost::asio::any_io_executor executor, done_type done) {
    this->cancel(); // an attempt that is still in progress is given up
    auto end = std::make_shared<GMqttPeer>(options_, newId(), GMqttPeer::GSide::CLIENT, executor);
    auto attempt = std::make_shared<GAttempt>(GAttempt{.done = std::move(done), .end = end});
    attempt_ = attempt;
    end->begin_([attempt,
                 where =
                     options_.host + ":" + std::to_string(options_.port)](std::string_view why) {
        if(attempt->cancelled) {
            return;
        }
        done_type call = std::move(attempt->done);
        attempt->done = nullptr;
        std::shared_ptr<GMqttPeer> ready = attempt->end.lock();
        attempt->end.reset(); // it is the caller's now: cancel() leaves it alone
        if(not call) {
            return;
        }
        if(why.empty() && ready) {
            call(std::move(ready), std::string_view{});
        }
        else {
            call(
                nullptr,
                "no connection through the MQTT broker at " + where + ": " + std::string(why)
            );
        }
    });
}

/******************************************************************************/
/** Gives up the attempt in progress, if there is one: its end is dropped, its callback is not
 *  called. */
void GMqttConnector::cancel() {
    if(auto attempt = attempt_.lock()) {
        attempt->cancelled = true;
        attempt->done = nullptr;
        if(auto end = attempt->end.lock()) {
            end->drop_();
        }
    }
    attempt_.reset();
}

/******************************************************************************/
/** @return true: every connect() is an attempt of its own */
bool GMqttConnector::reconnectable() const {
    return true;
}

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
