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
#include <memory>
#include <string>

// Boost headers
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT
#include <boost/program_options.hpp>

// Geneva headers
#include "common/GCommonEnums.hpp"
#include "common/GModuleManifest.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/consumers/GMPIConsumerT.hpp"
#include "geneva/GConsumerPlugin.hpp"
#include "geneva/GConsumerSetup.hpp"

namespace Gem::Geneva {

namespace c2 = Gem::Courtier;
namespace po = boost::program_options;

/******************************************************************************/
/** @brief Provider for the MPI consumer (built on every rank; self-determines master vs worker). */
class GMPIConsumerProvider final : public GConsumerProviderT {
public:
    [[nodiscard]] std::string getMnemonic() const override { return "mpi"; }
    [[nodiscard]] std::string getName() const override { return "GMPIConsumerT"; }
    [[nodiscard]] bool needsClient() const override { return true; }
    // MPI is deliberately NOT a bindsListeningPort() consumer: it is built on every rank and
    // self-determines master/worker, so it is constructed normally rather than reused from the registry.
    // The master/worker role is fixed by the process rank, discovered only when setup() runs on each rank.
    [[nodiscard]] bool determinesRoleAtRuntime() const override { return true; }

    ConsumerSetup setup(const ConsumerSpec &spec) override {
        // MPI fixes the master/worker split by rank; the consumer is built on every rank and branches.
        // Forward the [mpi] command-line options into the consumer's config.
        Gem::Courtier::Consumers::MPIConsumerConfig mpi_config;
        mpi_config.useAsyncReq = spec.mpi_async_req;
        // 0 == hardware concurrency: leave the default-constructed config's recommendation in place.
        if(spec.mpi_n_handler_threads != 0) {
            mpi_config.nHandlerThreads = spec.mpi_n_handler_threads;
        }
        mpi_config.serializationMode = spec.serialization_mode;
        mpi_config.masterCleanSessIntervalMSec = spec.mpi_clean_sess_interval;
        auto consumer = std::make_shared<c2::GMPIConsumerT<gen::GGenome>>(
            nullptr, nullptr, mpi_config);
        ConsumerSetup setup;
        if(consumer->isMasterNode()) {
            consumer->setCloneFunction(individualCloneFunction());
            consumer->startServer();
            setup.consumer = consumer;
        }
        else {
            // Worker rank: serve through the consumer (kept alive by the capture); no consumer to register.
            setup.run_worker = [consumer]() { consumer->runWorker(); };
        }
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
    buildClient(const ConsumerSpec & /*spec*/) const override {
        return nullptr; // the mpi worker loop comes from setup().run_worker, not a socket client
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
