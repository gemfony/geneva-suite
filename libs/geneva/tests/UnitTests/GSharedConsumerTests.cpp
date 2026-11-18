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

/**
 * The single-consumer model: a process has at most ONE work consumer (GConsumerRegistry), shared by
 * every algorithm. This file pins three things: (1) the registry holds exactly one consumer and
 * ensureConsumer builds it only once; (2) the meta-EA evaluates its umbrella-individuals on its OWN
 * orchestration pool while their inner sub-EAs all share the one work consumer (the deadlock-free way
 * to nest optimizations); (3) several independent algorithms submitting at once fan in to that one
 * consumer. (A naive nested EA-in-EA that ran BOTH tiers on the single work pool would re-enter the pool
 * and deadlock -- which is exactly why nesting goes through the meta-EA's separate pool.)
 */

#include <catch2/catch_test_macros.hpp>

#include <boost/program_options.hpp>

#include <any>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

#include "common/GCommonEnums.hpp"
#include "common/GExceptions.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/consumers/GStdThreadConsumerConfig.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"
#include "courtier/consumers/GWebsocketConsumerConfig.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/Go2.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GMetaEvolutionaryAlgorithm.hpp"
#include "hap/GRandomFactory.hpp"

namespace gen = Gem::Geneva::Genome;
namespace oa = Gem::Geneva::OptimizationAlgorithms;

namespace {

/** @brief A tiny flat sphere: 3 constrained doubles in [-5, 5), started at 3.0. The inner problem. */
class InnerSphere : public gen::GGenomeT<InnerSphere> {
public:
    using genome_only = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    InnerSphere() {
        gen::GGenomeBuilder b;
        b.addDoubleGroup(3, -5., 5.).init(3.0);
        this->setGenome(b.build());
    }
    InnerSphere(const InnerSphere &) = default;

    std::shared_ptr<oa::GAdaptionConfigBase> buildAdaptionConfig() const {
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
        cfg->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
        return cfg;
    }

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline<double>(v);
        double s = 0.;
        for(double const x : v) {
            s += x * x;
        }
        return {s};
    }
};

/**
 * @brief A meta individual whose evaluation RUNS an inner EA over InnerSphere through the default
 * (un-injected) submission path -- i.e. exactly the nesting the invariant targets. Its own genome is
 * one constrained double (so the outer EA has something to adapt); the value is unused.
 */
class MetaSphere : public gen::GGenomeT<MetaSphere> {
public:
    using genome_only = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    MetaSphere() {
        gen::GGenomeBuilder b;
        b.addDoubleGroup(1, -5., 5.).init(3.0);
        this->setGenome(b.build());
    }
    MetaSphere(const MetaSphere &) = default;

    std::shared_ptr<oa::GAdaptionConfigBase> buildAdaptionConfig() const {
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
        cfg->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
        return cfg;
    }

protected:
    std::vector<double> evaluate() override {
        auto inner = std::make_shared<oa::GEvolutionaryAlgorithm>();
        inner->setPopulationSizes(4, 2);
        inner->setMaxIteration(2);
        inner->setReportIteration(100000);
        InnerSphere const src;
        inner->push_back(src.clone());
        inner->setAdaptionConfig(src.buildAdaptionConfig());
        // Deliberately NO setLocalConsumer(): init() defaults to the thread-pool consumer, which is the
        // un-injected path the registry must funnel into the single shared consumer.
        inner->optimize();

        auto best = inner->getBestGlobalIndividual<InnerSphere>();
        std::vector<double> v;
        best->streamline<double>(v);
        double s = 0.;
        for(double const x : v) {
            s += x * x;
        }
        return {s};
    }
};

/** @brief The configuration of the consumer of GProbeConsumerProvider, a family of the tests. */
struct GProbeConsumerConfig {
    /// The mnemonic the probe provider is registered under
    static constexpr std::string_view MNEMONIC = "probe-consumer";
    /// Passed on to the thread-pool consumer the probe builds
    unsigned int nThreads = 1;
    /// Shows which configuration reached the provider
    std::string label = "default";
};

/**
 * @brief A consumer provider of the tests: it records every configuration it is given (through
 * configOf(), as a real provider reads it) and how often it was asked to write its configuration
 * files, and builds a thread-pool consumer.
 */
class GProbeConsumerProvider final : public Gem::Geneva::GConsumerProviderT {
public:
    /** @brief The mnemonic of the probe family. @return "probe-consumer" */
    [[nodiscard]] std::string getMnemonic() const override {
        return std::string(GProbeConsumerConfig::MNEMONIC);
    }
    /** @brief The name of this provider. @return "GProbeConsumerProvider" */
    [[nodiscard]] std::string getName() const override {
        return "GProbeConsumerProvider";
    }
    /** @brief The probe is local. @return false */
    [[nodiscard]] bool needsClient() const override {
        return false;
    }

    /** @brief Records the configuration of @p spec and builds a thread-pool consumer from it.
     *  @param spec The consumer specification @return The consumer */
    std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>>
    setup(const Gem::Courtier::ConsumerSpec &spec) override {
        const auto config = Gem::Courtier::configOf<GProbeConsumerConfig>(spec);
        configsSeen.push_back(config);
        return std::make_shared<Gem::Courtier::GStdThreadConsumerT<gen::GGenome>>(
            Gem::Courtier::GStdThreadConsumerConfig{.nThreads = config.nThreads}
        );
    }

    /** @brief The probe has no options. @return The default configuration */
    [[nodiscard]] std::any
    configFromCommandLine(const boost::program_options::variables_map & /*vm*/) const override {
        return GProbeConsumerConfig{};
    }

    /** @brief Records the configuration of @p spec; the probe has no client.
     *  @param spec The consumer specification @return nullptr */
    [[nodiscard]] std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>>
    buildClient(const Gem::Courtier::ConsumerSpec &spec) const override {
        clientConfigsSeen.push_back(Gem::Courtier::configOf<GProbeConsumerConfig>(spec));
        return nullptr;
    }

    /** @brief Counts the calls. */
    void materializeConfigFiles() const override {
        ++nMaterialized;
    }

    /** @brief The probe adds no options. */
    void addCLOptions(
        boost::program_options::options_description & /*visible*/,
        boost::program_options::options_description & /*hidden*/
    ) override {                                      /* no options */
    }

    std::vector<GProbeConsumerConfig> configsSeen;               ///< What setup() was given
    mutable std::vector<GProbeConsumerConfig> clientConfigsSeen; ///< What buildClient() was given
    mutable std::size_t nMaterialized = 0; ///< Calls of materializeConfigFiles()
};

/** @brief Registers a GProbeConsumerProvider for the life of a test case and removes it again, so
 *  the process-global catalogue is left as it was found (also when a REQUIRE ends the case). */
struct GProbeRegistration {
    /// The registered provider
    std::shared_ptr<GProbeConsumerProvider> provider = std::make_shared<GProbeConsumerProvider>();
    /// Whether the provider could be registered (the mnemonic was free)
    bool registered =
        Gem::Geneva::consumerProviderStore()->setOnce(provider->getMnemonic(), provider);

    GProbeRegistration() = default;
    GProbeRegistration(const GProbeRegistration &) = delete;
    GProbeRegistration &operator=(const GProbeRegistration &) = delete;
    GProbeRegistration(GProbeRegistration &&) = delete;
    GProbeRegistration &operator=(GProbeRegistration &&) = delete;
    /** @brief Removes the provider from the catalogue. */
    ~GProbeRegistration() {
        if(registered) {
            (void)Gem::Geneva::consumerProviderStore()->remove(provider->getMnemonic());
        }
    }
};

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("Consumer registry holds one consumer", "[consumer][sharing][registry]") {
    using Registry = Gem::Courtier::GConsumerRegistryT<gen::GGenome>;
    using consumer_t = Gem::Courtier::GStdThreadConsumerT<gen::GGenome>;

    auto &reg = Registry::instance();
    reg.clear();

    CHECK(reg.consumer() == nullptr);

    // ensureConsumer builds exactly once; later (and concurrent-shaped) calls share the same consumer.
    std::size_t factory_calls = 0;
    auto factory = [&factory_calls]() -> Registry::consumer_ptr {
        ++factory_calls;
        return std::make_shared<consumer_t>();
    };
    auto first = reg.ensureConsumer(factory);
    auto second = reg.ensureConsumer(factory);
    CHECK(factory_calls == 1);     // the second call did NOT build a new consumer
    CHECK(first == second);        // it returned the same consumer
    CHECK(reg.consumer() == first);

    // setConsumer replaces it (e.g. Go2 registering the consumer it built); clear empties.
    auto replacement = std::make_shared<consumer_t>();
    reg.setConsumer(replacement);
    CHECK(reg.consumer() == replacement);
    reg.clear();
    CHECK(reg.consumer() == nullptr);
}

/******************************************************************************/

TEST_CASE(
    "Concurrent algorithms fan in to one shared thread-pool consumer",
    "[consumer][sharing][stress]") {
    using StcConsumer = Gem::Courtier::GStdThreadConsumerT<gen::GGenome>;

    // The shared thread-pool consumer is meant to serve several algorithms at once (the fan-in case):
    // each submitter waits on its OWN per-batch counter in dispatch_, not a global pool drain. Here K
    // independent EAs run on K separate threads, all un-injected so all converge on the ONE registered
    // thread-pool consumer. This exercises concurrent dispatch_ calls on the shared pool and asserts
    // (a) every algorithm still converges and (b) only ONE pool was built (bounded threads, no
    // per-algorithm oversubscription). The submitting threads are NOT pool workers, so they block on
    // their batches while the pool drains them -- no pool-reentrancy (a nested EA-in-EA on the SAME pool
    // would instead be split across kinds; see the two-tier meta-optimization model).
    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().clear();
    StcConsumer::instances_constructed().store(0);

    constexpr int K = 4;
    std::vector<double> results(K, 1.0e9); // each thread writes only its own slot -> no data race

    std::vector<std::thread> threads;
    threads.reserve(K);
    for(int t = 0; t < K; ++t) {
        threads.emplace_back([t, &results]() {
            auto ea = std::make_shared<oa::GEvolutionaryAlgorithm>();
            ea->setPopulationSizes(12, 4);
            ea->setMaxIteration(40);
            ea->setReportIteration(100000);
            InnerSphere const src;
            ea->push_back(src.clone());
            ea->setAdaptionConfig(src.buildAdaptionConfig());
            // No setLocalConsumer(): the default thread-pool consumer is resolved (and shared) via the registry.
            ea->optimize();

            auto best = ea->getBestGlobalIndividual<InnerSphere>();
            std::vector<double> v;
            best->streamline<double>(v);
            double s = 0.;
            for(double const x : v) {
                s += x * x;
            }
            results[t] = s;
        });
    }
    for(auto &th : threads) {
        th.join();
    }

    // Assertions on the main thread only (Catch2 macros are not thread-safe).
    for(double const s : results) {
        CHECK(s < 20.0); // every concurrent algorithm converged (well below the f=27 start)
    }
    CHECK(StcConsumer::instances_constructed().load() == 1); // one shared pool for all K algorithms

    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().clear();
}

/******************************************************************************/

TEST_CASE(
    "Meta-EA evaluates umbrella-individuals on its own pool; sub-EAs share the one work consumer",
    "[consumer][sharing][metaea]") {
    using StcConsumer = Gem::Courtier::GStdThreadConsumerT<gen::GGenome>;

    // The meta-EA evaluates its umbrella-individuals (MetaSphere -- each runs an inner EA) on its OWN
    // orchestration pool, NOT the work consumer. The inner EAs, un-injected, all converge on the one
    // shared work consumer. Because the orchestration pool and the work consumer are distinct pools, an
    // umbrella-individual blocking on its inner EA never starves the pool it runs on -- no deadlock. (A
    // plain EA here would instead evaluate the umbrellas on the work consumer, whose workers would then
    // block on inner work posted back to the same pool -- the deadlock the meta-EA exists to avoid.)
    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().clear();
    StcConsumer::instances_constructed().store(0);

    auto meta = std::make_shared<oa::GMetaEvolutionaryAlgorithm>();
    meta->setPopulationSizes(4, 2);
    meta->setMaxIteration(1);
    meta->setReportIteration(100000);
    meta->setNOrchestrationThreads(2);
    MetaSphere const src;
    meta->push_back(src.clone());
    meta->setAdaptionConfig(src.buildAdaptionConfig());
    meta->optimize();

    auto best = meta->getBestGlobalIndividual<MetaSphere>();
    REQUIRE(best); // completed without deadlock
    // Exactly one work consumer was built and shared by every inner EA; the meta-EA's own orchestration
    // pool is not a consumer, so it does not add to this count.
    CHECK(StcConsumer::instances_constructed().load() == 1);

    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().clear();
}

/******************************************************************************/

TEST_CASE("buildConsumer registers the process consumer", "[consumer][sharing][registry]") {
    using Registry = Gem::Courtier::GConsumerRegistryT<gen::GGenome>;

    auto &reg = Registry::instance();
    reg.clear();

    auto stc = Gem::Geneva::buildConsumer(Gem::Courtier::ConsumerSpec{.mnemonic = "stc"});
    REQUIRE(stc);
    CHECK(reg.consumer() == stc); // registered, so un-injected algorithms submit through it

    // A later build replaces the single process consumer (the process has one work endpoint).
    auto stc2 = Gem::Geneva::buildConsumer(Gem::Courtier::GStdThreadConsumerConfig{.nThreads = 1});
    REQUIRE(stc2);
    CHECK(stc2 != stc); // a genuinely new consumer was built
    CHECK(reg.consumer() == stc2);

    reg.clear();
}

// ---------------------------------------------------------------------------
// A program that knows its consumer when it is compiled passes the configuration struct; the struct
// names the mnemonic. That must build what a spec with the same mnemonic and struct builds.
TEST_CASE("the typed front door and a spec build the same consumer", "[consumer][setup][config]") {
    using Registry = Gem::Courtier::GConsumerRegistryT<gen::GGenome>;

    auto &reg = Registry::instance();
    reg.clear();

    const GProbeRegistration probe;
    REQUIRE(probe.registered);

    const GProbeConsumerConfig config{.nThreads = 2, .label = "chosen in code"};
    auto typed = Gem::Geneva::buildConsumer(config);
    auto by_spec = Gem::Geneva::buildConsumer(
        Gem::Courtier::ConsumerSpec{.mnemonic = "probe-consumer", .config = config}
    );
    REQUIRE(typed);
    REQUIRE(by_spec);
    CHECK(typeid(*typed) == typeid(*by_spec));
    CHECK(reg.consumer() == by_spec);

    // The provider was reached under the struct's mnemonic and read the same configuration twice.
    REQUIRE(probe.provider->configsSeen.size() == 2);
    CHECK(probe.provider->configsSeen[0].label == "chosen in code");
    CHECK(probe.provider->configsSeen[1].label == "chosen in code");
    CHECK(probe.provider->configsSeen[1].nThreads == 2);

    reg.clear();
}

TEST_CASE("the typed front door and a spec build the same client", "[consumer][setup][config]") {
    const GProbeRegistration probe;
    REQUIRE(probe.registered);

    const GProbeConsumerConfig config{.label = "chosen in code"};
    (void)Gem::Geneva::buildConsumerClient(config);
    (void)Gem::Geneva::buildConsumerClient(
        Gem::Courtier::ConsumerSpec{.mnemonic = "probe-consumer", .config = config}
    );
    REQUIRE(probe.provider->clientConfigsSeen.size() == 2);
    CHECK(probe.provider->clientConfigsSeen[0].label == "chosen in code");
    CHECK(probe.provider->clientConfigsSeen[1].label == "chosen in code");
}

TEST_CASE(
    "the typed front door builds a built-in consumer from its struct",
    "[consumer][setup][config]"
) {
    using Registry = Gem::Courtier::GConsumerRegistryT<gen::GGenome>;
    using StcConsumer = Gem::Courtier::GStdThreadConsumerT<gen::GGenome>;

    auto &reg = Registry::instance();
    reg.clear();

    const std::size_t before = StcConsumer::instances_constructed().load();
    auto stc = Gem::Geneva::buildConsumer(Gem::Courtier::GStdThreadConsumerConfig{.nThreads = 1});
    CHECK(std::dynamic_pointer_cast<StcConsumer>(stc) != nullptr);
    CHECK(StcConsumer::instances_constructed().load() == before + 1);
    CHECK(reg.consumer() == stc);

    reg.clear();
}

// ---------------------------------------------------------------------------
// A process that runs without Go2 (the lead of an MPI cascade) builds its client from the consumer
// options of its command line: a client only where the consumer gives it the role CLIENT.
TEST_CASE(
    "buildClientFromCommandLine builds a client for the role CLIENT only",
    "[consumer][setup]"
) {
    using Gem::Courtier::processRole;
    const auto build = [](std::vector<std::string> args) {
        args.insert(args.begin(), "program");
        args.emplace_back("--unknownOptionIsIgnored");
        std::vector<char *> argv;
        for(auto &arg : args) {
            argv.push_back(arg.data());
        }
        return Gem::Geneva::buildClientFromCommandLine(static_cast<int>(argv.size()), argv.data());
    };
    const bool gpu_allowed = Gem::Hap::gpuRandomNumbersAllowed();
    Gem::Hap::setGpuRandomNumbersAllowed(true);

    const auto submitter = build({"-c", "beast"});
    CHECK(submitter.role == processRole::SUBMITTER);
    CHECK(submitter.client == nullptr);
    const auto local = build({"--consumer", "stc", "--client"});
    CHECK(local.role == processRole::SUBMITTER);
    CHECK(local.client == nullptr);
    CHECK(Gem::Hap::gpuRandomNumbersAllowed());
    CHECK(build({}).role == processRole::SUBMITTER); // no --consumer: the default consumer's role

    const auto client = build({"-c", "beast", "--client"});
    CHECK(client.role == processRole::CLIENT);
    CHECK(client.client != nullptr);
    CHECK_FALSE(Gem::Hap::gpuRandomNumbersAllowed());

    CHECK_THROWS_AS(build({"-c", "does_not_exist"}), geneva_exception);
    Gem::Hap::setGpuRandomNumbersAllowed(gpu_allowed);
}

// ---------------------------------------------------------------------------
// --update-configs writes the configuration files a consumer provider owns alone: Go2 asks every
// provider of the catalogue through materializeConsumerConfigFiles().
TEST_CASE(
    "every consumer provider is asked to write its configuration files",
    "[consumer][setup][config]"
) {
    const GProbeRegistration probe;
    REQUIRE(probe.registered);

    Gem::Geneva::materializeConsumerConfigFiles();
    CHECK(probe.provider->nMaterialized == 1);
}

// ---------------------------------------------------------------------------
// A consumer a program hands to Go2::registerConsumer() is the process consumer of the run. Go2
// builds the consumer of its mnemonic lazily, when the run is configured (the first of optimize(),
// clientRun() and a role-at-runtime clientMode()); that build must not replace the registered one.
TEST_CASE("a consumer given to Go2::registerConsumer() is used", "[consumer][sharing][go2]") {
    using Registry = Gem::Courtier::GConsumerRegistryT<gen::GGenome>;
    using StcConsumer = Gem::Courtier::GStdThreadConsumerT<gen::GGenome>;
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "geneva_registerconsumer_tests";
    std::filesystem::create_directories(base);

    auto &reg = Registry::instance();
    reg.clear();

    int const argc = 3;
    char arg0[] = "unit-test";
    char arg1[] = "--consumer";
    char arg2[] = "stc";
    char *argv[] = {arg0, arg1, arg2, nullptr};
    {
        Gem::Geneva::Go2 go(argc, argv, base / "Go2.json");

        auto registered = std::make_shared<StcConsumer>();
        StcConsumer::instances_constructed().store(0);
        go.registerConsumer(registered);
        CHECK(reg.consumer() == registered);

        // optimize() configures the run, then throws: no individual was registered.
        CHECK_THROWS(go.optimize());

        // Still the process consumer every algorithm submits to; the mnemonic's was not built.
        CHECK(reg.consumer() == registered);
        CHECK(StcConsumer::instances_constructed().load() == 0);
    }
    reg.clear();
}

// ---------------------------------------------------------------------------
// Go2::consumer() hands out the consumer of the run -- the one every algorithm submits through --
// without owning it: what a caller keeps of it does not keep the consumer (and a networked
// consumer's server) alive beyond the Go2.
TEST_CASE("Go2::consumer() does not keep the consumer beyond its Go2", "[consumer][sharing][go2]") {
    using Registry = Gem::Courtier::GConsumerRegistryT<gen::GGenome>;
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / "geneva_go2consumer_tests";
    std::filesystem::create_directories(base);
    auto &reg = Registry::instance();
    reg.clear();

    // Whether what a caller kept still holds a consumer, whatever the type it was handed
    const auto holds = [](const auto &kept) {
        if constexpr(requires { kept.expired(); }) {
            return not kept.expired();
        }
        else {
            return kept != nullptr;
        }
    };

    int const argc = 3;
    char arg0[] = "unit-test";
    char arg1[] = "--consumer";
    char arg2[] = "stc";
    char *argv[] = {arg0, arg1, arg2, nullptr};
    std::optional<decltype(std::declval<Gem::Geneva::Go2 &>().consumer())> kept;
    {
        Gem::Geneva::Go2 go(argc, argv, base / "Go2.json");
        kept = go.consumer(); // configures the run: the consumer of "stc" is built
        REQUIRE(holds(*kept));
        CHECK(reg.consumer() != nullptr);
    }
    CHECK_FALSE(holds(*kept)); // it went with the Go2
    CHECK(reg.consumer() == nullptr);
    reg.clear();
}

// ---------------------------------------------------------------------------
// The option for the number of io threads of the socket server was registered and shown in the
// help text, but its value never reached the consumer: it always ran as many io threads as the
// machine has hardware threads. The value given on the command line -- or the default -- must be
// in the spec the consumer is built from.
TEST_CASE(
    "the io thread option of the websocket consumer takes effect",
    "[consumer][setup][options]"
) {
    namespace po = boost::program_options;

    po::options_description visible;
    po::options_description hidden;
    Gem::Geneva::addConsumerOptions(visible, hidden);
    po::options_description all;
    all.add(visible).add(hidden);

    const auto spec_for =
        [&all](const std::string &mnemonic, const std::vector<std::string> &args) {
            po::variables_map vm;
            po::store(po::command_line_parser(args).options(all).run(), vm);
            po::notify(vm);
            return Gem::Geneva::specFromCommandLine(mnemonic, vm);
        };
    const auto beast_threads = [&spec_for](const std::vector<std::string> &args) {
        return Gem::Courtier::configOf<Gem::Courtier::GWebsocketConsumerConfig>(
                   spec_for("beast", args)
        )
            .nThreads;
    };

    CHECK(beast_threads({"--beast_nListenerThreads", "7"}) == 7);
    CHECK(beast_threads({"--beast_nListenerThreads", "0"}) == 0); // hardware threads

    // Without the option the documented default applies, not the number of hardware threads.
    CHECK(beast_threads({}) == Gem::Courtier::GCONSUMERLISTENERTHREADS);
}

// ---------------------------------------------------------------------------
// The serialization options name the modes there are (their help text used to offer the removed
// Boost archives TEXTMODE, XMLMODE and BINARYMODE), and the parser refuses a value that names none.
TEST_CASE(
    "the serialization options name the modes there are and refuse others",
    "[consumer][setup][options]"
) {
    namespace po = boost::program_options;

    po::options_description visible;
    po::options_description hidden;
    Gem::Geneva::addConsumerOptions(visible, hidden);
    po::options_description all;
    all.add(visible).add(hidden);
    CHECK(
        all.find("beast_serializationMode", false)
            .description()
            .contains(Gem::Common::serializationModeChoices())
    );
    for(const auto &option : all.options()) {
        for(const std::string_view stale :
            {"TEXTMODE", "XMLMODE", "BINARYMODE (", "GEM_JSONMODE"}) {
            INFO(option->long_name() << ": " << stale);
            CHECK_FALSE(option->description().contains(stale));
        }
    }

    const auto parse = [&all](const std::string &value) {
        po::variables_map vm;
        po::store(
            po::command_line_parser(std::vector<std::string>{"--beast_serializationMode", value})
                .options(all)
                .run(),
            vm
        );
        po::notify(vm);
        return Gem::Courtier::configOf<Gem::Courtier::GWebsocketConsumerConfig>(
                   Gem::Geneva::specFromCommandLine("beast", vm)
        )
            .serializationMode;
    };
    for(const auto mode : Gem::Common::SERIALIZATIONMODES) {
        CHECK(parse(std::to_string(std::to_underlying(mode))) == mode);
    }
    CHECK_THROWS(parse("0")); // the removed text archive
    CHECK_THROWS(parse("2")); // the removed binary archive
}

// ---------------------------------------------------------------------------
// The defaults of the websocket consumer's options are those of its configuration struct: an option
// left unset leaves the consumer as a default-constructed configuration describes it, and a default
// changed in the struct changes what the command line gives, without a second place to edit. The
// port is the one exception: on the command line server and client are two processes and meet on
// GCONSUMERDEFAULTPORT, while a configuration built in a program lets the server pick a free port.
TEST_CASE(
    "the websocket consumer's options default to its configuration struct",
    "[consumer][setup][options]"
) {
    namespace po = boost::program_options;

    po::options_description visible;
    po::options_description hidden;
    Gem::Geneva::addConsumerOptions(visible, hidden);
    po::options_description all;
    all.add(visible).add(hidden);
    po::variables_map vm;
    po::store(po::command_line_parser(std::vector<std::string>{}).options(all).run(), vm);
    po::notify(vm);

    const auto beast = Gem::Courtier::configOf<Gem::Courtier::GWebsocketConsumerConfig>(
        Gem::Geneva::specFromCommandLine("beast", vm)
    );
    const Gem::Courtier::GWebsocketConsumerConfig beast_defaults;
    CHECK(beast.ip == beast_defaults.ip);
    CHECK(beast.port == Gem::Courtier::GCONSUMERDEFAULTPORT);
    CHECK(beast.serializationMode == beast_defaults.serializationMode);
    CHECK(beast.nThreads == beast_defaults.nThreads);
    CHECK(beast.pingInterval == beast_defaults.pingInterval);
    CHECK(beast.verboseControlFrames == beast_defaults.verboseControlFrames);
    CHECK(beast.clientPrefetchDepth == beast_defaults.clientPrefetchDepth);
    CHECK(beast.clientParallelEvaluations == beast_defaults.clientParallelEvaluations);
    CHECK(beast.clientFirstContactLimit == beast_defaults.clientFirstContactLimit);
    CHECK(beast.clientReconnectLimit == beast_defaults.clientReconnectLimit);
    CHECK(beast.clientUndeliveredDir == beast_defaults.clientUndeliveredDir);
}

// ---------------------------------------------------------------------------
// The thread consumer registered an option "stcCapableOfFullReturn" that nothing ever read. An
// option that is offered must have an effect, so it is gone; the consumers' option set must not
// offer it (the neighbouring stc option is there, so the lookup itself works).
TEST_CASE(
    "the consumers do not offer the unread option stcCapableOfFullReturn",
    "[consumer][setup][options]"
) {
    namespace po = boost::program_options;

    po::options_description visible;
    po::options_description hidden;
    Gem::Geneva::addConsumerOptions(visible, hidden);

    CHECK(hidden.find_nothrow("nWorkerThreads", false) != nullptr);
    CHECK(visible.find_nothrow("stcCapableOfFullReturn", false) == nullptr);
    CHECK(hidden.find_nothrow("stcCapableOfFullReturn", false) == nullptr);
}

// ---------------------------------------------------------------------------
// The consumer catalog is data in the shared provider store, not a hard-coded table + switch.

TEST_CASE("Consumer provider store exposes the built-in consumer catalog", "[consumer][sharing][registry]") {
    // The built-in local + socket consumers are always registered; unknown mnemonics are not (the retired
    // "sc" serial consumer is now "stc" with one thread, so "sc" is no longer a known mnemonic).
    CHECK(Gem::Geneva::isKnownConsumer("stc"));
    CHECK(Gem::Geneva::isKnownConsumer("beast"));
    CHECK_FALSE(Gem::Geneva::isKnownConsumer("sc"));
    CHECK_FALSE(Gem::Geneva::isKnownConsumer("does_not_exist"));

    // The local consumer needs no networked client; the socket consumers do. An unknown mnemonic never does.
    CHECK_FALSE(Gem::Geneva::consumerNeedsClient("stc"));
    CHECK(Gem::Geneva::consumerNeedsClient("beast"));
    CHECK_FALSE(Gem::Geneva::consumerNeedsClient("does_not_exist"));

    // The listing names each registered consumer, and the count covers at least the two always-built ones.
    const std::string listing = Gem::Geneva::consumerListing();
    CHECK(listing.contains("stc:"));
    CHECK(listing.contains("beast:"));
    CHECK_FALSE(listing.contains("sc:"));
    CHECK(Gem::Geneva::consumerCount() >= 2);

    // The always-built consumers take their client/server role from --client (known before the consumer is
    // built), so they do NOT determine it at runtime. This is the generic capability generic drivers query
    // instead of naming a specific consumer. An unknown mnemonic is never role-at-runtime.
    CHECK_FALSE(Gem::Geneva::consumerDeterminesRoleAtRuntime("stc"));
    CHECK_FALSE(Gem::Geneva::consumerDeterminesRoleAtRuntime("beast"));
    CHECK_FALSE(Gem::Geneva::consumerDeterminesRoleAtRuntime("does_not_exist"));
    // The MPI consumer, when built into this binary, self-assigns the role from its process rank.
    if(Gem::Geneva::isKnownConsumer("mpi")) {
        CHECK(Gem::Geneva::consumerDeterminesRoleAtRuntime("mpi"));
    }
}
