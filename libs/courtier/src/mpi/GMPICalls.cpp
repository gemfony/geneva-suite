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

#include "courtier/mpi/GMPICalls.hpp"

// Standard headers go here
#include <climits>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"

namespace Gem::Courtier::Consumers {

namespace {

// The handle carries the bytes of the MPI_Request (an int in MPICH, a pointer in Open MPI), XORed
// with those of MPI_REQUEST_NULL.
static_assert(
    sizeof(MPI_Request) <= sizeof(std::uintptr_t),
    "an MPI_Request must fit into a GMPIRequest"
);
static_assert(
    std::is_trivially_copyable_v<MPI_Request>,
    "an MPI_Request must be trivially copyable"
);

/******************************************************************************/
/** @param request An MPI request @return Its bytes, in an otherwise zero std::uintptr_t */
std::uintptr_t bytesOf(MPI_Request request) {
    std::uintptr_t bytes = 0;
    std::memcpy(&bytes, static_cast<const void *>(&request), sizeof(MPI_Request));
    return bytes;
}

/******************************************************************************/
/**
 * The handle is the request's bytes XOR those of MPI_REQUEST_NULL, so that NONE (0) is
 * MPI_REQUEST_NULL by construction and every other request has a handle that is not NONE.
 *
 * @param request An MPI request
 * @return Its handle; NONE for MPI_REQUEST_NULL
 */
GMPIRequest toHandle(MPI_Request request) {
    // An opaque handle, as std::byte is: every value is valid, not only the named NONE.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<GMPIRequest>(bytesOf(request) ^ bytesOf(MPI_REQUEST_NULL));
}

/******************************************************************************/
/**
 * Undoes toHandle(): the same XOR with the bytes of MPI_REQUEST_NULL.
 *
 * @param handle A handle made by toHandle()
 * @return The MPI request; MPI_REQUEST_NULL for NONE
 */
MPI_Request toRequest(GMPIRequest handle) {
    MPI_Request request = MPI_REQUEST_NULL;
    const std::uintptr_t bytes = static_cast<std::uintptr_t>(handle) ^ bytesOf(MPI_REQUEST_NULL);
    std::memcpy(static_cast<void *>(&request), &bytes, sizeof(MPI_Request));
    return request;
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * Makes the calls on courtier's communicator of @p environment.
 *
 * @param environment The process's MPI environment
 * @throws gem_exception if @p environment is null or belongs to a bystander (no communicator)
 */
GMPICalls::GMPICalls(std::shared_ptr<GMPIEnvironment> environment)
  : environment_(std::move(environment))
  , communicator_(environment_ ? environment_->communicator() : MPI_COMM_NULL)
  , rank_(communicator_ != MPI_COMM_NULL ? environment_->rank() : -1)
  , size_(communicator_ != MPI_COMM_NULL ? environment_->size() : 0) {
    if(communicator_ == MPI_COMM_NULL) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "No environment, or the environment of a process that takes no part in courtier's "
            << "protocol" << '\n'
        );
    }
}

/******************************************************************************/
/** @return This process's rank in courtier's communicator */
int GMPICalls::rank() const {
    return rank_;
}

/******************************************************************************/
/** @return The number of ranks of courtier's communicator */
int GMPICalls::size() const {
    return size_;
}

/******************************************************************************/
/**
 * Looks once for a message from any rank with any tag (MPI_Iprobe) and asks for its length
 * (MPI_Get_count).
 *
 * @return The message's source, tag and length, or nothing if no message is there
 * @throws gem_exception for a message whose length MPI cannot count in an int
 */
std::optional<GMPIProbed> GMPICalls::probe() {
    int available = 0;
    MPI_Status status{};
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, communicator_, &available, &status);
    if(available == 0) {
        return std::nullopt;
    }
    int count = 0;
    MPI_Get_count(&status, MPI_CHAR, &count);
    if(count == MPI_UNDEFINED || count < 0) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer() << "The message from rank " << status.MPI_SOURCE
                                            << " is longer than INT_MAX bytes" << '\n'
        );
    }
    return GMPIProbed{
        .source = status.MPI_SOURCE,
        .tag = status.MPI_TAG,
        .size = static_cast<std::size_t>(count)
    };
}

/******************************************************************************/
/**
 * Receives the message the last probe() reported (MPI_Recv with its source and tag). As only one
 * caller receives on the communicator, it is that message.
 *
 * @param probed What probe() reported
 * @return The message's bytes
 */
std::string GMPICalls::receive(const GMPIProbed &probed) {
    std::string message(probed.size, '\0');
    MPI_Recv(
        message.data(),
        static_cast<int>(probed.size),
        MPI_CHAR,
        probed.source,
        probed.tag,
        communicator_,
        MPI_STATUS_IGNORE
    );
    return message;
}

/******************************************************************************/
/**
 * Starts the send of a message (MPI_Isend). The bytes must stay where they are until the send has
 * completed or has been given up.
 *
 * @param rank The receiving rank
 * @param tag The kind of message
 * @param bytes The message
 * @return The handle of the send
 * @throws gem_exception for a message longer than INT_MAX bytes
 */
GMPIRequest GMPICalls::startSend(int rank, int tag, std::string_view bytes) {
    if(bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer() << "A message of " << bytes.size()
                                            << " bytes is longer than MPI can send at once" << '\n'
        );
    }
    MPI_Request request = MPI_REQUEST_NULL;
    MPI_Isend(
        bytes.data(),
        static_cast<int>(bytes.size()),
        MPI_CHAR,
        rank,
        tag,
        communicator_,
        &request
    );
    // The request is completed by testSome() or given up by giveUp(), through its handle.
    return toHandle(request); // NOLINT(clang-analyzer-optin.mpi.MPI-Checker)
}

/******************************************************************************/
/**
 * Looks which of the sends have completed (MPI_Testsome), without waiting. Every one that has is
 * set to NONE; MPI has freed its request.
 *
 * @param requests The sends; NONE entries are skipped
 * @return How many completed
 */
std::size_t GMPICalls::testSome(std::span<GMPIRequest> requests) {
    if(requests.empty()) {
        return 0;
    }
    requests_.resize(requests.size());
    indices_.resize(requests.size());
    for(std::size_t i = 0; i < requests.size(); ++i) {
        requests_[i] = toRequest(requests[i]);
    }
    int n_completed = 0;
    MPI_Testsome(
        static_cast<int>(requests_.size()),
        requests_.data(),
        &n_completed,
        indices_.data(),
        MPI_STATUSES_IGNORE
    );
    if(n_completed == MPI_UNDEFINED || n_completed <= 0) {
        return 0;
    }
    for(int i = 0; i < n_completed; ++i) {
        requests[static_cast<std::size_t>(indices_[static_cast<std::size_t>(i)])] =
            GMPIRequest::NONE;
    }
    return static_cast<std::size_t>(n_completed);
}

/******************************************************************************/
/**
 * Gives up a send that has not completed: MPI_Cancel, then MPI_Request_free. The send goes to a
 * rank that does not receive; whether the cancel succeeds is MPI's business, and a request that is
 * freed while active may still be read by MPI. So the bytes are kept for the rest of the process:
 * they are released from their owner on purpose and never freed. This happens only for a rank that
 * hangs or left without reading, at the end of the transport's work.
 *
 * @param handle The send
 * @param bytes Its bytes
 */
void GMPICalls::giveUp(GMPIRequest handle, std::unique_ptr<std::string> bytes) {
    MPI_Request request = toRequest(handle);
    if(request != MPI_REQUEST_NULL) {
        MPI_Cancel(&request);
        MPI_Request_free(&request);
    }
    // NOLINTNEXTLINE(bugprone-unused-return-value) -- kept on purpose, see above
    static_cast<void>(bytes.release());
}

/******************************************************************************/
/** @param rank A rank that has fallen silent: the environment ends the job where it would finalize
 *  MPI (GMPIEnvironment::noteLostRank()) */
void GMPICalls::noteLostRank([[maybe_unused]] int rank) {
    environment_->noteLostRank();
}

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
