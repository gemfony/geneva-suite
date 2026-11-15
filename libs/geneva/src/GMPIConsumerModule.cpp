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
 * The MPI consumer as a module of its own (libgemfony-geneva-mpi).
 *
 * MPI is the one consumer that needs a library not every machine has. Linked into
 * libgemfony-geneva, it kept every Geneva program from starting where no MPI library is installed
 * -- even for a run on the thread-pool consumer. Here, its provider lives in a library that Geneva
 * loads at run time when it is present next to libgemfony-geneva (see loadOptionalConsumerModules()
 * in GConsumerSetup.cpp); the consumer "mpi" exists exactly when that load succeeds. The library
 * also carries GMPISubClientOptimizer and GMPISubClientIndividual, which a program that uses them
 * links directly. The process's one MPI environment is not defined here but in courtier's MPI
 * library (libgemfony-courtier-mpi), which this module links: the module, the program and any other
 * library of the process share it.
 */

// Standard headers
#include <any>
#include <cstdint>
#include <memory>
#include <string>

// Boost headers
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT
#include <boost/program_options.hpp>

// Geneva headers
#include "common/GCommonEnums.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GModuleManifest.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/consumers/GMPIConsumerConfig.hpp"
#include "courtier/consumers/GMPIConsumerT.hpp"
#include "courtier/mpi/GMPIEnvironmentAccess.hpp"
#include "geneva/GConsumerPlugin.hpp"
#include "geneva/GConsumerSetup.hpp"

namespace Gem::Geneva {

namespace c2 = Gem::Courtier;
namespace po = boost::program_options;

/******************************************************************************/
namespace {

/** @brief Throws unless this process has the role @p wanted.
 *  @param wanted The role the caller is for
 *  @param caller The name of the calling function, for the message */
void requireRole(c2::processRole wanted, const char *caller) {
    const c2::processRole actual = c2::mpiProcessRole();
    if(actual != wanted) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GMPIConsumerProvider::" << caller << "(): Error!" << '\n'
            << "This process has the role " << static_cast<int>(actual) << " in the MPI "
            << "environment, but " << caller << "() is for the role " << static_cast<int>(wanted)
            << " only (see consumerRole())" << '\n'
        );
    }
}

} // namespace

/******************************************************************************/
/** @brief Provider for the MPI consumer. The role of a process follows its rank in courtier's
 *  communicator (see Gem::Courtier::mpiEnvironment()): the submitter builds the consumer, a client
 *  the MPI client, a bystander neither. */
class GMPIConsumerProvider final : public GConsumerProviderT {
public:
    [[nodiscard]] std::string getMnemonic() const override { return "mpi"; }
    [[nodiscard]] std::string getName() const override { return "GMPIConsumerT"; }
    [[nodiscard]] bool needsClient() const override { return true; }
    // MPI is deliberately NOT a bindsListeningPort() consumer: there is no port to share, and the
    // consumer is built on the submitter only.
    [[nodiscard]] bool determinesRoleAtRuntime() const override { return true; }

    /** @brief The role of this process in the MPI environment; --client plays no part.
     *  @return The role of this process */
    [[nodiscard]] c2::processRole
    role(const c2::ConsumerSpec & /*spec*/, bool /*client_requested*/) const override {
        return c2::mpiProcessRole();
    }

    std::shared_ptr<c2::GBaseConsumerT<gen::GGenome>> setup(const c2::ConsumerSpec &spec) override {
        requireRole(c2::processRole::SUBMITTER, "setup");
        auto consumer = std::make_shared<c2::GMPIConsumerT<gen::GGenome>>(
            c2::mpiEnvironment(),
            c2::configOf<c2::Consumers::MPIConsumerConfig>(spec)
        );
        consumer->startServer();
        return consumer;
    }

    [[nodiscard]] std::any configFromCommandLine(const po::variables_map &vm) const override {
        c2::Consumers::MPIConsumerConfig config;
        if(vm.contains("mpi_asyncReq")) {
            config.useAsyncReq = vm["mpi_asyncReq"].as<bool>();
        }
        // 0 == hardware concurrency: leave the struct's recommendation for this machine in place.
        if(vm.contains("mpi_nHandlerThreads") &&
           vm["mpi_nHandlerThreads"].as<std::uint32_t>() != 0) {
            config.nHandlerThreads = vm["mpi_nHandlerThreads"].as<std::uint32_t>();
        }
        if(vm.contains("mpi_cleanSessInterval")) {
            config.masterCleanSessIntervalMSec = vm["mpi_cleanSessInterval"].as<std::uint32_t>();
        }
        if(vm.contains("mpi_serializationMode")) {
            config.serializationMode =
                vm["mpi_serializationMode"].as<Gem::Common::serializationMode>();
        }
        return config;
    }

    [[nodiscard]] std::shared_ptr<c2::GBaseClientT<gen::GGenome>>
    buildClient(const c2::ConsumerSpec &spec) const override {
        requireRole(c2::processRole::CLIENT, "buildClient");
        return std::make_shared<c2::Consumers::GMPIConsumerWorkerNodeT<gen::GGenome>>(
            c2::mpiEnvironment(),
            c2::configOf<c2::Consumers::MPIConsumerConfig>(spec)
        );
    }

    void addCLOptions(po::options_description &visible, po::options_description &hidden) override {
        using Gem::Common::serializationMode;
        // The defaults mirror MPIConsumerConfig so an unset option leaves the consumer's own default in place.
        visible.add_options()(
            "mpi_asyncReq", po::value<bool>()->default_value(true),
            "\t[mpi] Whether clients prefetch the next work item")(
            "mpi_nHandlerThreads", po::value<std::uint32_t>()->default_value(0),
            "\t[mpi] The number of request-handler threads (0 == hardware concurrency)");
        hidden.add_options()(
            "mpi_cleanSessInterval", po::value<std::uint32_t>()->default_value(1000),
            "\t[mpi] Interval in ms between master session-completion checks")(
            "mpi_serializationMode",
            po::value<serializationMode>()->default_value(c2::GCONSUMERSERIALIZATIONMODE),
            "\t[mpi] Serialization in TEXTMODE (0), XMLMODE (1), BINARYMODE (2), GEM_BINARYMODE (3) or GEM_JSONMODE (4)");
    }
};

/******************************************************************************/

} /* namespace Gem::Geneva */

/******************************************************************************/

extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();

/**
 * @brief The module's entry point, resolved by the loader: one consumer contribution, "mpi".
 * @return The module's process-lifetime manifest
 */
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return Gem::Geneva::consumerManifest<Gem::Geneva::GMPIConsumerProvider, "mpi">();
}
