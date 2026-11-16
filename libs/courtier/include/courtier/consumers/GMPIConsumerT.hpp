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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

// MPI
#include <mpi.h>

// Geneva headers
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "courtier/GCourtierEnums.hpp" // GMPICONSUMERCHECKOUTWAIT, processRole
#include "courtier/consumers/GNetworkedConsumerT.hpp"
#include "courtier/mpi/GMPIEnvironment.hpp"       // MPI initialization, communicator, role
#include "courtier/transport/GMPITransportT.hpp"  // the master node

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The courtier MPI consumer. It reuses the existing, battle-tested Gem::Courtier::Consumers master
 * node and drives it from the courtier per-batch queue by injecting the
 * work-item source/sink callbacks at master-node construction (they are required constructor
 * parameters -- the master cannot usefully run without them).
 *
 * The consumer is the master only: it works on the communicator of the GMPIEnvironment it is given
 * and is built on the process whose role there is SUBMITTER (rank RANK_MASTER_NODE of the
 * communicator, which need not be rank 0 of MPI_COMM_WORLD). Like the socket consumers, it has a
 * client of its own, GMPIConsumerWorkerNodeT, which a process whose role is CLIENT builds and runs
 * instead; a BYSTANDER (a process without a communicator) builds neither. A program asks for the
 * role first (the environment's role(), or Gem::Geneva::consumerRole() for a Geneva program). The
 * consumer holds the environment, so MPI stays initialized while it exists.
 *
 * @tparam processable_type The work-item type distributed to the worker ranks and collected back.
 */
template <typename processable_type>
class GMPIConsumerT final : public GNetworkedConsumerT<processable_type> {
public:
    using master_node_type = Gem::Courtier::Consumers::GMPIConsumerMasterNodeT<processable_type>;
    using config_type = Gem::Courtier::Consumers::MPIConsumerConfig;

    /***************************************************************************/
    /** @brief Builds the master node on the submitter.
     *
     *  @param environment The process's MPI environment; the consumer keeps it for as long as it
     *         exists. Must not be null.
     *  @param config Configuration for the master node (timeouts, buffer sizes, etc.)
     *  @throws geneva_exception if @p environment is null or the role of this process is not
     *          SUBMITTER */
    explicit GMPIConsumerT(
        std::shared_ptr<Gem::Courtier::Consumers::GMPIEnvironment> environment,
        config_type config = config_type{}
    )
      : config_(config)
      , environment_(std::move(environment)) {
        if(not environment_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIConsumerT<>::GMPIConsumerT(): Error!" << '\n'
                << "The MPI environment must not be empty" << '\n'
            );
        }

        if(environment_->role() != Gem::Courtier::processRole::SUBMITTER) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIConsumerT<>::GMPIConsumerT(): Error!" << '\n'
                << "The MPI consumer is built on the submitter only; this process has the role "
                << static_cast<int>(environment_->role())
                << ". A client builds a GMPIConsumerWorkerNodeT instead." << '\n'
            );
        }

        // Drive the master node from the courtier per-batch queue: the work-item source/sink are
        // required constructor parameters of the master node.
        master_ = std::make_shared<master_node_type>(
            environment_->communicator(),
            config_,
            [this]() -> std::unique_ptr<processable_type> {
                return this->checkoutWait(checkout_wait_);
            },
            [this](Gem::Courtier::GCommandContainerT<processable_type> &&frame) {
                this->checkin(std::move(frame));
            }
        );
    }

    /** @brief The destructor stops the server. MPI is finalized by the environment, when its
     *  last holder lets go of it. */
    ~GMPIConsumerT() override {
        this->stopServer();
    }

    GMPIConsumerT(const GMPIConsumerT &) = delete;
    GMPIConsumerT(GMPIConsumerT &&) = delete;
    GMPIConsumerT &operator=(const GMPIConsumerT &) = delete;
    GMPIConsumerT &operator=(GMPIConsumerT &&) = delete;

    /***************************************************************************/
    /** @brief The number of distinct blobs the master has interned for transport (blob
     *  send-once). One per distinct genome structure across all worker ranks.
     *  @return The count of interned blobs. */
    [[nodiscard]] std::size_t getInternedBlobCount() const {
        return master_->getInternedBlobCount();
    }

    /***************************************************************************/
    /** @brief Starts the background threads that serve the clients' requests. */
    void startServer() {
        if(not server_started_.exchange(true)) {
            master_->async_startProcessing();
        }
    }

    /** @brief Signals the clients to stop and joins the master's background threads. */
    void stopServer() {
        if(not server_stopped_.exchange(true)) {
            this->requestStop();
            master_->shutdown(); // sends the stop signal to every client and joins its threads
        }
    }

private:
    /***************************************************************************/
    config_type config_;
    /// The process's MPI environment. Declared before the nodes, so that it is destroyed after
    /// them: MPI must still be initialized while they shut down.
    std::shared_ptr<Gem::Courtier::Consumers::GMPIEnvironment> environment_;

    std::shared_ptr<master_node_type> master_;

    std::atomic<bool> server_started_{false};
    std::atomic<bool> server_stopped_{false};

    /// How long the master blocks for a pending item before answering a worker with "no data".
    std::chrono::milliseconds checkout_wait_{Gem::Courtier::GMPICONSUMERCHECKOUTWAIT};
};

/******************************************************************************/

} /* namespace Gem::Courtier */
