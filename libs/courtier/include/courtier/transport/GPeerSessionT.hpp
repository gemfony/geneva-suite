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
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Boost headers go here
#include <boost/asio/post.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GCourtierHelperFunctions.hpp" // fkToStr, describeException()
#include "courtier/GServerSessionLogic.hpp"      // GWakeFn, GCheckoutResult
#include "courtier/GWireCodec.hpp"               // scope-wrapped (de)serialization, HELLO
#include "courtier/GWireProtocolT.hpp"           // the WORK tag
#include "courtier/GWireSerializationContext.hpp"
#include "courtier/transport/GPeerI.hpp" // the connection

namespace Gem::Courtier::Consumers {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The server's side of the conversation with one client, over any transport (GPeerI). The server's
 * shell starts one session for every peer its listener makes; the session lasts as long as the
 * connection.
 *
 * The protocol (GFrameKind). The session greets its client with HELLO when the connection opens;
 * it names the keep-alive interval of the server's ends, which the client takes.
 * It answers every PULL with one WORK, in order; a PULL that cannot be met at once is counted, the
 * session is parked with the consumer and answers when it is woken for an item that has become
 * available (see GNetworkedConsumerT::checkoutOrPark()). A RESULT is checked in by the ids in its
 * head and asks for nothing.
 *
 * Messages that cannot be read. The head of a message is read first and by itself. A head that
 * cannot be read, or that names a kind a client does not send, ends the connection
 * (GPeerClose::ERROR); the lease gives back what the client held. A RESULT whose payload cannot be
 * read fails its item (failPayloadItem), which the item's retry budget and the submission policy
 * then treat like any failure. A RESULT that carries no result (the client could not read its item,
 * or could not return it) and a payload that cannot be read both make the session forget the
 * client's blob state: its blobs are sent in full again.
 *
 * An item whose answer cannot be encoded ends as failed through the consumer (failPayloadItem): it
 * is that item's failure. The request is still owed, and the client's blob state is forgotten.
 *
 * The end of the run. Once the server has been stopped, the session no longer answers: it closes
 * the connection with GPeerClose::RUN_ENDED, also when it still owes answers. It looks at the stop
 * before it answers owed requests.
 *
 * Threads. Every handler of the session runs on its peer's executor (a strand: one at a time), and
 * a wake-up is posted there. That executor runs nothing but the roles' handlers: the transport
 * moves the bytes on threads of its own (GPeerI.hpp, rule 2), so the session may take as long as
 * it needs to decode a message, check an item out and encode it.
 *
 * Ownership. The session is owned by its peer (through the peer's handlers), so it lives exactly as
 * long as its connection; it refers to the peer weakly. It releases what it holds in its
 * destructor, never in its onClosed handler: a peer that is discarded with its executor's context
 * ends without onClosed (rule 5). The destructor forgets the client's blob state and signs the
 * session off; the hooks (and the lease of the client's items the shell keeps in them) go with it.
 *
 * Signing on. The session signs on (serverSignOn(true)) in start(), after its peer's start() has
 * returned: whoever learns of the session through the sign-on can then only close a peer that has
 * been started (GPeerI.hpp, rule 2).
 *
 * @tparam processable_type The work-item type exchanged with the client (must be processable)
 */
template <typename processable_type>
class GPeerSessionT final : public std::enable_shared_from_this<GPeerSessionT<processable_type>> {
public:
    //-------------------------------------------------------------------------
    /** @brief The session's server-side hooks: how it pulls work, returns results, fails an item,
     *  checks for a stopped server, and signs itself on/off. */
    struct SessionHooks {
        /// Checks an item out or parks the session; the argument is how a parked session is woken
        std::move_only_function<GCheckoutResult<processable_type>(GWakeFn)> getPayloadItem;
        /// Checks a returned frame in
        std::move_only_function<void(GCommandContainerT<processable_type> &&)> putPayloadItem;
        /// Ends an item that getPayloadItem handed out as failed (its answer could not be encoded,
        /// or its RESULT could not be read): its correlation id, and why
        std::move_only_function<void(Gem::Courtier::CORRELATION_ID_TYPE, const std::string &)>
            failPayloadItem;
        /// Whether the server has been stopped
        std::move_only_function<bool()> checkServerStopped;
        /// Called with true by start(), after the peer has been started, and with false by the
        /// destructor of a session that signed on
        std::move_only_function<void(bool)> serverSignOn;
        /// Called once, when the first message of the client has been read (may be empty): the
        /// shell counts the clients that are connected, until the session signs off (on the
        /// session's executor)
        std::move_only_function<void()> clientSpoke;
    };

    //-------------------------------------------------------------------------
    /**
     * @brief The only allowed constructor for this class. The session signs on in start().
     *
     * @param hooks How the session pulls work, returns results, sees a stopped server and signs on/off
     * @param serialization_mode Which serialization format (binary or JSON) the wire protocol uses
     * @param wire_registry The consumer's blob send-once registry, or nullptr to send every blob in full
     * @param peer_id This connection's id in that registry
     * @param keep_alive The keep-alive interval of the server's ends (GListenerI::keepAlive()),
     *        named in the HELLO
     */
    GPeerSessionT(
        SessionHooks hooks,
        Gem::Common::serializationMode serialization_mode,
        Gem::Courtier::GWireBlobRegistry *wire_registry = nullptr,
        Gem::Courtier::GWirePeerId peer_id = 0,
        std::chrono::milliseconds keep_alive = std::chrono::milliseconds::zero()
    )
      : hooks_(std::move(hooks))
      , serialization_mode_(serialization_mode)
      , wire_registry_(wire_registry)
      , peer_id_(peer_id)
      , keep_alive_(keep_alive) {
        // Blob send-once for this session's peer: a blob is sent in full only on the first work
        // item to this peer and referenced by content id thereafter. Every connection is a peer of
        // its own, so a client that connects again receives its blobs afresh. Disabled (every
        // blob in full) when no registry is supplied.
        wire_ctx_.enabled = (wire_registry_ != nullptr);
        wire_ctx_.peer = peer_id_;
        wire_ctx_.registry = wire_registry_;
        wire_ctx_.mode = serialization_mode_;
    }

    //-------------------------------------------------------------------------
    /** @brief The destructor. Forgets the client's blob state and signs the session off. */
    ~GPeerSessionT() {
        this->forgetBlobs_(); // the shared blob store is left intact for the other peers
        if(signed_on_) {
            hooks_.serverSignOn(false);
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Takes the peer and starts it, then signs the session on (see the class comment).
     * Returns at once; all work proceeds on the peer's executor. From here on the peer owns this
     * session through its handlers.
     *
     * @param peer The connection to the client; not started yet
     */
    void start(const std::shared_ptr<GPeerI> &peer) {
        auto self = this->shared_from_this();
        peer_ = peer;
        max_message_bytes_ = peer->maxMessageBytes();
        peer->start(
            GPeerHandlers{
                .onOpen = [self] { self->when_open_(); },
                .onMessage =
                    [self](std::string &&message) { self->when_message_(std::move(message)); },
                .onClosed =
                    [self](GPeerEnd end, GPeerClose how, std::string_view detail) {
                        self->when_closed_(end, how, detail);
                    }
            }
        );
        // Only now may anyone who learns of this session close its peer.
        hooks_.serverSignOn(true);
        signed_on_ = true;
    }

    //-------------------------------------------------------------------------
    // Deleted functions
    GPeerSessionT() = delete;
    GPeerSessionT(const GPeerSessionT<processable_type> &) = delete;
    GPeerSessionT(GPeerSessionT<processable_type> &&) = delete;
    GPeerSessionT<processable_type> &operator=(const GPeerSessionT<processable_type> &) = delete;
    GPeerSessionT<processable_type> &operator=(GPeerSessionT<processable_type> &&) = delete;

private:
    //-------------------------------------------------------------------------
    /** @brief The connection is open: the client is greeted with HELLO first. */
    void when_open_() {
        if(auto peer = peer_.lock()) {
            peer->send(makeMessage(
                Gem::Courtier::buildHello<processable_type>(serialization_mode_, keep_alive_)
            ));
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Called for every message from the client (see the class comment). A message that
     * arrives after the session has closed its connection (the client sent it before it knew of
     * the close; GPeerI.hpp, rule 6) is still read -- a result in it is checked in -- but nothing
     * is answered or served any more.
     *
     * @param message One complete message from the client
     */
    void when_message_(const std::string &message) {
        auto peer = peer_.lock();
        if(not peer) {
            return;
        }
        if(not spoke_) {
            spoke_ = true;
            if(hooks_.clientSpoke) {
                hooks_.clientSpoke();
            }
        }
        command_container_.reset();
        std::string_view payload;
        try {
            payload = Gem::Courtier::frameHeadFromString(
                std::string_view{message},
                command_container_,
                serialization_mode_
            );
        }
        catch(...) {
            this->protocolError_(*peer, Gem::Courtier::describeException(std::current_exception()));
            return;
        }
        switch(command_container_.kind()) {
        case GFrameKind::PULL:
            ++owed_replies_;
            break;
        case GFrameKind::RESULT:
            this->checkIn_(payload);
            break;
        default:
            this->protocolError_(
                *peer,
                "a frame of a kind a client does not send: " +
                    Gem::Courtier::fkToStr(command_container_.kind())
            );
            return;
        }
        if(not stopping_) {
            this->serve_(*peer);
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Checks a RESULT in: its payload is read under the session's wire scope (a blob
     * referenced by id in it resolves against the server's registry) and the frame goes to the
     * consumer. A payload that cannot be read fails the item the head names (see the class
     * comment).
     *
     * @param payload The payload part of the message
     */
    void checkIn_(std::string_view payload) {
        const Gem::Courtier::CORRELATION_ID_TYPE id = command_container_.outcome().correlation_id;
        try {
            Gem::Courtier::wireDecodePayload(
                payload,
                command_container_,
                wire_ctx_.enabled ? &wire_ctx_ : nullptr,
                serialization_mode_
            );
        }
        catch(...) {
            const std::string what =
                "The result of client " + std::to_string(peer_id_) +
                " could not be read: " + Gem::Courtier::describeException(std::current_exception());
            glogger << "GPeerSessionT<processable_type>: " << what << '\n' << GLOGGING;
            this->forgetBlobs_();
            hooks_.failPayloadItem(id, what);
            return;
        }
        if(command_container_.payloadEmpty()) {
            this->forgetBlobs_(); // the client returned no result; it may lack a blob
        }
        hooks_.putPayloadItem(std::move(command_container_));
    }

    //-------------------------------------------------------------------------
    /** @brief A message that cannot be handled ends the connection (see the class comment)
     *  @param peer The connection @param what Why */
    void protocolError_(GPeerI &peer, const std::string &what) {
        glogger << "GPeerSessionT<processable_type>: client " << peer_id_ << ": " << what << '\n'
                << "The connection is closed." << '\n'
                << GLOGGING;
        if(not stopping_) { // a connection that is closing already keeps what its close said
            peer.close(GPeerClose::ERROR);
            closed_with_error_ = true;
            stopping_ = true; // nothing more is served on this connection
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Answers the requests this session owes an answer to, for as long as there are items.
     * When there are none it stays parked until it is woken. Once the server has been stopped the
     * connection is closed instead, with the word that the run has ended.
     *
     * @param peer The connection of this session
     */
    void serve_(GPeerI &peer) {
        while(owed_replies_ > 0 && not parked_ && not stopping_ &&
              not hooks_.checkServerStopped()) {
            if(auto answer = this->answer_pull_()) {
                --owed_replies_;
                peer.send(makeMessage(std::move(*answer)));
            }
        }
        // The stop may have come while the loop ran (the consumer then hands out neither an item
        // nor a promise, see answer_pull_()); a session that was not parked then is not woken
        // for it.
        if(not closed_with_error_ && (stopping_ || hooks_.checkServerStopped())) {
            peer.close(GPeerClose::RUN_ENDED);
            stopping_ = true;
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief How this session is woken while it is parked. The function is called by whichever
     * thread made work available, so it only posts to the session's executor. It holds the
     * session weakly and no executor: the consumer's list of parked callers must neither keep a
     * session alive nor hold a strand beyond the life of its io context. It reports whether the
     * session was still there.
     *
     * @return The wake function handed to the consumer with every request for an item
     */
    GWakeFn wake_function_() {
        return [weak = this->weak_from_this()] {
            auto self = weak.lock();
            auto peer = self ? self->peer_.lock() : nullptr;
            if(not peer) {
                return false;
            }
            // The handler keeps the connection as well as the session, so that the session can
            // take its item in any case: the wake-up stands for an item that another parked
            // session did not get. If the connection has ended meanwhile, the answer is dropped,
            // the item is among those the lease gives back, and that wakes the next one.
            auto executor = peer->executor();
            boost::asio::post(executor, [self = std::move(self), peer = std::move(peer)] {
                self->parked_ = false;
                self->serve_(*peer);
            });
            return true;
        };
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Called once when the connection is over. A close of either end and the end of the
     * server are no news; anything else is logged.
     *
     * @param end Why the connection ended
     * @param how What the other end said, if it closed in the regular way
     * @param detail The transport's own words about it
     */
    void when_closed_(GPeerEnd end, [[maybe_unused]] GPeerClose how, std::string_view detail) {
        if(end != GPeerEnd::CLOSED_HERE && end != GPeerEnd::CLOSED_THERE &&
           not hooks_.checkServerStopped()) {
            glogger << "GPeerSessionT<processable_type>: the connection to client " << peer_id_
                    << " has ended: " << detail << '\n'
                    << GLOGGING;
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Answers one PULL: checks an item out and encodes it as WORK under this session's wire
     * scope (blob send-once), its ids in the head; or parks the session if nothing is pending.
     *
     * @return The answer; nothing if the session is parked instead, if the server is stopping, or
     * if the item could not be encoded (it has then ended as failed, and the request is still owed)
     */
    std::optional<std::string> answer_pull_() {
        auto result = hooks_.getPayloadItem(this->wake_function_());
        if(result.parked) {
            parked_ = true;
            return std::nullopt;
        }
        if(not result.item) {
            stopping_ = true; // neither an item nor a promise: the server is stopping
            return std::nullopt;
        }
        const Gem::Courtier::CORRELATION_ID_TYPE id = result.item->getCorrelationId();
        command_container_.reset(GFrameKind::WORK);
        command_container_.setTag(Gem::Courtier::GWireProtocolT<processable_type>::workTag());
        command_container_.setOutcome(result.item->processingOutcome());
        command_container_.setItem(std::move(result.item));
        try {
            std::string bytes = Gem::Courtier::wireEncode(
                command_container_,
                wire_ctx_.enabled ? &wire_ctx_ : nullptr,
                serialization_mode_
            );
            // An item the transport cannot carry fails here, and only it: handed to the peer,
            // it would end the connection, and the next client would meet it again.
            if(bytes.size() > max_message_bytes_) {
                throw gem_exception(
                    g_error_streamer() << "the message of " << bytes.size()
                                       << " bytes is longer than the transport can carry ("
                                       << max_message_bytes_ << " bytes)" << '\n'
                );
            }
            return bytes;
        }
        catch(...) {
            const std::string what = "The work item could not be encoded for client " +
                                     std::to_string(peer_id_) + ": " +
                                     Gem::Courtier::describeException(std::current_exception());
            command_container_.reset();
            this->forgetBlobs_(); // the failed encoding may have counted a blob as sent
            hooks_.failPayloadItem(id, what);
        }
        return std::nullopt;
    }

    //-------------------------------------------------------------------------
    /** @brief Forgets which blobs the client holds: they are sent in full again */
    void forgetBlobs_() {
        if(wire_registry_ != nullptr) {
            wire_registry_->forgetPeer(peer_id_);
        }
    }

    //-------------------------------------------------------------------------
    // Data

    std::weak_ptr<GPeerI> peer_; ///< The connection (it owns this session, not the reverse)
    SessionHooks hooks_;         ///< How the session reaches the server

    Gem::Common::serializationMode serialization_mode_ = Gem::Common::serializationMode::GEM_BINARY;

    /// The frame being read or written
    GCommandContainerT<processable_type> command_container_{GFrameKind::NONE};

    std::size_t owed_replies_ = 0; ///< PULLs of the client that have no answer yet
    bool parked_ = false; ///< The consumer holds this session's wake function; nothing is served
    /// The session closes its connection (an error, or the end of the run): nothing is answered or
    /// served any more
    bool stopping_ = false;
    bool signed_on_ = false; ///< start() has signed the session on (the destructor signs it off)
    bool spoke_ = false;     ///< A message of the client has been read (clientSpoke was called)
    bool closed_with_error_ = false; ///< The session has closed its connection with ERROR

    /// The shared (consumer-owned) blob registry and this session's peer id, plus the wire scope
    /// installed around (de)serialisation (blob send-once). wire_registry_ is null when the
    /// feature is disabled.
    Gem::Courtier::GWireBlobRegistry *wire_registry_ = nullptr;
    Gem::Courtier::GWirePeerId peer_id_ = 0;
    std::chrono::milliseconds keep_alive_; ///< Named in the HELLO
    /// The longest message the peer's transport carries (GPeerI::maxMessageBytes()); set by start()
    std::size_t max_message_bytes_ = std::numeric_limits<std::size_t>::max();
    Gem::Courtier::GWireSerializationContext wire_ctx_;
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
