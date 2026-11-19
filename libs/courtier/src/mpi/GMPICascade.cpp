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
 * The MPI cascade (see GMPICascade.hpp): everything of GMPICascade that is not a template.
 */

#include "courtier/mpi/GMPICascade.hpp"

// Standard headers go here
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

// MPI headers go here
#include <mpi.h>

// Geneva headers go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GCourtierHelperFunctions.hpp"
#include "courtier/mpi/GMPIEnvironmentAccess.hpp"
#include "courtier/mpi/GMPIPolledCalls.hpp"

namespace Gem::Courtier {

namespace {

/// The two commands a lead announces to its members, one MPI_INT on the status communicator
constexpr int ROUNDSTOP = 0;
constexpr int ROUNDGO = 1;

/// The first number of rank 0's packed configuration (GMPICascade::packed_config_type)
constexpr std::int64_t CONFIGFAILED = 0;
constexpr std::int64_t CONFIGPROVIDED = 1;

/******************************************************************************/
/**
 * @brief What a role is in the cascade, for messages.
 * @param role The role
 * @return Its name
 */
std::string_view roleName(processRole role) {
    switch(role) {
    case processRole::SUBMITTER:
        return "the submitter";
    case processRole::CLIENT:
        return "a lead";
    case processRole::BYSTANDER:
        return "a member";
    }
    return "an unknown role";
}

/******************************************************************************/
/**
 * @brief What a process of a role calls, for messages.
 * @param role The role
 * @return The calls
 */
std::string_view callsOf(processRole role) {
    switch(role) {
    case processRole::SUBMITTER:
        return "the submitter submits through its consumer and takes no part in the rounds";
    case processRole::CLIENT:
        return "a lead calls lead(), round() and release()";
    case processRole::BYSTANDER:
        return "a member calls serve()";
    }
    return "";
}

/******************************************************************************/
/**
 * @brief The name of a layout, for messages.
 * @param layout The layout
 * @return Its name
 */
std::string_view layoutName(GMPICascadeLayout layout) {
    switch(layout) {
    case GMPICascadeLayout::SUBMITTER_AND_GROUPS:
        return "SUBMITTER_AND_GROUPS";
    case GMPICascadeLayout::GROUPS_ONLY:
        return "GROUPS_ONLY";
    }
    return "an unknown layout";
}

/******************************************************************************/
/**
 * @brief What a program has to add when the role of a lead or the submitter was not confirmed,
 * for messages.
 * @return The advice
 */
std::string_view confirmationAdvice() {
    return "Call requireRole() with the role the consumer gave this process (a program without a "
           "consumer, which calls round() itself: requireRole(processRole::CLIENT)) before the "
           "first lead() or round(); a wrong consumer would otherwise run nothing while the "
           "members idle.";
}

/******************************************************************************/
/**
 * @brief Whether MPI can be used in this process now.
 * @return Whether MPI is initialized and not finalized
 */
bool mpiUsable() noexcept {
    int initialized = 0;
    int finalized = 0;
    // both may be called at any time and report nothing but their flag
    std::ignore = MPI_Initialized(&initialized);
    std::ignore = MPI_Finalized(&finalized);
    return initialized != 0 && finalized == 0;
}

} // namespace

/******************************************************************************/
/**
 * @brief Splits @p base into the cascade's groups (see the class comment). Collective on @p base:
 * every process of it constructs its cascade at the same point. Without configuration emission
 * the cascade builds the process's MPI environment, which initializes MPI with
 * MPI_THREAD_MULTIPLE unless the program has done so.
 *
 * @param config_on_rank0 Called on rank 0 of @p base only (and without MPI under configuration
 *                        emission); provides the configuration of every rank. It runs while the
 *                        MPI environment is being built and must not ask for it or for the
 *                        process's role (mpiEnvironment(), mpiProcessRole()), nor build anything
 *                        that does; such a call throws, and every rank refuses the cascade
 * @param base The communicator whose processes form the cascade; the caller's, never freed. A
 *             communicator other than MPI_COMM_WORLD implies that the program initialized MPI.
 * @throws gem_exception if an MPI environment exists already, if MPI has been finalized or
 *         cannot provide MPI_THREAD_MULTIPLE, if rank 0 could not provide the configuration (rank
 *         0 rethrows what its callable threw), or if the configuration is refused by
 *         GMPICascadeConfig::check() -- on every rank alike (a rank with an environment of its own
 *         throws that, the others that another rank could not take part); MPI is then finalized
 *         again if the environment initialized it
 */
GMPICascade::GMPICascade(std::function<GMPICascadeConfig()> config_on_rank0, MPI_Comm base)
  : base_(base) {
    if(Gem::Common::GParserBuilder::updateInPlace()) {
        // Configuration emission: no MPI call at all. The callable writes its file.
        config_ = config_on_rank0();
        config_.check();
        return;
    }
    try {
        environment_ =
            mpiEnvironmentOn([this, &config_on_rank0] { return this->split_(config_on_rank0); });
    }
    catch(...) {
        // This rank could not build its environment (one exists already, or the thread level is
        // too low) and so never reached split_(). While MPI can still be used, it takes part in
        // the agreement of split_() with a refusal, so that every other rank refuses too instead
        // of waiting in it.
        if(not agreed_ && mpiUsable()) {
            this->refuseWithOthers_();
        }
        throw;
    }
}

/******************************************************************************/
/**
 * @brief The destructor, on every rank. In this order: nothing without MPI or once MPI is
 * finalized; a member that has neither served nor received the stop ends the job (its lead would
 * wait for ever); a lead releases its group; a lead or the submitter whose role was never
 * confirmed with requireRole() ends the job, unless an exception unwinds through the cascade
 * (std::uncaught_exceptions() > 0: the class rules out a cascade constructed during an unwinding;
 * a lead that used the cascade has ended it at the first use already); the
 * submitter ends the job if an exception unwinds through the cascade and no MPI consumer was built
 * on it (nobody would tell the leads, which wait for its greeting); then the communicators are
 * freed and the environment is let go of, which finalizes MPI if it initialized it and this was its
 * last holder.
 */
GMPICascade::~GMPICascade() {
    if(not environment_) {
        return; // no MPI was used
    }
    int finalized = 0;
    // MPI_Finalized may be called at any time and reports nothing but the flag: no error to look at
    std::ignore = MPI_Finalized(&finalized);
    if(finalized != 0) {
        return; // the program finalized MPI while the cascade lived: nothing can be done
    }
    if(role_ == processRole::BYSTANDER && not served_ && not stop_received_) {
        this->endJob_(
            "~GMPICascade()",
            "a member of the cascade left without serve(); its lead would wait for ever. A member "
            "calls serve() before its cascade is destroyed."
        );
    }
    if(role_ == processRole::CLIENT) {
        this->release();
    }
    const bool unwinding = std::uncaught_exceptions() > 0; // through the cascade (see the class)
    if(role_ != processRole::BYSTANDER && not role_confirmed_ && not unwinding) {
        this->endJob_(
            "~GMPICascade()",
            std::string("the role of ") + std::string(roleName(role_)) + " was never confirmed. " +
                std::string(confirmationAdvice())
        );
    }
    if(role_ == processRole::SUBMITTER && unwinding && not environment_->consumerBuilt()) {
        this->endJob_(
            "~GMPICascade()",
            "an exception leaves the submitter before an MPI consumer was built on it. The leads "
            "wait for its greeting, and MPI_Finalize would wait for them. The exception's text is "
            "not known here: a program that wants it in the log catches it inside the cascade's "
            "scope and logs and rethrows it, or ends the job with abortJob() and the text."
        );
    }
    for(MPI_Comm *communicator : {&courtier_, &group_, &status_}) {
        if(*communicator != MPI_COMM_NULL) {
            this->mpiChecked_(MPI_Comm_free(communicator), "MPI_Comm_free");
        }
    }
    environment_.reset();
}

/******************************************************************************/
/**
 * @brief The cascade's role of this process (not the environment's, which in GROUPS_ONLY is
 * BYSTANDER on a lead too).
 * @return SUBMITTER, CLIENT (a lead) or BYSTANDER (a member); SUBMITTER without MPI
 */
processRole GMPICascade::role() const noexcept {
    return role_;
}

/******************************************************************************/
/**
 * @brief The group's communicator, for the halves of a round.
 * @return The communicator; MPI_COMM_NULL on the submitter and without MPI
 */
MPI_Comm GMPICascade::group() const noexcept {
    return group_;
}

/******************************************************************************/
/**
 * @brief The configuration every rank uses.
 * @return Rank 0's configuration, as every rank received it
 */
const GMPICascadeConfig &GMPICascade::config() const noexcept {
    return config_;
}

/******************************************************************************/
/**
 * @brief On a member: calls @p member's setUp(), its serveRound() for every round its lead
 * announces, and its tearDown() after the stop; then waits in a barrier with the lead and the other
 * members (leaveTogether_()), so that it returns only when every member of the group has torn down
 * and the lead's release() returns with it. The stop is latched: a further call returns 0 at once
 * and calls nothing. An exception out of @p member's functions ends the MPI job.
 *
 * @param member The member's half; outlives the cascade
 * @return 0, the exit code of the process
 * @throws gem_exception without MPI, or on a process that is not a member
 */
int GMPICascade::serve(GMPIGroupMemberI &member) {
    this->requireMpi_("serve()");
    this->requireCascadeRole_(processRole::BYSTANDER, "serve()");
    this->refuseInside_("serve()");
    // Latched: after the stop nothing is posted on the status communicator, where no broadcast of
    // the lead would ever answer, and the member half is not set up again.
    if(stop_received_) {
        return 0;
    }
    this->guarded_("setUp()", [&] { member.setUp(group_); });
    while(this->waitForRound_()) {
        this->guarded_("serveRound()", [&] { member.serveRound(group_); });
    }
    this->guarded_("tearDown()", [&] { member.tearDown(group_); });
    this->leaveTogether_();
    served_ = true;
    return 0;
}

/******************************************************************************/
/**
 * @brief On a lead: calls @p lifecycle's setUp() now, in step with the members' setUp(), and its
 * tearDown() at release(). Called once, before the first round(). Optional for a lead whose
 * members' setUp() and tearDown() take no part in anything collective: such a lead is released
 * without a tearDown(); a lead whose members' lifecycle is collective must call it. An exception
 * out of setUp() ends the MPI job.
 *
 * @param lifecycle The lead's lifecycle; outlives the cascade
 * @throws gem_exception without MPI, on a process that is not a lead, after release(), after
 *         the first round() or on a second call. Before requireRole() the MPI job is ended instead, after the lifecycle was
 *         set up and the group released (setUp() and tearDown() run in step with the members')
 */
void GMPICascade::lead(GMPIGroupLifecycleI &lifecycle) {
    this->requireMpi_("lead()");
    this->requireCascadeRole_(processRole::CLIENT, "lead()");
    this->refuseInside_("lead()");
    const std::scoped_lock lock(round_mutex_);
    if(released_) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPICascade::lead(): Error!" << '\n'
            << "The group was released (release()); lead() comes before the first round" << '\n'
        );
    }
    if(announced_) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPICascade::lead(): Error!" << '\n'
            << "A round was announced already; lead() comes before the first round, so that "
            << "its setUp() pairs with the members' setUp()" << '\n'
        );
    }
    if(lifecycle_ != nullptr) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPICascade::lead(): Error!" << '\n'
            << "lead() was called before; a group is set up once" << '\n'
        );
    }
    lifecycle_ = &lifecycle;
    this->guarded_("setUp()", [this] { lifecycle_->setUp(group_); });
    this->requireConfirmedRole_("lead()");
}

/******************************************************************************/
/**
 * @brief On a lead: announces the stop to the members, then calls the tearDown() of the lifecycle
 * passed to lead(), then waits in a barrier until every member has torn down and left serve()
 * (leaveTogether_()): when it returns, nothing of the group runs any more. Waits for a round that
 * runs; latched; a later round() or lead() throws. Does nothing on another process and without
 * MPI. An exception out of tearDown() ends the MPI job.
 */
void GMPICascade::release() noexcept {
    if(not environment_ || role_ != processRole::CLIENT) {
        return;
    }
    this->refuseInside_("release()");
    const std::scoped_lock lock(round_mutex_);
    this->releaseLocked_();
}

/******************************************************************************/
/**
 * @brief Announces the stop to the members, tears down the lifecycle passed to lead() and waits in
 * the barrier until every member has left serve() (leaveTogether_()), once. Called with the round
 * lock held, on a lead with MPI. An exception out of tearDown() ends the MPI job.
 */
void GMPICascade::releaseLocked_() noexcept {
    if(released_) {
        return;
    }
    released_ = true;
    this->guarded_("release()", [this] { this->announce_(ROUNDSTOP); });
    if(lifecycle_ != nullptr) {
        this->guarded_("tearDown()", [this] { lifecycle_->tearDown(group_); });
    }
    this->leaveTogether_();
}

/******************************************************************************/
/**
 * @brief The last step of a release, on the lead and on every member after its tearDown(): a
 * barrier on the status communicator. So a release is complete only once every member has left
 * its rounds and torn down -- also when the lead ends the job right after it (a lead whose role was
 * not confirmed), no member is still serving when the job ends. An MPI error ends the job.
 */
void GMPICascade::leaveTogether_() noexcept {
    // NOLINTBEGIN(clang-analyzer-optin.mpi.MPI-Checker): waitStarted_() completes the request; the
    // checker does not know it
    MPI_Request request = MPI_REQUEST_NULL;
    this->waitStarted_(
        MPI_Ibarrier(status_, &request),
        request,
        config_.pauseMin,
        config_.pauseMax,
        "the end of the group's rounds"
    );
}
// NOLINTEND(clang-analyzer-optin.mpi.MPI-Checker)

/******************************************************************************/
/**
 * @brief Ends the MPI job at the first use of a lead whose role requireRole() has not confirmed:
 * releases the group first, so that every member's serve() returns and no process is left waiting,
 * then logs what the program has to add and ends the job as after a failed half (endJob_()). A
 * thrown exception would not do: inside a client's item processor it only flags the item, and the
 * job would run on and end with 0. Called with the round lock held.
 *
 * @param where The function that was called
 */
void GMPICascade::requireConfirmedRole_(std::string_view where) {
    if(role_confirmed_) {
        return;
    }
    this->releaseLocked_();
    this->endJob_(
        where,
        std::string("the role of ") + std::string(roleName(role_)) +
            " was not confirmed with requireRole(). " + std::string(confirmationAdvice()) +
            "\nThe group was released: its members have left serve()."
    );
}

/******************************************************************************/
/**
 * @brief On a lead or the submitter: confirms that the consumer gave this process the role the
 * cascade gave it. Every lead and the submitter must call it, a lead before its first lead() or
 * round() (see the class comment).
 *
 * @param consumer_role The role this process's consumer gave it (a program without a consumer
 *                      passes processRole::CLIENT on a lead)
 * @throws gem_exception on a member (it has no consumer), or if @p consumer_role differs from
 *         role(); the text names the base rank, the layout, both roles and the remedy
 */
void GMPICascade::requireRole(processRole consumer_role) {
    if(role_ == processRole::BYSTANDER) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPICascade::requireRole(): Error!" << '\n'
            << "This process (base rank " << base_rank_ << ") is a member of the cascade; a member "
            << "has no consumer, it calls serve()" << '\n'
        );
    }
    if(consumer_role == role_) {
        role_confirmed_ = true;
        return;
    }
    const std::string_view remedy =
        config_.layout == GMPICascadeLayout::SUBMITTER_AND_GROUPS
            ? "Start the job with --consumer mpi."
            : "Start every lead with --client and a networked consumer (for example --consumer "
              "beast), and the job without the MPI consumer.";
    throw gem_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In GMPICascade::requireRole(): Error!" << '\n'
        << "Base rank " << base_rank_ << " of a cascade laid out " << layoutName(config_.layout)
        << ": the consumer gave this process the role of " << roleName(consumer_role)
        << ", the cascade the role of " << roleName(role_) << '\n'
        << remedy << '\n'
    );
}

/******************************************************************************/
/**
 * @brief On the submitter: ends the MPI job explicitly. It logs @p reason as a warning, with the
 * base rank, and calls MPI_Abort on the base communicator with @p code, which ends every process of
 * the job; Open MPI's launcher then ends with @p code. This is the way for a submitter that has
 * caught a failure and does not rethrow it (see the class comment, "A submitter that fails"):
 * returning instead would leave the leads waiting for its consumer's greeting. Where MPI is not in
 * use (configuration emission) or was finalized already, the job is this process: the reason is
 * logged and the process ends with @p code (std::_Exit). Called once: a second call, from another
 * thread while the first ends the job, waits for that end.
 *
 * @param code The exit code of the job (the error code of MPI_Abort)
 * @param reason Why the job ends; logged before it ends
 * @throws gem_exception on a lead or a member, which end the job through the cascade's other
 *         rules; nothing is ended then
 */
void GMPICascade::abortJob(int code, std::string_view reason) {
    this->requireCascadeRole_(processRole::SUBMITTER, "abortJob()");
    if(aborting_.test_and_set()) {
        // The first call ends the job; it never returns, and neither does this one
        for(;;) {
            aborting_.wait(true);
        }
    }
    if(environment_ && mpiUsable()) {
        this->endJob_("abortJob()", reason, code);
    }
    try {
        glogger << "In GMPICascade, abortJob(): " << reason << '\n'
                << "MPI is not in use: the process is ended with the exit code " << code << "."
                << '\n'
                << GWARNING;
    }
    catch(...) { // NOLINT(bugprone-empty-catch): the process ends in any case
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(code);
}

/******************************************************************************/
/**
 * @brief The callable of the environment, called once MPI is initialized: broadcasts rank 0's
 * configuration, checks it and makes the splits (see the class comment). Rank 0 must never throw
 * before the broadcast, or the other ranks would wait in it for ever.
 *
 * @param config_on_rank0 Provides the configuration; called on rank 0 of base_ only
 * @return Courtier's communicator: the submitter's and the leads' in SUBMITTER_AND_GROUPS,
 *         MPI_COMM_NULL on a member and on every process in GROUPS_ONLY
 * @throws gem_exception if rank 0 could not provide the configuration (rank 0 rethrows what
 *         its callable threw), or if GMPICascadeConfig::check() refuses it
 */
MPI_Comm GMPICascade::split_(const std::function<GMPICascadeConfig()> &config_on_rank0) {
    this->mpiChecked_(MPI_Comm_rank(base_, &base_rank_), "MPI_Comm_rank");

    // Rank 0's configuration, or a marker that it has none, for every rank.
    packed_config_type packed{};
    packed[0] = CONFIGFAILED;
    std::exception_ptr failure;
    if(base_rank_ == 0) {
        try {
            const GMPICascadeConfig provided = config_on_rank0();
            packed = {
                CONFIGPROVIDED,
                provided.groupSize,
                static_cast<std::int64_t>(provided.layout),
                provided.pauseMin.count(),
                provided.pauseMax.count()
            };
        }
        catch(...) {
            failure = std::current_exception();
        }
    }
    if(this->agree_(packed, false)) {
        // Some ranks refused, this one did not. A throw would not end anything: where the cascade
        // initialized MPI, the environment finalizes it on the way out, and MPI_Finalize waits for
        // every process of the job -- also for the refusing rank, whose program keeps its own
        // environment for as long as it likes. So the job is ended.
        this->endJob_(
            "GMPICascade()",
            "another rank of the base communicator could not take part in the cascade (an MPI "
            "environment existed there already, or MPI's thread level is too low there). Build the "
            "cascade on every rank before any MPI environment exists."
        );
    }
    if(packed[0] != CONFIGPROVIDED) {
        if(failure) {
            std::rethrow_exception(failure);
        }
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPICascade::GMPICascade(): Error!" << '\n'
            << "Rank 0 of the base communicator could not provide the cascade's configuration"
            << '\n'
        );
    }
    config_.groupSize = Gem::Common::narrow<std::uint16_t>(packed[1]);
    // An unknown value is kept as it is, so that check() refuses it with its number.
    config_.layout =
        static_cast<GMPICascadeLayout>( // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
            Gem::Common::narrow<Gem::Common::ENUMBASETYPE>(packed[2])
        );
    config_.pauseMin = std::chrono::microseconds(packed[3]);
    config_.pauseMax = std::chrono::microseconds(packed[4]);
    config_.check(); // on every rank alike, after the broadcast

    // The layout: groups start after the submitter (SUBMITTER_AND_GROUPS) or at rank 0. Both
    // splits are made by every rank in both layouts (they are collective); the layout only
    // colours differently.
    const int g = config_.groupSize;
    const int first = config_.layout == GMPICascadeLayout::SUBMITTER_AND_GROUPS ? 1 : 0;
    const bool submitter = base_rank_ < first;
    const bool lead = not submitter && (base_rank_ - first) % g == 0;
    const int colour = submitter ? MPI_UNDEFINED : (base_rank_ - first) / g;
    const int key = lead ? 0 : base_rank_; // the lead is rank 0 of its group
    this->mpiChecked_(MPI_Comm_split(base_, colour, key, &group_), "MPI_Comm_split");
    const bool in_courtier =
        config_.layout == GMPICascadeLayout::SUBMITTER_AND_GROUPS && (submitter || lead);
    this->mpiChecked_(
        MPI_Comm_split(base_, in_courtier ? 0 : MPI_UNDEFINED, base_rank_, &courtier_),
        "MPI_Comm_split"
    );
    if(submitter) {
        role_ = processRole::SUBMITTER;
    }
    else {
        role_ = lead ? processRole::CLIENT : processRole::BYSTANDER;
    }

    if(group_ != MPI_COMM_NULL) {
        // The announcements travel on a communicator of their own, apart from the halves' traffic.
        this->mpiChecked_(
            MPI_Comm_set_errhandler(group_, MPI_ERRORS_ARE_FATAL),
            "MPI_Comm_set_errhandler"
        );
        this->mpiChecked_(MPI_Comm_dup(group_, &status_), "MPI_Comm_dup");
        this->mpiChecked_(
            MPI_Comm_set_errhandler(status_, MPI_ERRORS_ARE_FATAL),
            "MPI_Comm_set_errhandler"
        );
    }
    return courtier_;
}

/******************************************************************************/
/**
 * @brief The first two collectives of the construction, on every rank of the base communicator:
 * the broadcast of rank 0's configuration, then the agreement whether any rank refuses (a
 * maximum over all ranks), so that a rank that cannot take part ends the construction everywhere.
 *
 * @param packed Rank 0's packed configuration (sent on rank 0, received elsewhere)
 * @param refuse Whether this rank refuses
 * @return Whether any rank refused
 */
bool GMPICascade::agree_(packed_config_type &packed, bool refuse) noexcept {
    this->broadcast_(
        packed.data(),
        static_cast<int>(packed.size()),
        MPI_INT64_T,
        base_,
        GMPIPEERPOLLPAUSEMIN,
        GMPIPEERPOLLPAUSEMAX,
        "GMPICascade(), the broadcast of the configuration"
    );
    int refused_here = refuse ? 1 : 0;
    int refused_anywhere = 0;
    // NOLINTBEGIN(clang-analyzer-optin.mpi.MPI-Checker): waitStarted_() completes the request; the
    // checker does not know it
    MPI_Request request = MPI_REQUEST_NULL;
    this->waitStarted_(
        MPI_Iallreduce(&refused_here, &refused_anywhere, 1, MPI_INT, MPI_MAX, base_, &request),
        request,
        GMPIPEERPOLLPAUSEMIN,
        GMPIPEERPOLLPAUSEMAX,
        "GMPICascade(), the agreement of the ranks"
    );
    agreed_ = true;
    return refused_anywhere != 0;
}
// NOLINTEND(clang-analyzer-optin.mpi.MPI-Checker)

/******************************************************************************/
/**
 * @brief On a rank that could not build its environment: takes part in agree_() with a refusal
 * (rank 0 with a failure marker instead of a configuration), so that no other rank waits for it.
 */
void GMPICascade::refuseWithOthers_() noexcept {
    this->mpiChecked_(MPI_Comm_rank(base_, &base_rank_), "MPI_Comm_rank");
    packed_config_type packed{};
    packed[0] = CONFIGFAILED;
    std::ignore = this->agree_(packed, true);
}

/******************************************************************************/
/**
 * @brief Checks that a round may be announced and announces it. Called by round() with the round
 * lock held; throws before anything is announced, so the group stays in step.
 *
 * @throws gem_exception without MPI, on a process that is not a lead, or after release();
 *         before requireRole() the group is released and the MPI job ended instead
 */
void GMPICascade::announceRound_() {
    this->requireMpi_("round()");
    this->requireCascadeRole_(processRole::CLIENT, "round()");
    this->requireConfirmedRole_("round()");
    if(released_) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPICascade::round(): Error!" << '\n'
            << "The group was released (release()); no round follows the stop" << '\n'
        );
    }
    announced_ = true;
    this->announce_(ROUNDGO);
}

/******************************************************************************/
/**
 * @brief Broadcasts @p command from the lead to its members on the status communicator and waits
 * for the broadcast to complete, polled. An MPI error ends the job (broadcast_()).
 *
 * @param command ROUNDGO or ROUNDSTOP
 */
void GMPICascade::announce_(int command) {
    this->broadcast_(
        &command,
        1,
        MPI_INT,
        status_,
        config_.pauseMin,
        config_.pauseMax,
        "the announcement of a command to the group"
    );
}

/******************************************************************************/
/**
 * @brief Broadcasts @p count elements of @p type from rank 0 of @p comm with MPI_Ibcast and waits
 * for the completion, polled. The one broadcast of the cascade. An error of the start or of the
 * wait cannot be survived: a started collective cannot be cancelled, and a throw would leave it
 * active over a buffer that goes; and a rank that has not started it leaves the others waiting in
 * it. So any error ends the MPI job (endJob_()).
 *
 * @param buffer The elements (sent on rank 0, received elsewhere); valid until the return
 * @param count The number of elements
 * @param type Their MPI datatype
 * @param comm The communicator
 * @param pause_min The first pause of the wait
 * @param pause_max The longest pause of the wait
 * @param what What is broadcast, for the message
 */
void GMPICascade::broadcast_(
    void *buffer,
    int count,
    MPI_Datatype type,
    MPI_Comm comm,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max,
    std::string_view what
) noexcept {
    // NOLINTBEGIN(clang-analyzer-optin.mpi.MPI-Checker): waitStarted_() completes the request; the
    // checker does not know it
    MPI_Request request = MPI_REQUEST_NULL;
    this->waitStarted_(
        MPI_Ibcast(buffer, count, type, 0, comm, &request),
        request,
        pause_min,
        pause_max,
        what
    );
}
// NOLINTEND(clang-analyzer-optin.mpi.MPI-Checker)

/******************************************************************************/
/**
 * @brief Waits, polled, for a collective whose start returned @p started. An error of the start or
 * of the wait ends the job (see broadcast_()).
 *
 * @param started What the start of the collective returned
 * @param request Its request
 * @param pause_min The first pause of the wait
 * @param pause_max The longest pause of the wait
 * @param what The collective, for the message
 */
void GMPICascade::waitStarted_(
    int started,
    MPI_Request &request,
    std::chrono::microseconds pause_min,
    std::chrono::microseconds pause_max,
    std::string_view what
) noexcept {
    this->mpiChecked_(started, what);
    int completed = MPI_ERR_OTHER;
    try {
        completed = waitPolled(std::span<MPI_Request>(&request, 1), pause_min, pause_max);
    }
    catch(...) {
        // only checkPollPauses() throws, and the pauses were checked at construction
        this->endJob_(what, std::current_exception());
    }
    this->mpiChecked_(completed, what);
}

/******************************************************************************/
/**
 * @brief Ends the MPI job if the calling thread runs a function of the program for the cascade (a
 * lead's half, setUp(), serveRound(), tearDown(); see InsideMark): on a lead, round(), lead() and
 * release() take the round lock, which that thread holds, and would wait for ever; on a member,
 * serve() would take the group out of step.
 *
 * @param where The function that was called
 */
void GMPICascade::refuseInside_(std::string_view where) const noexcept {
    if(inside_thread_.load() != std::this_thread::get_id()) {
        return;
    }
    std::string reason;
    try {
        const char *what = inside_what_.load();
        const std::string inside = what != nullptr ? what : "a function the cascade runs";
        reason = std::format(
            "{} was called from inside {}; it would wait for the cascade for ever or take the "
            "group out of step. Call it after {} has returned.",
            where,
            inside,
            inside
        );
    }
    catch(...) { // NOLINT(bugprone-empty-catch): the job ends in any case
    }
    this->endJob_(where, reason);
}

/******************************************************************************/
/**
 * @brief Ends the MPI job if an MPI call of the cascade failed (possible only on a communicator
 * whose error handler returns, such as a base communicator with MPI_ERRORS_RETURN): an error of
 * the MPI library leaves the group, or a collective, in a state that cannot be repaired.
 *
 * @param result What the call returned
 * @param call The call, for the message
 */
void GMPICascade::mpiChecked_(int result, std::string_view call) const noexcept {
    if(result == MPI_SUCCESS) {
        return;
    }
    std::string reason;
    try {
        reason = std::string(call) + " failed with the MPI error code " + std::to_string(result);
    }
    catch(...) { // NOLINT(bugprone-empty-catch): the job ends in any case
    }
    this->endJob_(call, reason);
}

/******************************************************************************/
/**
 * @brief On a member: waits, polled, for the lead's next command. An MPI error ends the job
 * (broadcast_()).
 * @return true for a round, false for the stop (which is latched)
 */
bool GMPICascade::waitForRound_() {
    int command = ROUNDSTOP;
    this->broadcast_(
        &command,
        1,
        MPI_INT,
        status_,
        config_.pauseMin,
        config_.pauseMax,
        "the wait for a command of the lead"
    );
    stop_received_ = (command != ROUNDGO);
    return not stop_received_;
}

/******************************************************************************/
/**
 * @brief Refuses a function that needs MPI in the state without MPI (configuration emission).
 * @param where The function that was called
 * @throws gem_exception without MPI
 */
void GMPICascade::requireMpi_(std::string_view where) const {
    if(not environment_) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPICascade::" << where << ": Error!" << '\n'
            << "The cascade makes no MPI call while the configuration files are emitted "
            << "(--update-configs), so " << where << " cannot be used" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Refuses a function called by a process of the wrong role, before anything is announced.
 * @param needed The role the function is for
 * @param where The function that was called
 * @throws gem_exception unless this process has the role @p needed
 */
void GMPICascade::requireCascadeRole_(processRole needed, std::string_view where) const {
    if(role_ != needed) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPICascade::" << where << ": Error!" << '\n'
            << where << " is called on " << roleName(needed) << " of the cascade, but this "
            << "process (base rank " << base_rank_ << ") is " << roleName(role_) << "; "
            << callsOf(role_) << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Ends the MPI job after an exception left a half or a lifecycle function of the group:
 * logs it and calls MPI_Abort on the base communicator.
 *
 * @param where The function that was running
 * @param failure The exception
 */
void GMPICascade::endJob_(
    std::string_view where,
    const std::exception_ptr &failure
) const noexcept {
    std::string reason;
    try {
        reason = "an exception left " + std::string(where) + ": " + describeException(failure) +
                 "\nThe group can no longer be assumed to be in step.";
    }
    catch(...) { // NOLINT(bugprone-empty-catch): the job ends in any case
    }
    this->endJob_(where, reason);
}

/******************************************************************************/
/**
 * @brief Ends the MPI job: logs @p reason and calls MPI_Abort on the base communicator with
 * @p code, which ends every process of the job (in SUBMITTER_AND_GROUPS the whole run, in
 * GROUPS_ONLY one client job).
 *
 * @param where The function that was running
 * @param reason Why
 * @param code The exit code of the job (the error code of MPI_Abort); EXIT_FAILURE by default
 */
void GMPICascade::endJob_(
    std::string_view where,
    std::string_view reason,
    int code
) const noexcept {
    std::string text;
    try {
        text = "In GMPICascade (base rank " + std::to_string(base_rank_) + "), " +
               std::string(where) + ": " + std::string(reason) + "\nThe job is ended (MPI_Abort).";
    }
    catch(...) { // NOLINT(bugprone-empty-catch): the job ends in any case
    }
    Consumers::endMpiJob(base_, text, code);
}

/******************************************************************************/

} /* namespace Gem::Courtier */
