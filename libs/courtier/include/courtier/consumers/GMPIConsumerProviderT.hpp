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
#include <memory>
#include <string>

// Geneva header files go here
#include "common/GCommonEnums.hpp" // serializationModeChoices()
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "courtier/GConsumerProviderT.hpp"
#include "courtier/consumers/GMPIConsumerConfig.hpp"
#include "courtier/consumers/GMPIConsumerT.hpp"
#include "courtier/mpi/GMPIEnvironmentAccess.hpp" // mpiEnvironment(), mpiProcessRole()

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The provider of the MPI consumer ("mpi"). The role of a process follows its rank in
 * courtier's communicator (mpiEnvironment()): the submitter builds the consumer, a client the MPI
 * client, a bystander neither; --client plays no part. A module with this provider links
 * courtier's MPI library (libgemfony-courtier-mpi), which holds the process's one MPI environment.
 * @tparam processable_type The item type
 */
template <ProcessableItem processable_type>
class GMPIConsumerProviderT final
  : public GConfiguredConsumerProviderT<processable_type, Consumers::MPIConsumerConfig> {
    using base_type = GConfiguredConsumerProviderT<processable_type, Consumers::MPIConsumerConfig>;
    using config_type = Consumers::MPIConsumerConfig;

public:
    /** @brief The consumer's class @return Its name */
    [[nodiscard]] std::string getName() const override {
        return "GMPIConsumerT";
    }
    /** @brief A networked consumer @return true */
    [[nodiscard]] bool needsClient() const override {
        return true;
    }
    /** @brief The role follows the rank @return true */
    [[nodiscard]] bool determinesRoleAtRuntime() const override {
        return true;
    }
    /** @brief The role of this process in the MPI environment; --client plays no part
     *  @return The role of this process */
    [[nodiscard]] processRole role(const ConsumerSpec &, bool) const override {
        return mpiProcessRole();
    }

    /** @brief Builds the consumer and starts it; on the submitter only
     *  @param spec Mnemonic and configuration @return It */
    typename base_type::consumer_ptr setup(const ConsumerSpec &spec) override {
        requireRole_(processRole::SUBMITTER, "setup");
        auto consumer = std::make_shared<GMPIConsumerT<processable_type>>(
            mpiEnvironment(),
            configOf<config_type>(spec)
        );
        consumer->startServer();
        return consumer;
    }
    /** @brief Builds the MPI client; on a client only @param spec Mnemonic and configuration
     *  @return It */
    [[nodiscard]] typename base_type::client_ptr
    buildClient(const ConsumerSpec &spec) const override {
        requireRole_(processRole::CLIENT, "buildClient");
        return std::make_shared<Consumers::GMPIConsumerWorkerNodeT<processable_type>>(
            mpiEnvironment(),
            configOf<config_type>(spec)
        );
    }

protected:
    /** @brief The --mpi_* options @param gpb The launcher's parser */
    void addOptions_(Gem::Common::GParserBuilder &gpb) override {
        const config_type defaults;
        this->option(
            gpb,
            "mpi_nHandlerThreads",
            &config_type::nHandlerThreads,
            defaults.nHandlerThreads,
            "[mpi] The number of threads the submitter's sessions run on (0 == as many "
            "as the machine has hardware threads); one more thread polls for every client"
        );
        this->option(
            gpb,
            "mpi_prefetchDepth",
            &config_type::clientPrefetchDepth,
            defaults.clientPrefetchDepth,
            "[mpi] n: how many work items a client holds at once -- the one(s) being "
            "evaluated and the next one, fetched while the current one is evaluated (1: "
            "none is fetched ahead; how many it evaluates at once: "
            "--mpi_parallelEvaluations). 2 by default, unlike --beast_prefetchDepth. "
            "Waiting requests are served oldest first, whether their client evaluates or "
            "not: when fewer items become available than requests wait (between "
            "iterations, say), one sent while evaluating may get an item before a newer "
            "one of an idle client"
        );
        this->option(
                gpb,
                "mpi_serializationMode",
                &config_type::serializationMode,
                defaults.serializationMode,
                "[mpi] Serialization in " + Gem::Common::serializationModeChoices()
        )
            .setHidden();
        this->option(
                gpb,
                "mpi_parallelEvaluations",
                &config_type::clientParallelEvaluations,
                defaults.clientParallelEvaluations,
                "[mpi] m: how many of the items a client holds (--mpi_prefetchDepth) it "
                "evaluates at the same time; from 1 to the prefetch depth"
        )
            .setHidden();
        this->option(
                gpb,
                "mpi_pingInterval",
                &config_type::pingInterval,
                defaults.pingInterval,
                "[mpi] Keep-alive of the submitter: every rank is sent a sign of life this "
                "often (in seconds); a rank not heard from for more than twice that time is "
                "lost, and the job is ended when the run is over (0 == off). The clients take "
                "it from the submitter"
        )
            .setHidden();
        this->option(
                gpb,
                "mpi_firstContactLimit",
                &config_type::clientFirstContactLimit,
                defaults.clientFirstContactLimit,
                "[mpi] Seconds a client waits for the submitter's greeting before it gives up "
                "(0 == for as long as it runs)"
        )
            .setHidden();
    }

private:
    /** @brief Throws unless this process has the role @p wanted
     *  @param wanted The role the caller is for @param caller The calling function */
    static void requireRole_(processRole wanted, const char *caller) {
        const processRole actual = mpiProcessRole();
        if(actual != wanted) {
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMPIConsumerProviderT::" << caller << "(): Error!" << '\n'
                << "This process has the role " << static_cast<int>(actual) << " in the MPI "
                << "environment, but " << caller << "() is for the role "
                << static_cast<int>(wanted) << " only" << '\n'
            );
        }
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier */
