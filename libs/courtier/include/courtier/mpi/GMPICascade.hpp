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

/*
 * The MPI cascade: a client of courtier shares each evaluation with a small group of MPI processes,
 * its members. A member runs this library and the user's member half, nothing else of the
 * collection. Part of libgemfony-courtier-mpi.
 */

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// No guard on GENEVA_BUILD_WITH_MPI_CONSUMER: see courtier/mpi/GMPIEnvironment.hpp.

// Standard headers go here
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

// MPI headers go here
#include <mpi.h>

// Geneva headers go here
#include "courtier/GCourtierEnums.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp"
#include "courtier/mpi/GMPIEnvironment.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * What a process of a group does once before its first round and once after its last. The lead
 * (GMPICascade::lead(), GMPICascade::release()) and the members (GMPICascade::serve()) run it as a
 * pair, in step; it is typically collective on the group. An exception out of either function ends
 * the MPI job (see GMPICascade).
 */
class GMPIGroupLifecycleI {
public:
    /** @brief The destructor */
    virtual ~GMPIGroupLifecycleI() = default;

    /** @brief Called on the group's communicator once, before the first round */
    virtual void setUp([[maybe_unused]] MPI_Comm group) { /* nothing */
    }
    /** @brief Called on the group's communicator once, after the last round */
    virtual void tearDown([[maybe_unused]] MPI_Comm group) { /* nothing */
    }
};

/******************************************************************************/
/**
 * The member's half of a group evaluation: serveRound() is called on every member of a group, in
 * step with the lead's half of the same round (the callable the lead passes to
 * GMPICascade::round()). An exception out of it ends the MPI job (see GMPICascade).
 */
class GMPIGroupMemberI : public GMPIGroupLifecycleI {
public:
    /** @brief Serves one round on the group's communicator */
    virtual void serveRound(MPI_Comm group) = 0;
};

/******************************************************************************/
/**
 * The MPI cascade of this process: it splits the processes of a base communicator into groups of
 * GMPICascadeConfig::groupSize processes, a lead and its members, and runs the rounds in which a
 * lead and its members evaluate one item together.
 *
 * <b>The two layouts</b> (GMPICascadeConfig::layout, set by the program; base rank r, group size g):
 *
 * | layout               | base rank                     | role (role())        | group                |
 * |----------------------|-------------------------------|----------------------|----------------------|
 * | SUBMITTER_AND_GROUPS | 0                             | SUBMITTER            | none                 |
 * |                      | r > 0 with (r - 1) % g == 0   | CLIENT (a lead)      | (r - 1) / g, rank 0  |
 * |                      | every other r > 0             | BYSTANDER (a member) | (r - 1) / g          |
 * | GROUPS_ONLY          | r with r % g == 0             | CLIENT (a lead)      | r / g, rank 0        |
 * |                      | every other r                 | BYSTANDER (a member) | r / g                |
 *
 * In SUBMITTER_AND_GROUPS the submitter and the leads form courtier's communicator: a consumer
 * built afterwards finds rank 0 as SUBMITTER and every lead as CLIENT (mpiProcessRole() equals
 * role()). In GROUPS_ONLY no process takes part in courtier's MPI protocol (mpiProcessRole() is
 * BYSTANDER everywhere); a lead reaches its work by another transport, a websocket client for
 * example, or by none. A last group with fewer than g processes is allowed.
 *
 * <b>Construction.</b> Every process of the base communicator constructs the cascade at the same
 * point (the splits are collective; a process that does not, keeps the others waiting), and before
 * the process's MPI environment exists: before the first call of mpiEnvironment() or
 * mpiProcessRole(), and so before any MPI consumer or client is built. Otherwise every rank throws
 * "An MPI environment exists already". The cascade builds the environment itself
 * (mpiEnvironmentOn()), which initializes MPI with MPI_THREAD_MULTIPLE unless the program has done
 * so. Only rank 0 of the base communicator calls the configuration callable; its values are
 * broadcast and checked on every rank alike, so that a bad configuration is refused by every rank
 * with the same text. The communicators the cascade makes get MPI_ERRORS_ARE_FATAL.
 *
 * <b>No MPI.</b> If the program has entered configuration emission (--update-configs,
 * Gem::Common::beginConfigEmission()), the cascade makes no MPI call: it calls the configuration
 * callable (which writes its file), checks the result and has the role SUBMITTER without a group;
 * serve(), lead() and round() throw, release() and the destructor do nothing.
 *
 * <b>The rounds.</b> round() announces a round to the members (a "go" broadcast on a communicator
 * of the group's own) and calls the lead's half; every member's serve() waits for the
 * announcement, polled without waiting busily, and calls GMPIGroupMemberI::serveRound(). release()
 * announces the stop, after which serve() returns. A lead that serves a client of courtier calls
 * round() from the client's item processor (setItemProcessor() of the client classes), once per
 * item.
 *
 * <b>Threads.</b> The constructor, serve(), lead(), requireRole() and the destructor are called by
 * the program's main thread. round() may be called by any thread; rounds run one after the other
 * (a lock is held from the announcement to the return of the lead's half). release() may be called
 * by any thread, not concurrently with the destructor; it waits for a round that runs, and a later
 * round() throws. role(), group() and config() may be called by any thread.
 *
 * <b>Lifetime.</b> The member and lifecycle objects passed to serve() and lead() are declared
 * before the cascade, so that they outlive it. The cascade outlives every call of an item
 * processor that uses it (a client's run() returns only once no evaluation runs any more). A
 * consumer or client that uses the MPI environment is declared after the cascade, so that it goes
 * before the cascade frees its communicators. The program must not finalize MPI while the cascade
 * lives; if it has, the destructor does nothing.
 *
 * <b>The role must be confirmed.</b> Every lead and the submitter call requireRole() with the
 * role their consumer gave them; a wrong consumer (for example a lead without --client) is then
 * refused instead of running nothing while its members idle. A program without a consumer, which
 * calls round() itself, confirms with requireRole(processRole::CLIENT). A lead that has not
 * confirmed its role is refused at its first lead() or round(): the cascade releases its group
 * (the members' serve() returns) and throws, so that no process is left waiting. A lead or the
 * submitter that never confirmed its role and never used the cascade ends the MPI job in the
 * destructor (unless an exception is unwinding), the last safeguard.
 *
 * <b>Failures.</b> An exception out of a lifecycle function, a member half or a lead half leaves
 * the group out of step, which cannot be repaired: the cascade logs it and ends the MPI job with
 * MPI_Abort on the base communicator. In SUBMITTER_AND_GROUPS that is the whole optimization, in
 * GROUPS_ONLY one client job. A member that leaves without serve() ends the job the same way (its
 * lead would wait for ever). A call made by the wrong role, before requireRole() or after
 * release(), throws a geneva_exception before anything is announced, so the group stays in step.
 *
 * One per process; not copyable, not movable.
 */
class GMPICascade {
public:
    /** @brief Splits @p base into the cascade's groups; collective on @p base (see the class) */
    explicit GMPICascade(std::function<GMPICascadeConfig()>, MPI_Comm = MPI_COMM_WORLD);
    /** @brief Releases a lead's group, frees the communicators and lets go of the environment */
    ~GMPICascade();

    GMPICascade(const GMPICascade &) = delete;
    GMPICascade(GMPICascade &&) = delete;
    GMPICascade &operator=(const GMPICascade &) = delete;
    GMPICascade &operator=(GMPICascade &&) = delete;

    /** @brief The cascade's role of this process: SUBMITTER, CLIENT (a lead), BYSTANDER (a member) */
    [[nodiscard]] processRole role() const noexcept;
    /** @brief The group's communicator; MPI_COMM_NULL on the submitter and without MPI */
    [[nodiscard]] MPI_Comm group() const noexcept;
    /** @brief The configuration of rank 0 of the base communicator, as every rank received it */
    [[nodiscard]] const GMPICascadeConfig &config() const noexcept;

    /** @brief On a member: serves the rounds of its lead until the stop; returns 0 */
    int serve(GMPIGroupMemberI &);
    /** @brief On a lead: sets up @p lifecycle now and tears it down at release() */
    void lead(GMPIGroupLifecycleI &);
    /** @brief On a lead: announces a round and calls the lead's half with the group's communicator */
    template <typename F>
    decltype(auto) round(F &&);
    /** @brief On a lead: announces the stop to the members; latched, waits for a running round */
    void release() noexcept;
    /** @brief On a lead or the submitter: confirms that the consumer gave this process role() */
    void requireRole(processRole);

private:
    /** @brief The callable of the environment: broadcasts rank 0's configuration, makes the splits */
    MPI_Comm split_(const std::function<GMPICascadeConfig()> &);
    /** @brief Checks that a round may be announced, then announces it (round lock held) */
    void announceRound_();
    /** @brief Announces the stop and tears the lead's lifecycle down (round lock held) */
    void releaseLocked_() noexcept;
    /** @brief Releases the group and throws unless requireRole() confirmed the role (round lock held) */
    void requireConfirmedRole_(std::string_view);
    /** @brief Broadcasts a command to the members on the status communicator */
    void announce_(int);
    /** @brief On a member: waits for the next command; false for the stop */
    bool waitForRound_();
    /** @brief Throws unless MPI is in use */
    void requireMpi_(std::string_view) const;
    /** @brief Throws unless this process has the given role in the cascade */
    void requireCascadeRole_(processRole, std::string_view) const;
    /** @brief Calls a lifecycle or member function; an exception out of it ends the job */
    template <typename F>
    void guarded_(std::string_view, F &&) noexcept;
    /** @brief Ends the MPI job after an exception left a half of the group */
    [[noreturn]] void endJob_(std::string_view, const std::exception_ptr &) const noexcept;
    /** @brief Ends the MPI job with a reason */
    [[noreturn]] void endJob_(std::string_view, std::string_view) const noexcept;

    /// Keeps MPI initialized; null without MPI
    std::shared_ptr<Consumers::GMPIEnvironment> environment_;
    MPI_Comm base_ = MPI_COMM_NULL; ///< The base communicator (the caller's; never freed)
    /// The submitter's and the leads' communicator, given to the environment
    MPI_Comm courtier_ = MPI_COMM_NULL;
    MPI_Comm group_ = MPI_COMM_NULL;            ///< The group's communicator, for the halves
    MPI_Comm status_ = MPI_COMM_NULL;           ///< The group's communicator for the announcements
    int base_rank_ = 0;                         ///< This process's rank in base_
    GMPICascadeConfig config_;                  ///< Rank 0's configuration
    processRole role_ = processRole::SUBMITTER; ///< The cascade's role of this process

    std::mutex round_mutex_; ///< Held by a round from its announcement to its end; guards released_
    bool released_ = false;  ///< A lead: the stop was announced
    bool stop_received_ = false;              ///< A member: the stop arrived (latched)
    bool served_ = false;                     ///< A member: serve() has run to its end
    std::atomic<bool> role_confirmed_{false}; ///< requireRole() was given role()
    std::atomic<bool> used_{false};           ///< A lead: lead() or round() was called
    /// A lead: the lifecycle passed to lead(), torn down by release(); not owned (it outlives the
    /// cascade), null if lead() was not called
    GMPIGroupLifecycleI *lifecycle_ = nullptr;
};

/******************************************************************************/
/**
 * @brief On a lead: announces a round to the members and calls @p lead_half with the group's
 * communicator. Rounds run one after the other, whichever threads call this. An exception out of
 * @p lead_half ends the MPI job (see the class); the checks before the announcement throw.
 *
 * @param lead_half Callable with the group's communicator (MPI_Comm); the lead's half of the round
 * @return What @p lead_half returns
 * @throws geneva_exception before anything is announced: on a process that is not a lead, without
 *         MPI, before requireRole() (the group is then released) or after release()
 */
template <typename F>
decltype(auto) GMPICascade::round(F &&lead_half) {
    const std::scoped_lock lock(round_mutex_); // one round at a time
    this->announceRound_();
    try {
        return std::invoke(std::forward<F>(lead_half), group_);
    }
    catch(...) {
        this->endJob_("the lead's half of a round", std::current_exception());
    }
}

/******************************************************************************/
/**
 * @brief Calls @p function; an exception out of it ends the MPI job, because the group can then
 * no longer be assumed to be in step.
 *
 * @param where The function that is called, for the message
 * @param function What to call
 */
template <typename F>
void GMPICascade::guarded_(std::string_view where, F &&function) noexcept {
    try {
        std::forward<F>(function)();
    }
    catch(...) {
        this->endJob_(where, std::current_exception());
    }
}

/******************************************************************************/

} /* namespace Gem::Courtier */
