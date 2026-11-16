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
 * The watch of the cascade's test program; see GMPICallWatch.hpp. The signatures are those of the
 * host's mpi.h (Open MPI 5).
 */

#include "GMPICallWatch.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <print>
#include <set>
#include <tuple>
#include <vector>

#include <mpi.h>

namespace {

/** @brief The bytes of an MPI handle as a number (an int in MPICH, a pointer in Open MPI)
 *  @param handle The handle @return Its bytes */
template <typename T>
std::uintptr_t keyOf(const T &handle) {
    static_assert(sizeof(T) <= sizeof(std::uintptr_t));
    std::uintptr_t key = 0;
    std::memcpy(&key, static_cast<const void *>(&handle), sizeof(T));
    return key;
}

/** @brief The counters and sets of the watch; several threads call MPI on a lead */
struct Watch {
    std::atomic<std::size_t> initThreads{0};
    std::atomic<std::size_t> ibcasts{0};
    std::atomic<std::size_t> collectivesStarted{0};
    std::atomic<std::size_t> splits{0};
    std::atomic<std::size_t> communicatorsMade{0};
    std::atomic<bool> failIbcastStart{false}; ///< The next MPI_Ibcast fails at its start
    std::atomic<bool> failTestall{false};     ///< The next MPI_Testall fails
    /// Guards the two sets, and is held from an MPI call to the bookkeeping of its handles: MPI may
    /// give a handle that another thread's call has just freed to a new request, and its insertion
    /// must not come between that call and the erasure of the old one (a data guard of the watch)
    std::mutex mutex;
    std::set<std::uintptr_t> pendingCollectives; ///< Started non-blocking collectives not completed
    std::set<std::uintptr_t> aliveCommunicators; ///< Communicators made and not freed
};

/** @brief The one watch of the process @return It */
Watch &watch() {
    static Watch the_watch;
    return the_watch;
}

/** @brief Notes a started non-blocking collective; the caller holds the watch's mutex
 *  @param result What the start returned @param request Its request @return @p result */
int started(int result, const MPI_Request *request) {
    if(result == MPI_SUCCESS && *request != MPI_REQUEST_NULL) {
        ++watch().collectivesStarted;
        watch().pendingCollectives.insert(keyOf(*request));
    }
    return result;
}

/** @brief Notes a communicator that was made
 *  @param result What the call returned @param communicator The new communicator @return @p result */
int made(int result, const MPI_Comm *communicator) {
    if(result == MPI_SUCCESS && *communicator != MPI_COMM_NULL) {
        ++watch().communicatorsMade;
        const std::scoped_lock lock(watch().mutex);
        watch().aliveCommunicators.insert(keyOf(*communicator));
    }
    return result;
}

} // namespace

namespace Gem::Tests::CallWatch {

std::size_t watchedInitThreads() {
    return watch().initThreads.load();
}
std::size_t watchedIbcasts() {
    return watch().ibcasts.load();
}
std::size_t watchedCollectivesStarted() {
    return watch().collectivesStarted.load();
}
std::size_t watchedCollectivesPending() {
    const std::scoped_lock lock(watch().mutex);
    return watch().pendingCollectives.size();
}
std::size_t watchedSplits() {
    return watch().splits.load();
}
std::size_t watchedCommunicatorsAlive() {
    const std::scoped_lock lock(watch().mutex);
    return watch().aliveCommunicators.size();
}

void failNextIbcastStart() {
    watch().failIbcastStart = true;
}
void failNextTestall() {
    watch().failTestall = true;
}

} /* namespace Gem::Tests::CallWatch */

// NOLINTBEGIN(readability-identifier-naming,readability-inconsistent-declaration-parameter-name)
// The MPI functions the watch sees: the names and parameters are MPI's.
extern "C" {

int MPI_Init_thread(int *argc, char ***argv, int required, int *provided) {
    ++watch().initThreads;
    return PMPI_Init_thread(argc, argv, required, provided);
}

int MPI_Ibcast(
    void *buffer,
    int count,
    MPI_Datatype datatype,
    int root,
    MPI_Comm comm,
    MPI_Request *request
) {
    ++watch().ibcasts;
    if(watch().failIbcastStart.exchange(false)) {
        return MPI_ERR_OTHER;
    }
    const std::scoped_lock lock(watch().mutex); // from the start to the bookkeeping
    return started(PMPI_Ibcast(buffer, count, datatype, root, comm, request), request);
}

int MPI_Iscatter( // NOLINT(readability-function-size): MPI's parameters
    const void *sendbuf,
    int sendcount,
    MPI_Datatype sendtype,
    void *recvbuf,
    int recvcount,
    MPI_Datatype recvtype,
    int root,
    MPI_Comm comm,
    MPI_Request *request
) {
    const std::scoped_lock lock(watch().mutex); // from the start to the bookkeeping
    return started(
        PMPI_Iscatter(
            sendbuf,
            sendcount,
            sendtype,
            recvbuf,
            recvcount,
            recvtype,
            root,
            comm,
            request
        ),
        request
    );
}

int MPI_Igather( // NOLINT(readability-function-size): MPI's parameters
    const void *sendbuf,
    int sendcount,
    MPI_Datatype sendtype,
    void *recvbuf,
    int recvcount,
    MPI_Datatype recvtype,
    int root,
    MPI_Comm comm,
    MPI_Request *request
) {
    const std::scoped_lock lock(watch().mutex); // from the start to the bookkeeping
    return started(
        PMPI_Igather(
            sendbuf,
            sendcount,
            sendtype,
            recvbuf,
            recvcount,
            recvtype,
            root,
            comm,
            request
        ),
        request
    );
}

int MPI_Iallreduce(
    const void *sendbuf,
    void *recvbuf,
    int count,
    MPI_Datatype datatype,
    MPI_Op op,
    MPI_Comm comm,
    MPI_Request *request
) {
    const std::scoped_lock lock(watch().mutex); // from the start to the bookkeeping
    return started(PMPI_Iallreduce(sendbuf, recvbuf, count, datatype, op, comm, request), request);
}

int MPI_Ibarrier(MPI_Comm comm, MPI_Request *request) {
    const std::scoped_lock lock(watch().mutex); // from the start to the bookkeeping
    return started(PMPI_Ibarrier(comm, request), request);
}

int MPI_Testall(
    int count,
    MPI_Request array_of_requests[],
    int *flag,
    MPI_Status array_of_statuses[]
) {
    if(watch().failTestall.exchange(false)) {
        return MPI_ERR_OTHER;
    }
    // MPI sets a completed request to MPI_REQUEST_NULL: note the handles before the call
    std::vector<std::uintptr_t> before;
    before.reserve(static_cast<std::size_t>(count));
    for(int i = 0; i < count; ++i) {
        if(array_of_requests[i] != MPI_REQUEST_NULL) {
            before.push_back(keyOf(array_of_requests[i]));
        }
    }
    const std::scoped_lock lock(watch().mutex); // from the test to the bookkeeping
    const int result = PMPI_Testall(count, array_of_requests, flag, array_of_statuses);
    if(result == MPI_SUCCESS && *flag != 0) {
        for(const auto key : before) {
            watch().pendingCollectives.erase(key);
        }
    }
    return result;
}

int MPI_Comm_split(MPI_Comm comm, int color, int key, MPI_Comm *newcomm) {
    ++watch().splits;
    return made(PMPI_Comm_split(comm, color, key, newcomm), newcomm);
}

int MPI_Comm_dup(MPI_Comm comm, MPI_Comm *newcomm) {
    return made(PMPI_Comm_dup(comm, newcomm), newcomm);
}

int MPI_Comm_free(MPI_Comm *comm) {
    const auto key = keyOf(*comm);
    const int result = PMPI_Comm_free(comm);
    if(result == MPI_SUCCESS) {
        const std::scoped_lock lock(watch().mutex);
        watch().aliveCommunicators.erase(key);
    }
    return result;
}

int MPI_Finalize() {
    int rank = -1;
    PMPI_Comm_rank(MPI_COMM_WORLD, &rank);
    std::size_t pending = 0;
    std::size_t alive = 0;
    {
        const std::scoped_lock lock(watch().mutex);
        pending = watch().pendingCollectives.size();
        alive = watch().aliveCommunicators.size();
    }
    const bool ok = pending == 0 && alive == 0;
    if(ok) {
        std::println(
            "OK: rank {}: {} non-blocking collectives started and completed, {} communicators made "
            "and freed",
            rank,
            watch().collectivesStarted.load(),
            watch().communicatorsMade.load()
        );
    }
    else {
        std::println(
            "FAIL: rank {}: {} non-blocking collective(s) not completed, {} communicator(s) not "
            "freed",
            rank,
            pending,
            alive
        );
    }
    std::ignore = std::fflush(stdout);
    const int result = PMPI_Finalize();
    if(not ok) {
        std::_Exit(1);
    }
    return result;
}

} // extern "C"
// NOLINTEND(readability-identifier-naming,readability-inconsistent-declaration-parameter-name)
