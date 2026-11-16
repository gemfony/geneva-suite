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
 * Waits for MPI without waiting busily: every wait here looks once at once, then pauses between two
 * looks, with a pause that starts at a lower bound and doubles up to an upper one. They know
 * nothing of the cascade (GMPICascade.hpp), which uses them, and serve any program that holds MPI
 * requests -- the members of a cascade among them. Part of libgemfony-courtier-mpi.
 *
 * The pauses are checked with checkPollPauses() (GMPICascadeConfig.hpp) on every call.
 */

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// No guard on GENEVA_BUILD_WITH_MPI_CONSUMER: see courtier/mpi/GMPIEnvironment.hpp.

// Standard headers go here
#include <chrono>
#include <span>

// MPI headers go here
#include <mpi.h>

namespace Gem::Courtier {

/******************************************************************************/

/** @brief Waits until every request of the span has completed (MPI_Testall, polled) */
[[nodiscard]] int
    waitPolled(std::span<MPI_Request>, std::chrono::microseconds, std::chrono::microseconds);

/** @brief Waits until MPI_Iprobe finds a message of the given source and tag on a communicator */
[[nodiscard]] MPI_Status
probePolled(int, int, MPI_Comm, std::chrono::microseconds, std::chrono::microseconds);

/** @brief MPI_Iscatter of the given number of elements per process, completed with waitPolled() */
[[nodiscard]] int mpiScatter(
    const void *,
    int,
    void *,
    MPI_Datatype,
    int,
    MPI_Comm,
    std::chrono::microseconds,
    std::chrono::microseconds
);

/** @brief MPI_Igather of the given number of elements per process, completed with waitPolled() */
[[nodiscard]] int mpiGather(
    const void *,
    int,
    void *,
    MPI_Datatype,
    int,
    MPI_Comm,
    std::chrono::microseconds,
    std::chrono::microseconds
);

/******************************************************************************/

} /* namespace Gem::Courtier */
