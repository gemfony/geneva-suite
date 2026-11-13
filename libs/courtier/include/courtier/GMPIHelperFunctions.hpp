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

// MPI library
#include <mpi.h>

// standard headers
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace Gem::Courtier {

enum MPIStatusCode {
    /**
     * The operation has succeeded without errors
     */
    SUCCESS,
    /**
     * The operation was stopped due to the halt criterion
     */
    STOPPED,
    /**
     * The operation has completed with an error
     */
    ERROR
};

/**
 * Stores the result of an MPI while operation
 */
struct MPICompletionStatus {
    /**
     * Type of completion
     */
    MPIStatusCode statusCode{};
    /**
     * The status which was returned by MPI at that time
     */
    MPI_Status mpiStatus{};
};

/******************************************************************************/
/**
 * @brief Returns the message size (element count) of a completed MPI request.
 *
 * The return value is undefined for a status of a not-yet-completed request, so MPI_Test should be
 * called first.
 *
 * @param status A reference to a (completed) MPI status object
 * @param dataType The MPI datatype of the message elements (defaults to MPI_CHAR)
 * @return The number of elements of type @p dataType transferred by the operation
 */
inline int mpiGetCount(const MPI_Status &status, MPI_Datatype dataType = MPI_CHAR) {
    int count{};
    MPI_Get_count(&status, dataType, &count);
    return count;
}

/******************************************************************************/
/**
 * @brief Converts an MPI error code into a human-readable description.
 *
 * @param mpiError The integer MPI error code (typically taken from an MPI_Status's MPI_ERROR field)
 * @return A string describing the given error code
 */
inline std::string mpiErrorString(int mpiError) {
    char errorMessage[MPI_MAX_ERROR_STRING];
    int messageLength = 0;
    MPI_Error_string(mpiError, errorMessage, &messageLength);
    // the string in errorMessage is null terminated, so it can be handed to the std::string constructor
    return std::string{errorMessage};
}

/******************************************************************************/
/**
 * @brief Returns the number of processes in a given MPI communicator.
 *
 * @param comm The MPI communicator to query
 * @return The number of processes in @p comm
 */
inline std::uint32_t mpiSize(const MPI_Comm &comm) {
    int size{};
    MPI_Comm_size(comm, &size);
    return static_cast<std::uint32_t>(size);
}

/******************************************************************************/
// The machinery the two collectives share; not part of the interface.
namespace detail {

/**
 * Waits for the completion of an async MPI request by checking in a cyclic manner as long as a predicate returns true
 * @param request the request to wait on
 * @param pollIntervalMSec the time between checks
 * @param runWhile predicate, when this is false the waiting will be aborted
 * @return status of the operation when terminated
 */
inline MPICompletionStatus waitForRequestCompletionWhile(
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
inline MPICompletionStatus mpiCollectiveWhile(
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

} /* namespace detail */

/******************************************************************************/
/**
 * @brief Performs an asynchronous scatter and blocks until it completes or a predicate returns false.
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
[[nodiscard]] inline MPICompletionStatus mpiScatterWhile(
    const void *sendBuf,
    const std::uint32_t &sendCount,
    void *recvBuf,
    MPI_Datatype type,
    const std::function<bool()> &runWhile,
    const std::uint32_t &root,
    MPI_Comm comm,
    const std::uint64_t &pollIntervalMSec
) {
    return detail::mpiCollectiveWhile(
        &MPI_Iscatter, sendBuf, sendCount, recvBuf, type, runWhile, root, comm, pollIntervalMSec
    );
}

/******************************************************************************/
/**
 * @brief Performs an asynchronous gather and blocks until it completes or a predicate returns false.
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
[[nodiscard]] inline MPICompletionStatus mpiGatherWhile(
    const void *sendBuf,
    const std::uint32_t &sendCount,
    void *recvBuf,
    MPI_Datatype type,
    const std::function<bool()> &runWhile,
    const std::uint32_t &root,
    MPI_Comm comm,
    const std::uint64_t &pollIntervalMSec
) {
    return detail::mpiCollectiveWhile(
        &MPI_Igather, sendBuf, sendCount, recvBuf, type, runWhile, root, comm, pollIntervalMSec
    );
}

} /* namespace Gem::Courtier */
