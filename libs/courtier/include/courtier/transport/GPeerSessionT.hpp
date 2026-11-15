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
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

// Boost headers go here
#include <boost/asio/post.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GLogger.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GCourtierHelperFunctions.hpp" // fkToStr
#include "courtier/GServerSessionLogic.hpp" // the shared server decisions (absorb, serve, refuse)
#include "courtier/GWireCodec.hpp"          // scope-wrapped (de)serialization
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
 * A request for work (PULL, or the pull a RETURN carries) that cannot be met is not answered with
 * NO_WORK: the session is parked with the consumer and answers when it is woken for an item that
 * has become available (see GNetworkedConsumerT::checkoutOrPark()). While it is parked a session
 * only counts the requests it owes an answer to. A RESULT is a returned result that asks for
 * nothing: it is checked in and no answer is owed. The client sees NO_WORK only when a request had
 * to be refused.
 *
 * An item whose answer cannot be encoded (its serialization throws) ends as failed through the
 * consumer (failPayloadItem): it is that item's failure, and the session knows which item it is. The
 * client's other items stay with it, the request is still owed, and the session goes on with the
 * next item. The client's blob state is forgotten (the failed encoding may have counted a blob as
 * sent). A failure that is not the item's -- the checkout itself throws -- is a refusal (below).
 *
 * Messages that cannot be read. A message from the client that cannot be decoded is attributed by
 * its kind, which is read first (the frame is reset to NONE before each decode). A RESULT is not
 * answered (it asked for nothing); a request (PULL, RETURN, UNREADABLE) is answered with NO_WORK,
 * and so is a message whose kind could not be read either, or a frame of a kind a client does not
 * send (WORK, say). In every case the session gives back
 * what it holds: it can no longer tell which items the client still works on. An UNREADABLE from
 * the client says that it could not read the session's last answer: the session forgets which
 * blobs the client holds (they are sent in full again), gives back what it holds, and owes the
 * client the next item.
 *
 * The end of the run. Once the server has been stopped, the session no longer answers: it closes
 * the connection with GPeerClose::RUN_ENDED, also when it still owes answers (the client need not
 * ask again), and sends no NO_WORK. It looks at the stop before it answers owed requests.
 *
 * Threads. Every handler of the session runs on its peer's executor (a strand: one at a time), and
 * a wake-up is posted there. That executor runs nothing but the roles' handlers: the transport
 * moves the bytes on threads of its own (GPeerI.hpp, rule 2), so the session may take as long as
 * it needs to decode a request, check an item out and encode it. send() hands the message over and
 * returns; the session assumes nothing about when, or on which thread, it is written.
 *
 * Ownership. The session is owned by its peer (through the peer's handlers), so it lives exactly as
 * long as its connection; it refers to the peer weakly. It releases what it holds in its
 * destructor, never in its onClosed handler: a peer that is discarded with its executor's context
 * ends without onClosed (rule 5). The destructor forgets the client's blob state and signs the
 * session off (serverSignOn(false)); the hooks themselves (and whatever the shell keeps in them,
 * such as the lease of the client's items) are destroyed with the session.
 *
 * Signing on. The session signs on (serverSignOn(true)) in start(), after its peer's start() has
 * returned, and not before: whoever learns of the session through the sign-on (the shell, which
 * closes every signed-on peer at the end of a run) can then only close a peer that has been
 * started (GPeerI.hpp, rule 2: start() before anything else). A session that is never started
 * never signs on, and does not sign off.
 *
 * @tparam processable_type The work-item type exchanged with the client (must be processable)
 */
template <typename processable_type>
class GPeerSessionT final : public std::enable_shared_from_this<GPeerSessionT<processable_type>> {
public:
    //-------------------------------------------------------------------------
    /** @brief The session's server-side hooks: how it pulls work, returns results, gives back what
     *  it holds, checks for a stopped server, and signs itself on/off. */
    struct SessionHooks {
        /// Checks an item out or parks the session; the argument is how a parked session is woken
        std::move_only_function<GCheckoutResult<processable_type>(GWakeFn)> getPayloadItem;
        /// Checks a returned frame in
        std::move_only_function<void(GCommandContainerT<processable_type> &&)> putPayloadItem;
        /// Ends an item that getPayloadItem handed out as failed (its answer could not be
        /// encoded): its correlation id, and why
        std::move_only_function<void(Gem::Courtier::CORRELATION_ID_TYPE, const std::string &)>
            failPayloadItem;
        /// Puts every item back that this session has handed out and not seen return
        std::move_only_function<void()> abandonItems;
        /// Whether the server has been stopped
        std::move_only_function<bool()> checkServerStopped;
        /// Called with true by start(), after the peer has been started, and with false by the
        /// destructor of a session that signed on
        std::move_only_function<void(bool)> serverSignOn;
    };

    //-------------------------------------------------------------------------
    /**
     * @brief The only allowed constructor for this class. The session signs on in start().
     *
     * @param hooks How the session pulls work, returns results, sees a stopped server and signs on/off
     * @param serialization_mode Which serialization format (binary or JSON) the wire protocol uses
     * @param wire_registry The consumer's blob send-once registry, or nullptr to send every blob in full
     * @param peer_id This connection's id in that registry
     */
    GPeerSessionT(
        SessionHooks hooks,
        Gem::Common::serializationMode serialization_mode,
        Gem::Courtier::GWireBlobRegistry *wire_registry = nullptr,
        Gem::Courtier::GWirePeerId peer_id = 0
    )
      : get_payload_item_(std::move(hooks.getPayloadItem))
      , put_payload_item_(std::move(hooks.putPayloadItem))
      , fail_payload_item_(std::move(hooks.failPayloadItem))
      , abandon_items_(std::move(hooks.abandonItems))
      , check_server_stopped_(std::move(hooks.checkServerStopped))
      , server_sign_on_(std::move(hooks.serverSignOn))
      , serialization_mode_(serialization_mode)
      , wire_registry_(wire_registry)
      , peer_id_(peer_id) {
        // Engage the blob send-once wire form for this session's peer: the server sends a given
        // blob in full only on the first work item to this peer and references it by content id
        // thereafter. Every connection is a peer of its own, so a client that connects again
        // receives its blobs afresh. Disabled (-> self-contained full-payload form) when no
        // registry is supplied.
        wire_ctx_.enabled = (wire_registry_ != nullptr);
        wire_ctx_.peer = peer_id_;
        wire_ctx_.registry = wire_registry_;
        wire_ctx_.mode = serialization_mode_;
    }

    //-------------------------------------------------------------------------
    /** @brief The destructor. Forgets the client's blob state and signs the session off. */
    ~GPeerSessionT() {
        // The shared blob store is left intact for the other peers.
        if(wire_registry_ != nullptr) {
            wire_registry_->forgetPeer(peer_id_);
        }
        if(signed_on_) {
            this->server_sign_on_(false);
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
        peer->start(
            GPeerHandlers{
                .onOpen = {},
                .onMessage =
                    [self](std::string &&message) { self->when_message_(std::move(message)); },
                .onClosed =
                    [self](GPeerEnd end, GPeerClose how, std::string_view detail) {
                        self->when_closed_(end, how, detail);
                    }
            }
        );
        // Only now may anyone who learns of this session close its peer.
        this->server_sign_on_(true);
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
    /**
     * @brief Called for every message from the client: checks a returned result in and answers,
     * or notes that the client is owed a work item and serves what can be served.
     *
     * @param message One complete message from the client
     */
    void when_message_(const std::string &message) {
        auto peer = peer_.lock();
        if(not peer) {
            return;
        }
        auto absorbed = this->absorb_(message);
        if(auto *answer = std::get_if<std::string>(&absorbed)) {
            peer->send(std::move(*answer));
        }
        else if(std::holds_alternative<Gem::Courtier::GPullDue>(absorbed)) {
            ++owed_replies_;
        }
        // GNothingDue: a RESULT, which asks for nothing
        this->serve_(*peer);
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Answers the requests this session owes an answer to, for as long as there are items.
     * When there are none it stays parked until it is woken. Once the server has been stopped the
     * connection is closed instead, with the word that the run has ended; owed requests are then
     * not answered (the client need not ask again).
     *
     * @param peer The connection of this session
     */
    void serve_(GPeerI &peer) {
        if(this->check_server_stopped_()) {
            peer.close(GPeerClose::RUN_ENDED);
            return;
        }

        while(owed_replies_ > 0 && not parked_ && not stopping_) {
            if(auto answer = this->answer_pull_()) {
                --owed_replies_;
                peer.send(std::move(*answer));
            }
        }

        // The stop may have come while the loop ran (the consumer then hands out neither an item
        // nor a promise, see answer_pull_()); a session that was not parked then is not woken
        // for it.
        if(stopping_ || this->check_server_stopped_()) {
            peer.close(GPeerClose::RUN_ENDED);
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief How this session is woken while it is parked. The function is called by whichever
     * thread made work available, so it only posts to the session's executor. It holds the
     * session weakly and no executor: the consumer's list of parked callers must neither keep a
     * session alive nor hold a strand beyond the life of its io context. It reports whether the
     * session was still there, so that a wake-up meant for an item is not spent on a session that
     * has ended.
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
            // take its item in any case (see when_woken_()).
            auto executor = peer->executor();
            boost::asio::post(executor, [self = std::move(self), peer = std::move(peer)] {
                self->when_woken_(*peer);
            });
            return true;
        };
    }

    //-------------------------------------------------------------------------
    /** @brief Runs on the session's executor after a wake-up: an item has become available (or the
     * server stops), so the session asks again. Others may have been faster; then it is parked
     * once more.
     *
     * The wake-up stands for an item that another parked session did not get, so the session
     * takes it even if its connection has ended in the meantime. The answer is then not sent (the
     * peer drops it), the item is among those the session gives back at its end, and that wakes
     * the next one.
     *
     * @param peer The connection of this session */
    void when_woken_(GPeerI &peer) {
        parked_ = false;
        this->serve_(peer);
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Called once when the connection is over. A regular close by either end and the end of
     * the server are no news; anything else is logged.
     *
     * @param end Why the connection ended
     * @param how What the other end said, if it closed in the regular way
     * @param detail The transport's own words about it
     */
    void when_closed_(GPeerEnd end, [[maybe_unused]] GPeerClose how, std::string_view detail) {
        if(end != GPeerEnd::CLOSED_HERE && end != GPeerEnd::CLOSED_THERE &&
           not this->check_server_stopped_()) {
            glogger << "GPeerSessionT<processable_type>: the connection to client " << peer_id_
                    << " has ended: " << detail << '\n'
                    << GLOGGING;
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Reads an incoming message and acts on all of it but the request for an item: a RETURN
     * or a RESULT is checked in, and a RETURN, like a PULL, leaves the client owed a work item; an
     * UNREADABLE does too, after the session has forgotten the client's blobs and given back what
     * it holds. A message that cannot be read or handled is refused by its kind (see the class
     * comment).
     *
     * @param message One complete message from the client
     * @return The answer if the message is answered by it (NO_WORK if it had to be refused);
     * GPullDue if the client is now owed a work item; GNothingDue if it is owed nothing
     */
    Gem::Courtier::GAbsorbResult absorb_(const std::string &message) {
        // A decode that fails leaves the kind it has read (the kind is archived first), or NONE.
        command_container_.reset();
        GFrameKind kind = GFrameKind::NONE;
        try {
            // De-serialize the object (under the wire scope, so an id-referenced blob in a returned
            // result resolves against this server's registry).
            Gem::Courtier::wireDecode(
                message,
                command_container_,
                wire_ctx_.enabled ? &wire_ctx_ : nullptr,
                serialization_mode_
            ); // may throw
            kind = command_container_.kind();

            if(kind == GFrameKind::UNREADABLE) {
                return this->unreadableAnswer_();
            }
            if(kind != GFrameKind::PULL && kind != GFrameKind::RETURN &&
               kind != GFrameKind::RESULT && kind != GFrameKind::BLOB_REQUEST) {
                // A frame a client does not send: it cannot be handled, whatever it was meant to be
                glogger << "GPeerSessionT<processable_type>: client " << peer_id_
                        << " sent a frame of a kind a client does not send: "
                        << Gem::Courtier::fkToStr(kind) << '\n'
                        << GLOGGING;
                return this->refused_(kind);
            }
            return Gem::Courtier::absorbServerRequest(
                command_container_,
                put_payload_item_,
                wire_registry_,
                serialization_mode_
            );
        }
        catch(...) {
            if(kind == GFrameKind::NONE) {
                kind = command_container_.kind(); // what the failed decode had read
            }
            logRefusal_(std::current_exception());
            return this->refused_(kind);
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief The client could not read the last answer of this session: it is sent its blobs in
     * full again, what it held is given back, and it is owed the next item.
     *
     * @return GPullDue
     */
    Gem::Courtier::GAbsorbResult unreadableAnswer_() {
        glogger << "GPeerSessionT<processable_type>: client " << peer_id_
                << " could not read an answer; it gets its blobs in full again and what it held "
                   "is put back"
                << '\n'
                << GLOGGING;
        if(wire_registry_ != nullptr) {
            wire_registry_->forgetPeer(peer_id_);
        }
        abandon_items_();
        return Gem::Courtier::GPullDue{};
    }

    //-------------------------------------------------------------------------
    /**
     * @brief What a message gets that could not be read or handled, by its kind: a RESULT asked
     * for nothing and is not answered; anything else is answered with NO_WORK. The session gives
     * back what it holds in either case.
     *
     * @param kind The kind the message was read as; NONE if not even that could be read
     * @return GNothingDue for a RESULT, the NO_WORK answer otherwise
     */
    Gem::Courtier::GAbsorbResult refused_(GFrameKind kind) {
        try {
            return this->refusal_(kind);
        }
        catch(...) {
            // Not even the refusal could be made (out of memory, say): no exception may leave a
            // session thread, and the client cannot be answered, so the connection ends. The
            // session's lease gives back what it holds when the session goes. (No test: only a
            // failing allocation gets here; the harness has no allocator to make one fail.)
            logRefusal_(std::current_exception());
            if(auto peer = peer_.lock()) {
                peer->close(GPeerClose::ERROR);
            }
            stopping_ = true; // nothing more is served on this connection
            return Gem::Courtier::GNothingDue{};
        }
    }

    /** @brief The body of refused_(), which may throw @param kind See refused_()
     *  @return See refused_() */
    Gem::Courtier::GAbsorbResult refusal_(GFrameKind kind) {
        // Messages keep their boundaries, so the connection is still usable: closing it instead
        // would end the client. The session cannot tell any more which of its items are still
        // being worked on, so it gives back all it holds; a result that arrives for one of them
        // later is still accepted.
        abandon_items_();
        if(kind == GFrameKind::RESULT) {
            return Gem::Courtier::GNothingDue{};
        }
        if(kind == GFrameKind::NONE) {
            glogger
                << "GPeerSessionT<processable_type>: not even the kind of the message of client "
                << peer_id_ << " could be read; it is answered as a request" << '\n'
                << GLOGGING;
        }
        return Gem::Courtier::refusalReply<processable_type>(serialization_mode_);
    }

    //-------------------------------------------------------------------------
    /** @brief Logs why a request was refused @param failure The exception that refused it */
    static void logRefusal_(const std::exception_ptr &failure) {
        glogger << "GPeerSessionT<processable_type>: a request was refused: " << describe_(failure)
                << '\n'
                << GLOGGING;
    }

    /** @brief @param failure An exception @return What it says, for the log and the item */
    static std::string describe_(const std::exception_ptr &failure) {
        try {
            std::rethrow_exception(failure);
        }
        catch(const std::exception &e) {
            return e.what();
        }
        catch(...) {
            return "an exception that is not a std::exception";
        }
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Answers one request: checks an item out and encodes it under this session's wire
     * scope (blob send-once), or parks the session if nothing is pending.
     *
     * @return The answer (WORK; NO_WORK only if the request had to be refused); nothing if the
     * session is parked instead, if the server is stopping, or if the item could not be encoded
     * (it has then ended as failed, and the request is still owed)
     */
    std::optional<std::string> answer_pull_() {
        return this->refusing_([&]() -> std::optional<std::string> {
            auto result = get_payload_item_(this->wake_function_());
            if(result.parked) {
                parked_ = true;
                return std::nullopt;
            }
            if(not result.item) {
                // Neither an item nor a promise: the server is stopping. The request is not
                // answered; the connection is closed with RUN_ENDED instead (serve_()).
                stopping_ = true;
                return std::nullopt;
            }
            const Gem::Courtier::CORRELATION_ID_TYPE id = result.item->getCorrelationId();
            Gem::Courtier::serveWorkItem(command_container_, [&result] {
                return std::move(result.item);
            });
            try {
                return Gem::Courtier::wireEncode(
                    command_container_,
                    wire_ctx_.enabled ? &wire_ctx_ : nullptr,
                    serialization_mode_
                );
            }
            catch(...) {
                this->itemFailed_(id, std::current_exception());
            }
            return std::nullopt;
        });
    }

    //-------------------------------------------------------------------------
    /**
     * @brief An item could not be encoded: it ends as failed through the consumer, and the
     * client's blob state is forgotten (see the class comment).
     *
     * @param id The item's correlation id
     * @param failure Why it could not be encoded
     */
    void itemFailed_(Gem::Courtier::CORRELATION_ID_TYPE id, const std::exception_ptr &failure) {
        const std::string what = "The work item could not be encoded for client " +
                                 std::to_string(peer_id_) + ": " + describe_(failure);
        command_container_.reset();
        if(wire_registry_ != nullptr) {
            wire_registry_->forgetPeer(peer_id_);
        }
        fail_payload_item_(id, what);
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Runs the answer to a request and turns a failure into a refusal (NO_WORK), so that
     * no exception reaches the executor's thread. The item of the answer may have been checked
     * out before the failure, so the session gives back all it holds (refused_()).
     *
     * @tparam StepF A callable () -> std::optional<std::string>
     * @param step The step to run
     * @return What @p step returns, or the NO_WORK answer if it threw
     */
    template <typename StepF>
    std::optional<std::string> refusing_(StepF &&step) {
        try {
            return std::forward<StepF>(step)();
        }
        catch(...) {
            logRefusal_(std::current_exception());
        }
        auto refusal = this->refused_(GFrameKind::PULL);
        if(auto *answer = std::get_if<std::string>(&refusal)) {
            return std::move(*answer);
        }
        return std::nullopt; // not even the refusal could be made; the connection ends
    }

    //-------------------------------------------------------------------------
    // Data

    std::weak_ptr<GPeerI> peer_; ///< The connection (it owns this session, not the reverse)

    std::move_only_function<GCheckoutResult<processable_type>(GWakeFn)> get_payload_item_;
    std::move_only_function<void(GCommandContainerT<processable_type> &&)> put_payload_item_;
    std::move_only_function<void(Gem::Courtier::CORRELATION_ID_TYPE, const std::string &)>
        fail_payload_item_;
    std::move_only_function<void()> abandon_items_;
    std::move_only_function<bool()> check_server_stopped_;
    std::move_only_function<void(bool)> server_sign_on_;

    Gem::Common::serializationMode serialization_mode_ = Gem::Common::serializationMode::GEM_BINARY;

    GCommandContainerT<processable_type> command_container_{
        GFrameKind::NONE
    }; ///< Holds the current frame and its payload (if any)

    std::size_t owed_replies_ = 0; ///< Requests of the client that have no answer yet
    bool parked_ = false; ///< The consumer holds this session's wake function; nothing is served
    bool stopping_ = false; ///< The consumer said it is stopping: nothing is served any more
    bool signed_on_ = false; ///< start() has signed the session on (the destructor signs it off)

    /// The shared (consumer-owned) blob registry and this session's peer id, plus the wire scope
    /// installed around (de)serialisation so a work item's blob is sent to this peer only once
    /// (blob send-once). wire_registry_ is null when the feature is disabled.
    Gem::Courtier::GWireBlobRegistry *wire_registry_ = nullptr;
    Gem::Courtier::GWirePeerId peer_id_ = 0;
    Gem::Courtier::GWireSerializationContext wire_ctx_;

    //-------------------------------------------------------------------------
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
