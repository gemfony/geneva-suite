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

#include "courtier/GMPIHelperFunctions.hpp"

#include <mpi.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace Gem::Courtier {

/******************************************************************************/
/**
 * Returns the message size of a completed request.
 * The return value is undefined for the status on not yet completed requests.
 * Therefore you should call MPI_Test first.
 *
 * @param status a reference to an MPI status object
 * @param dataType the datatype of the MPI message. Defaults to MPI_CHAR
 * @return the amount of objects of type dataType transferred by the operation
 */
int mpiGetCount(const MPI_Status &status, MPI_Datatype dataType) {
    int count{};
    MPI_Get_count(&status, dataType, &count);

    return count;
}

/**
 * returns the number of processes in a given MPI Communicator
 * @param comm the communicator
 * @return the number of processes in the communicator
 */
std::uint32_t mpiSize(const MPI_Comm &comm) {
    int size{};
    MPI_Comm_size(comm, &size);

    return size;
}

/**
 * Converts an MPI error code to the corresponding string which explains the error
 *
 * @param mpiError integer error code used by MPI and typically stored in an MPI_Status.status field
 * @return a string corresponding to the error code
 */
std::string mpiErrorString(int mpiError) {
    char errorMessage[MPI_MAX_ERROR_STRING];
    int messageLength = 0;
    MPI_Error_string(mpiError, errorMessage, &messageLength);

    // the string in errorMessage is null terminated, which allows us to pass it to the std::string constructor
    return std::string{errorMessage};
}

namespace {

/**
 * Waits for the completion of an async MPI request by checking in a cyclic manner as long as a predicate returns true
 * @param request the request to wait on
 * @param pollIntervalMSec the time between checks
 * @param runWhile predicate, when this is false the waiting will be aborted
 * @return status of the operation when terminated
 */
MPICompletionStatus waitForRequestCompletionWhile(
    MPI_Request &request,
    const std::uint64_t &pollIntervalMSec,
    const std::function<bool()> &runWhile
) {
    int isCompleted{0};
    MPI_Status status{};

    while(runWhile()) {
        MPI_Test(&request, &isCompleted, &status);

        // return appropriate result in case of completion
        if(isCompleted) {
            if(status.MPI_ERROR == MPI_SUCCESS) {
                return MPICompletionStatus{.statusCode=MPIStatusCode::SUCCESS, .mpiStatus=status};
            }
            return MPICompletionStatus{.statusCode=MPIStatusCode::ERROR, .mpiStatus=status};
        }

        // sleep some time before polling again for completion status
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMSec));
    }

    // the execution shall be stopped due to the stop criterion returning false

    // return appropriate result
    return MPICompletionStatus{.statusCode=MPIStatusCode::STOPPED, .mpiStatus=status};
}

/**
 * Starts an async symmetric collective (MPI_Iscatter or MPI_Igather -- their signatures are
 * identical, with sendCount elements travelling per process in either direction) and blocks
 * until the request has completed or a predicate returns false. The one shared implementation
 * behind mpiScatterWhile() / mpiGatherWhile().
 *
 * @param collective The collective to start (MPI_Iscatter or MPI_Igather)
 * @param sendBuf The buffer data is sent from (see the public wrappers for the per-direction meaning)
 * @param sendCount The number of elements travelling per process
 * @param recvBuf The buffer data is received into
 * @param type The MPI datatype of the elements
 * @param runWhile Predicate polled while waiting; waiting is aborted once it returns false
 * @param root The rank of the root process of the collective
 * @param comm The MPI communicator over which the collective is performed
 * @param pollIntervalMSec The time in milliseconds between completion checks
 * @return The completion status of the operation when it terminated
 */
// NOLINTNEXTLINE(readability-function-size) -- 9 parameters mirror the shared MPI_Iscatter/MPI_Igather call signature plus the poll predicate/interval; splitting would just pass most of them straight back together
MPICompletionStatus mpiCollectiveWhile(
    decltype(&MPI_Iscatter) collective,
    const void *sendBuf,
    const std::uint32_t &sendCount,
    void *recvBuf,
    MPI_Datatype type,
    const std::function<bool()> &runWhile,
    const std::uint32_t &root,
    MPI_Comm comm,
    const std::uint64_t &pollIntervalMSec
) {
    MPI_Request requestHandle{};

    collective(
        sendBuf,
        static_cast<int>(sendCount), // elements sent per process
        type,
        recvBuf,
        static_cast<int>(sendCount), // elements received per process
        type,
        static_cast<int>(root), // root rank
        comm,
        &requestHandle
    );

    return waitForRequestCompletionWhile(requestHandle, pollIntervalMSec, runWhile);
}

} // anonymous namespace

/**
 * Performs an async scatter and blocks until the request has completed or a predicate returns false
 *
 * @param sendBuf The buffer holding the data to be scattered (only meaningful on the root process)
 * @param sendCount The number of elements sent to each process
 * @param recvBuf The buffer into which this process receives its slice of the scattered data
 * @param type The MPI datatype of the elements being scattered
 * @param runWhile Predicate polled while waiting; waiting is aborted once it returns false
 * @param root The rank of the root process performing the scatter
 * @param comm The MPI communicator over which the scatter is performed
 * @param pollIntervalMSec The time in milliseconds between completion checks
 * @return The completion status of the operation when it terminated
 */
MPICompletionStatus mpiScatterWhile(
    const void *sendBuf,
    const std::uint32_t &sendCount,
    void *recvBuf,
    MPI_Datatype type,
    const std::function<bool()> &runWhile,
    const std::uint32_t &root = 0,
    MPI_Comm comm = MPI_COMM_WORLD,
    const std::uint64_t &pollIntervalMSec = 1
) {
    return mpiCollectiveWhile(
        &MPI_Iscatter, sendBuf, sendCount, recvBuf, type, runWhile, root, comm, pollIntervalMSec
    );
}

/**
 * Performs an async gather and blocks until the request has completed or a predicate returns false
 *
 * @param sendBuf The buffer holding this process's contribution to the gather
 * @param sendCount The number of elements sent by each process
 * @param recvBuf The buffer into which the gathered data is collected (only meaningful on the root process)
 * @param type The MPI datatype of the elements being gathered
 * @param runWhile Predicate polled while waiting; waiting is aborted once it returns false
 * @param root The rank of the root process collecting the gathered data
 * @param comm The MPI communicator over which the gather is performed
 * @param pollIntervalMSec The time in milliseconds between completion checks
 * @return The completion status of the operation when it terminated
 */
MPICompletionStatus mpiGatherWhile(
    const void *sendBuf,
    const std::uint32_t &sendCount,
    void *recvBuf,
    MPI_Datatype type,
    const std::function<bool()> &runWhile,
    const std::uint32_t &root = 0,
    MPI_Comm comm = MPI_COMM_WORLD,
    const std::uint64_t &pollIntervalMSec = 1
) {
    return mpiCollectiveWhile(
        &MPI_Igather, sendBuf, sendCount, recvBuf, type, runWhile, root, comm, pollIntervalMSec
    );
}

} /* namespace Gem::Courtier */
