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
 * The one MPI environment of a process, and the three functions that hand it out. They live in a
 * shared library of their own (libgemfony-courtier-mpi) for two reasons: the state must be defined
 * in exactly one shared object, so that a program, a module it loads (such as Geneva's MPI module)
 * and any other library of the process find the same object whether or not the program exports its
 * symbols; and that object calls MPI, which libgemfony-courtier must not link, so that a program
 * that does not use MPI starts on a machine without it.
 */

#include "courtier/mpi/GMPIEnvironmentAccess.hpp"

// Standard headers go here
#include <functional>
#include <memory>
#include <mutex>

// MPI headers go here
#include <mpi.h>

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/mpi/GMPIEnvironment.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/*
 * The state. This library is one shared object, so these objects exist once in a process, whoever
 * links what.
 */
namespace {

using environment_type = Consumers::GMPIEnvironment;

/// Guards the two pointers below: the environment is created at most once while one is alive.
std::mutex environmentMutex; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// The environment, without keeping it: the holders (the consumer, the client, the cascade, ...)
/// decide how long it lives, and MPI is finalized when the last of them lets go. Once it has
/// expired, the next call builds a new one (whose constructor refuses if MPI was finalized
/// meanwhile).
std::weak_ptr<environment_type>
    environment; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// "The anchor": keeps the environment alive from its creation until the first call of
/// mpiEnvironment() or mpiEnvironmentOn() has returned it to a holder. It bridges the time between
/// "what is my role" (mpiProcessRole(), which keeps nothing) and "build the consumer"; without it,
/// the environment built for the question would die, and MPI be finalized, before the consumer
/// exists. If no holder ever takes it (an error path), it lasts until static destruction.
std::shared_ptr<environment_type>
    anchor; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// Whether this thread is building the environment (obtainLocked()), and so runs make_base with
/// environmentMutex held: a call of the three functions from there would lock it again, which
/// std::mutex does not allow (a deadlock).
thread_local bool building = false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/** @brief Refuses a call of the three functions from the callable of mpiEnvironmentOn() (or from
 *  anything it calls), which runs while the environment is being built.
 *  @param where The function that was called
 *  @throws gem_exception on the thread that builds the environment */
void refuseWhileBuilding(const char *where) {
    if(building) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "In " << where << ": Error!" << '\n'
            << where << " was called while the MPI environment is being built on this thread, "
            << "from the callable of mpiEnvironmentOn() (for a cascade: its configuration "
            << "callable). That callable must not ask for the MPI environment or the process's "
            << "role, nor build anything that does (an MPI consumer or client)." << '\n'
        );
    }
}

/** Marks this thread as building the environment, for its lifetime. */
class Building {
public:
    /** @brief Marks this thread */
    Building() {
        building = true;
    }
    /** @brief Removes the mark */
    ~Building() {
        building = false;
    }
    Building(const Building &) = delete;
    Building(Building &&) = delete;
    Building &operator=(const Building &) = delete;
    Building &operator=(Building &&) = delete;
};

/** @brief The living environment, or a new one on @p make_base (anchored). Call with
 *  environmentMutex held.
 *  @param make_base Returns the base communicator once MPI is initialized
 *  @return The environment */
std::shared_ptr<environment_type> obtainLocked(const std::function<MPI_Comm()> &make_base) {
    auto current = environment.lock();
    if(not current) {
        const Building marked;
        current = std::make_shared<environment_type>(nullptr, nullptr, make_base);
        environment = current;
        anchor = current;
    }
    return current;
}

/** @brief The base communicator of an environment built by default. */
MPI_Comm worldCommunicator() {
    return MPI_COMM_WORLD;
}

} // namespace

/******************************************************************************/
/**
 * @brief The process's MPI environment for courtier, built on MPI_COMM_WORLD if none exists.
 * @return The environment; the caller keeps it for as long as it uses MPI
 */
std::shared_ptr<Consumers::GMPIEnvironment> mpiEnvironment() {
    refuseWhileBuilding("mpiEnvironment()");
    const std::scoped_lock lock(environmentMutex);
    auto result = obtainLocked(worldCommunicator);
    anchor.reset(); // a holder has it now
    return result;
}

/******************************************************************************/
/**
 * @brief Builds the process's MPI environment on the communicator @p make_base returns.
 * @param make_base Called once MPI is initialized; returns the base communicator
 * @return The environment; the caller keeps it for as long as it uses MPI
 * @throws gem_exception if an environment exists already, or if called from the callable of
 *         another mpiEnvironmentOn() on this thread
 */
std::shared_ptr<Consumers::GMPIEnvironment>
mpiEnvironmentOn(const std::function<MPI_Comm()> &make_base) {
    refuseWhileBuilding("mpiEnvironmentOn()");
    const std::scoped_lock lock(environmentMutex);
    if(not environment.expired()) {
        throw Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "An MPI environment exists already; mpiEnvironmentOn() must be the first call "
            << "that builds it" << '\n'
        );
    }
    auto result = obtainLocked(make_base);
    anchor.reset(); // a holder has it now
    return result;
}

/******************************************************************************/
/**
 * @brief The role of this process, without keeping the environment.
 * @return The role on courtier's communicator
 */
processRole mpiProcessRole() {
    refuseWhileBuilding("mpiProcessRole()");
    const std::scoped_lock lock(environmentMutex);
    return obtainLocked(worldCommunicator)->role();
}

/******************************************************************************/

} /* namespace Gem::Courtier */
