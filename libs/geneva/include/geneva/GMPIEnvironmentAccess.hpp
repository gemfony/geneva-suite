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

// The environment is only available when the MPI consumer was enabled at configure time.
#ifdef GENEVA_BUILD_WITH_MPI_CONSUMER

// Standard header files go here
#include <functional>
#include <memory>

// MPI headers go here
#include <mpi.h>

// Geneva header files go here
#include "courtier/GCourtierEnums.hpp"
#include "courtier/transport/GMPIEnvironment.hpp"

namespace Gem::Geneva {

/** The process's MPI environment for courtier, kept by the caller for as long as it uses MPI.
 *  Built on MPI_COMM_WORLD if none exists. A program that names these functions links the MPI
 *  module (Geneva::geneva-mpi); the module holds the one object there is. */
[[nodiscard]] std::shared_ptr<Gem::Courtier::Consumers::GMPIEnvironment> mpiEnvironment();

/** Builds the environment on the communicator @p make_base returns; it is called once MPI is
 *  initialized. For a program that brings or splits its own communicator. Must be the first of
 *  these functions to be called: it throws if an environment exists already. */
[[nodiscard]] std::shared_ptr<Gem::Courtier::Consumers::GMPIEnvironment>
mpiEnvironmentOn(const std::function<MPI_Comm()> &make_base);

/** The role of this process, without keeping the environment (builds it on MPI_COMM_WORLD if
 *  none exists). */
[[nodiscard]] Gem::Courtier::processRole mpiProcessRole();

} // namespace Gem::Geneva

#endif /* GENEVA_BUILD_WITH_MPI_CONSUMER */
