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
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

// Boost headers go here

// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named (named-member emitter)
#include "common/GExceptions.hpp"   // describeException()
#include "common/GLogger.hpp"       // the warning of processWorkFrame()
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GProcessable.hpp"
#include "courtier/GProcessingOutcome.hpp"
#include "courtier/GWireProtocolT.hpp"  // the library seam (tag + payload type)
#include "weft/GArchivePolymorphic.hpp" // GArchive codecs for the GEM_BINARY / GEM_JSON wire arm

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The HEAD of a frame: what a reader must know before, and without, the payload. It is archived on
 * its own and read first (container_from_string()), so that a payload that cannot be read still
 * leaves its reader knowing what the message was and which item it named.
 *
 *  - every frame: its kind;
 *  - WORK and RESULT: the library's tag and the GProcessingOutcome, which names the item (its
 *    correlation id and lineage id) and, on a RESULT, says how its processing ended;
 *  - HELLO: the server's build (the commit of its Geneva sources), the identity of its program,
 *    the keep-alive interval and the release of Geneva (GEM_VERSION_STRING).
 *
 * A client reads the HELLO of a server of another release, so that it can say that the two do not
 * match. The kind and the fields of a HELLO therefore keep their order and their binary layout
 * from release to release; a field is only ever appended, after the last one, and a reader ignores
 * what follows the fields it knows (the test "The binary layout of a HELLO is fixed").
 */
struct GFrameHead {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /**
     * @brief (De)serializes the head. The kind is read first; a kind that is not a GFrameKind is
     * refused (the message is not a frame), and the rest depends on the kind.
     * @tparam archive_type The GArchive codec type
     * @param ar The archive to read from / write to
     * @param version The (unused) serialization version number
     */
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        archive_named(ar, "kind", kind);
        if constexpr(std::remove_cvref_t<archive_type>::is_loading) {
            if(not isFrameKind(kind)) {
                throw Gem::Common::gem_exception(
                    Gem::Common::g_error_streamer()
                    << "The message is not a frame: its kind " << std::to_underlying(kind)
                    << " is not a GFrameKind" << '\n'
                );
            }
        }
        if(kind == GFrameKind::WORK || kind == GFrameKind::RESULT) {
            archive_named(ar, "tag", tag);
            archive_named(ar, "outcome", outcome);
        }
        else if(kind == GFrameKind::HELLO) {
            archive_named(ar, "build", build);
            archive_named(ar, "program", program);
            archive_named(ar, "keepAlive", keepAlive);
            archive_named(ar, "release", release);
        }
    }
    ///////////////////////////////////////////////////////////////////////

    GFrameKind kind{GFrameKind::NONE}; ///< What the frame does to the transport (courtier's own)
    std::uint32_t tag{0};              ///< What it means to the using library (relayed only)
    GProcessingOutcome outcome;        ///< The item it names, and how it ended (WORK, RESULT)
    std::string build;                 ///< HELLO: the server's build (its Geneva commit)
    std::string program;               ///< HELLO: the identity of the server's program ("": none)
    std::uint64_t keepAlive{0};        ///< HELLO: the keep-alive interval in ms (0: none)
    std::string release;               ///< HELLO: the server's Geneva release ("": not named)
};

/******************************************************************************************/
////////////////////////////////////////////////////////////////////////////////////////////
/******************************************************************************************/
/**
 * One message of the networked consumer/worker session protocol: its head (GFrameHead) and its
 * payload.
 *
 * The class is deliberately two-layered, and the layers belong to different owners:
 *
 *  - **Courtier's layer** is the head: the frame kind, the GProcessingOutcome (which names the item
 *    of a WORK or a RESULT) and the build a HELLO announces. Courtier acts on these: dispatch,
 *    check-in, reconciliation.
 *  - **The using library's layer** is an opaque numeric @c tag and, for a RESULT that does not ship
 *    the work item, a library-defined payload (GWireProtocolT<processable_type>::payload_type).
 *    Courtier stores and relays both without ever interpreting them.
 *
 * The payload is a variant with one alternative per frame purpose -- nothing, a work item, or the
 * library's own payload type -- so each frame carries exactly what it means. Courtier knows
 * exactly ONE of the alternatives from the inside: the work item, which it must be able to
 * process() and route. Everything else it archives blind.
 *
 * @tparam processable_type The type of the processable work item carried as payload
 */
template <typename processable_type>
class GCommandContainerT {
    // The payload must be a processable (status / process() lifecycle).
    static_assert(
        std::is_base_of_v<Gem::Courtier::GProcessable, processable_type>,
        "processable_type must derive from Gem::Courtier::GProcessable"
    );

public:
    /** @brief The library-defined payload alternative, supplied through the GWireProtocolT seam. */
    using library_payload_type = wire_payload_t<processable_type>;

    /** @brief The payload of a frame: one alternative per frame purpose. */
    using payload_variant = std::variant<
        std::monostate,                    // HELLO / PULL, and a RESULT that carries no result
        std::unique_ptr<processable_type>, // WORK, and a RESULT that ships the whole item
        library_payload_type>;             // a RESULT carrying the library's data

    //-------------------------------------------------------------------------
    /** @brief Initialization with a frame kind only @param kind The frame kind */
    explicit GCommandContainerT(GFrameKind kind) {
        head_.kind = kind;
    }

    //-------------------------------------------------------------------------
    /** @brief Initialization with a frame kind and a work item
     *  @param kind The frame kind
     *  @param payload_ptr The work item transported by this frame (sole ownership is taken by move) */
    GCommandContainerT(GFrameKind kind, std::unique_ptr<processable_type> payload_ptr)
      : payload_(std::move(payload_ptr)) {
        head_.kind = kind;
    }

    //-------------------------------------------------------------------------
    // Defaulted constructors, destructor and move assigment operator

    GCommandContainerT() = default;
    GCommandContainerT(GCommandContainerT &&cp) noexcept = default;
    ~GCommandContainerT() = default;

    GCommandContainerT &operator=(GCommandContainerT &&cp) noexcept = default;

    //-------------------------------------------------------------------------
    // Deleted copy-constructors and assignment operator -- the class is non-copyable

    GCommandContainerT(const GCommandContainerT &) = delete;
    GCommandContainerT &operator=(const GCommandContainerT &) = delete;

    //-------------------------------------------------------------------------
    /** @brief Resets the frame to a new kind, dropping head and payload, so a reused container
     *  never carries stale data into the next message.
     *  @param kind The new frame kind (defaults to NONE, i.e. cleared)
     *  @return A reference to this object */
    const GCommandContainerT &reset(GFrameKind kind = GFrameKind::NONE) {
        head_ = GFrameHead{};
        head_.kind = kind;
        payload_ = payload_variant{};
        return *this;
    }

    //-------------------------------------------------------------------------
    /** @brief Resets the frame to a new kind carrying a work item
     *  @param kind The new frame kind
     *  @param payload_ptr The work item to carry (sole ownership is taken by move)
     *  @return A reference to this object */
    const GCommandContainerT &
    reset(GFrameKind kind, std::unique_ptr<processable_type> payload_ptr) {
        reset(kind);
        payload_ = std::move(payload_ptr);
        return *this;
    }

    //-------------------------------------------------------------------------
    // The head -- courtier's own

    /** @brief Sets the frame kind. @param kind The new frame kind */
    void setKind(GFrameKind kind) noexcept {
        head_.kind = kind;
    }
    /** @brief @return The kind of this frame */
    [[nodiscard]] GFrameKind kind() const noexcept {
        return head_.kind;
    }

    /** @brief Sets the outcome of a WORK or a RESULT (it names the item).
     *  @param outcome The item's lifecycle report */
    void setOutcome(GProcessingOutcome outcome) {
        head_.outcome = std::move(outcome);
    }
    /** @brief @return The outcome of a WORK or a RESULT (default-constructed otherwise) */
    [[nodiscard]] const GProcessingOutcome &outcome() const noexcept {
        return head_.outcome;
    }

    /** @brief Makes the frame a HELLO with a build, a program identity, a keep-alive interval and
     *  a release
     *  @param build The server's build (the commit of its Geneva sources)
     *  @param program The identity of the server's program (empty: none)
     *  @param keep_alive The keep-alive interval of the connection (0: none)
     *  @param release The server's Geneva release (GEM_VERSION_STRING) */
    void setHello(
        std::string build,
        std::string program,
        std::chrono::milliseconds keep_alive,
        std::string release
    ) {
        reset(GFrameKind::HELLO);
        head_.build = std::move(build);
        head_.program = std::move(program);
        head_.release = std::move(release);
        head_.keepAlive =
            keep_alive.count() > 0 ? static_cast<std::uint64_t>(keep_alive.count()) : 0;
    }
    /** @brief @return The build a HELLO announces */
    [[nodiscard]] const std::string &build() const noexcept {
        return head_.build;
    }
    /** @brief @return The identity of the program a HELLO announces (empty: none) */
    [[nodiscard]] const std::string &program() const noexcept {
        return head_.program;
    }
    /** @brief @return The Geneva release a HELLO names (empty: none) */
    [[nodiscard]] const std::string &release() const noexcept {
        return head_.release;
    }
    /** @brief @return The keep-alive interval a HELLO names (0: none) */
    [[nodiscard]] std::chrono::milliseconds keepAlive() const noexcept {
        return std::chrono::milliseconds(
            static_cast<std::chrono::milliseconds::rep>(head_.keepAlive)
        );
    }

    //-------------------------------------------------------------------------
    // The library layer -- courtier stores and relays, never interprets

    /** @brief Sets the library-defined tag of this frame. @param tag The library's own message tag */
    void setTag(std::uint32_t tag) noexcept {
        head_.tag = tag;
    }
    /** @brief @return The library-defined tag of this frame (0 on courtier's own control frames) */
    [[nodiscard]] std::uint32_t tag() const noexcept {
        return head_.tag;
    }

    /** @brief Stores a library-defined payload in this frame. @param payload The library's payload */
    void setLibraryPayload(library_payload_type payload) {
        payload_ = std::move(payload);
    }
    /** @brief @return A pointer to the carried library payload, or nullptr if the frame carries
     *  something else */
    [[nodiscard]] const library_payload_type *libraryPayload() const noexcept {
        return std::get_if<library_payload_type>(&payload_);
    }

    /** @brief @return Whether the payload carries nothing (a frame of the head only) */
    [[nodiscard]] bool payloadEmpty() const noexcept {
        return std::holds_alternative<std::monostate>(payload_);
    }

    //-------------------------------------------------------------------------
    // The work-item alternative -- the one payload courtier knows from the inside

    /** @brief Stores a work item in this frame. @param payload_ptr The work item (ownership taken by move) */
    void setItem(std::unique_ptr<processable_type> payload_ptr) {
        payload_ = std::move(payload_ptr);
    }

    /** @brief Retrieves the carried work item (a non-destructive borrow; releaseItem() takes it).
     *  @return A pointer to the carried work item, or nullptr if this frame carries none */
    [[nodiscard]] const processable_type *item() const noexcept {
        const auto *held = std::get_if<std::unique_ptr<processable_type>>(&payload_);
        return held != nullptr ? held->get() : nullptr;
    }

    /** @brief Extracts (moves out) the carried work item; the payload is empty afterwards.
     *  @return The work item (empty if the frame carried none) */
    std::unique_ptr<processable_type> releaseItem() {
        auto *held = std::get_if<std::unique_ptr<processable_type>>(&payload_);
        if(held == nullptr) {
            return {};
        }
        auto out = std::move(*held);
        payload_ = payload_variant{};
        return out;
    }

    //-------------------------------------------------------------------------
    /**
	  * @brief Processing of the carried work item: through @p processor if one is given and the
	  * item is due, otherwise through the item's process().
	  *
	  * A failure of the user's processing code is recorded on the item by
	  * GProcessingContainerT::process(), which rethrows a typed g_processing_exception; it is let
	  * through to the caller (processWorkFrame()), which returns the item flagged. An item that is
	  * not due is handed to its own process() also when a processor is given: the processing
	  * lifecycle refuses it there, and no processor is spent on it.
	  *
	  * @param processor Processes a due item in place of its process(); nullptr: process()
	  * @throws gem_exception if the frame holds no work item
	  * @throws g_processing_exception (from the item) if processing flagged or threw an error
	  */
    void process(GItemProcessorT<processable_type> *processor = nullptr) {
        auto *held = std::get_if<std::unique_ptr<processable_type>>(&payload_);
        if(held == nullptr || not *held) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer()
                << "Tried to process a work item while the frame carries none" << '\n'
            );
        }
        if(processor != nullptr && (*held)->is_due_for_processing()) {
            (*processor)(**held);
        }
        else {
            (*held)->process();
        }
    }

    //-------------------------------------------------------------------------
    /** @brief Archives the head @param mode The serialization format @return The bytes */
    [[nodiscard]] std::string headToString(Gem::Common::serializationMode mode) const {
        return toString_(head_, "head", mode);
    }
    /** @brief Archives the payload @param mode The serialization format @return The bytes */
    [[nodiscard]] std::string payloadToString(Gem::Common::serializationMode mode) const {
        return toString_(payload_, "payload", mode);
    }
    /** @brief Reads the head; the payload is left as it is
     *  @param bytes The archived head @param mode The serialization format */
    void headFromString(std::string_view bytes, Gem::Common::serializationMode mode) {
        fromString_(bytes, "head", head_, mode);
    }
    /** @brief Reads the payload @param bytes The archived payload
     *  @param mode The serialization format */
    void payloadFromString(std::string_view bytes, Gem::Common::serializationMode mode) {
        fromString_(bytes, "payload", payload_, mode);
    }

private:
    //-------------------------------------------------------------------------
    /** @brief Archives @p value under @p name @param value What is archived
     *  @param name Its name in the archive @param mode The format @return The bytes */
    template <typename value_type>
    static std::string
    toString_(const value_type &value, const char *name, Gem::Common::serializationMode mode) {
        if(mode == Gem::Common::serializationMode::GEM_JSON) {
            Gem::Weft::GJsonOArchive oa;
            oa &Gem::Weft::make_nvp(name, value);
            return oa.str();
        }
        // GArchive flat-binary codec. The blob send-once interning lives inside the work item's
        // own save() (an ambient GWireSerializationScope, if any, is honoured there).
        Gem::Weft::GBinaryOArchive oa;
        oa &Gem::Weft::make_nvp(name, value);
        return oa.str();
    }

    /** @brief Reads @p value from @p bytes @param bytes The archive @param name Its name in it
     *  @param value Where it is read to @param mode The format */
    template <typename value_type>
    static void fromString_(
        std::string_view bytes,
        const char *name,
        value_type &value,
        Gem::Common::serializationMode mode
    ) {
        if(mode == Gem::Common::serializationMode::GEM_JSON) {
            Gem::Weft::GJsonIArchive ia(bytes);
            ia &Gem::Weft::make_nvp(name, value);
            return;
        }
        Gem::Weft::GBinaryIArchive ia(bytes);
        ia &Gem::Weft::make_nvp(name, value);
    }

    //-------------------------------------------------------------------------
    // Data

    GFrameHead head_;         ///< What a reader needs before the payload (courtier's own)
    payload_variant payload_; ///< What the frame carries (one alternative per purpose)
};

/******************************************************************************/
/**
 * @brief Worker side: turns a frame whose work item has just been processed into the RESULT the
 * using library wants sent back.
 *
 * Courtier contributes the parts it owns -- the RESULT kind and the GProcessingOutcome snapshot that
 * carries status, errors, timings and the ids of the item. The library contributes the rest through
 * GWireProtocolT::buildReturn(): the tag, and either "ship the whole item" or a payload of its own.
 *
 * @tparam processable_type The payload type of the command container
 * @param container The frame holding the just-processed work item; rewritten in place into a RESULT
 * @return The processed work item if the RESULT does not ship it (a library payload replaces it),
 *         for a caller that keeps it; empty if the frame holds it
 */
template <typename processable_type>
std::unique_ptr<processable_type> makeReturnFrame(GCommandContainerT<processable_type> &container) {
    auto item_ptr = container.releaseItem();
    GProcessingOutcome outcome = container.outcome();
    std::uint32_t tag = 0;
    typename GCommandContainerT<processable_type>::library_payload_type payload;
    bool ships_item = true;

    if(item_ptr) {
        outcome = item_ptr->processingOutcome();
        ships_item = GWireProtocolT<processable_type>::buildReturn(*item_ptr, tag, payload);
    }

    container.reset(GFrameKind::RESULT);
    container.setTag(tag);
    container.setOutcome(std::move(outcome));
    if(ships_item) {
        container.setItem(std::move(item_ptr));
        return nullptr;
    }
    container.setLibraryPayload(std::move(payload));
    return item_ptr;
}

/******************************************************************************/
/**
 * @brief Worker side: makes @p frame the RESULT of an item that failed in a way that left nothing to
 * return -- its payload could not be read, its processing or the library's return seam threw, its
 * result could not be encoded: the outcome alone, ERROR_FLAGGED with the reason. The outcome names
 * the item (the ids from the head of its WORK).
 *
 * @tparam processable_type The payload type of the command container
 * @param frame Becomes the RESULT
 * @param outcome The item's outcome as known (from the head of its WORK, or after processing)
 * @param reason Why it failed
 */
template <typename processable_type>
void makeFailedResult(
    GCommandContainerT<processable_type> &frame,
    GProcessingOutcome outcome,
    std::string_view reason
) {
    outcome.status = processingStatus::ERROR_FLAGGED;
    outcome.error_descriptions += reason;
    const std::uint32_t tag = frame.tag();
    frame.reset(GFrameKind::RESULT);
    frame.setTag(tag);
    frame.setOutcome(std::move(outcome));
}

/******************************************************************************/
/**
 * @brief Worker side: processes the work item a WORK frame carries, without letting a failure of
 * the item's processing escape.
 *
 * A failure of the user's processing code surfaces as a g_processing_exception, with the work item
 * already flagged (EXCEPTION_CAUGHT) and its error text stored. Any other exception out of the
 * item's processing -- e.g. the gem_exception the processing lifecycle throws for an item that
 * is not due -- leaves the item unflagged; it is flagged here (ERROR_FLAGGED, the text appended to
 * its error descriptions, unless the item already reports an error). Neither ends the worker: the
 * flagged item is returned like any other result, and the server's submission policy decides what
 * happens to the slot. The failure ends the item's task here, so it is reported here (GERROR: the
 * worker's log and its GEM-EXCEPTION.log), once.
 *
 * A due item is processed by @p processor if one is given (a client's item processor, see
 * GBaseClientT::setItemProcessor()), otherwise by its own process(). Any exception out of the
 * item's processing or out of the processor, of whatever type, leaves the item flagged in the same
 * way.
 *
 * @tparam processable_type The payload type of the command container
 * @param container The frame holding the work item to process
 * @param worker_name The worker's name, used in the report
 * @param processor Processes a due item in place of its process(); nullptr: the item's process()
 * @throws gem_exception if the frame holds no work item
 */
template <typename processable_type>
void processWorkFrame(
    GCommandContainerT<processable_type> &container,
    std::string_view worker_name,
    // not deduced: a caller may pass nullptr, from which no type could be deduced
    std::type_identity_t<GItemProcessorT<processable_type>> *processor = nullptr
) {
    // Flags the item (unless it reports an error already) and puts it back into the frame.
    const auto return_flagged = [&](std::string_view reason) {
        auto item = container.releaseItem();
        if(not item) {
            return false; // no item to return: a protocol error, not a failed evaluation
        }
        if(not item->has_errors()) {
            auto outcome = item->processingOutcome();
            outcome.status = processingStatus::ERROR_FLAGGED;
            outcome.error_descriptions += "The work item could not be processed: ";
            outcome.error_descriptions += reason;
            item->applyProcessingOutcome(outcome);
        }
        container.setItem(std::move(item));
        glogger << "In " << worker_name << ":" << '\n'
                << "The work item could not be processed:" << '\n'
                << reason << '\n'
                << "It is returned to the server flagged; the worker keeps running." << '\n'
                << GERROR;
        return true;
    };

    try {
        container.process(processor);
    }
    catch(const g_processing_exception &e) {
        glogger << "In " << worker_name << ":" << '\n'
                << "The work item flagged a processing exception:" << '\n'
                << e.what() << '\n'
                << "It is returned to the server flagged; the worker keeps running." << '\n'
                << GERROR;
    }
    catch(const std::exception &e) {
        if(not return_flagged(e.what())) {
            throw;
        }
    }
    catch(...) {
        if(not return_flagged("an exception that is not derived from std::exception")) {
            throw;
        }
    }
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Reads the head of a message (see GFrameHead): the container is reset and gets the head;
 * its payload is left empty.
 *
 * A message is the length of its head (four bytes, least significant first), the archived head,
 * and the archived payload.
 *
 * @tparam processable_type The payload type of the command container
 * @param descr The message
 * @param container The frame to fill (reset first)
 * @param serMode The serialization format the message was produced with
 * @return The part of @p descr that holds the payload (a view into it)
 * @throws gem_exception if the head cannot be read
 */
template <typename processable_type>
std::string_view frameHeadFromString(
    std::string_view descr,
    GCommandContainerT<processable_type> &container,
    Gem::Common::serializationMode serMode
) {
    container.reset();
    try {
        if(descr.size() < 4) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer()
                << "the message is shorter than the length of a head" << '\n'
            );
        }
        std::size_t head_length = 0;
        for(std::size_t i = 0; i < 4; ++i) {
            head_length |= static_cast<std::size_t>(static_cast<unsigned char>(descr[i]))
                        << (8 * i);
        }
        if(head_length > descr.size() - 4) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer() << "the head is longer than the message" << '\n'
            );
        }
        container.headFromString(descr.substr(4, head_length), serMode);
        return descr.substr(4 + head_length);
    }
    catch(...) {
        container.reset();
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "The head of a message cannot be read: "
            << Gem::Common::describeException(std::current_exception()) << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Reads the payload of a message whose head has been read (frameHeadFromString()).
 *
 * @tparam processable_type The payload type of the command container
 * @param payload The payload part of the message
 * @param container The frame whose head has been read
 * @param serMode The serialization format the message was produced with
 * @throws gem_exception if the payload cannot be read
 */
template <typename processable_type>
void framePayloadFromString(
    std::string_view payload,
    GCommandContainerT<processable_type> &container,
    Gem::Common::serializationMode serMode
) {
    try {
        container.payloadFromString(payload, serMode);
    }
    catch(...) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "The payload of a " << Gem::Common::enumName(container.kind()) << " cannot be read: "
            << Gem::Common::describeException(std::current_exception()) << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Conversion of a GCommandContainerT to a message: the length of the head, the head, the
 * payload (see frameHeadFromString()).
 *
 * @tparam processable_type The payload type of the command container
 * @param container The command container to be serialized
 * @param serMode The serialization format to use (GArchive binary or JSON)
 * @return The message
 * @throws gem_exception if serialization fails
 */
template <typename processable_type>
std::string container_to_string(
    const GCommandContainerT<processable_type> &container,
    Gem::Common::serializationMode serMode
) {
    try {
        const std::string head = container.headToString(serMode);
        std::string payload = container.payloadToString(serMode);
        std::string message;
        message.reserve(4 + head.size() + payload.size());
        for(std::size_t i = 0; i < 4; ++i) {
            message.push_back(static_cast<char>((head.size() >> (8 * i)) & 0xFFU));
        }
        message += head;
        message += payload;
        return message;
    }
    catch(...) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << Gem::Common::describeException(std::current_exception()) << '\n'
            << "with serializationMode == " << Gem::Common::enumName(serMode) << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Loading of a GCommandContainerT from a message: its head, then its payload.
 *
 * @tparam processable_type The payload type of the command container
 * @param descr The message to load from
 * @param container The command container to be filled (reset before loading)
 * @param serMode The serialization format the message was produced with
 * @throws gem_exception if the head or the payload cannot be read
 */
template <typename processable_type>
void container_from_string(
    std::string_view descr,
    GCommandContainerT<processable_type> &container,
    Gem::Common::serializationMode serMode
) {
    framePayloadFromString(frameHeadFromString(descr, container, serMode), container, serMode);
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Courtier */

/******************************************************************************/
