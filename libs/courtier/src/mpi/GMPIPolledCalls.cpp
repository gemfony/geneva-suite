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

/*
 * The polled waits of courtier's MPI library (see GMPIPolledCalls.hpp).
 */

#include "courtier/mpi/GMPIPolledCalls.hpp"

// Standard headers go here
#include <algorithm>
#include <chrono>
#include <span>
#include <thread>

// MPI headers go here
#include <mpi.h>

// Geneva headers go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp"

namespace Gem::Courtier {

namespace {

/******************************************************************************/
/**
 * @brief Looks until @p look returns true: the first look at once, then a pause before each
 * further look that starts at @p pause_min and doubles up to @p pause_max. The one loop of the
 * polled waits.
 *
 * @param look Returns true when what is waited for is there (or when waiting must end)
 * @param pause_min The first pause
 * @param pause_max The longest pause
 * @throws geneva_exception if the pauses break checkPollPauses()'s rule
 */
template <typename look_type>
void pollUntil(
    const look_type &look,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    checkPollPauses(pause_min, pause_max);
    auto pause = pause_min;
    while(not look()) {
        std::this_thread::sleep_for(pause);
        pause = std::min(2 * pause, pause_max);
    }
}

} // namespace

/******************************************************************************/
/**
 * @brief Waits until every request of @p requests has completed, with MPI_Testall polled as
 * pollUntil() does. Completed requests are set to MPI_REQUEST_NULL by MPI.
 *
 * @param requests The requests; an empty span returns at once
 * @param pause_min The first pause between two looks
 * @param pause_max The longest pause between two looks
 * @return What the last MPI_Testall returned: MPI_SUCCESS, or an error code on a communicator
 *         whose error handler returns. The wait ends then, and a request may still be active: its
 *         buffers must stay valid, and a started collective cannot be cancelled -- the caller can
 *         only end the job (MPI_Abort)
 * @throws geneva_exception if the pauses break checkPollPauses()'s rule
 */
int waitPolled(
    std::span<MPI_Request> requests,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    if(requests.empty()) {
        return MPI_SUCCESS;
    }
    const int count = Gem::Common::narrow<int>(requests.size());
    int result = MPI_SUCCESS;
    pollUntil(
        [&] {
            int done = 0;
            result = MPI_Testall(count, requests.data(), &done, MPI_STATUSES_IGNORE);
            return result != MPI_SUCCESS || done != 0;
        },
        pause_min,
        pause_max
    );
    return result;
}

/******************************************************************************/
/**
 * @brief Waits, as waitPolled() does, until MPI_Iprobe finds a message from @p source with @p tag
 * on @p comm. The message stays where it is; a receive of it then returns at once.
 *
 * @param source The rank to wait for (or MPI_ANY_SOURCE)
 * @param tag The tag to wait for (or MPI_ANY_TAG)
 * @param comm The communicator
 * @param pause_min The first pause between two looks
 * @param pause_max The longest pause between two looks
 * @return The status of the message found
 * @throws geneva_exception if MPI_Iprobe reports an error (on a communicator whose error handler
 *         returns), or if the pauses break checkPollPauses()'s rule
 */
MPI_Status probePolled(
    int source,
    int tag,
    MPI_Comm comm,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    MPI_Status status{};
    int result = MPI_SUCCESS;
    pollUntil(
        [&] {
            int found = 0;
            result = MPI_Iprobe(source, tag, comm, &found, &status);
            return result != MPI_SUCCESS || found != 0;
        },
        pause_min,
        pause_max
    );
    if(result != MPI_SUCCESS) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In probePolled(): Error!" << '\n'
            << "MPI_Iprobe failed with the error code " << result << '\n'
        );
    }
    return status;
}

/******************************************************************************/
/**
 * @brief Scatters @p count elements per process from @p root with MPI_Iscatter and waits for
 * the completion with waitPolled(). Collective on @p comm.
 *
 * @param send_buf The elements of all processes, in rank order (read on @p root only)
 * @param count The number of elements per process
 * @param recv_buf Where this process's elements go
 * @param type The MPI datatype of the elements
 * @param root The rank that scatters
 * @param comm The communicator
 * @param pause_min The first pause between two looks
 * @param pause_max The longest pause between two looks
 * @return MPI_SUCCESS, or the error code of MPI_Iscatter (nothing was started then; the other
 *         processes may wait in it) or of the wait (see waitPolled(): the collective may still be
 *         active over the buffers) -- after an error the caller can only end the job (MPI_Abort)
 * @throws geneva_exception if the pauses break checkPollPauses()'s rule
 */
int mpiScatter(
    const void *send_buf,
    int count,
    void *recv_buf,
    MPI_Datatype type,
    int root,
    MPI_Comm comm,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    checkPollPauses(pause_min, pause_max); // before anything is started
    // NOLINTBEGIN(clang-analyzer-optin.mpi.MPI-Checker): waitPolled() completes the request; the
    // checker does not know it
    MPI_Request request = MPI_REQUEST_NULL;
    const int started =
        MPI_Iscatter(send_buf, count, type, recv_buf, count, type, root, comm, &request);
    if(started != MPI_SUCCESS) {
        return started;
    }
    return waitPolled(std::span<MPI_Request>(&request, 1), pause_min, pause_max);
    // NOLINTEND(clang-analyzer-optin.mpi.MPI-Checker)
}

/******************************************************************************/
/**
 * @brief Gathers @p count elements per process to @p root with MPI_Igather and waits for the
 * completion with waitPolled(). Collective on @p comm.
 *
 * @param send_buf This process's elements
 * @param count The number of elements per process
 * @param recv_buf Where the elements of all processes go, in rank order (written on @p root only)
 * @param type The MPI datatype of the elements
 * @param root The rank that gathers
 * @param comm The communicator
 * @param pause_min The first pause between two looks
 * @param pause_max The longest pause between two looks
 * @return MPI_SUCCESS, or the error code of MPI_Igather (nothing was started then; the other
 *         processes may wait in it) or of the wait (see waitPolled(): the collective may still be
 *         active over the buffers) -- after an error the caller can only end the job (MPI_Abort)
 * @throws geneva_exception if the pauses break checkPollPauses()'s rule
 */
int mpiGather(
    const void *send_buf,
    int count,
    void *recv_buf,
    MPI_Datatype type,
    int root,
    MPI_Comm comm,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max
) {
    checkPollPauses(pause_min, pause_max); // before anything is started
    // NOLINTBEGIN(clang-analyzer-optin.mpi.MPI-Checker): waitPolled() completes the request; the
    // checker does not know it
    MPI_Request request = MPI_REQUEST_NULL;
    const int started =
        MPI_Igather(send_buf, count, type, recv_buf, count, type, root, comm, &request);
    if(started != MPI_SUCCESS) {
        return started;
    }
    return waitPolled(std::span<MPI_Request>(&request, 1), pause_min, pause_max);
    // NOLINTEND(clang-analyzer-optin.mpi.MPI-Checker)
}

/******************************************************************************/

} /* namespace Gem::Courtier */
