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

// Standard headers go here
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/******************************************************************************/
/**
 * @file
 * The MPI calls the MPI transport makes, behind an interface of their own. The transport's poll
 * (GMPIPollCore) is written against it and includes no mpi.h, so that it is compiled into
 * libgemfony-courtier, which links no MPI, and is tested with an in-memory stand-in under
 * ThreadSanitizer. The binding to MPI (Gem::Courtier::GMPICalls, courtier/mpi/GMPICalls.hpp) lives
 * in libgemfony-courtier-mpi.
 */

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * An opaque handle for a send that has been started (an MPI_Request in the binding). NONE stands for
 * no request: a send that has completed, or none at all.
 */
enum class GMPIRequest : std::uintptr_t {
    NONE = 0
};

/******************************************************************************/
/** @brief A message that has arrived and has not been received yet: what a probe tells. */
struct GMPIProbed {
    int source = -1;      ///< The sending rank
    int tag = -1;         ///< The kind of message
    std::size_t size = 0; ///< Its length in bytes
};

/******************************************************************************/
/**
 * The calls of the MPI transport on courtier's communicator, one per MPI function it needs (or two,
 * where they are always made together). Every call refers to that one communicator.
 *
 * Threads: startSend() may be called from several threads at once (MPI_THREAD_MULTIPLE); probe(),
 * receive(), testSome(), giveUp() and noteLostRank() are called by one thread at a time (the poll's
 * strand). A
 * probe() that reports a message is followed by the receive() of exactly that message, before any
 * other probe() or receive(): only one caller receives on the communicator, so a probe and the
 * receive that follows it always mean the same message.
 *
 * Errors: the binding sets MPI_ERRORS_ARE_FATAL on the communicator, so the calls do not report
 * MPI errors; a call that is used against its contract throws.
 */
class GMPICallsI {
public:
    /** @brief The default constructor */
    GMPICallsI() = default;
    /** @brief The destructor */
    virtual ~GMPICallsI() = default;

    GMPICallsI(const GMPICallsI &) = delete;
    GMPICallsI(GMPICallsI &&) = delete;
    GMPICallsI &operator=(const GMPICallsI &) = delete;
    GMPICallsI &operator=(GMPICallsI &&) = delete;

    /** @brief This process's rank in the communicator (MPI_Comm_rank); for the log */
    [[nodiscard]] virtual int rank() const = 0;

    /** @brief The number of ranks of the communicator (MPI_Comm_size); the poll keeps one
     *  receiver per rank */
    [[nodiscard]] virtual int size() const = 0;

    /** @brief Looks once, without waiting, for a message from any rank with any tag (MPI_Iprobe,
     *  then MPI_Get_count for its length) */
    [[nodiscard]] virtual std::optional<GMPIProbed> probe() = 0;

    /** @brief Receives the message the last probe() reported (MPI_Recv) */
    [[nodiscard]] virtual std::string receive(const GMPIProbed &) = 0;

    /** @brief Starts the send of a message (MPI_Isend); the bytes must stay where they are until the
     *  send has completed or has been given up. At most INT_MAX bytes. */
    [[nodiscard]] virtual GMPIRequest startSend(int, int, std::string_view) = 0;

    /** @brief Looks, without waiting, which of the sends have completed (MPI_Testsome); each one
     *  that has is set to NONE and frees its request */
    virtual std::size_t testSome(std::span<GMPIRequest>) = 0;

    /** @brief Gives up a send that has not completed (MPI_Cancel, then MPI_Request_free) and takes
     *  its bytes, which the binding keeps for as long as the transport may still read them */
    virtual void giveUp(GMPIRequest, std::unique_ptr<std::string>) = 0;

    /** @brief Notes that a rank has fallen silent (the poll's keep-alive): MPI_Finalize would wait
     *  for it, so the binding has the job ended instead (GMPIEnvironment). Called by the poll's
     *  thread; no MPI call. */
    virtual void noteLostRank(int) = 0;
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
