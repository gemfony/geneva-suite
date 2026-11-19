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

// No guard on GENEVA_BUILD_WITH_MPI_CONSUMER: see courtier/mpi/GMPIEnvironment.hpp.

// Standard headers go here
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// MPI headers go here
#include <mpi.h>

// Geneva headers go here
#include "courtier/mpi/GMPIEnvironment.hpp"
#include "courtier/transport/GMPICallsI.hpp"

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * The MPI calls of the MPI transport (GMPICallsI), made on courtier's communicator of an
 * environment. Part of libgemfony-courtier-mpi; the poll that uses it (GMPIPollCore) is not.
 *
 * It holds the environment, and with it MPI initialized, for as long as it exists. A request
 * handle is the MPI_Request itself, its bytes carried in the handle (MPI_REQUEST_NULL is
 * GMPIRequest::NONE). A send that is given up is cancelled and its request freed; its bytes are
 * kept for the rest of the process, because MPI may still read them and says nothing about when it
 * has stopped.
 */
class GMPICalls final : public GMPICallsI {
public:
    /** @brief Makes the calls on courtier's communicator of @p environment */
    explicit GMPICalls(std::shared_ptr<GMPIEnvironment>);

    /** @brief The destructor; makes no MPI call */
    ~GMPICalls() override = default;

    GMPICalls(const GMPICalls &) = delete;
    GMPICalls(GMPICalls &&) = delete;
    GMPICalls &operator=(const GMPICalls &) = delete;
    GMPICalls &operator=(GMPICalls &&) = delete;

    /** @brief This process's rank in courtier's communicator */
    [[nodiscard]] int rank() const override;
    /** @brief The number of ranks of courtier's communicator */
    [[nodiscard]] int size() const override;
    /** @brief MPI_Iprobe for any source and any tag, MPI_Get_count for the length */
    [[nodiscard]] std::optional<GMPIProbed> probe() override;
    /** @brief MPI_Recv of the message a probe reported */
    [[nodiscard]] std::string receive(const GMPIProbed &) override;
    /** @brief MPI_Isend */
    [[nodiscard]] GMPIRequest startSend(int, int, std::string_view) override;
    /** @brief MPI_Testsome */
    std::size_t testSome(std::span<GMPIRequest>) override;
    /** @brief MPI_Cancel and MPI_Request_free; the bytes are kept */
    void giveUp(GMPIRequest, std::unique_ptr<std::string>) override;
    /** @brief Tells the environment (GMPIEnvironment::noteLostRank()) */
    void noteLostRank(int) override;

private:
    std::shared_ptr<GMPIEnvironment> environment_; ///< Keeps MPI initialized
    MPI_Comm communicator_;                        ///< Courtier's communicator
    int rank_;                                     ///< This process's rank in it
    int size_;                                     ///< Its number of ranks
    std::vector<MPI_Request> requests_;            ///< Scratch of testSome()
    std::vector<int> indices_;                     ///< Scratch of testSome()
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
