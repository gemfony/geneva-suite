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

// No guard on GEM_BUILD_WITH_MPI_CONSUMER: only a program that uses MPI includes this header,
// and that definition belongs to the collection's own build, which a program outside it does not
// have.

// Standard headers go here
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string_view>
#include <tuple>

// MPI headers go here
#include <mpi.h>

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "courtier/GCourtierEnums.hpp"

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * @brief Ends the MPI job: logs @p text as a warning, flushes the program's streams and calls
 * MPI_Abort on @p communicator with @p code, which ends every process of the job. Does not
 * return. The one way courtier ends a job (GMPICascade, ~GMPIEnvironment()).
 *
 * @param communicator The communicator whose processes are ended
 * @param text What is logged
 * @param code The exit code of the job (the error code of MPI_Abort)
 */
[[noreturn]] inline void
endMpiJob(MPI_Comm communicator, std::string_view text, int code) noexcept {
    try {
        glogger << text << '\n' << GWARNING;
    }
    catch(...) { // NOLINT(bugprone-empty-catch): the job ends in any case
    }
    std::cout.flush();
    std::cerr.flush();
    std::ignore = MPI_Abort(communicator, code); // does not return; nothing to do if it did
    std::abort();                                // not reached: MPI_Abort does not return
}

/******************************************************************************/
/**
 * What the MPI transport needs to know about the process it runs in: MPI is initialized with the
 * thread level the transport needs, there is a communicator that belongs to courtier alone, and
 * the process has a role on it. One object states all of that; the consumer and the client hold
 * it for as long as they use MPI. A process has one such object, which mpiEnvironment() and its
 * siblings in courtier/mpi/GMPIEnvironmentAccess.hpp (libgemfony-courtier-mpi) hand out.
 *
 * The constructor does everything, so there is no state to set afterwards:
 *  - It initializes MPI with MPI_THREAD_MULTIPLE unless that has been done, and checks the level
 *    in either case. The master calls MPI from several threads at once, and a cascaded client
 *    calls it from its evaluation while the transport polls.
 *  - It duplicates the base communicator. Courtier's traffic is thereby separate from whatever
 *    the application sends on the base communicator; the transport receives from any source with
 *    any tag and would otherwise take the application's messages. The duplicate is made here and
 *    nowhere else, because duplicating is a collective call: every process of the base
 *    communicator must construct its environment, or the others wait for it. A program in which
 *    only some processes of MPI_COMM_WORLD take part passes the communicator of those that do.
 *  - A process that takes no part in courtier's protocol (in a cascade: a member; in a cascade
 *    laid out GROUPS_ONLY, every process) passes MPI_COMM_NULL. It has the role BYSTANDER and no
 *    communicator. A cascade builds the environment itself (courtier/mpi/GMPICascade.hpp).
 *
 * A consumer built on the submitter notes that it exists (noteConsumer()): from then on its end
 * tells the clients that the run has ended, however it ends. A cascade on the submitter asks for
 * it (consumerBuilt()) when an exception unwinds through it (GMPICascade).
 *
 * The destructor frees the duplicate and finalizes MPI if this object initialized it. A program
 * that initialized MPI itself finalizes it itself, and must let go of its consumer or client
 * before it does: the destructor does nothing once MPI is finalized.
 *
 * A rank that the transport's keep-alive has found silent (noteLostRank()) would keep MPI_Finalize
 * waiting for ever: the destructor then ends the job (MPI_Abort on the base communicator, exit code
 * EXIT_FAILURE) instead of finalizing, after the program's own output -- on the submitter after the
 * run has finished with the other clients, on a client after its run() has returned. A program
 * that initialized MPI itself is told in the log that its MPI_Finalize will wait for the lost
 * ranks.
 *
 * MPI errors: the duplicate gets MPI_ERRORS_ARE_FATAL, whatever the base communicator has. The
 * transport does not check the return values of its MPI calls.
 */
class GMPIEnvironment {
public:
    /***************************************************************************/
    /**
     * @brief Makes MPI ready for courtier and gives this process its role.
     *
     * @param argc Pointer to the program's argc for MPI_Init_thread (may be nullptr)
     * @param argv Pointer to the program's argv for MPI_Init_thread (may be nullptr)
     * @param base The communicator of the processes that take part; MPI_COMM_NULL for a process
     *             that takes no part
     * @throws gem_exception if MPI has been finalized, or if the thread level is below
     *         MPI_THREAD_MULTIPLE
     */
    explicit GMPIEnvironment(
        int *argc = nullptr,
        char ***argv = nullptr,
        MPI_Comm base = MPI_COMM_WORLD
    )
      : GMPIEnvironment(argc, argv, [base] { return base; }) {
    }

    /***************************************************************************/
    /** @brief As the other constructor, for a base communicator that can only be made once MPI
     *  is initialized: @p make_base is called after the initialization and returns it.
     *
     * @param argc Pointer to the program's argc for MPI_Init_thread (may be nullptr)
     * @param argv Pointer to the program's argv for MPI_Init_thread (may be nullptr)
     * @param make_base Called once MPI is initialized and its thread level is checked; returns the
     *                 communicator of the processes that take part, or MPI_COMM_NULL for a process
     *                 that takes no part
     * @throws gem_exception if MPI has been finalized, or if the thread level is below
     *         MPI_THREAD_MULTIPLE; whatever @p make_base throws. In each case MPI is finalized
     *         again if this constructor initialized it.
     */
    GMPIEnvironment(int *argc, char ***argv, const std::function<MPI_Comm()> &make_base) {
        int finalized = 0;
        MPI_Finalized(&finalized);
        if(finalized != 0) {
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIEnvironment::GMPIEnvironment(): Error!" << '\n'
                << "MPI has been finalized in this process and cannot be used again." << '\n'
            );
        }

        int initialized = 0;
        MPI_Initialized(&initialized);
        int level = 0;
        if(initialized == 0) {
            MPI_Init_thread(argc, argv, MPI_THREAD_MULTIPLE, &level);
            i_initialized_mpi_ = true;
        }
        else {
            MPI_Query_thread(&level);
        }
        if(level != MPI_THREAD_MULTIPLE) {
            if(i_initialized_mpi_) {
                MPI_Finalize(); // nobody else will: the destructor does not run for this object
            }
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIEnvironment::GMPIEnvironment(): Error!" << '\n'
                << "courtier's MPI transport requires MPI with the thread level "
                << "MPI_THREAD_MULTIPLE (a.k.a. " << MPI_THREAD_MULTIPLE << "), but "
                << (i_initialized_mpi_ ? "the runtime environment only supports level "
                                       : "the calling program initialized MPI with level ")
                << level << '\n'
                << (i_initialized_mpi_
                        ? ""
                        : "Initialize MPI with MPI_Init_thread() and MPI_THREAD_MULTIPLE, or "
                          "leave the initialization to courtier.\n")
            );
        }

        try {
            base_ = make_base();
        }
        catch(...) {
            if(i_initialized_mpi_) {
                MPI_Finalize(); // as above: the destructor does not run for this object
            }
            throw;
        }

        if(base_ != MPI_COMM_NULL) {
            MPI_Comm_dup(base_, &communicator_);
            MPI_Comm_set_errhandler(communicator_, MPI_ERRORS_ARE_FATAL);
            MPI_Comm_rank(communicator_, &rank_);
            MPI_Comm_size(communicator_, &size_);
        }
    }

    /***************************************************************************/
    /** @brief Frees courtier's communicator and finalizes MPI if this object initialized it. Does
     *  nothing if MPI has been finalized already. */
    ~GMPIEnvironment() {
        int finalized = 0;
        MPI_Finalized(&finalized);
        if(finalized != 0) {
            return;
        }
        if(lost_rank_.load() && i_initialized_mpi_) {
            endMpiJob(
                base_,
                "ranks were lost during the run; MPI_Finalize would wait for them: the job is "
                "ended",
                EXIT_FAILURE
            );
        }
        if(lost_rank_.load()) {
            try {
                glogger << "ranks were lost during the run: the program's MPI_Finalize will wait "
                           "for them"
                        << '\n'
                        << GWARNING;
            }
            catch(...) { // NOLINT(bugprone-empty-catch): nothing to do about a log line
            }
        }
        if(communicator_ != MPI_COMM_NULL) {
            MPI_Comm_free(&communicator_);
        }
        if(i_initialized_mpi_) {
            MPI_Finalize();
        }
    }

    GMPIEnvironment(const GMPIEnvironment &) = delete;
    GMPIEnvironment(GMPIEnvironment &&) = delete;
    GMPIEnvironment &operator=(const GMPIEnvironment &) = delete;
    GMPIEnvironment &operator=(GMPIEnvironment &&) = delete;

    /***************************************************************************/
    /** @brief The communicator courtier works on.
     *  @return A duplicate of the base communicator, or MPI_COMM_NULL on a bystander */
    [[nodiscard]] MPI_Comm communicator() const noexcept {
        return communicator_;
    }

    /** @brief The communicator this environment was built for.
     *  @return The base communicator as it was given to the constructor */
    [[nodiscard]] MPI_Comm base() const noexcept {
        return base_;
    }

    /** @brief What this process does in courtier's protocol.
     *  @return BYSTANDER without a communicator, SUBMITTER on rank RANK_MASTER_NODE of it,
     *          CLIENT on every other rank */
    [[nodiscard]] Gem::Courtier::processRole role() const noexcept {
        if(communicator_ == MPI_COMM_NULL) {
            return Gem::Courtier::processRole::BYSTANDER;
        }
        return rank_ == RANK_MASTER_NODE ? Gem::Courtier::processRole::SUBMITTER
                                         : Gem::Courtier::processRole::CLIENT;
    }

    /** @brief The rank of this process in courtier's communicator.
     *  @return The rank
     *  @throws gem_exception on a bystander */
    [[nodiscard]] int rank() const {
        this->requireCommunicator_("rank()");
        return rank_;
    }

    /** @brief Notes that a consumer was built on this process; its end tells the clients. Any
     *  thread. */
    void noteConsumer() noexcept {
        consumer_built_ = true;
    }

    /** @brief Notes that a rank of courtier's communicator was lost (it fell silent): the
     *  destructor ends the job instead of finalizing MPI (see the class comment). Any thread. */
    void noteLostRank() noexcept {
        lost_rank_.store(true);
    }

    /** @brief Whether a consumer was built on this process (noteConsumer()). Any thread.
     *  @return true once noteConsumer() was called */
    [[nodiscard]] bool consumerBuilt() const noexcept {
        return consumer_built_.load();
    }

    /** @brief The number of processes in courtier's communicator.
     *  @return The size
     *  @throws gem_exception on a bystander */
    [[nodiscard]] int size() const {
        this->requireCommunicator_("size()");
        return size_;
    }

private:
    /***************************************************************************/
    /** @brief Refuses a question that has no answer on a bystander.
     *  @param what The function that was asked */
    void requireCommunicator_(const char *what) const {
        if(communicator_ == MPI_COMM_NULL) {
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIEnvironment::" << what << ": Error!" << '\n'
                << "This process takes no part in courtier's MPI protocol and has no "
                << "communicator." << '\n'
            );
        }
    }

    MPI_Comm base_ = MPI_COMM_NULL;         ///< The communicator the constructor was given
    MPI_Comm communicator_ = MPI_COMM_NULL; ///< Courtier's duplicate of it
    int rank_ = -1;                         ///< The rank in communicator_; -1 on a bystander
    int size_ = 0;                          ///< The size of communicator_; 0 on a bystander
    bool i_initialized_mpi_ = false;        ///< Whether the constructor initialized MPI
    /// A consumer was built in this process (noteConsumer())
    std::atomic<bool> consumer_built_{false};
    /// A rank was lost during the run (noteLostRank())
    std::atomic<bool> lost_rank_{false};
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
