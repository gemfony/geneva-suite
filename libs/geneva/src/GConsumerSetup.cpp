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

#include "geneva/GConsumerSetup.hpp"

// Standard headers
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

// Default values for the consumer command-line options.
#include "courtier/GCourtierEnums.hpp"

#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GParserBuilder.hpp" // GParserBuilder::updateInPlace() -- materialize the networked config too
#include "courtier/GBaseClientT.hpp"
#include "courtier/GConsumerRegistry.hpp"

// The concrete courtier consumers -- known ONLY here.
#include "courtier/consumers/GAsioConsumerT.hpp"
#include "courtier/consumers/GNetworkedConsumerT.hpp" // apply the config-file timeout treatment
#include "courtier/consumers/GNetworkedTimeoutConfig.hpp" // GNetworkedTimeoutConfig
#include "courtier/consumers/GMPIConsumerT.hpp" // self-guarded by GENEVA_BUILD_WITH_MPI_CONSUMER
#include "courtier/consumers/GStdThreadConsumerT.hpp"
#include "courtier/consumers/GWebsocketConsumerT.hpp"

// The networked clients are wire-compatible with the courtier socket servers and are reused as-is;
// like the consumers, the concrete client types are known ONLY here.
#include "courtier/transport/GAsioTransportT.hpp"     // GAsioConsumerClientT
#include "courtier/transport/GWebsocketTransportT.hpp" // GWebsocketClientT

// The GPU consumer + the marshaller store it is built from -- known ONLY here (folded into
// gemfony-courtier / geneva only when the GPU consumer is built).
#ifdef GENEVA_BUILD_WITH_GPU_CONSUMER
#include "courtier/gpu/GGPUConsumer.hpp"
#include "geneva/GMarshallerSetup.hpp"
#include "geneva/genome/GBaseGPUMarshallerT.hpp"
#endif /* GENEVA_BUILD_WITH_GPU_CONSUMER */

namespace Gem::Geneva {

namespace c2 = Gem::Courtier;
namespace po = boost::program_options;

namespace {

/**
 * @brief Builds the polymorphic clone functor for GGenome.
 *
 * Copy-construction would slice the held individual, so a virtual clone() is used instead.
 *
 * @return A functor that deep-copies a GGenome via its virtual clone()
 */
std::function<std::unique_ptr<gen::GGenome>(const std::unique_ptr<gen::GGenome> &)>
individualCloneFunction() {
    return [](const std::unique_ptr<gen::GGenome> &p) { return p->clone(); };
}

/******************************************************************************/
// The concrete consumer providers. Each moves the construction / option / client logic that the old
// hard-coded table + switch held for one mnemonic into its own provider; they register into
// consumerProviderStore() at static init (see the registrar below). Building a consumer has side effects,
// so provide() (inherited) stays the default nullptr and the consumer is built through setup().

/** @brief Provider for the local single-/multi-threaded (std::thread pool) consumer. This is the only
 *  local consumer: serial execution is this consumer with one worker thread (nWorkerThreads == 1). */
class GStdThreadConsumerProvider final : public GConsumerProviderT {
public:
    [[nodiscard]] std::string getMnemonic() const override { return "stc"; }
    [[nodiscard]] std::string getName() const override { return "GStdThreadConsumerT"; }
    [[nodiscard]] bool needsClient() const override { return false; }

    ConsumerSetup setup(const ConsumerSpec &spec) override {
        auto consumer = std::make_shared<c2::GStdThreadConsumerT<gen::GGenome>>(spec.n_threads);
        consumer->setCloneFunction(individualCloneFunction());
        ConsumerSetup setup;
        setup.consumer = consumer;
        return setup;
    }

    [[nodiscard]] ConsumerSpec specFromCommandLine(const po::variables_map &vm) const override {
        ConsumerSpec spec;
        if(vm.contains("nWorkerThreads")) {
            spec.n_threads = static_cast<unsigned int>(vm["nWorkerThreads"].as<std::size_t>());
        }
        return spec;
    }

    [[nodiscard]] std::shared_ptr<c2::GBaseClientT<gen::GGenome>>
    buildClient(const ConsumerSpec & /*spec*/) const override {
        return nullptr; // local-only
    }

    void addCLOptions(po::options_description & /*visible*/, po::options_description &hidden) override {
        // 0 means "the consumer's own default" (hardware concurrency).
        hidden.add_options()(
            "nWorkerThreads", po::value<std::size_t>()->default_value(0),
            "\t[stc] The number of worker threads (0 == hardware concurrency)")(
            "stcCapableOfFullReturn", po::value<bool>()->default_value(true),
            "\t[stc] A debugging option toggling timeouts in the executor");
    }
};

/** @brief Provider for the Asio (raw socket) networked consumer. */
class GAsioConsumerProvider final : public GConsumerProviderT {
public:
    [[nodiscard]] std::string getMnemonic() const override { return "asio"; }
    [[nodiscard]] std::string getName() const override { return "GAsioConsumerT"; }
    [[nodiscard]] bool needsClient() const override { return true; }
    [[nodiscard]] bool bindsListeningPort() const override { return true; }

    ConsumerSetup setup(const ConsumerSpec &spec) override {
        auto consumer = std::make_shared<c2::GAsioConsumerT<gen::GGenome>>(
            spec.port, spec.n_threads, spec.serialization_mode);
        consumer->setCloneFunction(individualCloneFunction());
        consumer->startServer();
        ConsumerSetup setup;
        setup.consumer = consumer;
        return setup;
    }

    [[nodiscard]] ConsumerSpec specFromCommandLine(const po::variables_map &vm) const override {
        ConsumerSpec spec;
        if(vm.contains("asio_port")) { spec.port = vm["asio_port"].as<unsigned short>(); }
        if(vm.contains("asio_serializationMode")) {
            spec.serialization_mode = vm["asio_serializationMode"].as<Gem::Common::serializationMode>();
        }
        if(vm.contains("asio_ip")) { spec.ip = vm["asio_ip"].as<std::string>(); }
        if(vm.contains("asio_maxReconnects")) {
            spec.max_reconnects = vm["asio_maxReconnects"].as<std::size_t>();
        }
        if(vm.contains("asio_prefetchDepth")) {
            spec.client_prefetch_depth = vm["asio_prefetchDepth"].as<std::size_t>();
        }
        spec.n_threads = 0; // networked IO threads: hardware concurrency
        return spec;
    }

    [[nodiscard]] std::shared_ptr<c2::GBaseClientT<gen::GGenome>>
    buildClient(const ConsumerSpec &spec) const override {
        return std::make_shared<c2::Consumers::GAsioConsumerClientT<gen::GGenome>>(
            spec.ip, spec.port, spec.serialization_mode, spec.max_reconnects, spec.client_prefetch_depth);
    }

    void addCLOptions(po::options_description &visible, po::options_description &hidden) override {
        using Gem::Common::serializationMode;
        visible.add_options()(
            "asio_ip", po::value<std::string>()->default_value(c2::GCONSUMERDEFAULTSERVER),
            "\t[asio] The name or ip of the server")(
            "asio_port", po::value<unsigned short>()->default_value(c2::GCONSUMERDEFAULTPORT),
            "\t[asio] The port of the server");
        hidden.add_options()(
            "asio_serializationMode",
            po::value<serializationMode>()->default_value(c2::GCONSUMERSERIALIZATIONMODE),
            "\t[asio] Serialization in TEXTMODE (0), XMLMODE (1), BINARYMODE (2), GEM_BINARYMODE (3) or GEM_JSONMODE (4)")(
            "asio_nProcessingThreads",
            po::value<std::size_t>()->default_value(c2::GCONSUMERLISTENERTHREADS),
            "\t[asio] The number of threads used to process incoming connections")(
            "asio_maxReconnects",
            po::value<std::size_t>()->default_value(c2::GASIOCONSUMERMAXCONNECTIONATTEMPTS),
            "\t[asio] The maximum number of client reconnection attempts")(
            "asio_prefetchDepth",
            po::value<std::size_t>()->default_value(1),
            "\t[asio] Max work items a client holds at once (1 == serial; >1 overlaps fetch/compute/return)");
    }
};

/** @brief Provider for the Boost.Beast (websocket) networked consumer. */
class GWebsocketConsumerProvider final : public GConsumerProviderT {
public:
    [[nodiscard]] std::string getMnemonic() const override { return "beast"; }
    [[nodiscard]] std::string getName() const override { return "GWebsocketConsumerT"; }
    [[nodiscard]] bool needsClient() const override { return true; }
    [[nodiscard]] bool bindsListeningPort() const override { return true; }

    ConsumerSetup setup(const ConsumerSpec &spec) override {
        auto consumer = std::make_shared<c2::GWebsocketConsumerT<gen::GGenome>>(
            spec.port, spec.n_threads, spec.serialization_mode);
        consumer->setCloneFunction(individualCloneFunction());
        consumer->startServer();
        ConsumerSetup setup;
        setup.consumer = consumer;
        return setup;
    }

    [[nodiscard]] ConsumerSpec specFromCommandLine(const po::variables_map &vm) const override {
        ConsumerSpec spec;
        if(vm.contains("beast_port")) { spec.port = vm["beast_port"].as<unsigned short>(); }
        if(vm.contains("beast_serializationMode")) {
            spec.serialization_mode = vm["beast_serializationMode"].as<Gem::Common::serializationMode>();
        }
        if(vm.contains("beast_ip")) { spec.ip = vm["beast_ip"].as<std::string>(); }
        if(vm.contains("beast_verboseControlFrames")) {
            spec.verbose_control_frames = vm["beast_verboseControlFrames"].as<bool>();
        }
        if(vm.contains("beast_prefetchDepth")) {
            spec.client_prefetch_depth = vm["beast_prefetchDepth"].as<std::size_t>();
        }
        spec.n_threads = 0;
        return spec;
    }

    [[nodiscard]] std::shared_ptr<c2::GBaseClientT<gen::GGenome>>
    buildClient(const ConsumerSpec &spec) const override {
        return std::make_shared<c2::Consumers::GWebsocketClientT<gen::GGenome>>(
            spec.ip, spec.port, spec.serialization_mode, spec.verbose_control_frames,
            spec.client_prefetch_depth);
    }

    void addCLOptions(po::options_description &visible, po::options_description &hidden) override {
        using Gem::Common::serializationMode;
        visible.add_options()(
            "beast_ip", po::value<std::string>()->default_value(c2::GCONSUMERDEFAULTSERVER),
            "\t[beast] The name or ip of the server")(
            "beast_port", po::value<unsigned short>()->default_value(c2::GCONSUMERDEFAULTPORT),
            "\t[beast] The port of the server");
        hidden.add_options()(
            "beast_serializationMode",
            po::value<serializationMode>()->default_value(c2::GCONSUMERSERIALIZATIONMODE),
            "\t[beast] Serialization in TEXTMODE (0), XMLMODE (1), BINARYMODE (2), GEM_BINARYMODE (3) or GEM_JSONMODE (4)")(
            "beast_nListenerThreads",
            po::value<std::size_t>()->default_value(c2::GCONSUMERLISTENERTHREADS),
            "\t[beast] The number of threads used to listen for incoming connections")(
            "beast_pingInterval",
            po::value<std::size_t>()->default_value(c2::GBEASTCONSUMERPINGINTERVAL),
            "\t[beast] The number of seconds between two consecutive pings")(
            "beast_verboseControlFrames",
            po::value<bool>()->default_value(false)->implicit_value(true),
            "\t[beast] Announce ping/pong/close frames")(
            "beast_prefetchDepth",
            po::value<std::size_t>()->default_value(1),
            "\t[beast] Max work items a client holds at once (1 == serial; >1 overlaps fetch/compute/return)");
    }
};

#ifdef GENEVA_BUILD_WITH_MPI_CONSUMER
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
#endif /* GENEVA_BUILD_WITH_MPI_CONSUMER */

#ifdef GENEVA_BUILD_WITH_GPU_CONSUMER
/**
 * @brief Provider for the (local, device-only) GPU consumer.
 *
 * The GPU consumer is a normal mnemonic ("gpu") built through buildConsumerSetup() like every other
 * consumer. The one GPU-specific piece -- the problem's device marshaller (flatten / scatter + kernel) --
 * is contributed into marshallerProviderStore() by the problem; setup() looks it up under its device
 * target ("cuda"), reads its scalar kind and builds the matching GGPUConsumerT<..,scalar_type> around it.
 * A missing marshaller means the problem never opted into GPU evaluation -> a clear error.
 *
 * The scalar (float / double) stays a compile-time template parameter of GGPUConsumerT; the marshaller
 * declares it at runtime (GGPUMarshallerHandle::scalarKind()) and buildGPUConsumer() recovers the typed
 * marshaller for the one matching instantiation. The GPU consumer is device-only and local (no wire, no
 * client), so a CPU run uses a CPU consumer (e.g. --consumer stc) via the individual's own evaluate().
 */
class GGPUConsumerProvider final : public GConsumerProviderT {
public:
    [[nodiscard]] std::string getMnemonic() const override { return "gpu"; }
    [[nodiscard]] std::string getName() const override { return "GGPUConsumerT"; }
    [[nodiscard]] bool needsClient() const override { return false; }

    ConsumerSetup setup(const ConsumerSpec & /*spec*/) override {
        // The problem contributes its GPU marshaller into marshallerProviderStore() (compiled-in: before
        // constructing Go2; a loaded individual module: at module load). Its absence means "--consumer gpu"
        // was selected for a problem that never registered a device marshaller.
        std::shared_ptr<Gem::Common::GProviderT<GGPUMarshallerHandle>> base;
        if(not marshallerProviderStore()->get("cuda", base) || not base) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGPUConsumerProvider::setup(): Error!" << '\n'
                << "\"--consumer gpu\" was selected but no GPU marshaller is registered for device target" << '\n'
                << "\"cuda\". A GPU problem must register its marshaller before constructing Go2, e.g." << '\n'
                << "Gem::Geneva::registerGPUMarshaller<YourMarshaller>(\"cuda\", <gpu-config-file>)." << '\n'
            );
        }
        auto provider           = std::static_pointer_cast<GGPUMarshallerProviderBase>(base);
        auto handle             = provider->provide();
        const std::string &cfg  = provider->gpuConfigFile();

        ConsumerSetup setup;
        switch(handle->scalarKind()) {
            case GPUScalarKind::Double:
                setup.consumer = buildGPUConsumer<double>(handle, cfg);
                break;
            case GPUScalarKind::Float:
                setup.consumer = buildGPUConsumer<float>(handle, cfg);
                break;
        }
        return setup;
    }

    [[nodiscard]] ConsumerSpec specFromCommandLine(const po::variables_map & /*vm*/) const override {
        return ConsumerSpec{};
    }

    [[nodiscard]] std::shared_ptr<c2::GBaseClientT<gen::GGenome>>
    buildClient(const ConsumerSpec & /*spec*/) const override {
        return nullptr;
    }

    void addCLOptions(po::options_description & /*visible*/, po::options_description & /*hidden*/) override {
        /* the gpu consumer's config (backend / kernel) lives in its own config file, not on the command line */
    }

private:
    /**
     * @brief Recovers the scalar-typed marshaller from the handle and builds the matching GPU consumer.
     * @tparam scalar_type The device scalar the marshaller was built for (matched to its scalarKind())
     * @param handle The scalar-agnostic marshaller handle from the store
     * @param configFile The GPU-consumer config file (backend + kernel selection)
     * @return The ready GPU consumer (clone function set)
     */
    template <typename scalar_type>
    static std::shared_ptr<c2::GBaseConsumerT<gen::GGenome>>
    buildGPUConsumer(const std::shared_ptr<GGPUMarshallerHandle> &handle, const std::string &configFile) {
        auto marshaller = std::dynamic_pointer_cast<
            Gem::Courtier::GPU::GGPUEvaluableI<gen::GGenome, scalar_type>>(handle);
        if(not marshaller) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGPUConsumerProvider::buildGPUConsumer(): Error!" << '\n'
                << "The registered GPU marshaller does not match its own declared scalar kind." << '\n'
            );
        }
        auto consumer = std::make_shared<
            Gem::Courtier::GPU::GGPUConsumerT<gen::GGenome, scalar_type>>(configFile, marshaller);
        consumer->setCloneFunction(individualCloneFunction());
        return consumer;
    }
};
#endif /* GENEVA_BUILD_WITH_GPU_CONSUMER */

/******************************************************************************/
/**
 * @brief Registers every built-in consumer provider into the process-global store at static init.
 *
 * GConsumerSetup.cpp is always linked (Go2 calls its free functions), so this static object runs and the
 * consumer catalog is populated before any consumer is selected. A runtime-loaded consumer module would
 * register its own provider into the same store.
 */
struct ConsumerProviderRegistrar {
    ConsumerProviderRegistrar() {
        auto store = consumerProviderStore();
        store->setOnce(GDEFAULTCONSUMERMNEMONIC, std::make_shared<GStdThreadConsumerProvider>());
        store->setOnce("asio", std::make_shared<GAsioConsumerProvider>());
        store->setOnce("beast", std::make_shared<GWebsocketConsumerProvider>());
#ifdef GENEVA_BUILD_WITH_MPI_CONSUMER
        store->setOnce("mpi", std::make_shared<GMPIConsumerProvider>());
#endif /* GENEVA_BUILD_WITH_MPI_CONSUMER */
#ifdef GENEVA_BUILD_WITH_GPU_CONSUMER
        store->setOnce("gpu", std::make_shared<GGPUConsumerProvider>());
#endif /* GENEVA_BUILD_WITH_GPU_CONSUMER */
    }
};

const ConsumerProviderRegistrar g_consumer_provider_registrar{};

/******************************************************************************/
/**
 * @brief Resolves a mnemonic to its consumer provider (downcast from the homogeneous store).
 *
 * @param mnemonic The consumer mnemonic to look up
 * @return The matching consumer provider, or nullptr if the mnemonic is unknown
 */
std::shared_ptr<GConsumerProviderT> lookupConsumerProvider(const std::string &mnemonic) {
    std::shared_ptr<Gem::Common::GProviderT<c2::GBaseConsumerT<gen::GGenome>>> base;
    if(consumerProviderStore()->get(mnemonic, base) && base) {
        return std::static_pointer_cast<GConsumerProviderT>(base);
    }
    return nullptr;
}

/**
 * @brief Returns all registered consumer providers (as concrete GConsumerProviderT), sorted by mnemonic.
 *
 * Sorting gives the help/listing output a deterministic order independent of the store's internal layout.
 *
 * @return The registered providers, ordered by mnemonic
 */
std::vector<std::shared_ptr<GConsumerProviderT>> allConsumerProviders() {
    std::vector<std::shared_ptr<GConsumerProviderT>> providers;
    for(const auto &base : consumerProviderStore()->getContentSnapshot()) {
        if(base) { providers.push_back(std::static_pointer_cast<GConsumerProviderT>(base)); }
    }
    std::ranges::sort(providers, {}, [](const auto &p) { return p->getMnemonic(); });
    return providers;
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * @brief Builds a consumer (and, for MPI workers, a worker loop) from a consumer specification.
 *
 * Resolves the mnemonic against the consumer-provider store and invokes the matching provider's setup().
 * A socket consumer whose process already holds a consumer reuses it rather than binding the port again;
 * the freshly-built consumer (if any) is registered as the process's single consumer. An MPI worker rank
 * yields a run_worker callable instead of a consumer.
 *
 * @param spec The consumer specification (mnemonic plus port/threads/serialization settings)
 * @return A ConsumerSetup holding the consumer and/or worker loop; empty for an unknown mnemonic
 */
ConsumerSetup buildConsumerSetup(const ConsumerSpec &spec) {
    auto &registry = c2::GConsumerRegistryT<gen::GGenome>::instance();

    auto provider = lookupConsumerProvider(spec.mnemonic);
    if(not provider) {
        return {}; // unknown mnemonic -> empty setup
    }

    // Idempotent networked build: a socket server binds a port, so if the process already has a consumer,
    // reuse it rather than binding again -- one listening endpoint per process. (MPI is excluded: it does
    // not report bindsListeningPort(), so it is constructed normally and self-determines master/worker.)
    if(provider->bindsListeningPort()) {
        if(auto existing = registry.consumer()) {
            ConsumerSetup setup;
            setup.consumer = existing;
            return setup;
        }
    }

    ConsumerSetup setup = provider->setup(spec);

    // Networked consumers (ASIO / websocket / MPI, all GNetworkedConsumerT) read their SERVER-side timeout /
    // death-detection treatment from a config file (created with scale-free adaptive defaults if absent), so
    // a user can select "fixed" / "wait_indefinitely" or tune the adaptive lease purely via the config --
    // no command line. Applied once here for whichever networked consumer was built; local / GPU consumers
    // (not GNetworkedConsumerT) are unaffected.
    if(auto *networked =
           dynamic_cast<c2::GNetworkedConsumerT<gen::GGenome> *>(setup.consumer.get())) {
        c2::GNetworkedTimeoutConfig timeout_cfg;
        timeout_cfg.load("./config/GNetworkedConsumer.json");
        networked->applyTimeoutConfig(timeout_cfg);
    }
    else if(Gem::Common::GParserBuilder::updateInPlace()) {
        // --update-configs (build-time materialization) forces the local thread-pool consumer, so no
        // networked consumer is built above and its config would be missed. Materialize it here too --
        // loading it under update-in-place writes/refreshes the file with scale-free adaptive defaults --
        // so GNetworkedConsumer.json joins every binary's emitted config set, like its other configs.
        c2::GNetworkedTimeoutConfig timeout_cfg;
        timeout_cfg.load("./config/GNetworkedConsumer.json");
    }

    // Register the freshly-built consumer as the process's single consumer so every algorithm submits
    // through it. An MPI worker rank has no consumer (run_worker only), so nothing is registered there.
    if(setup.consumer) {
        registry.setConsumer(setup.consumer);
    }

    return setup;
}

/******************************************************************************/
/**
 * @brief Returns the process consumer, building the default one on first use.
 *
 * The default choice is this layer's, not the algorithms' and not courtier's: the registry is asked for
 * the one consumer and, only if the process has none, hands the build back here, where the concrete
 * consumer types are known. The build goes through the provider store like every other consumer, so a
 * registered replacement for the default mnemonic is honoured.
 *
 * @return The process's single consumer (never null)
 */
std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> ensureProcessConsumer() {
    return c2::GConsumerRegistryT<gen::GGenome>::instance().ensureConsumer(
        []() -> std::shared_ptr<c2::GBaseConsumerT<gen::GGenome>> {
            auto provider = lookupConsumerProvider(GDEFAULTCONSUMERMNEMONIC);
            if(not provider) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In Gem::Geneva::ensureProcessConsumer(): Error!" << '\n'
                    << "The default consumer \"" << GDEFAULTCONSUMERMNEMONIC << "\" is not registered"
                    << '\n'
                    << "in the consumer-provider store." << '\n'
                );
            }
            return provider->setup(ConsumerSpec{.mnemonic = GDEFAULTCONSUMERMNEMONIC}).consumer;
        });
}

/******************************************************************************/
/**
 * @brief Reads a consumer specification out of the parsed command-line options.
 *
 * Delegates the per-consumer option reading to the matching provider; the mnemonic is filled in here.
 * An unknown mnemonic yields a spec carrying only the mnemonic.
 *
 * @param mnemonic The selected consumer mnemonic (e.g. "asio", "beast", "stc")
 * @param vm The parsed program-options map produced during command-line parsing
 * @return A ConsumerSpec populated from vm for the given mnemonic
 */
ConsumerSpec specFromCommandLine(
    const std::string &mnemonic, const boost::program_options::variables_map &vm) {
    auto provider = lookupConsumerProvider(mnemonic);
    if(not provider) {
        ConsumerSpec spec;
        spec.mnemonic = mnemonic;
        return spec;
    }
    ConsumerSpec spec = provider->specFromCommandLine(vm);
    spec.mnemonic = mnemonic;
    return spec;
}

/******************************************************************************/
/**
 * @brief Builds the networked client matching a consumer specification.
 *
 * Delegates to the matching provider's buildClient(); the local consumer (stc) and MPI (whose worker
 * loop comes from buildConsumerSetup().run_worker) return nullptr.
 *
 * @param spec The consumer specification (mnemonic plus connection/serialization settings)
 * @return A client for asio/beast; nullptr for local-only or non-client consumers
 */
std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>>
buildConsumerClient(const ConsumerSpec &spec) {
    auto provider = lookupConsumerProvider(spec.mnemonic);
    return provider ? provider->buildClient(spec) : nullptr;
}

/******************************************************************************/
/**
 * @brief Registers all consumer-related command-line options.
 *
 * Iterates the registered consumer providers and lets each add its options, splitting user-facing options
 * from rarely-used/debug ones.
 *
 * @param visible The options description for user-facing options (shown in --help)
 * @param hidden The options description for hidden/advanced options
 */
void addConsumerOptions(
    boost::program_options::options_description &visible,
    boost::program_options::options_description &hidden) {
    for(const auto &provider : allConsumerProviders()) {
        provider->addCLOptions(visible, hidden);
    }
}

/******************************************************************************/
/**
 * @brief Reports whether a mnemonic names a supported consumer.
 *
 * @param mnemonic The consumer mnemonic to test
 * @return true if the mnemonic is a known/supported consumer, false otherwise
 */
bool isKnownConsumer(const std::string &mnemonic) {
    return consumerProviderStore()->exists(mnemonic);
}

/******************************************************************************/
/**
 * @brief Reports whether a consumer requires a separate networked client process.
 *
 * @param mnemonic The consumer mnemonic to test
 * @return true if the consumer can have a client (networked consumers), false otherwise (incl. unknown mnemonics)
 */
bool consumerNeedsClient(const std::string &mnemonic) {
    auto provider = lookupConsumerProvider(mnemonic);
    return provider != nullptr && provider->needsClient();
}

/******************************************************************************/
/**
 * @brief Reports whether a consumer determines each process's client/server role at runtime (e.g. from an
 * MPI rank) rather than from --client.
 *
 * @param mnemonic The consumer mnemonic to test
 * @return true if the consumer self-assigns the role at runtime, false otherwise (incl. unknown mnemonics)
 */
bool consumerDeterminesRoleAtRuntime(const std::string &mnemonic) {
    auto provider = lookupConsumerProvider(mnemonic);
    return provider != nullptr && provider->determinesRoleAtRuntime();
}

/******************************************************************************/
/**
 * @brief Produces a human-readable listing of all supported consumers.
 *
 * @return A newline-separated string mapping each mnemonic to its class name
 */
std::string consumerListing() {
    std::string result;
    for(const auto &provider : allConsumerProviders()) {
        result += std::format("{}:  {}\n", provider->getMnemonic(), provider->getName());
    }
    return result;
}

/******************************************************************************/
/**
 * @brief Returns the number of supported consumers.
 *
 * @return The count of registered consumer providers (build-dependent, e.g. +1 with MPI)
 */
std::size_t consumerCount() {
    return consumerProviderStore()->size();
}

/******************************************************************************/

} /* namespace Gem::Geneva */
