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

// Standard header files go here
#include <functional>
#include <optional>

// Boost header files go here

// Geneva headers go here
#include "geneva/GMPISubClientIndividual.hpp"
#include "geneva/Go2.hpp"

namespace Gem::Geneva {

/**
     * This is a class which supports all functionality that is given by `Go2` and additionally adds
     * the functionality of MPI-sub-clients. Each sub-client group can use their own MPI communicator in order
     * to together solve an evaluation.
     * This class only makes sense when the consumer is set to GMPIConsumerT and therefore is not allowed to use with other
     * consumers.
     */
class GMPISubClientOptimizer // NOLINT(cppcoreguidelines-special-member-functions)
  : public Go2 {
public:
    /**
         * @brief Parses the command line for relevant parameters and allows to specify a default config file name.
         *
         * @param argc Number of command line arguments
         * @param argv Array of command line argument strings
         * @param configFilePath The name and location of the configuration file
         * @param userDescriptions Additional command line options (cmp. boost::program_options); defaults to an empty set
         * @param baseCommunicator MPI communicator that all processes instantiating GMPISubClientOptimizer call. In the
         *  most frequent and least complicated case the default value of MPI_COMM_WORLD will be correct.
         */
    GMPISubClientOptimizer(
        int argc,
        char **argv,
        std::string const &configFilePath,
        boost::program_options::options_description const &userDescriptions =
            boost::program_options::options_description(),
        MPI_Comm baseCommunicator = MPI_COMM_WORLD
    );
    /** @brief Deleted copy constructor */
    GMPISubClientOptimizer(GMPISubClientOptimizer const &) = delete;

    /** @brief The destructor: tears the consumer down first, then finalizes MPI if this object
     *  initialized it (every rank must call MPI_Finalize, or mpirun treats the exit as abnormal
     *  and kills the remaining ranks). */
    ~GMPISubClientOptimizer() override;

    /**
         * @brief Registers a function to be called by sub-clients.
         *
         * The callback receives the MPI_Comm communicator used by this process and all processes in the same sub-group,
         * over which the sub-clients cooperate to solve the fitness calculation.
         *
         * @param callback The function executed by sub-clients; it takes the sub-group communicator and returns an
         *  integer status code
         * @return A reference to this object, allowing call chaining
         */
    GMPISubClientOptimizer &
    registerSubClientJob(std::move_only_function<int(MPI_Comm)> callback);

    /**
         * @brief Checks whether the current process is a sub-client.
         *
         * @return True if this process acts as a sub-client, false otherwise
         */
    [[nodiscard]] bool isSubClient() const {
        return isSubClient_;
    }

protected:
    /**
         * @brief Triggers execution of the client job.
         *
         * @return An integer return value (suitable for the main function) indicating the execution status.
         */
    int clientRun_() override;

    /**
         * @brief Adds local configuration options to a GParserBuilder object.
         *
         * @param gpb The GParserBuilder object to which configuration options should be added
         */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

private:
    /**
         * @brief Starts a non-blocking MPI barrier over the base communicator.
         *
         * @return The MPI request handle that completes once all participating processes reach the barrier.
         */
    MPI_Request startAsyncBarrier() const;
    /**
         * MPI communicator used for communication between the geneva GMPIConsumerMasterNodeT and GMPIConsumerWorkerNodeT.
         */
    MPI_Comm genevaComm_{};
    /**
         * MPI communicator used for communication between sub-clients inside of their specific sub-group.
         */
    MPI_Comm subClientComm_{};
    /**
         * MPI communicator which has the same scope as subClientComm_ but is used for retrieving status information about of the current group.
         */
    MPI_Comm subClientStatusComm_{};
    /**
         * Total number of the MPI nodes which will instantiate this class
         */
    int baseCommSize_{};
    /**
         * Rank in the base communicator i.e. in the outer most communicator
         */
    int baseCommRank_{};
    /**
          * The number of sub-clients per geneva client. This means each geneva client is part of a sub-group consisting of
          * subClientGroupSize_ processes.
          */
    std::uint16_t subClientGroupSize_{4};
    /**
         * Flag which is true if the current process is a sub-client
         */
    bool isSubClient_{};
    /**
         * Whether THIS object performed the MPI initialization (and therefore owns the matching
         * MPI_Finalize in the destructor). False when the user pre-initialized MPI (custom base
         * communicator) or in a pure --update-configs run.
         */
    bool i_initialized_mpi_{false};
    /**
         * Callback function which is executed by sub-clients when clientRun() is called
         */
    std::move_only_function<int(MPI_Comm)> subClientJob_{[]([[maybe_unused]] MPI_Comm comm) -> int {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "GMPISubClientOptimizer::subClientJob_(MPI_Comm comm): Error!" << '\n'
            << "The sub-client job has not been set. Set it using the `GMPISubClientOptimizer "
               "&GMPISubClientOptimizer::registerSubClientJob(std::move_only_function<int(MPI_Comm)> "
               "callback)` method."
            << '\n'
        );
    }};

    /**
        * The color argument when creating the Geneva communicator.
        * The value is arbitrary but must be different from all sub-client colors.
        */
    const int M_MPI_GENEVA_COLOR{0};
};
} /* namespace Gem::Geneva */
