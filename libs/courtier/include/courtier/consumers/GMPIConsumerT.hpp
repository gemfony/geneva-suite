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

#include "common/GGlobalDefines.hpp"

// No guard on GENEVA_BUILD_WITH_MPI_CONSUMER: only a program that uses MPI includes this header,
// and that definition belongs to the collection's own build, which a program outside it does not
// have (see courtier/mpi/GMPIEnvironment.hpp).

// Standard headers
#include <memory>
#include <utility>

// Geneva headers
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "courtier/GCourtierEnums.hpp" // processRole
#include "courtier/consumers/GMPIConsumerConfig.hpp"
#include "courtier/consumers/GPeerConsumerT.hpp"
#include "courtier/mpi/GMPICalls.hpp"               // the MPI calls of courtier's communicator
#include "courtier/mpi/GMPIEnvironment.hpp"         // MPI initialization, communicator, role
#include "courtier/transport/GMPIPeerTransport.hpp" // the listener
#include "courtier/transport/GMPITransportT.hpp"    // the client, for users of this header

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The courtier MPI consumer: the server's shell (GPeerConsumerT) over a GMPIListener. Every other
 * rank of courtier's communicator is a client; its GPeerSessionT hands out work from the current
 * batch and collects results. Its client is GMPIConsumerWorkerNodeT; both are configured by an
 * MPIConsumerConfig.
 *
 * The consumer is built on the process whose role is SUBMITTER (rank RANK_MASTER_NODE of the
 * communicator, which need not be rank 0 of MPI_COMM_WORLD); a process whose role is CLIENT builds
 * the client, a BYSTANDER (a process without a communicator) neither. A program asks for the role
 * first (the environment's role(), or Gem::Geneva::consumerRole() for a Geneva program). The
 * consumer holds the environment, so MPI stays initialized while it exists.
 *
 * Threads. One thread polls courtier's communicator for every rank (the listener's); the sessions
 * run on nHandlerThreads threads (2 by default; 0: one per hardware thread). A session may take as
 * long as it likes over a message without holding up the poll.
 *
 * The start of a run (startServer()): every client rank is greeted, and its client then asks for
 * work. A client that has nothing to do waits at the server; its request is answered when an item
 * becomes available. A communicator without a client rank (a program started as one process) is
 * refused there, with a text that says how the program is started: an MPI job keeps the ranks it
 * was started with, so its work would wait for ever.
 *
 * The end of a run (stopServer()): every client is told that the run has ended and the server
 * waits for the clients to confirm, at most the stop grace (GMPICONSUMERSHUTDOWNGRACE and a
 * second); see GPeerConsumerT. A send that is still under way when the work ends (a large item for
 * a rank that has left without receiving it, say) is waited for up to the drain grace more
 * (GMPIPEERDRAINGRACE, 2 s) and then given up, so stopServer() may take that much longer than the
 * stop grace. A client that leaves by its own limits during the run is let go;
 * what it held is handed out again.
 *
 * Lost clients: a rank that dies ends the job -- the launcher (mpirun) aborts every rank when one
 * exits abnormally; MPI's errors being fatal on courtier's communicator only ends a rank whose own
 * MPI call fails. The items of a rank that hangs come back by the time lease.
 *
 * Size: MPI carries a message of at most INT_MAX bytes (about 2 GiB). A work item whose message
 * would be longer fails where the submitter hands it out, and a result that would be longer comes
 * back flagged from its client; the run goes on with the other items and clients.
 *
 * Silence: there is no keep-alive between the ranks, and no limit on how long a client waits for
 * its submitter once it has been greeted. A client whose submitter hangs, or ends without having
 * started its consumer and without telling it, waits until the job is ended (by the batch system
 * or the user); --mpi_firstContactLimit bounds only the wait for the greeting. Every way in which
 * the submitter's side of a client ends tells that client (a close of the run's end, or of an
 * error), so a client is not left waiting by a submitter that ends in any regular or failing way.
 *
 * @tparam processable_type The work-item type distributed to the client ranks and collected back
 */
template <typename processable_type>
class GMPIConsumerT final : public GPeerConsumerT<processable_type> {
public:
    /// The configuration this consumer (and its client) is built from
    using config_type = Gem::Courtier::Consumers::MPIConsumerConfig;

    /***************************************************************************/
    /** @brief Builds the consumer on the submitter.
     *
     *  @param environment The process's MPI environment; the consumer keeps it for as long as it
     *         exists. Must not be null.
     *  @param config Reads nHandlerThreads (the session threads; 0: one per hardware thread) and
     *         serializationMode. The client members are ignored.
     *  @throws geneva_exception if @p environment is null or the role of this process is not
     *          SUBMITTER */
    explicit GMPIConsumerT(
        std::shared_ptr<Gem::Courtier::Consumers::GMPIEnvironment> environment,
        const config_type &config = config_type{}
    )
      : GPeerConsumerT<processable_type>(
            CONSUMER_NAME,
            std::make_unique<Consumers::GMPIListener>(
                std::make_shared<Consumers::GMPICalls>(requireSubmitter_(std::move(environment))),
                Consumers::GMPIPeerOptions{.name = CONSUMER_NAME}
            ),
            config.nHandlerThreads,
            config.serializationMode
        ) { /* nothing */
    }

    /** @brief Stops the server while this object is still complete. MPI is finalized by the
     *  environment, when its last holder lets go of it. */
    ~GMPIConsumerT() override {
        this->stopServerNoThrow_();
    }

    GMPIConsumerT(const GMPIConsumerT &) = delete;
    GMPIConsumerT(GMPIConsumerT &&) = delete;
    GMPIConsumerT &operator=(const GMPIConsumerT &) = delete;
    GMPIConsumerT &operator=(GMPIConsumerT &&) = delete;

private:
    /***************************************************************************/
    /**
     * @brief Returns @p environment if this process is the submitter of it; throws otherwise.
     * @param environment The process's MPI environment
     * @return The environment
     */
    static std::shared_ptr<Gem::Courtier::Consumers::GMPIEnvironment>
    requireSubmitter_(std::shared_ptr<Gem::Courtier::Consumers::GMPIEnvironment> environment) {
        if(not environment) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIConsumerT<>::GMPIConsumerT(): Error!" << '\n'
                << "The MPI environment must not be empty" << '\n'
            );
        }
        if(environment->role() != Gem::Courtier::processRole::SUBMITTER) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIConsumerT<>::GMPIConsumerT(): Error!" << '\n'
                << "The MPI consumer is built on the submitter only; this process has the role "
                << static_cast<int>(environment->role())
                << ". A client builds a GMPIConsumerWorkerNodeT instead." << '\n'
            );
        }
        return environment;
    }

    /// The name of this consumer, for the log and the thread budget
    static constexpr const char *CONSUMER_NAME = "Gem::Courtier::GMPIConsumerT";
};

/******************************************************************************/

} /* namespace Gem::Courtier */
