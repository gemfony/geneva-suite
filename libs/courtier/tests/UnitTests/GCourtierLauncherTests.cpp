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
 * explicitly, prepare() and the release of the consumer.
 *
 * Every launcher here distributes GSimpleContainer, but for the one case that is refused because
 * of it: the item type is recorded once per process.
 */

// Standard header files go here
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Catch2 header files go here
#include <catch2/catch_test_macros.hpp>

// Geneva header files go here
#include "GDemoProcessingContainers.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "courtier/GLauncherT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GStdThreadConsumerConfig.hpp"
#include "launcher/GLauncherTestSupport.hpp"

using Gem::Courtier::GConsumerModules;
using Gem::Courtier::GLauncherT;
using Gem::Courtier::GSimpleContainer;
using Gem::Courtier::processRole;
using Gem::Courtier::Tests::GNeedyConsumerConfig;
using Gem::Courtier::Tests::lastNeedyConfig;
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
