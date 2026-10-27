/**
 * @file GMPISubClientIndividual.cpp
 */

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

#include "geneva/GMPISubClientIndividual.hpp"

namespace Gem::Geneva {

/**
 * @brief Returns the MPI communicator shared by the sub-client group.
 *
 * @return The (static) intra-group communicator used by individuals to talk to their sub-clients.
 */
MPI_Comm GMPISubClientIndividual::getCommunicator() {
    return GMPISubClientIndividual::communicator_;
}

/**
 * @brief Stores the MPI communicator that individuals use to coordinate with their sub-clients.
 *
 * @param communicator The intra-group communicator to record for all instances (held statically).
 */
void GMPISubClientIndividual::setCommunicator(const MPI_Comm &communicator) {
    communicator_ = communicator;
}

/**
 * @brief Stores the pending non-blocking MPI request used to track when the client has finished.
 *
 * @param request The MPI request handle (e.g. an asynchronous barrier) whose completion signals that the client is done.
 */
void GMPISubClientIndividual::setClientStatusRequest(const MPI_Request &request) {
    clientStatusRequest_ = request;
}

/**
 * @brief Records whether the current process acts as a geneva client or as a sub-client.
 *
 * @param mode The role to record for the current process, either CLIENT or SUB_CLIENT.
 */
void GMPISubClientIndividual::setClientMode(const ClientMode &mode) {
    clientMode_ = mode;
}

/**
 * @brief Determines the current status of the client this process is associated with.
 *
 * A process running in CLIENT mode is always reported as RUNNING. Otherwise the previously
 * registered non-blocking status request is tested: if it has not completed the client is still
 * RUNNING, a failed request yields ERROR, and a successful completion yields FINISHED.
 *
 * @return The client status (RUNNING, FINISHED or ERROR), determined from the pending status request.
 */
ClientStatus GMPISubClientIndividual::getClientStatus() {
    // If the optimization is finished this means that no Individual is being processed.
    // Therefore, clients can only call this method if they are running
    if(clientMode_ == CLIENT) {
        return ClientStatus::RUNNING;
    }

    MPI_Status status{};
    int isCompleted{};

    MPI_Test(&clientStatusRequest_, &isCompleted, &status);

    if(!isCompleted) {
        return ClientStatus::RUNNING;
    }

    if(status.MPI_ERROR != MPI_SUCCESS) {
        return ClientStatus::ERROR;
    }

    return ClientStatus::FINISHED;
}

/**
 * @brief Returns the role currently assigned to this process.
 *
 * @return The client mode of this process, either CLIENT or SUB_CLIENT.
 */
ClientMode GMPISubClientIndividual::getClientMode() {
    return clientMode_;
}

} // namespace Gem::Geneva
