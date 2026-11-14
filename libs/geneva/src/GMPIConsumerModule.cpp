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
 * MPI is the one consumer that needs a library not every machine has. Linked into libgemfony-geneva, it
 * kept every Geneva program from starting where no MPI library is installed -- even for a run on the
 * thread-pool consumer. Here, its provider lives in a library that Geneva loads at run time when it is
 * present next to libgemfony-geneva (see loadOptionalConsumerModules() in GConsumerSetup.cpp); the
 * consumer "mpi" exists exactly when that load succeeds. The library also carries GMPISubClientOptimizer
 * and GMPISubClientIndividual, which a program that uses them links directly.
 */

// Standard headers
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
#include "courtier/consumers/GMPIConsumerT.hpp"
#include "courtier/transport/GMPIEnvironment.hpp"
#include "geneva/GConsumerPlugin.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/GMPIEnvironmentAccess.hpp"

namespace Gem::Geneva {

namespace c2 = Gem::Courtier;
namespace po = boost::program_options;

/******************************************************************************/
/*
 * The one MPI environment of the process. This module is one shared library, so these objects
 * exist once, whoever links what.
 */
namespace {

using environment_type = c2::Consumers::GMPIEnvironment;

/// Guards the two pointers below: the environment is created at most once while one is alive.
std::mutex environmentMutex; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// The environment, without keeping it: the holders (the consumer, the client, the optimizer)
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

/** @brief The living environment, or a new one on @p make_base (anchored). Call with
 *  environmentMutex held.
 *  @param make_base Returns the base communicator once MPI is initialized
 *  @return The environment */
std::shared_ptr<environment_type> obtainLocked(const std::function<MPI_Comm()> &make_base) {
    auto current = environment.lock();
    if(not current) {
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
std::shared_ptr<c2::Consumers::GMPIEnvironment> mpiEnvironment() {
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
 * @throws geneva_exception if an environment exists already
 */
std::shared_ptr<c2::Consumers::GMPIEnvironment>
mpiEnvironmentOn(const std::function<MPI_Comm()> &make_base) {
    const std::scoped_lock lock(environmentMutex);
    if(not environment.expired()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In mpiEnvironmentOn(): Error!" << '\n'
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
c2::processRole mpiProcessRole() {
    const std::scoped_lock lock(environmentMutex);
    return obtainLocked(worldCommunicator)->role();
}

/******************************************************************************/
namespace {

/** @brief The configuration of the MPI consumer and its client, from the [mpi] fields of a spec.
 *  @param spec The consumer specification
 *  @return The configuration */
c2::Consumers::MPIConsumerConfig mpiConfigFrom(const ConsumerSpec &spec) {
    c2::Consumers::MPIConsumerConfig mpi_config;
    mpi_config.useAsyncReq = spec.mpi_async_req;
    // 0 == hardware concurrency: leave the default-constructed config's recommendation in place.
    if(spec.mpi_n_handler_threads != 0) {
        mpi_config.nHandlerThreads = spec.mpi_n_handler_threads;
    }
    mpi_config.serializationMode = spec.serialization_mode;
    mpi_config.masterCleanSessIntervalMSec = spec.mpi_clean_sess_interval;
    return mpi_config;
}

/** @brief Throws unless this process has the role @p wanted.
 *  @param wanted The role the caller is for
 *  @param caller The name of the calling function, for the message */
void requireRole(c2::processRole wanted, const char *caller) {
    const c2::processRole actual = mpiProcessRole();
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
 *  communicator (see mpiEnvironment()): the submitter builds the consumer, a client the MPI
 *  client, a bystander neither. */
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
    role(const ConsumerSpec & /*spec*/, bool /*client_requested*/) const override {
        return mpiProcessRole();
    }

    ConsumerSetup setup(const ConsumerSpec &spec) override {
        requireRole(c2::processRole::SUBMITTER, "setup");
        auto consumer = std::make_shared<c2::GMPIConsumerT<gen::GGenome>>(
            mpiEnvironment(),
            mpiConfigFrom(spec)
        );
        consumer->setCloneFunction(individualCloneFunction());
        consumer->startServer();
        ConsumerSetup setup;
        setup.consumer = consumer;
        return setup;
    }

    [[nodiscard]] ConsumerSpec specFromCommandLine(const po::variables_map &vm) const override {
        ConsumerSpec spec;
        if(vm.contains("mpi_asyncReq")) { spec.mpi_async_req = vm["mpi_asyncReq"].as<bool>(); }
        if(vm.contains("mpi_nHandlerThreads")) {
            spec.mpi_n_handler_threads = vm["mpi_nHandlerThreads"].as<std::uint32_t>();
        }
        if(vm.contains("mpi_cleanSessInterval")) {
            spec.mpi_clean_sess_interval = vm["mpi_cleanSessInterval"].as<std::uint32_t>();
        }
        if(vm.contains("mpi_serializationMode")) {
            spec.serialization_mode = vm["mpi_serializationMode"].as<Gem::Common::serializationMode>();
        }
        return spec;
    }

    [[nodiscard]] std::shared_ptr<c2::GBaseClientT<gen::GGenome>>
    buildClient(const ConsumerSpec &spec) const override {
        requireRole(c2::processRole::CLIENT, "buildClient");
        return std::make_shared<c2::Consumers::GMPIConsumerWorkerNodeT<gen::GGenome>>(
            mpiEnvironment(),
            mpiConfigFrom(spec)
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
