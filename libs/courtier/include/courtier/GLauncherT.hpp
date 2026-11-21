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
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

// Boost header files go here
#include <boost/core/demangle.hpp>

// Geneva header files go here
#include "common/GCommonHelperFunctions.hpp" // duration_from_string()
#include "common/GConfigEmission.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GModuleLoader.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GConsumerPlugin.hpp"
#include "courtier/GConsumerProviderT.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "courtier/GConsumerSpec.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GProcessable.hpp"
#include "courtier/consumers/GNetworkedConsumerT.hpp"
#include "courtier/consumers/GNetworkedTimeoutConfig.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief Records the item type the launchers of this process distribute: the first launcher's,
 * for the life of the process. One consumer transports one kind of item, and a process has one
 * consumer (Invariant 5).
 * @param item_type The item type of a launcher being built
 * @throws gem_exception if a launcher for another item type was built before
 */
void claimItemType(const std::type_info &item_type);

/******************************************************************************/
/**
 * @brief Where the consumer modules of an item type lie: the module of the consumer @c m is
 * @c directory/(prefix + m + suffix). The launcher loads the one of the chosen consumer if no
 * module named by the program or on the command line provides it.
 */
struct GConsumerModules {
    std::filesystem::path directory; ///< The directory of the modules
    std::string prefix;              ///< The file name before the mnemonic
    std::string suffix = ".so";      ///< The file name after the mnemonic

    /** @brief The module of a consumer @param mnemonic The consumer @return Its path */
    [[nodiscard]] std::filesystem::path moduleOf(const std::string &mnemonic) const {
        return directory / (prefix + mnemonic + suffix);
    }
    /** @brief The consumers whose modules lie in the directory @return Their mnemonics */
    [[nodiscard]] std::vector<std::string> available() const;
    /** @brief Modules that lie next to the running program
     *  @param prefix The file name before the mnemonic @return Their location */
    [[nodiscard]] static GConsumerModules besideProgram(std::string prefix);
};

/******************************************************************************/
namespace detail {

/** @brief A networked consumer reads its time limits from ./config/GNetworkedConsumer.json
 *  (written with the defaults if absent); any other consumer is left alone
 *  @param consumer The consumer just built */
template <ProcessableItem processable_type>
void applyNetworkedTimeouts(GBaseConsumerT<processable_type> &consumer) {
    if(auto *networked = dynamic_cast<GNetworkedConsumerT<processable_type> *>(&consumer)) {
        GNetworkedTimeoutConfig timeout_config;
        timeout_config.load("./config/GNetworkedConsumer.json");
        networked->applyTimeoutConfig(timeout_config);
    }
}

/** @brief An error of the free functions @param what Why @return The exception to throw */
inline gem_exception consumerError(const std::string &what) {
    return gem_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In Gem::Courtier's consumer functions: Error!" << '\n'
        << what << '\n'
    );
}

} // namespace detail

/******************************************************************************/
/**
 * @brief The consumers of an item type a program can choose: those of its catalogue and those
 * whose modules lie at @p location
 * @param location Where the consumer modules lie, if anywhere
 * @return "a, b, c", or "none"
 */
template <ProcessableItem processable_type>
[[nodiscard]] std::string consumerListing(const std::optional<GConsumerModules> &location);

/**
 * @brief The provider of a consumer: from the catalogue of the item type or, if no module loaded
 * so far provides it, from its module at @p location, which is loaded then. The launcher finds
 * its consumer this way; so does a program that builds its consumer or client without one.
 * @param mnemonic The consumer
 * @param location Where the consumer modules lie, if anywhere
 * @return The provider (never null)
 * @throws gem_exception if no module provides the consumer or its module cannot be loaded
 */
template <ProcessableItem processable_type>
[[nodiscard]] GConsumerProviderPtrT<processable_type>
consumerProvider(const std::string &mnemonic, const std::optional<GConsumerModules> &location);

/**
 * @brief Builds the consumer of a submitter without a launcher and registers it as the process
 * consumer (GConsumerRegistryT); a networked consumer reads its time limits from
 * ./config/GNetworkedConsumer.json, as under the launcher
 * @param spec The consumer and its configuration
 * @param location Where the consumer modules lie, if anywhere
 * @return The consumer
 * @throws gem_exception as consumerProvider(), or if the provider builds none
 */
template <ProcessableItem processable_type>
std::shared_ptr<GBaseConsumerT<processable_type>>
buildConsumer(const ConsumerSpec &spec, const std::optional<GConsumerModules> &location);

/** @brief As buildConsumer(const ConsumerSpec &, ...), from the configuration struct of the
 *  consumer's family, which names the consumer @param config The configuration
 *  @param location Where the consumer modules lie, if anywhere @return The consumer */
template <ProcessableItem processable_type, ConsumerConfig config_type>
std::shared_ptr<GBaseConsumerT<processable_type>>
buildConsumer(const config_type &config, const std::optional<GConsumerModules> &location) {
    return buildConsumer<processable_type>(
        ConsumerSpec{.mnemonic = std::string(config_type::MNEMONIC), .config = config},
        location
    );
}

/**
 * @brief Builds the client of a consumer without a launcher (the caller runs it)
 * @param spec The consumer and its configuration
 * @param location Where the consumer modules lie, if anywhere
 * @return The client
 * @throws gem_exception as consumerProvider(), or for a consumer without clients
 */
template <ProcessableItem processable_type>
std::shared_ptr<GBaseClientT<processable_type>>
buildConsumerClient(const ConsumerSpec &spec, const std::optional<GConsumerModules> &location);

/** @brief As buildConsumerClient(const ConsumerSpec &, ...), from the configuration struct of the
 *  consumer's family @param config The configuration
 *  @param location Where the consumer modules lie, if anywhere @return The client */
template <ProcessableItem processable_type, ConsumerConfig config_type>
std::shared_ptr<GBaseClientT<processable_type>>
buildConsumerClient(const config_type &config, const std::optional<GConsumerModules> &location) {
    return buildConsumerClient<processable_type>(
        ConsumerSpec{.mnemonic = std::string(config_type::MNEMONIC), .config = config},
        location
    );
}

/**
 * @brief The process consumer; if the process has none, the default consumer
 * (GDEFAULTCONSUMERMNEMONIC) is built from its module and registered. For a producer that runs
 * without a launcher and was given no consumer.
 * @param location Where the consumer modules lie, if anywhere
 * @return The process consumer (never null)
 * @throws gem_exception as consumerProvider()
 */
template <ProcessableItem processable_type>
std::shared_ptr<GBaseConsumerT<processable_type>>
ensureProcessConsumer(const std::optional<GConsumerModules> &location);

/******************************************************************************/
/**
 * @brief What a library that produces work (Geneva, for example) says about itself to the
 * launcher. Every function is called in every process, in the order of the launcher's steps
 * (GLauncherT::run()); all do nothing by default.
 */
class GProducerDescriptionI { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /** @brief The (defaulted) destructor */
    virtual ~GProducerDescriptionI() = default;

    /** @brief Step 2: the producer's keys of the launcher's configuration file
     *  @param gpb The launcher's parser */
    virtual void addConfiguration(Gem::Common::GParserBuilder &gpb) {
        (void)gpb;
    }
    /** @brief Step 3: options that must be known before modules are loaded
     *  @param gpb The launcher's parser */
    virtual void addEarlyOptions(Gem::Common::GParserBuilder &gpb) {
        (void)gpb;
    }
    /** @brief Step 3: what the producer accepts from a module besides consumers
     *  @return One handler per kind */
    virtual std::vector<Gem::Common::GContributionHandler> moduleHandlers() {
        return {};
    }
    /** @brief Step 3: further modules to load, from its early options and configuration
     *  @return Their paths */
    virtual std::vector<std::filesystem::path> modulesToLoad() {
        return {};
    }
    /** @brief Step 3: where the consumer modules of its item type lie, if the program did not say
     *  @return Their location, or nothing */
    [[nodiscard]] virtual std::optional<GConsumerModules> consumerModules() const {
        return {};
    }
    /** @brief Step 4: its options, in the one command line and the one help text
     *  @param gpb The launcher's parser */
    virtual void addOptions(Gem::Common::GParserBuilder &gpb) {
        (void)gpb;
    }
    /** @brief Step 6: after the parse, when the role of the process is known
     *  @param role The role */
    virtual void configure(processRole role) {
        (void)role;
    }
};

/******************************************************************************/
/**
 * @brief The start of a program that distributes items of one type: courtier owns its command
 * line, the modules, the choice of consumer, the role of the process, the client run and the
 * release of the consumer. The program says what it produces (Program); courtier and the command
 * line say where and how it is processed.
 *
 * run() goes through these steps, in every process:
 *  1. --update-configs: configuration emission is begun, the consumer is the default one, the
 *     process is the submitter (no role is asked for);
 *  2. the configuration file (keys "consumer", "module_paths", the description's);
 *  3. an early parse of --consumer, --module and the description's early options; the modules
 *     are loaded (configuration, addModulePath(), --module, the description's), each with the
 *     consumer handler and the description's handlers; then, if none of them provides the chosen
 *     consumer, its module from setConsumerModules() or the description's location;
 *  4. the full parse: the launcher's, the consumers', the description's and the program's
 *     options; --help and --showAll print the help text and make run() return 0, a malformed
 *     command line prints the error and the help text and makes it return 1;
 *  5. the consumer is checked (known, nothing missing, --client only for a consumer with
 *     clients) and the role of the process is asked for;
 *  6. the description is configured; 7. prepare(role), which may end the program;
 *  8. a bystander returns 0; a client builds its client and runs it; the submitter builds the
 *     consumer (or the program's, setConsumer(make)), registers it as the process consumer,
 *     calls produce() and releases the consumer again -- also if produce() throws.
 *
 * A choice made in main() (setConsumer(), setClientMode(), setMaxClientTime()) loses against an
 * option given explicitly on the command line, option by option, and wins against a default.
 *
 * @tparam processable_type The item type; one per process
 */
template <ProcessableItem processable_type>
class GLauncherT {
public:
    /** @brief A consumer of this item type */
    using consumer_ptr = std::shared_ptr<GBaseConsumerT<processable_type>>;

    /** @brief What the program does, by role */
    struct Program {
        /** Step 4: registers the program's own options into the launcher's parser */
        std::move_only_function<void(Gem::Common::GParserBuilder &)> userOptions;
        /** Step 7, every process: may end the program by returning an exit code */
        std::move_only_function<std::optional<int>(processRole)> prepare;
        /** Step 8, the submitter only: produces the work; its value is the exit code */
        std::move_only_function<int()> produce;
    };

    /**
     * @brief Takes the command line; nothing is parsed before run()
     * @param argc The number of arguments @param argv The arguments
     * @param config_file The launcher's configuration file
     * @throws gem_exception if a launcher for another item type was built in this process
     */
    GLauncherT(int argc, char **argv, std::filesystem::path config_file = "./config/GLauncher.json")
      : argc_(argc)
      , argv_(argv)
      , config_file_(std::move(config_file)) {
        claimItemType(typeid(processable_type));
    }

    /** @brief Chooses the consumer and its configuration (its struct names the mnemonic)
     *  @param config The configuration @return This launcher */
    template <ConsumerConfig config_type>
    GLauncherT &setConsumer(const config_type &config) {
        chosen_ = ConsumerSpec{.mnemonic = std::string(config_type::MNEMONIC), .config = config};
        return *this;
    }
    /** @brief A consumer the program builds itself, on the submitter only (a networked consumer
     *  binds its port when it is built) @param make Builds it @return This launcher */
    GLauncherT &setConsumer(std::move_only_function<consumer_ptr()> make) {
        make_consumer_ = std::move(make);
        return *this;
    }
    /** @brief Where the consumer modules of the item type lie @param modules Their location
     *  @return This launcher */
    GLauncherT &setConsumerModules(GConsumerModules modules) {
        consumer_modules_ = std::move(modules);
        return *this;
    }
    /** @brief A module to load, as with --module @param path The module @return This launcher */
    GLauncherT &addModulePath(std::filesystem::path path) {
        module_paths_.push_back(std::move(path));
        return *this;
    }
    /** @brief Whether the process is a client, unless --client says it
     *  @param client_mode Client or not @return This launcher */
    GLauncherT &setClientMode(bool client_mode) {
        client_mode_ = client_mode;
        return *this;
    }
    /** @brief How long a client runs at most, unless --max_client_duration says it
     *  @param max_time The time; 0: no limit @return This launcher */
    GLauncherT &setMaxClientTime(std::chrono::duration<double> max_time) {
        max_client_time_ = max_time;
        return *this;
    }
    /** @brief Processes the items of a client instead of their own process()
     *  @param processor The function @return This launcher */
    GLauncherT &registerItemProcessor(GItemProcessorT<processable_type> processor) {
        item_processor_ = std::move(processor);
        return *this;
    }

    /**
     * @brief Runs the program (see the class comment); once per launcher
     * @param description The producer; not owned, outlives the call
     * @param program What the program does
     * @return 0, the value of prepare() or produce(), 1 for a malformed command line, or 1 for a
     *         client that halted on an error
     * @throws gem_exception for a second call and for every refusal (unknown or unusable
     *         consumer, a module that cannot be loaded); whatever the program throws passes on
     *         after the consumer was released
     */
    int run(GProducerDescriptionI &description, Program program) {
        return run_(description, std::move(program));
    }
    /** @brief Runs a program without a producer description @param program What it does
     *  @return As run(GProducerDescriptionI &, Program) */
    int run(Program program) {
        GProducerDescriptionI none;
        return run_(none, std::move(program));
    }

    /**
     * @brief Whether the command line asks for the help text (--help, -h or --showAll), which run()
     * prints before anything else happens; a scan without side effects, for a program that would
     * otherwise start something first (an MPI cascade)
     * @return Whether one of the three options is given
     */
    [[nodiscard]] bool helpRequested() const {
        for(int i = 1; i < argc_; ++i) {
            const std::string_view arg(argv_[i]);
            if(arg == "--help" or arg == "-h" or arg == "--showAll") {
                return true;
            }
        }
        return false;
    }

    /** @brief The role of this process, valid once run() has asked for it @return The role */
    [[nodiscard]] processRole role() const {
        return role_;
    }

private:
    /** @brief See run() @param description The producer @param program The program
     *  @return The exit code */
    int run_(GProducerDescriptionI &description, Program program);
    /** @brief Steps 2 and 3 */
    void loadModules_(GProducerDescriptionI &description);
    /** @brief The provider of the chosen consumer, its module loaded if needed
     *  @param description The producer @return It */
    GConsumerProviderPtrT<processable_type>
    chosenProvider_(const GProducerDescriptionI &description);
    /** @brief Step 8 for a client @param provider Its provider @param spec Its spec
     *  @return The exit code */
    int runClient_(const GConsumerProviderT<processable_type> &provider, const ConsumerSpec &spec);
    /** @brief Step 8 for the submitter @param provider Its provider @param spec Its spec
     *  @param program The program @return The exit code */
    int runSubmitter_(
        GConsumerProviderT<processable_type> &provider,
        const ConsumerSpec &spec,
        Program &program
    );
    /** @brief The available consumers, for the help text and the refusals @param description The
     *  producer @return "a, b, c" */
    [[nodiscard]] std::string listing_(const GProducerDescriptionI &description) const;
    /** @brief A refusal of the launcher @param what Why @return The exception to throw */
    static gem_exception refusal_(const std::string &what) {
        return gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GLauncherT<" << boost::core::demangle(typeid(processable_type).name())
            << ">::run(): Error!" << '\n'
            << what << '\n'
        );
    }

    int argc_;                          ///< The command line
    char **argv_;                       ///< The command line
    std::filesystem::path config_file_; ///< The launcher's configuration file
    bool ran_ = false;                  ///< run() was called

    // Choices made in main()
    std::optional<ConsumerSpec> chosen_;                           ///< setConsumer(config)
    std::move_only_function<consumer_ptr()> make_consumer_;        ///< setConsumer(make)
    std::optional<GConsumerModules> consumer_modules_;             ///< setConsumerModules()
    std::vector<std::filesystem::path> module_paths_;              ///< addModulePath()
    std::optional<bool> client_mode_;                              ///< setClientMode()
    std::optional<std::chrono::duration<double>> max_client_time_; ///< setMaxClientTime()
    GItemProcessorT<processable_type> item_processor_;             ///< registerItemProcessor()

    // What run() found
    Gem::Common::GParserBuilder gpb_;       ///< The one parser of the command line and the file
    bool refresh_ = false;                  ///< --update-configs
    std::string file_consumer_;             ///< The key "consumer" of the configuration file
    std::vector<std::string> file_modules_; ///< The key "module_paths"
    std::string cl_consumer_;               ///< --consumer
    std::vector<std::string> cl_modules_;   ///< --module
    bool cl_client_ = false;                ///< --client
    bool cl_update_configs_ = false;     ///< --update-configs (read by configEmissionRequested())
    std::string cl_max_client_duration_; ///< --max_client_duration
    std::string mnemonic_;               ///< The chosen consumer
    processRole role_ = processRole::SUBMITTER; ///< The role of this process
};

/******************************************************************************/

template <ProcessableItem processable_type>
int GLauncherT<processable_type>::run_(GProducerDescriptionI &description, Program program) {
    if(ran_) {
        throw refusal_("run() was called a second time; a launcher runs once.");
    }
    ran_ = true;

    // Step 1
    refresh_ = Gem::Common::configEmissionRequested(argc_, argv_);
    if(refresh_) {
        Gem::Common::beginConfigEmission();
    }

    // Steps 2 and 3
    loadModules_(description);
    const GConsumerProviderPtrT<processable_type> provider = chosenProvider_(description);

    // Step 4: every option, in one parse and one help text
    gpb_.beginCLSection(
        "Consumer options (available consumers: " + listing_(description) + ")",
        "Further consumer options"
    );
    gpb_.registerCLFlag(
        "client",
        cl_client_,
        "Run as a client of a networked consumer. The MPI consumer ignores it: the role of a "
        "process follows its rank"
    );
    gpb_.registerCLParameter<std::string>(
        "max_client_duration",
        cl_max_client_duration_,
        std::string("00:00:00"),
        R"(The maximum run time of a client, as "hh:mm:ss"; "00:00:00": no limit)"
    );
    std::vector<GConsumerProviderPtrT<processable_type>> providers =
        consumerCatalog<processable_type>()->getContentSnapshot();
    std::ranges::sort(providers, {}, [](const auto &p) { return p->getMnemonic(); });
    for(const auto &p : providers) {
        p->registerOptions(gpb_);
    }
    description.addOptions(gpb_);
    if(program.userOptions) {
        gpb_.beginCLSection("Program options", "Further program options");
        program.userOptions(gpb_);
    }
    try {
        if(gpb_.parseCommandLine(argc_, argv_) == Gem::Common::GCL_HELP_REQUESTED) {
            return 0;
        }
    }
    catch(const Gem::Common::GCommandLineError &e) {
        std::cerr << e.what() << '\n' << e.usage() << '\n';
        return EXIT_FAILURE;
    }

    // Step 5: the chosen consumer and the role of this process
    if(const std::string missing = provider->missingPrerequisite(); not missing.empty()) {
        throw refusal_("The consumer \"" + mnemonic_ + "\" cannot be used: " + missing);
    }
    const bool client_requested =
        gpb_.givenExplicitly("client") ? cl_client_ : client_mode_.value_or(false);
    const bool role_at_runtime = provider->determinesRoleAtRuntime();
    if(client_requested && not refresh_ && not role_at_runtime && not provider->needsClient()) {
        throw refusal_(
            "Client mode was requested, but the consumer \"" + mnemonic_ + "\" has no clients."
        );
    }
    const std::any chosen_config =
        chosen_ && chosen_->mnemonic == mnemonic_ ? chosen_->config : std::any{};
    const ConsumerSpec spec{
        .mnemonic = mnemonic_,
        .config = provider->configuration(gpb_, chosen_config)
    };
    role_ = refresh_ ? processRole::SUBMITTER : provider->role(spec, client_requested);
    if(not role_at_runtime && (role_ == processRole::SUBMITTER) == client_requested &&
       not refresh_) {
        throw refusal_(
            "The consumer \"" + mnemonic_ + "\" states the role " +
            std::to_string(static_cast<int>(role_)) +
            " for this process, which contradicts the client switch, although it does not "
            "determine the role at run time."
        );
    }

    // Steps 6 and 7
    description.configure(role_);
    if(program.prepare) {
        if(const std::optional<int> code = program.prepare(role_)) {
            return *code;
        }
    }

    // Step 8
    switch(role_) {
    case processRole::BYSTANDER:
        return 0;
    case processRole::CLIENT:
        return runClient_(*provider, spec);
    case processRole::SUBMITTER:
        break;
    }
    const int code = runSubmitter_(*provider, spec, program);
    return refresh_ ? 0 : code;
}

/******************************************************************************/

template <ProcessableItem processable_type>
void GLauncherT<processable_type>::loadModules_(GProducerDescriptionI &description) {
    // Step 2: the configuration file
    gpb_.registerFileParameter<std::string>(
        "consumer",
        std::string(GDEFAULTCONSUMERMNEMONIC),
        [this](const std::string &c) { file_consumer_ = c; }
    ) << "The consumer used when none is given on the command line (--consumer).";
    gpb_.registerFileParameter<std::string>(
        "module_paths",
        file_modules_,
        std::vector<std::string>(),
        Gem::Common::VAR_IS_SECONDARY,
        "Modules (.so) loaded at the start, e.g. consumer modules. Repeated --module options add "
        "to "
        "this list."
    );
    description.addConfiguration(gpb_);
    gpb_.parseConfigFile(config_file_);

    // Step 3: the options that decide which modules are loaded
    gpb_.registerCLParameter<std::string>(
        "consumer,c",
        cl_consumer_,
        std::string(),
        "The consumer that processes the items (the available ones are listed below); default: "
        "the configuration file's, " +
            std::string(GDEFAULTCONSUMERMNEMONIC) + " if it names none"
    );
    gpb_.registerCLRepeatableParameter<std::string>(
        "module,m",
        cl_modules_,
        "A module (.so) to load at the start, e.g. a consumer's (repeatable)"
    );
    gpb_.registerCLFlag(
        std::string(Gem::Common::CONFIG_EMISSION_FLAG).substr(2),
        cl_update_configs_,
        "Write the configuration files of this program with their defaults, keep the values of "
        "present ones, and end; runs on the default consumer"
    );
    description.addEarlyOptions(gpb_);
    try {
        gpb_.parseKnownOptions(argc_, argv_);
    }
    catch(const Gem::Common::GCommandLineError &) {
        // The full parse reports it, with the help text of every option
    }

    std::vector<Gem::Common::GContributionHandler> handlers{
        consumerContributionHandler<processable_type>()
    };
    for(auto &handler : description.moduleHandlers()) {
        handlers.push_back(std::move(handler));
    }
    std::vector<std::filesystem::path> paths(file_modules_.begin(), file_modules_.end());
    paths.insert(paths.end(), module_paths_.begin(), module_paths_.end());
    paths.insert(paths.end(), cl_modules_.begin(), cl_modules_.end());
    for(auto &path : description.modulesToLoad()) {
        paths.push_back(std::move(path));
    }
    for(const auto &path : paths) {
        Gem::Common::loadModule(path, handlers);
    }
}

/******************************************************************************/

template <ProcessableItem processable_type>
GConsumerProviderPtrT<processable_type>
GLauncherT<processable_type>::chosenProvider_(const GProducerDescriptionI &description) {
    // The command line wins over main(), main() over the configuration file
    if(refresh_) {
        mnemonic_ = std::string(GDEFAULTCONSUMERMNEMONIC);
    }
    else if(not cl_consumer_.empty()) {
        mnemonic_ = cl_consumer_;
    }
    else if(chosen_) {
        mnemonic_ = chosen_->mnemonic;
    }
    else {
        mnemonic_ = file_consumer_;
    }

    return consumerProvider<processable_type>(
        mnemonic_,
        consumer_modules_ ? consumer_modules_ : description.consumerModules()
    );
}

/******************************************************************************/

template <ProcessableItem processable_type>
int GLauncherT<processable_type>::runClient_(
    const GConsumerProviderT<processable_type> &provider,
    const ConsumerSpec &spec
) {
    std::shared_ptr<GBaseClientT<processable_type>> client = provider.buildClient(spec);
    if(not client) {
        throw refusal_("The consumer \"" + mnemonic_ + "\" does not provide a client.");
    }
    if(item_processor_) {
        client->setItemProcessor(std::move(item_processor_));
    }
    client->setMaxTime(
        gpb_.givenExplicitly("max_client_duration") || not max_client_time_
            ? Gem::Common::duration_from_string(cl_max_client_duration_)
            : *max_client_time_
    );
    client->run();
    return client->terminalErrorFlagged() ? EXIT_FAILURE : 0;
}

/******************************************************************************/

template <ProcessableItem processable_type>
int GLauncherT<processable_type>::runSubmitter_(
    GConsumerProviderT<processable_type> &provider,
    const ConsumerSpec &spec,
    Program &program
) {
    if(refresh_) { // the files of the consumers, which are not built under --update-configs
        GNetworkedTimeoutConfig timeout_config;
        timeout_config.load("./config/GNetworkedConsumer.json");
        for(const auto &p : consumerCatalog<processable_type>()->getContentSnapshot()) {
            p->materializeConfigFiles();
        }
    }

    consumer_ptr consumer =
        make_consumer_ && not refresh_ ? make_consumer_() : provider.setup(spec);
    if(not consumer) {
        throw refusal_("No consumer was built for \"" + mnemonic_ + "\".");
    }
    detail::applyNetworkedTimeouts(*consumer);

    auto &registry = GConsumerRegistryT<processable_type>::instance();
    registry.setConsumer(consumer);
    // Released also if produce() throws, and only if the program did not replace it
    class Release {
    public:
        Release(GConsumerRegistryT<processable_type> &registry, consumer_ptr consumer)
          : registry_(registry)
          , consumer_(std::move(consumer)) {
        }
        Release(const Release &) = delete;
        Release &operator=(const Release &) = delete;
        Release(Release &&) = delete;
        Release &operator=(Release &&) = delete;
        ~Release() {
            if(registry_.consumer() == consumer_) {
                registry_.clear();
            }
        }

    private:
        GConsumerRegistryT<processable_type> &registry_;
        consumer_ptr consumer_;
    };
    const Release release(registry, std::move(consumer));
    return program.produce ? program.produce() : 0;
}

/******************************************************************************/

template <ProcessableItem processable_type>
std::string GLauncherT<processable_type>::listing_(const GProducerDescriptionI &description) const {
    return consumerListing<processable_type>(
        consumer_modules_ ? consumer_modules_ : description.consumerModules()
    );
}

/******************************************************************************/
// The free functions

template <ProcessableItem processable_type>
std::string consumerListing(const std::optional<GConsumerModules> &location) {
    std::vector<std::string> names;
    for(const auto &p : consumerCatalog<processable_type>()->getContentSnapshot()) {
        names.push_back(p->getMnemonic());
    }
    if(location) {
        for(auto &name : location->available()) {
            names.push_back(std::move(name));
        }
    }
    std::ranges::sort(names);
    const auto [first, last] = std::ranges::unique(names);
    names.erase(first, last);
    std::string result;
    for(const auto &name : names) {
        result += (result.empty() ? "" : ", ") + name;
    }
    return result.empty() ? "none" : result;
}

template <ProcessableItem processable_type>
GConsumerProviderPtrT<processable_type>
consumerProvider(const std::string &mnemonic, const std::optional<GConsumerModules> &location) {
    auto catalog = consumerCatalog<processable_type>();
    if(not catalog->exists(mnemonic)) {
        if(not location) {
            throw detail::consumerError(
                "No module provides the consumer \"" + mnemonic + "\" (available: " +
                consumerListing<processable_type>(location) + "). Name its module with --module."
            );
        }
        const std::filesystem::path module = location->moduleOf(mnemonic);
        if(not std::filesystem::exists(module)) {
            throw detail::consumerError(
                "The consumer \"" + mnemonic + "\" is not available: its module " +
                module.string() +
                " does not exist (available: " + consumerListing<processable_type>(location) + ")."
            );
        }
        try {
            Gem::Common::loadModule(
                module,
                std::array{consumerContributionHandler<processable_type>()}
            );
        }
        catch(const std::exception &e) {
            throw detail::consumerError(
                "The module " + module.string() + " of the consumer \"" + mnemonic +
                "\" could not be loaded:\n" + e.what()
            );
        }
        if(not catalog->exists(mnemonic)) {
            throw detail::consumerError(
                "The module " + module.string() + " does not provide the consumer \"" + mnemonic +
                "\"."
            );
        }
    }
    GConsumerProviderPtrT<processable_type> provider;
    catalog->get(mnemonic, provider);
    return provider;
}

template <ProcessableItem processable_type>
std::shared_ptr<GBaseConsumerT<processable_type>>
buildConsumer(const ConsumerSpec &spec, const std::optional<GConsumerModules> &location) {
    std::shared_ptr<GBaseConsumerT<processable_type>> consumer =
        consumerProvider<processable_type>(spec.mnemonic, location)->setup(spec);
    if(not consumer) {
        throw detail::consumerError("No consumer was built for \"" + spec.mnemonic + "\".");
    }
    detail::applyNetworkedTimeouts(*consumer);
    GConsumerRegistryT<processable_type>::instance().setConsumer(consumer);
    return consumer;
}

template <ProcessableItem processable_type>
std::shared_ptr<GBaseClientT<processable_type>>
buildConsumerClient(const ConsumerSpec &spec, const std::optional<GConsumerModules> &location) {
    std::shared_ptr<GBaseClientT<processable_type>> client =
        consumerProvider<processable_type>(spec.mnemonic, location)->buildClient(spec);
    if(not client) {
        throw detail::consumerError(
            "The consumer \"" + spec.mnemonic + "\" does not provide a client."
        );
    }
    return client;
}

template <ProcessableItem processable_type>
std::shared_ptr<GBaseConsumerT<processable_type>>
ensureProcessConsumer(const std::optional<GConsumerModules> &location) {
    return GConsumerRegistryT<processable_type>::instance().ensureConsumer([&location] {
        const std::string mnemonic(GDEFAULTCONSUMERMNEMONIC);
        std::shared_ptr<GBaseConsumerT<processable_type>> consumer =
            consumerProvider<processable_type>(mnemonic, location)
                ->setup(ConsumerSpec{.mnemonic = mnemonic});
        if(not consumer) {
            throw detail::consumerError("No consumer was built for \"" + mnemonic + "\".");
        }
        return consumer;
    });
}

/******************************************************************************/

} /* namespace Gem::Courtier */
