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

// The whole consumer is only available when the MPI consumer was enabled at configure time.
#ifdef GENEVA_BUILD_WITH_MPI_CONSUMER

// Standard headers
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

// MPI
#include <mpi.h>

// Geneva headers
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "courtier/GCourtierEnums.hpp" // GMPICONSUMERCHECKOUTWAIT, processRole
#include "courtier/consumers/GNetworkedConsumerT.hpp"
#include "courtier/transport/GMPIEnvironment.hpp" // MPI initialization, communicator, role
#include "courtier/transport/GMPITransportT.hpp"  // reuse the existing master/worker nodes

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The courtier MPI consumer. It reuses the existing, battle-tested Gem::Courtier::Consumers master
 * and worker nodes and drives the master from the courtier per-batch queue by injecting the
 * work-item source/sink callbacks at master-node construction (they are required constructor
 * parameters -- the master cannot usefully run without them).
 *
 * The consumer works on the communicator of the GMPIEnvironment it is given, and the process's
 * role on that communicator decides what it builds: the master node on the SUBMITTER (rank
 * RANK_MASTER_NODE of the communicator, which need not be rank 0 of MPI_COMM_WORLD; it runs the
 * optimization and serves work), the worker node on a CLIENT (every other rank), and nothing on a
 * BYSTANDER (a process without a communicator). So usage differs from the socket consumers -- the
 * program must branch on isMasterNode(): the master registers this consumer, startServer()s and
 * runs the optimization; a worker just runWorker()s until the master broadcasts the stop signal.
 * The consumer holds the environment, so MPI stays initialized while it exists.
 *
 * @tparam processable_type The work-item type distributed to the worker ranks and collected back.
 */
template <typename processable_type>
class GMPIConsumerT final : public GNetworkedConsumerT<processable_type> {
public:
    using master_node_type = Gem::Courtier::Consumers::GMPIConsumerMasterNodeT<processable_type>;
    using worker_node_type = Gem::Courtier::Consumers::GMPIConsumerWorkerNodeT<processable_type>;
    using config_type = Gem::Courtier::Consumers::MPIConsumerConfig;

    /***************************************************************************/
    /** @brief Builds the node that matches this process's role: the master node on the
     *  submitter, the worker node on a client, neither on a bystander.
     *
     *  @param environment The process's MPI environment; the consumer keeps it for as long as it
     *         exists. Must not be null.
     *  @param config Configuration for the master/worker nodes (timeouts, buffer sizes, etc.)
     *  @throws geneva_exception if @p environment is null */
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

        switch(environment_->role()) {
        case Gem::Courtier::processRole::SUBMITTER:
            // Drive the master node from the courtier per-batch queue: the work-item
            // source/sink are required constructor parameters of the master node.
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
            break;

        case Gem::Courtier::processRole::CLIENT:
            worker_ = std::make_shared<worker_node_type>(
                environment_->communicator(),
                environment_->rank(),
                [this]() -> bool { return worker_halt_.load(); },
                [this]() { ++n_worker_processed_; },
                config_
            );
            break;

        case Gem::Courtier::processRole::BYSTANDER:
            break; // takes no part in the protocol: no node
        }
    }

    /** @brief The destructor stops the server (on the master). MPI is finalized by the
     *  environment, when its last holder lets go of it. */
    ~GMPIConsumerT() override {
        if(isMasterNode()) {
            this->stopServer();
        }
    }

    GMPIConsumerT(const GMPIConsumerT &) = delete;
    GMPIConsumerT(GMPIConsumerT &&) = delete;
    GMPIConsumerT &operator=(const GMPIConsumerT &) = delete;
    GMPIConsumerT &operator=(GMPIConsumerT &&) = delete;

    /***************************************************************************/
    /** @brief Whether this node is the master.
     *  @return true on the submitter, false on a client or a bystander. */
    [[nodiscard]] bool isMasterNode() const noexcept {
        return environment_->role() == Gem::Courtier::processRole::SUBMITTER;
    }
    /** @brief This node's MPI rank.
     *  @return The rank within courtier's communicator (see GMPIEnvironment).
     *  @throws geneva_exception on a bystander, which has no communicator */
    [[nodiscard]] std::int32_t getRank() const {
        return environment_->rank();
    }
    /** @brief The number of ranks that take part.
     *  @return The size of courtier's communicator (see GMPIEnvironment).
     *  @throws geneva_exception on a bystander, which has no communicator */
    [[nodiscard]] std::int32_t getCommSize() const {
        return environment_->size();
    }

    /** @brief (Master only) The number of distinct blobs the master has interned for transport
     *  (blob send-once). One per distinct genome structure across all worker ranks; 0 on a worker.
     *  @return The count of interned blobs, or 0 if this is not the master. */
    [[nodiscard]] std::size_t getInternedBlobCount() const {
        return master_ ? master_->getInternedBlobCount() : 0;
    }

    /***************************************************************************/
    /** @brief (Master only) starts the background threads that serve worker requests. */
    void startServer() {
        if(isMasterNode() && master_ && not server_started_.exchange(true)) {
            master_->async_startProcessing();
        }
    }

    /** @brief (Master only) signals the workers to stop and joins the master's background threads. */
    void stopServer() {
        if(isMasterNode() && master_ && not server_stopped_.exchange(true)) {
            this->requestStop();
            master_->shutdown(); // sends the stop signal to every worker and joins its threads
        }
    }

    /** @brief (Worker only) runs the worker loop until the master broadcasts the stop signal. */
    void runWorker() {
        if(worker_) {
            worker_->run();
        }
    }

    /** @brief (Worker only) asks the worker loop to halt at the next opportunity. */
    void haltWorker() noexcept { worker_halt_.store(true); }

private:
    /***************************************************************************/
    config_type config_;
    /// The process's MPI environment. Declared before the nodes, so that it is destroyed after
    /// them: MPI must still be initialized while they shut down.
    std::shared_ptr<Gem::Courtier::Consumers::GMPIEnvironment> environment_;

    std::shared_ptr<master_node_type> master_;
    std::shared_ptr<worker_node_type> worker_;

    std::atomic<bool> server_started_{false};
    std::atomic<bool> server_stopped_{false};
    std::atomic<bool> worker_halt_{false};
    std::atomic<std::uint64_t> n_worker_processed_{0};

    /// How long the master blocks for a pending item before answering a worker with "no data".
    std::chrono::milliseconds checkout_wait_{Gem::Courtier::GMPICONSUMERCHECKOUTWAIT};
};

/******************************************************************************/

} /* namespace Gem::Courtier */

#endif /* GENEVA_BUILD_WITH_MPI_CONSUMER */
