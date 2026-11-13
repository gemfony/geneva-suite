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

// MPI
#include <mpi.h>

// Geneva headers
#include "courtier/transport/GMPITransportT.hpp" // reuse the existing master/worker nodes + MPI init
#include "courtier/consumers/GNetworkedConsumerT.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The courtier MPI consumer. It reuses the existing, battle-tested Gem::Courtier::Consumers master
 * and worker nodes and drives the master from the courtier per-batch queue by injecting the
 * work-item source/sink callbacks at master-node construction (they are required constructor
 * parameters -- the master cannot usefully run without them).
 *
 * MPI fixes the process layout at launch: rank 0 is the master (it runs the optimization and serves
 * work) and every other rank is a worker. So usage differs from the socket consumers -- the program
 * must branch on isMasterNode(): the master registers this consumer, startServer()s and runs the
 * optimization; a worker just runWorker()s until the master broadcasts the stop signal.
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
    /** @brief Initializes MPI (if not already done), learns this node's rank, and builds the
     *  matching node (master on rank 0, worker otherwise).
     *
     *  @param argc Pointer to the program's argc, forwarded to MPI_Init (may be nullptr if MPI is already initialized)
     *  @param argv Pointer to the program's argv, forwarded to MPI_Init (may be nullptr if MPI is already initialized)
     *  @param config Configuration for the master/worker nodes (timeouts, buffer sizes, etc.) */
    explicit GMPIConsumerT(int *argc = nullptr, char ***argv = nullptr, config_type config = config_type{})
        : config_(config), i_initialized_mpi_(Gem::Courtier::Consumers::initializeMPI(argc, argv))
    {
        // Reuse the existing, correct MPI initialization (MPI_THREAD_MULTIPLE).
        

        MPI_Comm_size(MPI_COMM_WORLD, &comm_size_);
        MPI_Comm_rank(MPI_COMM_WORLD, &comm_rank_);

        if(isMasterNode()) {
            // Drive the master node from the courtier per-batch queue: the work-item source/sink
            // are required constructor parameters of the master node.
            master_ = std::make_shared<master_node_type>(
                comm_size_,
                config_,
                [this]() -> std::unique_ptr<processable_type> {
                    return this->checkoutWait(checkout_wait_);
                },
                [this](Gem::Courtier::GCommandContainerT<processable_type> &&frame) {
                    this->checkin(std::move(frame));
                }
            );
        }
        else {
            worker_ = std::make_shared<worker_node_type>(
                comm_rank_,
                [this]() -> bool { return worker_halt_.load(); },
                [this]() { ++n_worker_processed_; },
                config_
            );
        }
    }

    /** @brief The destructor stops the server (on the master) and finalizes MPI if this object
     *  initialized it. */
    ~GMPIConsumerT() override {
        if(isMasterNode()) {
            this->stopServer();
        }
        if(i_initialized_mpi_) {
            int finalized = 0;
            MPI_Finalized(&finalized);
            if(not finalized) {
                MPI_Finalize();
            }
        }
    }

    GMPIConsumerT(const GMPIConsumerT &) = delete;
    GMPIConsumerT(GMPIConsumerT &&) = delete;
    GMPIConsumerT &operator=(const GMPIConsumerT &) = delete;
    GMPIConsumerT &operator=(GMPIConsumerT &&) = delete;

    /***************************************************************************/
    /** @brief Whether this node is the master.
     *  @return true on rank 0 (the master), false on any worker rank. */
    [[nodiscard]] bool isMasterNode() const noexcept { return comm_rank_ == 0; }
    /** @brief This node's MPI rank.
     *  @return The rank within MPI_COMM_WORLD. */
    [[nodiscard]] std::int32_t getRank() const noexcept { return comm_rank_; }
    /** @brief The total number of MPI ranks.
     *  @return The size of MPI_COMM_WORLD. */
    [[nodiscard]] std::int32_t getCommSize() const noexcept { return comm_size_; }

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
    std::int32_t comm_size_ = 1;
    std::int32_t comm_rank_ = 0;
    bool i_initialized_mpi_ = false;

    std::shared_ptr<master_node_type> master_;
    std::shared_ptr<worker_node_type> worker_;

    std::atomic<bool> server_started_{false};
    std::atomic<bool> server_stopped_{false};
    std::atomic<bool> worker_halt_{false};
    std::atomic<std::uint64_t> n_worker_processed_{0};

    /// How long the master blocks for a pending item before answering a worker with "no data".
    std::chrono::milliseconds checkout_wait_{50};
};

/******************************************************************************/

} /* namespace Gem::Courtier */

#endif /* GENEVA_BUILD_WITH_MPI_CONSUMER */
