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
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Boost headers go here
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

// Geneva headers go here
#include "common/concurrency/GMPMCQueueT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/transport/GMPICallsI.hpp"

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * How the MPI poll paces itself. The defaults are the constants of GCourtierEnums.hpp; a test may
 * shorten or lengthen them.
 */
struct GMPIPollTiming {
    /// The pause after a look that found traffic; a started send resets the pause to it as well
    std::chrono::microseconds pauseMin = GMPIPEERPOLLPAUSEMIN;
    /// The longest pause: without traffic the pause doubles up to it
    std::chrono::microseconds pauseMax = GMPIPEERPOLLPAUSEMAX;
    /// How many messages one link of the chain receives before it lets other handlers run
    int rounds = GMPIPEERPOLLROUNDS;
    /// How long the chain waits, after the last receiver has ended, for sends to complete
    std::chrono::milliseconds drainGrace = GMPIPEERDRAINGRACE;
};

/******************************************************************************/
/** @brief How much the poll has done; each count a snapshot. */
struct GMPIPollCounts {
    std::uint64_t received = 0; ///< Messages received
    std::uint64_t sent = 0;     ///< Sends started
    std::uint64_t polls = 0;    ///< Links of the chain that ran
    std::uint64_t givenUp = 0;  ///< Sends given up by shutdown(), because they had not completed
};

/******************************************************************************/
/**
 * What the poll delivers the messages of one rank to: the MPI transport's end of the connection to
 * that rank (its peer). The poll owns it from GMPIPollCore::addReceiver() until
 * GMPIPollCore::shutdown(); the receiver refers to the poll weakly.
 */
class GMPIPollReceiverI {
public:
    /** @brief The default constructor */
    GMPIPollReceiverI() = default;
    /** @brief The destructor */
    virtual ~GMPIPollReceiverI() = default;

    GMPIPollReceiverI(const GMPIPollReceiverI &) = delete;
    GMPIPollReceiverI(GMPIPollReceiverI &&) = delete;
    GMPIPollReceiverI &operator=(const GMPIPollReceiverI &) = delete;
    GMPIPollReceiverI &operator=(GMPIPollReceiverI &&) = delete;

    /** @brief Where received() runs. It must run its handlers one at a time and in the order they
     *  were posted (a strand, or a context with one thread), or the messages of a rank lose their
     *  order. Asked once, in GMPIPollCore::addReceiver(). */
    [[nodiscard]] virtual boost::asio::any_io_executor executor() = 0;

    /** @brief One message from the receiver's rank, with its tag; on executor(), in the order the
     *  rank sent them, whatever their tags */
    virtual void received(int, std::string &&) = 0;

    /** @brief The poll lets go of the receiver: the transport's work has ended (rule 7 of
     *  GPeerI.hpp). Called by GMPIPollCore::shutdown(), when no handler runs any more. */
    virtual void discard() = 0;
};

/******************************************************************************/
/**
 * The MPI transport's own work in one process: the poll. It is the only thing that receives on
 * courtier's communicator, and it completes the sends that the transport's peers start.
 *
 * It has no thread. It is a chain of handlers on a strand of its own on the executor it is given:
 * each link looks once for a message from any rank with any tag, receives it if one is there and
 * posts it to the executor of that rank's receiver, takes over the sends that were started since
 * and frees those that have completed. While messages keep coming it goes on at once -- after
 * GMPIPollTiming::rounds messages through a post, so that other handlers of the executor get their
 * turn --, otherwise after a pause that grows from GMPIPollTiming::pauseMin to pauseMax. A
 * received message and a started send reset the pause, and a started send ends a pause that is
 * under way (the "kick"): the answer to a send is what the poll waits for.
 *
 * Where it runs is its owner's choice and makes no difference to it: on a master an io context of
 * its own with one thread (no handler of a session holds it up), on a client the client's io
 * thread. Receivers run on executors of their own; the poll only posts to them, so a receiver that
 * takes long does not hold up the poll. Because one chain receives, a probe and the receive that
 * follows it always mean the same message, and because the messages of one rank are received in
 * the order they were sent (MPI does not let them overtake each other, whatever their tags) and
 * posted in that order to one serialized executor, they reach the receiver in that order.
 *
 * Membership is static: the receivers are added before start() and the table is not changed
 * afterwards. The chain ends by itself when every receiver has ended (receiverEnded()) and every
 * send has completed, or GMPIPollTiming::drainGrace after the last receiver has ended -- on a
 * client this is what lets io_context::run() return. shutdown() ends the poll in any case and gives
 * up the sends that have not completed; its owner calls it when no handler of the executors runs
 * any more.
 *
 * Ownership: the pending handlers of the chain and the owner (the MPI listener or connector) hold
 * the poll; the poll holds the receivers until shutdown(); a receiver refers to the poll weakly.
 * The poll holds the calls, and with them whatever keeps MPI initialized. Made with
 * std::make_shared (the chain refers to it through shared_from_this()).
 */
class GMPIPollCore final : public std::enable_shared_from_this<GMPIPollCore> {
public:
    /***************************************************************************/
    /** @brief Prepares the poll; nothing runs before start() */
    GMPIPollCore(
        std::shared_ptr<GMPICallsI>,
        const boost::asio::any_io_executor &,
        GMPIPollTiming = {}
    );

    /** @brief The destructor; makes no call to MPI (see shutdown()) */
    ~GMPIPollCore() = default;

    GMPIPollCore(const GMPIPollCore &) = delete;
    GMPIPollCore(GMPIPollCore &&) = delete;
    GMPIPollCore &operator=(const GMPIPollCore &) = delete;
    GMPIPollCore &operator=(GMPIPollCore &&) = delete;

    /***************************************************************************/
    /** @brief Adds the receiver of one rank's messages. Before start(), from one thread. */
    void addReceiver(int, std::shared_ptr<GMPIPollReceiverI>);

    /** @brief Starts the chain. Once, after the receivers have been added. From any thread. */
    void start();

    /** @brief Starts the send of one message and hands it to the poll. From any thread; the
     *  calls of one sender are sent in the order they are made. */
    void startSend(int, int, std::string);

    /** @brief A receiver tells that it has ended and will send nothing more. Once per receiver,
     *  after its last startSend(). From any thread. */
    void receiverEnded(int);

    /***************************************************************************/
    /** @brief Ends the poll's work and gives up the sends that have not completed. When no handler
     *  of the executors runs any more; idempotent. */
    std::size_t shutdown();

    /** @brief Whether the chain has ended (by itself, or through shutdown()). From any thread. */
    [[nodiscard]] bool chainEnded() const noexcept;

    /** @brief How much the poll has done. From any thread. */
    [[nodiscard]] GMPIPollCounts counts() const noexcept;

    /** @brief The timing the poll was made with. From any thread. */
    [[nodiscard]] const GMPIPollTiming &timing() const noexcept;

private:
    /***************************************************************************/
    /** @brief A send that has been started and whose completion the poll has yet to see */
    struct GStartedSend {
        GMPIRequest request = GMPIRequest::NONE;
        std::unique_ptr<std::string> bytes; ///< Must stay where they are until the send is over
    };

    /***************************************************************************/
    /** @brief One link of the chain */
    void poll_();
    /** @brief Receives and posts one message, if one is there */
    bool receiveOne_();
    /** @brief Takes over the sends started since the last look */
    void takeStarted_();
    /** @brief Frees the sends that have completed */
    void reap_();
    /** @brief A send was started: no pause */
    void kick_();
    /** @brief Counts one receiver as ended */
    void ended_(int);
    /** @brief Whether a rank is one of the communicator's */
    [[nodiscard]] bool validRank_(int) const noexcept;

    /***************************************************************************/
    // Data

    std::shared_ptr<GMPICallsI> calls_; ///< The calls to MPI; keeps MPI initialized
    GMPIPollTiming timing_;             ///< How the poll paces itself
    int size_;                          ///< The number of ranks of the communicator
    boost::asio::strand<boost::asio::any_io_executor> strand_; ///< The chain's own strand
    boost::asio::steady_timer timer_;                          ///< The pause between two links

    /// The receiver of every rank, by rank (empty for the others), and the executor it is posted
    /// to. Filled before the chain starts and not changed afterwards (until shutdown()), so it is
    /// read without a lock.
    std::vector<std::shared_ptr<GMPIPollReceiverI>> receivers_;
    std::vector<boost::asio::any_io_executor> receiver_executors_;

    /// Sends that were started and that the chain has not taken over yet
    Gem::Common::Concurrency::GMPMCQueueT<GStartedSend, 0> started_;
    std::atomic<bool> kick_pending_{false};  ///< A kick is on its way to the strand
    std::atomic<bool> started_chain_{false}; ///< start() has been called
    std::atomic<bool> chain_ended_{false};   ///< No further link runs
    std::atomic<bool> shut_down_{false};     ///< shutdown() has run

    std::atomic<std::uint64_t> n_received_{0}; ///< Messages received
    std::atomic<std::uint64_t> n_sent_{0};     ///< Sends started
    std::atomic<std::uint64_t> n_polls_{0};    ///< Links that ran
    std::atomic<std::uint64_t> n_given_up_{0}; ///< Sends given up

    // Touched by the chain only (on its strand), and by shutdown() when no handler runs:
    std::chrono::microseconds pause_;                   ///< The next pause
    std::vector<GMPIRequest> requests_;                 ///< Sends under way
    std::vector<std::unique_ptr<std::string>> buffers_; ///< Their bytes, same order
    std::vector<bool> ended_ranks_;                     ///< Receivers that have ended, by rank
    std::size_t live_receivers_ = 0;                    ///< Receivers that have not ended
    /// Set when the last receiver has ended
    std::optional<std::chrono::steady_clock::time_point> drain_deadline_;
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
