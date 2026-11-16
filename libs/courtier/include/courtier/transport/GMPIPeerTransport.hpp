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
#include <climits>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

// Geneva headers go here
#include "common/concurrency/GThreadBudget.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "common/concurrency/GThreadSafeKeyedStoreT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GWireSerializationContext.hpp" // GWirePeerId
#include "courtier/transport/GIoContext.hpp"
#include "courtier/transport/GMPICallsI.hpp"   // the MPI calls, behind an interface
#include "courtier/transport/GMPIPollCore.hpp" // the poll that receives for every peer
#include "courtier/transport/GPeerI.hpp"       // the interfaces

/******************************************************************************/
/**
 * @file
 * The MPI transport of courtier over the transport-neutral interfaces of GPeerI.hpp: a peer
 * (GMPIPeer) is one end of the connection between the submitter's rank and one client's rank; the
 * listener (GMPIListener) makes a peer for every client rank on the submitter, the connector
 * (GMPIConnector) the peer to the submitter on a client. Every MPI call goes through GMPICallsI and
 * the poll of GMPIPollCore, so this file includes no mpi.h and is compiled into libgemfony-courtier;
 * the binding to MPI (GMPICalls) is part of libgemfony-courtier-mpi, and the tests run the transport
 * on an in-memory stand-in for MPI.
 *
 * **What travels.** One message of the role is one MPI message with the tag TAG_PEER_MESSAGE. Three
 * control messages carry what a socket carries by itself: TAG_PEER_OPEN, the greeting the
 * submitter's listener sends every client rank when it starts (a client's end opens on it: there is
 * no connection to make, and the greeting is what tells a client that its master is there);
 * TAG_PEER_CLOSE, a close with what it says (one byte, GPeerClose); TAG_PEER_CLOSE_ACK, the answer
 * to a close, which echoes it. MPI keeps the messages of one rank to another in the order they were
 * sent, whatever their tags, and the poll hands them to the peer in that order.
 *
 * **The close (GPeerI.hpp, rule 6).** close() sends TAG_PEER_CLOSE after everything handed to send()
 * before it. A peer that receives a close answers it with TAG_PEER_CLOSE_ACK after it has handed its
 * role every message that arrived before the close, and reports GPeerEnd::CLOSED_THERE; the closing
 * peer reports GPeerEnd::CLOSED_HERE when the answer arrives. That answer is the only confirmation:
 * a close of the other end that crosses this end's close is no answer (both report
 * GPeerEnd::CLOSED_HERE_UNCONFIRMED), and neither is the passing of the close grace without one. A
 * peer whose role has called close() still hands it every message that arrives until its own close
 * has been sent; what arrives afterwards is dropped. A peer whose role asked for the close before it
 * was told of the open handles nothing that has arrived until its own close has been sent, so that
 * it can never confirm to the other end what no role received.
 *
 * **Ends.** There is no keep-alive: a rank that dies ends the job (the communicator has
 * MPI_ERRORS_ARE_FATAL), and the items of a rank that hangs come back by the consumer's time lease
 * (GMPIListener::detectsLoss() is false). A failure of the poll ends every peer of the process with
 * GPeerEnd::FAILED. A message longer than the peer's limit (MPI counts in int) is not sent; the peer
 * ends with GPeerEnd::FAILED, reported on its executor, and tells the other end with a close
 * (GPeerClose::ERROR).
 *
 * **Rules a change of this file must keep** (each is relied on elsewhere):
 *  - Every message between two ranks -- the role's, the greeting, a close, an answer, and any kind
 *    added later (a ping, say) -- is started through GMPIPollCore::startSend() from a context
 *    that serializes the sender (an end's strand; the listener's start() before it posts on_peer),
 *    and received only by the poll. That one ordered path is what makes "the close comes after
 *    everything sent before it" and "nothing arrived since" true; a message sent another way could
 *    overtake or fall behind.
 *  - The greeting goes out before anything an end on the submitter sends (start() greets before it
 *    hands the ends to the shell); a client's end treats a message of the role before it as a
 *    protocol error.
 *  - Every way an end ends tells the other rank once (tellOther_()), unless it knows: a client
 *    has no keep-alive and would otherwise wait for ever. A new way to end must go through
 *    finish_() after tellOther_(), or through endOfWork() before the poll's shutdown(). After
 *    shutdown() a send is dropped, so what must arrive is sent and drained before it
 *    (GMPIListener::endWork() does: tell, drainSends(), shutdown()).
 *  - An end's state is touched on its strand, and off it only when no handler runs (abort(),
 *    discard(), endOfWork()).
 *  - The busy count of the poll must balance: an end tells it of changes only (busy_told_), and an
 *    end that ends busy says "not busy" first.
 *  - On a client the poll runs on the client's io thread: anything that blocks that thread (a
 *    decode, an evaluation) stops the poll, and with it the answers the submitter waits for.
 *  - A receive is a blocking MPI_Recv on the one poll thread. On the submitter a long message
 *    from one rank holds up the messages of every other rank for its length; measured, the
 *    submitter's receives took 0.01 to 0.1 % of a run (Geneva's results are small), so the receive
 *    was left blocking.
 */

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/** @brief How the MPI transport's peers, and the poll they run on, are set up. A test may shorten
 *  the times. */
struct GMPIPeerOptions {
    /// Who the transport works for, for the log and the thread budget
    std::string name = "GMPIPeer";
    /// How long a closing peer waits for the other end's answer (GPeerI.hpp, rule 6)
    std::chrono::milliseconds closeGrace = GMPICONSUMERSHUTDOWNGRACE;
    /// The longest message the peer sends; MPI counts in int
    std::size_t maxMessageBytes = static_cast<std::size_t>(INT_MAX);
    /// How the poll paces itself
    GMPIPollTiming poll{};
};

/******************************************************************************/
/**
 * One end of the connection between the submitter's rank and a client's rank (see the file
 * comment). The poll of the process (GMPIPollCore) receives for it and hands it every message from
 * the other rank on its strand; it sends through the poll.
 *
 * When it opens: the submitter's end opens as soon as it is started; a client's end when the
 * greeting of the submitter's listener (TAG_PEER_OPEN) arrives, which may be long after its start
 * (the client waits for its master as long as its own limit says, GPeerClientOptions). What arrives
 * before the end has been started waits for it.
 *
 * Threads: everything but start(), close(), executor() and the busy flag happens on the peer's
 * strand; abort() and discard() when no handler of the peer runs any more. Ownership: the poll holds
 * the peer (as its receiver) until GMPIPollCore::shutdown(), and handlers posted to the strand hold
 * it too; the peer refers to the poll weakly. It is made with std::make_shared.
 */
class GMPIPeer final
  : public GPeerI
  , public GMPIPollReceiverI
  , public std::enable_shared_from_this<GMPIPeer> {
public:
    /** @brief When an end opens (see the class comment) */
    enum class GOpening : std::uint8_t {
        AT_START,   ///< As soon as it is started: the submitter's end
        AT_GREETING ///< When the greeting of the submitter arrives: a client's end
    };

    /** @brief Prepares the end; nothing happens before start() */
    GMPIPeer(
        std::weak_ptr<GMPIPollCore>,
        const boost::asio::any_io_executor &,
        int,
        GOpening,
        const GMPIPeerOptions &
    );

    /** @brief The destructor */
    ~GMPIPeer() override = default;

    GMPIPeer(const GMPIPeer &) = delete;
    GMPIPeer(GMPIPeer &&) = delete;
    GMPIPeer &operator=(const GMPIPeer &) = delete;
    GMPIPeer &operator=(GMPIPeer &&) = delete;

    /***************************************************************************/
    // GPeerI

    /** @brief Installs the handlers and opens on the strand (or waits for the greeting) */
    void start(GPeerHandlers) override;
    /** @brief Sends one message of the role through the poll. On the strand. */
    void send(GMessage) override;
    /** @brief Closes after what was handed to send() before; any thread, idempotent */
    void close(GPeerClose) override;
    /** @brief Ends at once, for teardown; tells the other end if it has not been told */
    void abort() override;
    /** @brief The longest message: GMPIPeerOptions::maxMessageBytes, at most INT_MAX (MPI counts
     *  in int) */
    [[nodiscard]] std::size_t maxMessageBytes() const override;
    /** @brief The strand every handler of this end runs on (also the poll's receiver executor) */
    [[nodiscard]] boost::asio::any_io_executor executor() override;
    /** @brief Tells the poll whether the owner evaluates: while every end it receives for has a
     *  busy owner, it looks less often. On the strand. */
    void setOwnerBusy(bool) override;

    /***************************************************************************/
    // GMPIPollReceiverI

    /** @brief One message from the other rank, on the strand */
    void received(int, std::string &&) override;
    /** @brief The poll lets go: the handlers are dropped without onClosed */
    void discard() override;
    /** @brief The poll has failed: the end reports GPeerEnd::FAILED */
    void pollFailed(const std::string &) override;

    /***************************************************************************/
    /** @brief The other end's rank. Any thread. @return The rank */
    [[nodiscard]] int rank() const noexcept;
    /** @brief What the owner said last through setOwnerBusy(). Any thread. @return Whether busy */
    [[nodiscard]] bool ownerBusy() const noexcept;
    /** @brief Whether the end has ended. Any thread. @return Whether it has */
    [[nodiscard]] bool ended() const noexcept;
    /** @brief The work of the transport ends: an end that has not ended tells the other rank, if
     *  that rank does not know (see there). When no handler of the end runs. */
    void endOfWork(GPeerClose);

private:
    /***************************************************************************/
    /** @brief Where an end is; read and written on the strand only */
    enum class GState : std::uint8_t {
        NEW,     ///< Not started
        WAITING, ///< Started; waits for the greeting
        OPEN,    ///< Open
        CLOSING, ///< Its close has been sent; waits for the answer
        ENDED    ///< Over
    };

    /** @brief One message that waits for the end to handle it */
    struct GArrival {
        int tag = 0;       ///< What it is
        std::string bytes; ///< Its bytes
    };

    /** @brief How an end ended before its start()'s work ran: reported when it runs */
    struct GEndReport {
        GPeerEnd end = GPeerEnd::CLOSED_HERE; ///< Why it ended
        GPeerClose how = GPeerClose::NORMAL;  ///< What the other end's close said
        std::string detail;                   ///< Words about it
    };

    /***************************************************************************/
    void started_(GPeerHandlers);
    void open_();
    [[nodiscard]] bool mayHandle_() const;
    void handleWaiting_();
    void handle_(int, std::string &&);
    void beginClose_(GPeerClose);
    void failSend_(std::size_t);
    void tellOther_(GPeerClose);
    void sendControl_(int, std::string);
    void finish_(GPeerEnd, GPeerClose, std::string_view);

    /***************************************************************************/
    std::weak_ptr<GMPIPollCore> core_; ///< The poll of this process (it holds this end)
    boost::asio::strand<boost::asio::any_io_executor> strand_; ///< Serializes this end
    boost::asio::any_io_executor executor_; ///< The strand, as executor() hands it out
    boost::asio::steady_timer close_timer_; ///< Bounds the wait for the answer to a close
    int rank_;                              ///< The other end
    GOpening opening_;                      ///< When this end opens
    std::chrono::milliseconds close_grace_; ///< How long a close waits for its answer
    std::size_t max_message_bytes_;         ///< The longest message sent
    std::string name_;                      ///< For the log

    GPeerHandlers handlers_;       ///< On the strand; dropped at the end
    GState state_ = GState::NEW;   ///< On the strand
    bool role_open_ = false;       ///< On the strand: the role was told of the open
    /// On the strand: the other rank knows that the connection is over -- it was sent this end's
    /// close or an answer to its own, or it closed itself (tellOther_())
    bool other_told_ = false;
    std::deque<GArrival> waiting_; ///< On the strand: what arrived before it could be handled
    std::atomic<bool> close_requested_{false}; ///< close() was called, or the end has ended
    /// The end found before start()'s work ran (abort(), a failed poll), reported when it runs
    std::optional<GEndReport> unreported_;
    std::atomic<bool> owner_busy_{false}; ///< What setOwnerBusy() was told last
    bool busy_told_ = false;              ///< On the strand: the poll counts this end as busy
    std::atomic<bool> ended_{false};      ///< The end has ended
};

/******************************************************************************/
/**
 * The submitter's side of the MPI transport (a GListenerI): a peer for every other rank of
 * courtier's communicator, all made in start(), and the poll that receives for them.
 *
 * Threads. The poll runs on an io context of the listener's own with one thread (accounted in the
 * thread budget as "<name>:poll"), so that no session holds it up; the peers' handlers run on
 * strands of the executor the shell hands to start(), and on_peer is called there, once for every
 * rank (GPeerI.hpp, rule 2).
 *
 * start() makes the poll and the peers, greets every client rank (TAG_PEER_OPEN: their ends open on
 * it) and hands every peer to the shell (on_peer, posted). stop() waits for those calls of on_peer,
 * at most a short bound of its own (GListenerI::stop()). endWork() stops and joins the poll's
 * thread, ends the poll (GMPIPollCore::shutdown(): sends that have not completed are given up, the
 * peers are let go) and shuts the poll's context down.
 *
 * Loss of a client: not noticed (detectsLoss() is false): a rank that dies ends the whole job, and
 * the items of a rank that hangs come back by the consumer's time lease.
 *
 * A submitter whose consumer is stopped without having been started has never greeted its clients,
 * which wait for that without a limit by default: announceEndWithoutStart() closes every client
 * rank with GPeerClose::RUN_ENDED instead (an end waiting for the greeting answers such a close), on
 * the poll's own thread, and waits for the answers, at most the stop grace.
 */
class GMPIListener final : public GListenerI {
public:
    /** @brief Prepares the listener; nothing happens before start() */
    explicit GMPIListener(std::shared_ptr<GMPICallsI>, GMPIPeerOptions = {});

    /** @brief Ends the poll if endWork() has not (an owner that did not keep the contract) */
    ~GMPIListener() override;

    GMPIListener(const GMPIListener &) = delete;
    GMPIListener(GMPIListener &&) = delete;
    GMPIListener &operator=(const GMPIListener &) = delete;
    GMPIListener &operator=(GMPIListener &&) = delete;

    /** @brief Makes the poll and a peer for every client rank, greets the ranks and hands the
     *  peers to the shell */
    void start(boost::asio::any_io_executor, on_peer_type) override;
    /** @brief Waits for the calls of on_peer under way; bounded, idempotent */
    void stop() override;
    /** @brief Joins the poll's thread, ends the poll and shuts its context down */
    void endWork() override;
    /** @brief The peers' close grace and a margin, so that a peer whose close is not answered
     *  ends by itself within it */
    [[nodiscard]] std::chrono::milliseconds stopGrace() const override;
    /** @brief false: the time lease reclaims the items of a rank that hangs */
    [[nodiscard]] bool detectsLoss() const override;
    /** @brief For a submitter that never started: tells every client rank that the run has ended,
     *  and waits for their answers, at most stopGrace() */
    void announceEndWithoutStart() override;

    /** @brief The number of client ranks (the peers made by start()). Any thread.
     *  @return The count; 0 before start() */
    [[nodiscard]] std::size_t nPeers() const noexcept;
    /** @brief What the poll has done; empty before start(). Any thread. @return The counts */
    [[nodiscard]] std::optional<GMPIPollCounts> pollCounts() const;

private:
    [[nodiscard]] std::vector<std::shared_ptr<GMPIPeer>>
    makePeers_(const std::shared_ptr<GMPIPollCore> &, const boost::asio::any_io_executor &) const;
    void startPoll_(const std::shared_ptr<GMPIPollCore> &);
    void handOver_(std::vector<std::shared_ptr<GMPIPeer>>);
    void tellEnds_();
    void joinPoll_();

    std::shared_ptr<GMPICallsI> calls_; ///< The MPI calls on courtier's communicator
    GMPIPeerOptions options_;           ///< How the peers and the poll are set up

    GIoContext poll_context_; ///< Where the poll runs
    std::optional<boost::asio::executor_work_guard<GIoContext::executor_type>> work_guard_;
    mutable std::mutex core_mutex_;      ///< Guards core_ (pollCounts() is asked from any thread)
    std::shared_ptr<GMPIPollCore> core_; ///< Made by start(); declared after its context

    /// Where on_peer and the peers' handlers run (the shell's executor); dropped by endWork()
    boost::asio::any_io_executor handler_executor_;
    on_peer_type on_peer_; ///< Set by start(), dropped by endWork()
    /// The calls of on_peer that have been posted and have not returned, by rank (stop() waits)
    Gem::Common::Concurrency::GThreadSafeKeyedStoreT<GWirePeerId, bool> on_peer_calls_;
    std::atomic<std::size_t> n_peers_{0}; ///< The peers made by start()
    /// The peers made by start(), to be told the end of the work (tellEnds_()); start() and endWork()
    std::vector<std::weak_ptr<GMPIPeer>> peers_;

    /// The poll thread's reservation in the process-wide thread budget
    Gem::Common::Concurrency::GThreadBudget::Reservation budget_;
    Gem::Common::Concurrency::GThreadGroup thread_; ///< The poll's thread
    std::atomic<bool> running_{false};              ///< The poll's thread runs
    std::atomic<bool> stopped_{false};              ///< stop() was called
};

/******************************************************************************/
/**
 * A client's side of the MPI transport (a GConnectorI): the peer to the submitter's rank
 * (RANK_MASTER_NODE), made at once and once -- an MPI client has one connection for the whole run
 * (reconnectable() is false). The poll runs on the executor given to connect(), the client's io
 * thread, and so does the peer (GPeerI.hpp, rule 7); the poll's chain ends by itself once the peer
 * has ended and its sends have completed, which lets the client's io context run out of work.
 * The destructor ends the poll (GMPIPollCore::shutdown()).
 */
class GMPIConnector final : public GConnectorI {
public:
    /** @brief Prepares the connector; nothing happens before connect() */
    explicit GMPIConnector(std::shared_ptr<GMPICallsI>, GMPIPeerOptions = {});

    /** @brief Gives up an attempt in progress and ends the poll */
    ~GMPIConnector() override;

    GMPIConnector(const GMPIConnector &) = delete;
    GMPIConnector(GMPIConnector &&) = delete;
    GMPIConnector &operator=(const GMPIConnector &) = delete;
    GMPIConnector &operator=(GMPIConnector &&) = delete;

    /** @brief Makes the peer to the submitter (the first time; refuses later) */
    void connect(boost::asio::any_io_executor, done_type) override;
    /** @brief Gives up the attempt in progress; its done is not called */
    void cancel() override;
    /** @brief @return false: one connection for the whole run */
    [[nodiscard]] bool reconnectable() const override;

    /** @brief What the poll has done; empty before connect(). Any thread. @return The counts */
    [[nodiscard]] std::optional<GMPIPollCounts> pollCounts() const;

private:
    std::shared_ptr<GMPICallsI> calls_;  ///< The MPI calls on courtier's communicator
    GMPIPeerOptions options_;            ///< How the peer and the poll are set up
    mutable std::mutex core_mutex_;      ///< Guards core_ (pollCounts() is asked from any thread)
    std::shared_ptr<GMPIPollCore> core_; ///< Made by the first connect()
    std::weak_ptr<GMPIPeer> peer_;       ///< The end made by connect(); told at the end of the work
    /// Set by cancel() and by the destructor; shared with the handler that reports the peer
    std::shared_ptr<std::atomic<bool>> cancelled_ = std::make_shared<std::atomic<bool>>(false);
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
