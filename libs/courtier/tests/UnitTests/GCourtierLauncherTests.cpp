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
 * @file
 * The launcher (courtier/GLauncherT.hpp) on the item type GSimpleContainer, with the consumer
 * modules of launcher/: the default consumer from its module, --help and --showAll, a malformed
 * command line, the refusals (an unknown consumer, a module that cannot be loaded, a module for
 * another item type, --client for a consumer without clients, a consumer that misses a
 * prerequisite, a second item type, a second run()), a choice in main() against options given
 * explicitly, prepare() and the release of the consumer, the client gate (--waitForClients,
 * --clientWaitLimit); consumer and client built without a launcher; the options of the shipped
 * thread-pool and websocket providers; --recordDir, the consumer's end-of-run report and the
 * streams closed when run() ends.
 *
 * Every launcher here distributes GSimpleContainer, but for the one case that is refused because
 * of it: the item type is recorded once per process.
 */

// Standard header files go here
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Catch2 header files go here
#include <catch2/catch_test_macros.hpp>

// Geneva header files go here
#include "GDemoProcessingContainers.hpp"
#include "common/record/GRecorder.hpp"
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "courtier/GLauncherT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GStdThreadConsumerConfig.hpp"
#include "courtier/consumers/GStdThreadConsumerProviderT.hpp"
#include "courtier/consumers/GWebsocketConsumerConfig.hpp"
#include "courtier/consumers/GWebsocketConsumerProviderT.hpp"
#include "launcher/GLauncherTestSupport.hpp"

using Gem::Courtier::GConsumerModules;
using Gem::Courtier::GLauncherT;
using Gem::Courtier::GSimpleContainer;
using Gem::Courtier::processRole;
using Gem::Courtier::Tests::GNeedyConsumerConfig;
using Gem::Courtier::Tests::lastNeedyConfig;
using Gem::Courtier::Tests::needyHasClients;
using Gem::Courtier::Tests::needySupplied;

namespace {

using launcher_type = GLauncherT<GSimpleContainer>;

/** @brief Where the consumer modules of these tests lie */
GConsumerModules testModules() {
    return GConsumerModules{
        .directory = GEM_LAUNCHER_TEST_MODULE_DIR,
        .prefix = "libgem_launcher_test_"
    };
}

/** @brief A command line, kept alive for the launcher */
struct CommandLine {
    std::vector<std::string> words;
    std::vector<char *> argv;
    explicit CommandLine(std::vector<std::string> args)
      : words(std::move(args)) {
        words.insert(words.begin(), "GCourtierLauncherTests");
        for(auto &w : words) {
            argv.push_back(w.data());
        }
        argv.push_back(nullptr);
    }
    [[nodiscard]] int argc() const {
        return static_cast<int>(words.size());
    }
    char **data() {
        return argv.data();
    }
};

/** @brief Collects what is written to std::cout while it lives */
struct CoutCapture {
    std::ostringstream text;
    std::streambuf *old = std::cout.rdbuf(text.rdbuf());
    CoutCapture() = default;
    CoutCapture(const CoutCapture &) = delete;
    CoutCapture &operator=(const CoutCapture &) = delete;
    CoutCapture(CoutCapture &&) = delete;
    CoutCapture &operator=(CoutCapture &&) = delete;
    ~CoutCapture() {
        std::cout.rdbuf(old);
    }
};

/** @brief Submits eight items through the process consumer @return How many came back processed */
std::size_t submitEight() {
    auto consumer = Gem::Courtier::GConsumerRegistryT<GSimpleContainer>::instance().consumer();
    if(not consumer) {
        return 0;
    }
    std::vector<std::unique_ptr<GSimpleContainer>> batch;
    for(std::size_t i = 0; i < 8; ++i) {
        batch.push_back(std::make_unique<GSimpleContainer>(i));
    }
    consumer->processBatch(
        std::span<std::unique_ptr<GSimpleContainer>>(batch.data(), batch.size()),
        Gem::Courtier::GSubmissionPolicy::full_success_or_fatal()
    );
    std::size_t processed = 0;
    for(const auto &item : batch) {
        processed += item && item->is_processed() ? 1 : 0;
    }
    return processed;
}

/** @brief The text of what a run throws; empty if it throws nothing */
template <typename function_type>
std::string refusalOf(function_type &&f) {
    try {
        f();
    }
    catch(const std::exception &e) {
        return e.what();
    }
    return {};
}

bool mentions(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}

} // namespace

/******************************************************************************/

TEST_CASE("Launcher: the default consumer comes from its module", "[launcher]") {
    CommandLine cl({});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    std::size_t processed = 0;
    int produced = 0;
    const int code = launcher.run({.produce = [&] {
        ++produced;
        processed = submitEight();
        return 7;
    }});
    CHECK(code == 7);
    CHECK(produced == 1);
    CHECK(processed == 8);
    CHECK(launcher.role() == processRole::SUBMITTER);
    // Released at the end of run()
    CHECK(Gem::Courtier::GConsumerRegistryT<GSimpleContainer>::instance().consumer() == nullptr);
}

TEST_CASE("Launcher: --help and --showAll return 0 and produce nothing", "[launcher]") {
    for(const std::string flag : {"--help", "--showAll"}) {
        CommandLine cl({flag});
        launcher_type launcher(cl.argc(), cl.data());
        launcher.setConsumerModules(testModules());
        CHECK(launcher.helpRequested()); // told before run(), without side effects
        int produced = 0;
        double tolerance = 0.;
        int code = -1;
        std::string help;
        {
            const CoutCapture capture;
            code = launcher.run({
                .userOptions =
                    [&](Gem::Common::GParserBuilder &gpb) {
                        gpb.registerCLParameter(
                            "tolerance",
                            tolerance,
                            0.1,
                            "the program's own option"
                        );
                    },
                .produce =
                    [&] {
                        ++produced;
                        return 0;
                    },
            });
            help = capture.text.str();
        }
        INFO(help);
        CHECK(code == 0);
        CHECK(produced == 0);
        CHECK(mentions(help, "--consumer"));
        CHECK(mentions(help, "--client"));
        CHECK(mentions(help, "--module"));
        CHECK(mentions(help, "--tolerance"));
        CHECK(mentions(help, "available consumers: needy, stc"));
        // A hidden consumer option is listed by --showAll only
        CHECK(mentions(help, "--nWorkerThreads") == (flag == "--showAll"));
    }
}

TEST_CASE("Launcher: a malformed command line returns 1 and produces nothing", "[launcher]") {
    CommandLine cl({"--nonsense"});
    CHECK_FALSE(launcher_type(cl.argc(), cl.data()).helpRequested());
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    int produced = 0;
    const int code = launcher.run({.produce = [&] {
        ++produced;
        return 0;
    }});
    CHECK(code == 1);
    CHECK(produced == 0);
}

TEST_CASE("Launcher: refusals", "[launcher]") {
    int produced = 0;
    launcher_type::Program program;
    auto run = [&](std::vector<std::string> args) {
        CommandLine cl(std::move(args));
        launcher_type launcher(cl.argc(), cl.data());
        launcher.setConsumerModules(testModules());
        return refusalOf([&] {
            (void)launcher.run({.produce = [&] {
                ++produced;
                return 0;
            }});
        });
    };

    SECTION("an unknown consumer") {
        const std::string msg = run({"--consumer", "nosuch"});
        CHECK(mentions(msg, "libgem_launcher_test_nosuch.so does not exist"));
        CHECK(mentions(msg, "available: needy, stc"));
    }
    SECTION("a module that cannot be loaded") {
        const std::string msg = run({"--module", "/nonexistent/libnothing.so"});
        CHECK(mentions(msg, "Could not load the module '/nonexistent/libnothing.so'"));
    }
    SECTION("a consumer module for another item type") {
        const std::string msg = run({"--module", GEM_LAUNCHER_TEST_FOREIGN_MODULE});
        CHECK(mentions(msg, "Gem::Courtier::GRandomNumberContainer"));
        CHECK(mentions(msg, "this program distributes Gem::Courtier::GSimpleContainer"));
    }
    SECTION("--client for a consumer without clients") {
        const std::string msg = run({"--client"});
        CHECK(mentions(msg, "the consumer \"stc\" has no clients"));
    }
    SECTION("--waitForClients for a consumer without clients") {
        const std::string msg = run({"--waitForClients", "2"});
        CHECK(mentions(msg, "--waitForClients was given, but the consumer \"stc\" has no clients"));
    }
    SECTION("a consumer that misses a prerequisite") {
        needySupplied() = false;
        const std::string msg = run({"--consumer", "needy"});
        CHECK(
            mentions(msg, "The consumer \"needy\" cannot be used: the test has supplied nothing")
        );
    }
    CHECK(produced == 0);
}

TEST_CASE("Launcher: a choice in main() against options given explicitly", "[launcher]") {
    needySupplied() = true;
    auto run = [](std::vector<std::string> args, bool choose) {
        lastNeedyConfig().reset();
        CommandLine cl(std::move(args));
        launcher_type launcher(cl.argc(), cl.data());
        launcher.setConsumerModules(testModules());
        if(choose) {
            launcher.setConsumer(GNeedyConsumerConfig{.nThreads = 3, .label = "main"});
        }
        REQUIRE(launcher.run({.produce = [] { return submitEight() == 8 ? 0 : 1; }}) == 0);
        REQUIRE(lastNeedyConfig().has_value());
        return *lastNeedyConfig();
    };

    // main() wins over the defaults of the command line ...
    auto config = run({}, true);
    CHECK(config.nThreads == 3);
    CHECK(config.label == "main");
    // ... and loses against an option given explicitly, option by option
    config = run({"--needy_threads", "2"}, true);
    CHECK(config.nThreads == 2);
    CHECK(config.label == "main");
    // --consumer wins over the consumer chosen in main(): stc, so needy is not built
    lastNeedyConfig().reset();
    {
        CommandLine cl({"--consumer", "stc"});
        launcher_type launcher(cl.argc(), cl.data());
        launcher.setConsumerModules(testModules());
        launcher.setConsumer(GNeedyConsumerConfig{});
        CHECK(launcher.run({.produce = [] { return submitEight() == 8 ? 0 : 1; }}) == 0);
        CHECK_FALSE(lastNeedyConfig().has_value());
    }
    // Without a choice in main() the command line's values hold, its defaults included
    config = run({"--consumer", "needy", "--needy_label", "cl"}, false);
    CHECK(config.nThreads == 1);
    CHECK(config.label == "cl");
}

TEST_CASE("Launcher: prepare() may end the program before anything runs", "[launcher]") {
    CommandLine cl({});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    int produced = 0;
    std::optional<processRole> seen;
    const int code = launcher.run({
        .prepare = [&](processRole role) -> std::optional<int> {
            seen = role;
            return 5;
        },
        .produce =
            [&] {
                ++produced;
                return 0;
            },
    });
    CHECK(code == 5);
    CHECK(seen == processRole::SUBMITTER);
    CHECK(produced == 0);
}

TEST_CASE("Launcher: the consumer is released when produce() throws", "[launcher]") {
    CommandLine cl({});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    const std::string msg = refusalOf([&] {
        (void)launcher.run({.produce = []() -> int {
            throw std::runtime_error("produce failed");
        }});
    });
    CHECK(msg == "produce failed");
    CHECK(Gem::Courtier::GConsumerRegistryT<GSimpleContainer>::instance().consumer() == nullptr);
}

TEST_CASE("Launcher: a consumer the program put in its place is not released", "[launcher]") {
    auto &registry = Gem::Courtier::GConsumerRegistryT<GSimpleContainer>::instance();
    auto replacement = std::make_shared<Gem::Courtier::GStdThreadConsumerT<GSimpleContainer>>(
        Gem::Courtier::GStdThreadConsumerConfig{.nThreads = 1}
    );
    CommandLine cl({});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    CHECK(launcher.run({.produce = [&] {
        registry.setConsumer(replacement);
        return 0;
    }}) == 0);
    CHECK(registry.consumer() == replacement);
    registry.clear();
}

TEST_CASE("Launcher: the program's own consumer is built on the submitter", "[launcher]") {
    CommandLine cl({});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    int made = 0;
    launcher.setConsumer([&]() -> launcher_type::consumer_ptr {
        ++made;
        return std::make_shared<Gem::Courtier::GStdThreadConsumerT<GSimpleContainer>>(
            Gem::Courtier::GStdThreadConsumerConfig{.nThreads = 2}
        );
    });
    CHECK(launcher.run({.produce = [] { return submitEight() == 8 ? 0 : 1; }}) == 0);
    CHECK(made == 1);
}

namespace {

/** @brief A consumer that evaluates in place and answers connectedClients() as the test says */
class GGatedConsumer final : public Gem::Courtier::GBaseConsumerT<GSimpleContainer> {
public:
    using answer_type = std::function<std::optional<std::size_t>()>;
    /** @param answer What connectedClients() answers, asked once per call */
    explicit GGatedConsumer(answer_type answer)
      : answer_(std::move(answer)) {
    }
    [[nodiscard]] std::optional<std::size_t> connectedClients() const override {
        return answer_();
    }

private:
    void dispatch_(std::span<item_ptr> items) override {
        for(auto &item : items) {
            if(item && item->getProcessingStatus() == Gem::Courtier::processingStatus::DO_PROCESS) {
                item->process();
            }
        }
    }
    answer_type answer_;
};

} // namespace

// The client gate: the submitter asks its consumer for the connected clients after building it and
// before produce(), until there are --waitForClients of them; at --clientWaitLimit it ends the run
// without producing. A networked server whose clients are started one after another would
// otherwise finish with the first and leave a later one waiting for a server that is gone.
TEST_CASE("Launcher: the client gate of --waitForClients and --clientWaitLimit", "[launcher]") {
    needySupplied() = true;
    needyHasClients() = true; // the gate is refused for a consumer without clients
    struct Restore {
        Restore() = default;
        Restore(const Restore &) = delete;
        Restore &operator=(const Restore &) = delete;
        Restore(Restore &&) = delete;
        Restore &operator=(Restore &&) = delete;
        ~Restore() {
            needyHasClients() = false;
        }
    } const restore;

    std::size_t asked = 0; // how often connectedClients() was asked
    std::optional<std::size_t> asked_before_produce;
    std::string out; // what the run wrote to std::cout
    // Runs the launcher on "needy" with a GGatedConsumer; returns the refusal, empty if none
    auto run = [&](std::vector<std::string> args, GGatedConsumer::answer_type answer) {
        args.insert(args.begin(), {"--consumer", "needy"});
        CommandLine cl(std::move(args));
        launcher_type launcher(cl.argc(), cl.data());
        launcher.setConsumerModules(testModules());
        launcher.setConsumer([&answer]() -> launcher_type::consumer_ptr {
            return std::make_shared<GGatedConsumer>(std::move(answer));
        });
        const CoutCapture capture;
        const std::string msg = refusalOf([&] {
            (void)launcher.run({.produce = [&] {
                asked_before_produce = asked;
                return submitEight() == 8 ? 0 : 1;
            }});
        });
        out = capture.text.str();
        return msg;
    };

    SECTION("the submitter waits until the consumer reports the clients, then produces") {
        // The program's output keeps its format: the waited time once set the precision of
        // std::cout to 1, which cut every double the program printed later to one digit
        const std::streamsize precision = std::cout.precision();
        const std::ios_base::fmtflags flags = std::cout.flags();
        // 0, 0, 0, 1, 1, 1, 2: the seventh answer is the first with two clients
        CHECK(run({"--waitForClients", "2"}, [&] {
                  return std::optional<std::size_t>(asked++ / 3);
              }).empty());
        CHECK(asked_before_produce == std::optional<std::size_t>(7));
        CHECK(asked == 7);
        CHECK(mentions(out, "Waiting for 2 clients (at most 600 s)"));
        CHECK(mentions(out, "Waiting for clients: 2 of 2 clients connected after"));
        CHECK(std::cout.precision() == precision);
        CHECK(std::cout.flags() == flags);
    }
    SECTION("too few clients end the run at the limit, before produce()") {
        const auto start = std::chrono::steady_clock::now();
        const std::string msg = run({"--waitForClients", "3", "--clientWaitLimit", "1"}, [&] {
            ++asked;
            return std::optional<std::size_t>(2);
        });
        CHECK(std::chrono::steady_clock::now() - start >= std::chrono::seconds(1));
        CHECK(mentions(
            msg,
            "only 2 of 3 clients connected within the limit of 1 s (--clientWaitLimit)"
        ));
        CHECK_FALSE(asked_before_produce.has_value());
        CHECK(asked > 1);
        CHECK(
            Gem::Courtier::GConsumerRegistryT<GSimpleContainer>::instance().consumer() == nullptr
        );
    }
    SECTION("a consumer that cannot tell how many clients it has is not waited for") {
        CHECK(run({"--waitForClients", "2"}, [&] {
                  ++asked;
                  return std::optional<std::size_t>{};
              }).empty());
        CHECK(asked_before_produce == std::optional<std::size_t>(1));
        CHECK(mentions(out, "cannot tell how many clients are connected"));
    }
    SECTION("without --waitForClients the consumer is not asked") {
        CHECK(run({}, [&] {
                  ++asked;
                  return std::optional<std::size_t>(0);
              }).empty());
        CHECK(asked_before_produce == std::optional<std::size_t>(0));
        CHECK_FALSE(mentions(out, "Waiting for"));
    }
}

TEST_CASE("Without a launcher: consumer and client from the modules", "[launcher]") {
    auto &registry = Gem::Courtier::GConsumerRegistryT<GSimpleContainer>::instance();
    registry.clear();

    SECTION("a consumer from its configuration struct, registered as the process consumer") {
        auto consumer = Gem::Courtier::buildConsumer<GSimpleContainer>(
            Gem::Courtier::GStdThreadConsumerConfig{.nThreads = 2},
            testModules()
        );
        CHECK(consumer != nullptr);
        CHECK(registry.consumer() == consumer);
        CHECK(submitEight() == 8);
    }
    SECTION("the default consumer, built once if the process has none") {
        auto first = Gem::Courtier::ensureProcessConsumer<GSimpleContainer>(testModules());
        CHECK(first != nullptr);
        CHECK(registry.consumer() == first);
        CHECK(Gem::Courtier::ensureProcessConsumer<GSimpleContainer>(testModules()) == first);
    }
    SECTION("refusals") {
        CHECK(mentions(
            refusalOf([] {
                (void)Gem::Courtier::buildConsumerClient<GSimpleContainer>(
                    Gem::Courtier::ConsumerSpec{.mnemonic = "stc"},
                    testModules()
                );
            }),
            "The consumer \"stc\" does not provide a client"
        ));
        CHECK(mentions(
            refusalOf([] {
                (void)Gem::Courtier::consumerProvider<GSimpleContainer>("nosuch", testModules());
            }),
            "libgem_launcher_test_nosuch.so does not exist"
        ));
        CHECK(mentions(
            refusalOf([] {
                (void)Gem::Courtier::consumerProvider<GSimpleContainer>("nosuch", std::nullopt);
            }),
            "No module provides the consumer \"nosuch\""
        ));
        CHECK(registry.consumer() == nullptr);
    }
    registry.clear();
}

TEST_CASE("Launcher: run() runs once", "[launcher]") {
    CommandLine cl({});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    CHECK(launcher.run({}) == 0);
    CHECK(mentions(refusalOf([&] { (void)launcher.run({}); }), "a launcher runs once"));
}

TEST_CASE("Launcher: a second item type in the process is refused", "[launcher]") {
    CommandLine cl({});
    const launcher_type first(cl.argc(), cl.data());
    const std::string msg = refusalOf([&] {
        const GLauncherT<Gem::Courtier::GRandomNumberContainer> second(cl.argc(), cl.data());
    });
    CHECK(mentions(msg, "A process distributes one kind of item"));
    CHECK(mentions(msg, "Gem::Courtier::GRandomNumberContainer"));
}

namespace {

/** @brief A producer description that records the launcher's calls */
class GRecordingDescription final : public Gem::Courtier::GProducerDescriptionI {
public:
    std::vector<std::string> calls;
    std::string own = "unset";

    void addConfiguration(Gem::Common::GParserBuilder &) override {
        calls.emplace_back("configuration");
    }
    void addEarlyOptions(Gem::Common::GParserBuilder &) override {
        calls.emplace_back("early");
    }
    std::vector<Gem::Common::GContributionHandler> moduleHandlers() override {
        calls.emplace_back("handlers");
        return {};
    }
    std::vector<std::filesystem::path> modulesToLoad() override {
        calls.emplace_back("modules");
        return {};
    }
    [[nodiscard]] std::optional<GConsumerModules> consumerModules() const override {
        return testModules();
    }
    void addOptions(Gem::Common::GParserBuilder &gpb) override {
        calls.emplace_back("options");
        gpb.registerCLParameter<std::string>(
            "producer_option",
            own,
            "unset",
            "the producer's option"
        );
    }
    void configure(processRole role) override {
        calls.emplace_back(
            role == processRole::SUBMITTER ? "configure submitter" : "configure other"
        );
    }
};

} // namespace

TEST_CASE("Launcher: the steps of a producer description in their order", "[launcher]") {
    CommandLine cl({"--producer_option", "given"});
    launcher_type launcher(cl.argc(), cl.data());
    GRecordingDescription description; // names the consumer modules itself
    std::vector<std::string> seen_at_produce;
    CHECK(launcher.run(description, {.produce = [&] {
                           seen_at_produce = description.calls;
                           return submitEight() == 8 ? 0 : 1;
                       }}) == 0);
    CHECK(
        seen_at_produce == std::vector<std::string>{
                               "configuration",
                               "early",
                               "handlers",
                               "modules",
                               "options",
                               "configure submitter"
                           }
    );
    CHECK(description.own == "given");
}

TEST_CASE("Launcher: --update-configs runs the program on the default consumer", "[refresh]") {
    needySupplied() = true;
    lastNeedyConfig().reset();
    CommandLine cl({"--update-configs", "--consumer", "needy"});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    int produced = 0;
    std::size_t processed = 0;
    CHECK(launcher.run({.produce = [&] {
        ++produced;
        processed = submitEight();
        return 3;
    }}) == 0);
    CHECK(produced == 1);
    CHECK(processed == 8);
    CHECK_FALSE(lastNeedyConfig().has_value()); // the default consumer, not the one asked for
    CHECK(std::filesystem::exists("./config/GLauncher.json"));
    CHECK(std::filesystem::exists("./config/GNetworkedConsumer.json"));
}

/******************************************************************************/
// The options of the shipped providers, parsed as the launcher parses them

namespace {

/** @brief Parses @p args (without the program's name) with the options of @p provider
 *  @param provider The provider @param args The command line @param help Receives --showAll's text
 *  @return The configuration the provider takes from the parse */
template <typename config_type, typename provider_type>
config_type
parsedConfig(provider_type &provider, std::vector<std::string> args, std::string *help = nullptr) {
    Gem::Common::GParserBuilder gpb;
    provider.registerOptions(gpb);
    CommandLine cl(std::move(args));
    {
        const CoutCapture capture;
        (void)gpb.parseCommandLine(cl.argc(), cl.data());
        if(help != nullptr) {
            *help = capture.text.str();
        }
    }
    return std::any_cast<config_type>(provider.configuration(gpb, std::any{}));
}

/** @brief The websocket provider of the tests' item type */
using GWebsocketProvider = Gem::Courtier::GWebsocketConsumerProviderT<GSimpleContainer>;
/** @brief The thread-pool provider of the tests' item type */
using GStdThreadProvider = Gem::Courtier::GStdThreadConsumerProviderT<GSimpleContainer>;
/** @brief Their configurations */
using GWebsocketConfig = Gem::Courtier::GWebsocketConsumerConfig;
using GStdThreadConfig = Gem::Courtier::GStdThreadConsumerConfig;

} // namespace

TEST_CASE("The io thread option of the websocket consumer takes effect", "[launcher][options]") {
    GWebsocketProvider provider;
    CHECK(
        parsedConfig<GWebsocketConfig>(provider, {"--beast_nListenerThreads", "7"}).nThreads == 7
    );
    CHECK(
        parsedConfig<GWebsocketConfig>(provider, {"--beast_nListenerThreads", "0"}).nThreads == 0
    ); // hardware
    // Without the option the documented default applies, not the number of hardware threads.
    CHECK(
        parsedConfig<GWebsocketConfig>(provider, {}).nThreads ==
        Gem::Courtier::GCONSUMERLISTENERTHREADS
    );
}

// The serialization option names the modes there are (its help text used to offer the removed
// Boost archives TEXTMODE, XMLMODE and BINARYMODE), and the parser refuses a value that names none.
TEST_CASE(
    "The serialization option names the modes there are and refuses others",
    "[launcher][options]"
) {
    GWebsocketProvider provider;
    std::string help;
    (void)parsedConfig<GWebsocketConfig>(provider, {"--showAll"}, &help);
    // The help text wraps its lines: compare with every run of white space as one blank
    const auto squeeze = [](std::string text) {
        std::string out;
        for(const char c : text) {
            const bool blank = c == ' ' || c == '\n' || c == '\t';
            if(not blank || out.empty() || out.back() != ' ') {
                out.push_back(blank ? ' ' : c);
            }
        }
        return out;
    };
    help = squeeze(help);
    CHECK(mentions(help, "--beast_serializationMode"));
    CHECK(mentions(help, squeeze(Gem::Common::serializationModeChoices())));
    for(const std::string_view stale : {"TEXTMODE", "XMLMODE", "BINARYMODE (", "GEM_JSONMODE"}) {
        INFO(stale);
        CHECK_FALSE(mentions(help, std::string(stale)));
    }
    for(const auto mode : Gem::Common::SERIALIZATIONMODES) {
        CHECK(
            parsedConfig<GWebsocketConfig>(
                provider,
                {"--beast_serializationMode", std::to_string(std::to_underlying(mode))}
            )
                .serializationMode == mode
        );
    }
    CHECK_THROWS(
        parsedConfig<GWebsocketConfig>(provider, {"--beast_serializationMode", "0"})
    ); // 0 is no serialization mode
    CHECK_THROWS(
        parsedConfig<GWebsocketConfig>(provider, {"--beast_serializationMode", "2"})
    ); // nor is 2
}

// The defaults of the websocket consumer's options are those of its configuration struct: an
// option left unset leaves the consumer as a default-constructed configuration describes it. The
// port is the one exception: on the command line server and client are two processes and meet on
// GCONSUMERDEFAULTPORT, while a configuration built in a program lets the server pick a free port.
TEST_CASE("The websocket consumer's options default to its configuration", "[launcher][options]") {
    GWebsocketProvider provider;
    const auto beast = parsedConfig<GWebsocketConfig>(provider, {});
    const Gem::Courtier::GWebsocketConsumerConfig defaults;
    CHECK(beast.ip == defaults.ip);
    CHECK(beast.port == Gem::Courtier::GCONSUMERDEFAULTPORT);
    CHECK(beast.listenAddress == defaults.listenAddress);
    CHECK(beast.serializationMode == defaults.serializationMode);
    CHECK(beast.nThreads == defaults.nThreads);
    CHECK(beast.pingInterval == defaults.pingInterval);
    CHECK(beast.verboseControlFrames == defaults.verboseControlFrames);
    CHECK(beast.clientPrefetchDepth == defaults.clientPrefetchDepth);
    CHECK(beast.clientParallelEvaluations == defaults.clientParallelEvaluations);
    CHECK(beast.clientFirstContactLimit == defaults.clientFirstContactLimit);
    CHECK(beast.clientReconnectLimit == defaults.clientReconnectLimit);
}

// --beast_listenAddress sets the address the server listens on; --beast_ip, the server's address
// for its clients, is another option and stays as it was.
TEST_CASE("The option --beast_listenAddress sets listenAddress", "[launcher][options]") {
    GWebsocketProvider provider;
    const auto beast =
        parsedConfig<GWebsocketConfig>(provider, {"--beast_listenAddress", "127.0.0.1"});
    CHECK(beast.listenAddress == "127.0.0.1");
    CHECK(beast.ip == Gem::Courtier::GWebsocketConsumerConfig{}.ip);
}

// The thread consumer once registered an option "stcCapableOfFullReturn" that nothing read. An
// option that is offered must have an effect, so it is gone (the neighbouring stc option is there,
// so the parse itself works).
TEST_CASE("The thread consumer does not offer stcCapableOfFullReturn", "[launcher][options]") {
    GStdThreadProvider provider;
    CHECK(parsedConfig<GStdThreadConfig>(provider, {"--nWorkerThreads", "3"}).nThreads == 3);
    CHECK_THROWS(parsedConfig<GStdThreadConfig>(provider, {"--stcCapableOfFullReturn", "1"}));
}

/******************************************************************************/

namespace {

/** @brief A fresh directory for the records of one test case; the recorder's directory is
 *  restored and the directory removed at the end */
class RecordDirectory {
public:
    explicit RecordDirectory(const std::string &tag)
      : dir_(
            std::filesystem::temp_directory_path() / "gem_launcher_record_tests" /
            (tag + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
        ) {
    }
    RecordDirectory(const RecordDirectory &) = delete;
    RecordDirectory &operator=(const RecordDirectory &) = delete;
    RecordDirectory(RecordDirectory &&) = delete;
    RecordDirectory &operator=(RecordDirectory &&) = delete;
    ~RecordDirectory() {
        Gem::Common::Record::recorder().setDirectory(previous_);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    [[nodiscard]] const std::filesystem::path &path() const {
        return dir_;
    }

private:
    std::filesystem::path dir_;
    std::filesystem::path previous_ = Gem::Common::Record::recorder().directory();
};

/** @brief A consumer that evaluates in place and counts something: its end-of-run report */
class GCountingConsumer final : public Gem::Courtier::GBaseConsumerT<GSimpleContainer> {
public:
    [[nodiscard]] figures_type endOfRunFigures() const override {
        return {{"counted", 3}};
    }

private:
    void dispatch_(std::span<item_ptr> items) override {
        for(auto &item : items) {
            if(item && item->getProcessingStatus() == Gem::Courtier::processingStatus::DO_PROCESS) {
                item->process();
            }
        }
    }
};

/** @brief The lines of a file */
std::vector<std::string> linesOf(const std::filesystem::path &file) {
    std::ifstream in(file);
    std::vector<std::string> lines;
    for(std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

TEST_CASE(
    "Launcher: --recordDir and the end-of-run report; the streams are closed at the end",
    "[launcher][record]"
) {
    const RecordDirectory dir("normal");
    CommandLine cl({"--recordDir", dir.path().string()});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    Gem::Common::Record::GStreamT<int> stream;
    CHECK(launcher.run({.produce = [&] {
        stream = Gem::Common::Record::recorder().stream<int>("launcher_records", {"n"});
        stream.append(1);
        submitEight();
        return 0;
    }}) == 0);
    // closeAll() at the end of run()
    CHECK(not stream.isOpen());
    CHECK(linesOf(dir.path() / "launcher_records.csv") == std::vector<std::string>{"n", "1"});
    // Regression: a local consumer counts nothing, and wrote a report all the same ("stc,0"), in
    // every run, backing the previous one up with a warning. It writes none.
    CHECK_FALSE(std::filesystem::exists(dir.path() / "courtier.csv"));
}

TEST_CASE("Launcher: a consumer that counts records its end-of-run report", "[launcher][record]") {
    const RecordDirectory dir("report");
    CommandLine cl({"--recordDir", dir.path().string()});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    launcher.setConsumer([] { return std::make_shared<GCountingConsumer>(); });
    CHECK(launcher.run({.produce = [] { return submitEight() == 8 ? 0 : 1; }}) == 0);
    CHECK(
        linesOf(dir.path() / "courtier.csv") ==
        std::vector<std::string>{
            "consumer,counted",
            std::string(Gem::Courtier::GDEFAULTCONSUMERMNEMONIC) + ",3"
        }
    );
}

TEST_CASE(
    "Launcher: the streams are closed and the report recorded when produce() throws",
    "[launcher][record]"
) {
    const RecordDirectory dir("throws");
    CommandLine cl({"--recordDir", dir.path().string()});
    launcher_type launcher(cl.argc(), cl.data());
    launcher.setConsumerModules(testModules());
    launcher.setConsumer([] { return std::make_shared<GCountingConsumer>(); });
    Gem::Common::Record::GStreamT<int> stream;
    const std::string msg = refusalOf([&] {
        (void)launcher.run({.produce = [&]() -> int {
            stream = Gem::Common::Record::recorder().stream<int>("launcher_records", {"n"});
            stream.append(2);
            throw std::runtime_error("produce failed");
        }});
    });
    CHECK(msg == "produce failed");
    CHECK(not stream.isOpen());
    CHECK(linesOf(dir.path() / "launcher_records.csv") == std::vector<std::string>{"n", "2"});
    CHECK(linesOf(dir.path() / "courtier.csv").size() == 2);
}
