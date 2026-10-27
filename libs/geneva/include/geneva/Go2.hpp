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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Boost header files go here
#include <boost/program_options.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GExceptions.hpp"
#include "common/GFactoryT.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierHelperFunctions.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GFaultInjector.hpp"
#include "common/GSigHupHandler.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/Interface/GOptimizerIT.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GFactoryStore.hpp"
#include "geneva/GenevaInitializer.hpp"
#include "hap/GRandomFactory.hpp"
#include "hap/GRandomT.hpp"

namespace Gem::Geneva {

/******************************************************************************/
// Default values for the variables used by the optimizer
const std::string GO2_DEF_DEFAULTCONFIGFILE{"config/Go2.json"}; // NOLINT
constexpr bool GO2_DEF_CLIENTMODE = false;
constexpr std::uint16_t GO2_DEF_NPRODUCERTHREADS = 0;
const std::string GO2_DEF_NOCONSUMER{"none"}; // NOLINT
// The default consumer mnemonic is the setup layer's (GConsumerSetup.hpp), stated once there: the
// command line and a standalone algorithm must fall back to the same consumer.
const std::string GO2_DEF_CONSUMER{GDEFAULTCONSUMERMNEMONIC}; // NOLINT
constexpr bool GO2_DEF_COPYBESTINDIVIDUALSONLY = true;

/******************************************************************************/
/**
 * @brief Sets the number of producer threads of the random number factory.
 *
 * @param n_producer_threads The number of threads the random number factory should use to produce random numbers
 */
void setRNFParameters(std::uint16_t n_producer_threads);

/******************************************************************************/
/** Syntactic sugar -- make the code easier to read */
using GOABase = oa::GOptimizationAlgorithmBase;

/******************************************************************************/
/**
 * This class allows to "chain" a number of optimization algorithms so that a given
 * set of individuals can be optimized using more than one algorithm in sequence. The
 * class also hides the details of client/server mode, consumer initialization, etc.
 */
class Go2 // NOLINT(cppcoreguidelines-special-member-functions)
  : public Interface::GOptimizerIT<Go2>
  , public Gem::Common::GPtrContainerT<gen::GGenome> {
public:
    /** @brief The default constructor */
    Go2() = delete;

    /**
     * @brief A constructor that first parses the command line for relevant parameters and allows to specify a default config file name.
     *
     * @param argc The number of command line arguments
     * @param argv The array of command line argument strings
     * @param config_filename The name and location of the configuration file
     * @param user_descriptions Additional user-defined command line options (cmp. boost::program_options); defaults to an empty set
     */
    Go2(int argc,
        char **argv,
        std::string const &config_filename,
        boost::program_options::options_description const &user_descriptions =
            boost::program_options::options_description());
    /** @brief Deleted copy constructor */
    Go2(Go2 const &) = delete;

    /** @brief Destructor. Releases the process consumer this Go2 established (clearing it from
     *  GConsumerRegistry) so a networked consumer's server threads are torn down by RAII at the end of
     *  the run rather than lingering until process exit. */
    ~Go2() override;

    /**
     * @brief Triggers execution of the client loop.
     * @return An integer return value (suitable for the main function) indicating the execution status
     */
    int clientRun();
    /**
     * @brief Checks whether this object is running in client mode.
     * @return True if this object represents a network client, false otherwise
     */
    //! [coding-naming-example#1]
    bool clientMode() const;
    //! [coding-naming-example#1]

    /**
     * @brief Specifies whether only the best individuals of a population should be copied to the next algorithm.
     * @param copy_best_individuals_only If true, only the best individuals are carried over between chained algorithms
     */
    void setCopyBestIndividualsOnly(bool copy_best_individuals_only);
    /**
     * @brief Checks whether only the best individuals are copied.
     * @return True if only the best individuals are carried over between chained algorithms
     */
    bool onlyBestIndividualsAreCopied() const;

    /**
     * @brief Allows to add an optimization algorithm to the chain.
     * @param alg A shared pointer to the optimization algorithm to append to the chain
     */
    void addAlgorithm(const std::shared_ptr<GOABase> &alg);
    /**
     * @brief Makes it easier to add algorithms (operator form of addAlgorithm).
     * @param alg A shared pointer to the optimization algorithm to append to the chain
     * @return A reference to this object, allowing call chaining
     */
    Go2 &operator&(const std::shared_ptr<GOABase> &alg);
    /**
     * @brief Allows to add an optimization algorithm through its mnemonic.
     * @param mn The mnemonic string identifying the algorithm to append (e.g. "ea", "sa")
     */
    void addAlgorithm(std::string const &mn);
    /**
     * @brief Makes it easier to add algorithms by mnemonic (operator form of addAlgorithm).
     * @param mn The mnemonic string identifying the algorithm to append (e.g. "ea", "sa")
     * @return A reference to this object, allowing call chaining
     */
    Go2 &operator&(std::string const &mn);

    /** @brief Supplies a custom, ready-to-use courtier consumer (clone function set, and -- for
     *  networked consumers -- server started) for this run, OVERRIDING the mnemonic-based consumer
     *  selection. Used to plug in a custom consumer (e.g. a GPU consumer) that Go2 does not know how to
     *  build itself; it becomes the process's single consumer that every algorithm submits through. Call
     *  after construction and before optimize().
     *  @param consumer The ready-to-use consumer to register as the process consumer (ownership is taken via move). */
    void registerConsumer(
        std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> consumer);

    /** @brief Selects the consumer (parallelization backend) by mnemonic (e.g. "stc", "asio", "beast",
     *  "mpi", "gpu") -- the programmatic equivalent of the --consumer command-line option / the "consumer"
     *  config key. It takes effect when the run is configured (at the first of optimize() / clientRun() /
     *  clientMode()); a --consumer value on the command line overrides this. For a fully custom consumer
     *  object (that Go2 cannot build from a mnemonic) use registerConsumer() instead.
     *  @param mnemonic The consumer mnemonic to use */
    void setConsumerName(std::string const &mnemonic);

    /** @brief Appends a runtime Geneva module (.so) path to load at startup -- the programmatic equivalent
     *  of a --module command-line option / a module_paths config entry. A module may contribute optimization
     *  algorithms (usable by their mnemonic) and/or the optimization individual. The module is loaded when
     *  the run is configured (a mnemonic it contributes is then available to setAlgorithmChain()).
     *  @param path Filesystem path to the module to load */
    void addModulePath(std::string const &path);
    /** @brief Replaces the list of runtime module (.so) paths to load at startup (see addModulePath()).
     *  @param paths The module paths to load */
    void setModulePaths(std::vector<std::string> paths);
    /** @brief @return The runtime module (.so) paths configured for loading (config + --module + programmatic). */
    std::vector<std::string> getModulePaths() const;

    /** @brief Sets the chain of optimization algorithms by mnemonic (e.g. {"ea", "sa"}) -- the programmatic
     *  equivalent of --optimizationAlgorithms. Resolution is deferred until the run is configured, so a
     *  mnemonic contributed by a runtime module (addModulePath()) is available by then. A
     *  --optimizationAlgorithms list on the command line overrides this.
     *  @param mnemonics The ordered algorithm mnemonics forming the chain */
    void setAlgorithmChain(std::vector<std::string> const &mnemonics);
    /** @brief @return The programmatic algorithm-chain mnemonics set via setAlgorithmChain() (empty if none). */
    std::vector<std::string> getAlgorithmChain() const;

    /**
     * @brief Retrieves the currently registered number of algorithms.
     * @return The number of algorithms in the chain
     */
    std::size_t getNAlgorithms() const;

    /**
     * @brief Allows to register a content creator (a factory producing the individuals to be optimized).
     * @param cc_ptr A shared pointer to the factory used to fill the initial population
     */
    void
        registerContentCreator(const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> &cc_ptr);

    /**
     * @brief @return The registered content-creator factory (compiled-in or loaded from a plugin), or an
     *  empty pointer if none. A generic launcher uses this to pull e.g. the OA-owned adaption config from a
     *  runtime-loaded individual it does not know the concrete type of.
     */
    std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> getContentCreator() const {
        return content_creator_ptr_;
    }

    /***************************************************************************/
    // The following is a trivial list of getters and setters
    /**
     * @brief Sets whether this object runs in client mode.
     * @param client_mode If true, this object represents a network client
     */
    //! [coding-naming-example#2]
    void setClientMode(bool client_mode);
    //! [coding-naming-example#2]

    /**
     * @brief Retrieves the number of random number production threads.
     * @return The configured number of producer threads
     */
    std::uint16_t getNProducerThreads() const;

    /**
     * @brief Loads some configuration data from arguments passed on the command line (or another char ** presented to it).
     * @param argc The number of command line arguments
     * @param argv The array of command line argument strings
     * @param user_options Additional user-defined command line options (cmp. boost::program_options); defaults to an empty set
     */
    void parseCommandLine(
        int argc,
        char **argv,
        boost::program_options::options_description const &user_options =
            boost::program_options::options_description()
    );
    /**
     * @brief Loads some configuration data from a configuration file.
     * @param config_filename The path to the configuration file to read
     */
    void parseConfigFile(std::filesystem::path const &config_filename);

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    void addConfigurationOptions(Gem::Common::GParserBuilder &gpb);

    /***************************************************************************/
    /**
     * @brief Allows to register a default algorithm via a shared pointer.
     * @param default_algorithm A shared pointer to the algorithm used when no other algorithm has been registered
     */
    void registerDefaultAlgorithm(const std::shared_ptr<GOABase> &default_algorithm);
    /**
     * @brief Allows to register a default algorithm via its mnemonic.
     * @param mn The mnemonic of the algorithm used when no other algorithm has been registered
     */
    void registerDefaultAlgorithm(std::string const &mn);

    /**
     * @brief Allows to register a pluggable optimization monitor.
     * @param pluggable_om A shared pointer to the pluggable optimization monitor to register
     */
    void registerPluggableOM(const std::shared_ptr<oa::GBasePluggableOM> &pluggable_om);
    /** @brief Allows to reset the local pluggable optimization monitors */
    void resetPluggableOM();

    /**
     * @brief Registers a process-global evaluation fault injector (for testing consumer / algorithm
     * error-handling and recovery paths). Registered like a pluggable monitor; pass nullptr to clear.
     * Carries no per-individual state and is a no-op on production runs where none is registered.
     * @param injector The fault injector to consult during each individual's process() (nullptr clears)
     */
    void registerFaultInjector(std::unique_ptr<GFaultInjector> injector) {
        if(injector) {
            GFaultInjectorRegistry::set(std::move(injector));
        }
        else {
            GFaultInjectorRegistry::clear();
        }
    }
    /**
     * @brief Allows to check whether pluggable optimization monitors were registered.
     * @return True if at least one pluggable optimization monitor is registered
     */
    bool hasOptimizationMonitors() const;

    /**
     * @brief Allows to set the maximum running time for a client.
     * @param max_duration The maximum time frame for which a client is allowed to run
     */
    void setMaxClientTime(std::chrono::duration<double> max_duration);
    /**
     * @brief Allows to retrieve the maximum running time for a client.
     * @return The maximum time frame for which a client is allowed to run
     */
    std::chrono::duration<double> getMaxClientTime() const;

    /**
     * @brief Retrieves the algorithms that were registered with this class.
     * @return A vector of shared pointers to the registered optimization algorithms
     */
    std::vector<std::shared_ptr<GOABase>> getRegisteredAlgorithms();

    /**
     * @brief Retrieves the name of the used consumer.
     * @return The name of the consumer requested by the user
     */
    std::string getConsumerName();

    /**
     * @brief Reports whether this Go2 was started in --update-configs mode.
     *
     * In that mode Go2 only refreshes the configuration files it owns and then exits without optimizing;
     * it also forces the local thread-pool consumer so no networked / GPU / MPI consumer is built. A
     * subclass that would otherwise insist on a particular consumer (e.g. GMPISubClientOptimizer requiring
     * the MPI consumer) uses this to relax that requirement during a pure config-refresh run.
     *
     * @return true if the binary was invoked with --update-configs, false otherwise
     */
    bool updateConfigsMode() const {
        return update_configs_mode_;
    }

    /**
     * @brief Registers an OA-owned adaption configuration for an algorithm type. When an
     * algorithm of the given personality type (e.g. "PERSONALITY_EA") runs in the chain, Go2 hands it this
     * config, which it adopts (after validating it matches the population's genome) instead of deriving a
     * default from the genome layout. This separates adaption authoring (OA-owned, optionally from a config
     * file) from genome authoring (the individual's factory). Passing a null config clears the entry.
     *
     * @param oa_personality_type The algorithm personality type the config applies to (e.g. "PERSONALITY_EA")
     * @param config The OA-owned adaption configuration to install (a null pointer clears the entry)
     */
    void registerAdaptionConfig(
        const std::string &oa_personality_type,
        std::shared_ptr<oa::GAdaptionConfigBase> config
    );

protected:
    /***************************************************************************/
    /**
     * @brief Releases the process consumer this Go2 established (clearing it from GConsumerRegistry
     * and dropping the own reference). Idempotent. The destructor calls this; a derived class whose
     * own teardown must run AFTER the consumer is gone (e.g. finalizing MPI) calls it from its own
     * destructor body first.
     */
    void releaseConsumer_();

    /**
     * @brief Triggers execution of the client loop.
     * @return An integer return value (suitable for the main function) indicating the execution status
     */
    virtual int clientRun_();

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    virtual void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb);

private:
    /***************************************************************************/

    /** @brief Where the single optimization problem (individual) came from, so the one-individual-per-
     *  process rule can name the incumbent when a second registration is refused. */
    enum class individualSource {
        NONE,        ///< no individual provided yet
        COMPILED_IN, ///< registered by user code via registerContentCreator()
        LOADED       ///< loaded at runtime from an individual plugin (--individual)
    };

    /** @brief Claims the single content-creator slot for the optimization problem, enforcing "exactly one
     *  individual per process": a second claim (a compiled-in registration when a plugin was loaded, or a
     *  second plugin) throws, naming both the incumbent and the newcomer. Shared by
     *  registerContentCreator() (COMPILED_IN) and the plugin load (LOADED).
     *  @param cc_ptr The content-creator factory to install (must not be empty)
     *  @param source Where this content creator came from (for the diagnostic on a double claim) */
    void claimContentCreator_(
        const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> &cc_ptr,
        individualSource source
    );

    // GOptimizerIT NVI hooks: keep these overrides private (do not
    // widen access -- matches oa::GOptimizationAlgorithmBase and the base's NVI contract).
    /**
     * @brief Retrieves the best individual found globally across the whole run.
     * @return A sole-ownership pointer to a copy of the globally best individual
     */
    std::unique_ptr<gen::GGenome> getBestGlobalIndividual_() const final;
    /**
     * @brief Retrieves a list of the globally best individuals found.
     * @return A vector of sole-ownership pointers to copies of the globally best individuals
     */
    std::vector<std::unique_ptr<gen::GGenome>>
    getBestGlobalIndividuals_() const final;
    /**
     * @brief Retrieves the best individual found in the current iteration.
     * @return A sole-ownership pointer to a copy of the best individual of the current iteration
     */
    std::unique_ptr<gen::GGenome> getBestIterationIndividual_() const final;
    /**
     * @brief Retrieves a list of the best individuals found in the current iteration.
     * @return A vector of sole-ownership pointers to copies of the current iteration's best individuals
     */
    std::vector<std::unique_ptr<gen::GGenome>>
    getBestIterationIndividuals_() const final;

    /**
     * @brief Returns one-word information about the type of optimization algorithm.
     * @return A short string identifying the algorithm personality type
     */
    std::string getAlgorithmPersonalityType_() const final;
    /**
     * @brief Returns the name of this optimization algorithm.
     * @return The human-readable name of this optimization algorithm
     */
    std::string getAlgorithmName_() const final;

    /** @brief Satisfies a requirement of GOptimizerIT */
    void evaluatePopulation_() final;

    /**
     * @brief Retrieval of the current iteration.
     * @return The continuous iteration count accumulated across the algorithm chain
     */
    uint32_t getIteration_() const final;

    /***************************************************************************/
    /**
     * @brief Sets the number of random number production threads.
     * @param n_producer_threads The number of threads used to produce random numbers
     */
    void setNProducerThreads(std::uint16_t n_producer_threads);

    /**
     * @brief Performs the actual optimization cycle.
     * @param offset The iteration offset at which to start the optimization
     * @return A pointer to this object after optimization completed
     */
    Go2 const *optimize_(std::uint32_t offset) final;

    /** @brief --update-configs pass: with GParserBuilder in update-in-place mode, produce one object from
     *  each registered algorithm factory and one individual from the content creator, so every algorithm
     *  config and the individual config is parsed and rewritten in canonical form (Go2.json was already
     *  refreshed by the constructor's parse). Runs no optimization. */
    void refreshAllConfigs_();

    // --- optimize_ sub-steps (decomposition) ---
    /** @brief Adds the Geneva default algorithm if none have been registered */
    void ensureAlgorithmPresent();
    /**
     * @brief Loads a checkpoint or fills the population from the content creator.
     * @param offset The iteration offset supplied to the optimization run
     * @return The first algorithm's iteration offset (carried over from a resumed checkpoint, or the passed-in offset)
     */
    std::uint32_t prepareInitialPopulation(std::uint32_t offset);
    /** @brief Throws if a configured checkpoint file does not fit the first algorithm in the chain. */
    void validateCheckpointFitsFirstAlgorithm() const;
    /** @brief Loads the configured checkpoint into the first algorithm. */
    void loadFirstAlgorithmCheckpoint();
    /** @brief Fills the (empty) population from the registered content creator, throwing if none is available. */
    void fillPopulationFromContentCreator();
    /** @brief fillPopulationFromContentCreator() loop: push default-population-size individuals from the creator. */
    void createIndividualsFromContentCreator();
    /**
     * @brief Runs the registered algorithms in sequence, threading the individuals between them.
     * @param first_algorithm_offset The iteration offset at which the first algorithm in the chain starts
     */
    void runAlgorithmChain(std::uint32_t first_algorithm_offset);
    /** @brief Sorts the collected individuals by their (min-only transformed) fitness */
    void sortIndividualsByFitness();

    // --- parseCommandLine sub-steps (decomposition) ---
    /**
     * @brief Emits the help message and exits the process, if --help / --showAll was requested.
     * @param vm The parsed command line variables map
     * @param general The general options group (e.g. help-related switches)
     * @param basic The basic options group
     * @param visible The options group shown in the standard help output
     * @param user_options The additional user-supplied options group
     * @param usage_string The usage line printed at the top of the help message
     */
    static void emitHelpIfRequested(
        boost::program_options::variables_map const &vm,
        boost::program_options::options_description const &general,
        boost::program_options::options_description const &basic,
        boost::program_options::options_description const &visible,
        boost::program_options::options_description const &user_options,
        std::string const &usage_string
    );
    /**
     * @brief Validates, initialises, configures and enrols the consumer chosen on the command line.
     * @param vm The parsed command line variables map from which the consumer choice is read
     */
    void setupChosenConsumer(boost::program_options::variables_map const &vm);
    /**
     * @brief Resolves a list of algorithm mnemonics against oaFactoryStore() and appends the produced
     * algorithms to the chain. An unknown mnemonic throws.
     * @param mnemonics The ordered algorithm mnemonics to instantiate and append
     */
    void resolveAlgorithmChain_(std::vector<std::string> const &mnemonics);
    /** @brief Loads every not-yet-loaded module in module_paths_ (plus the individual plugin path): registers
     *  each module's optimization algorithms into oaFactoryStore() and claims a contributed individual.
     *  Idempotent across calls via loaded_module_paths_ (a re-load would collide on already-registered OA
     *  mnemonics), so it is safe to call once during parsing (for command-line/config modules) and again from
     *  ensureConfigured_() (for programmatically-added modules). */
    void loadRequestedModules_();
    /** @brief Extracts --module / --individual paths from the raw command line via a permissive pre-parse
     *  (ignoring every other, not-yet-declared option), merging them into module_paths_ /
     *  individual_plugin_path_. Run before the help text and per-algorithm option surface are built, so a
     *  module's optimization algorithms are registered in oaFactoryStore() first and thus appear in --help
     *  and contribute their own command-line options.
     *  @param argc The number of command line arguments
     *  @param argv The array of command line argument strings */
    void extractEarlyModulePaths_(int argc, char **argv);
    /** @brief Idempotently finalizes configuration: loads any not-yet-loaded runtime modules, resolves the
     *  chosen consumer and resolves the algorithm chain -- honouring programmatic setters called after
     *  construction, with a command-line value overriding a programmatic one (D4). The constructor only
     *  PARSES the command line / config into members; nothing is built until this runs, at the first of
     *  optimize_() / clientRun() / clientMode(). */
    void ensureConfigured_();

    /***************************************************************************/
    // Initialization code for the Geneva library
    GenevaInitializer gi_;

    /***************************************************************************/
    // These parameters can enter the object through the constructor
    //! [coding-naming-example#3]
    bool client_mode_ =
        GO2_DEF_CLIENTMODE; ///< Specifies whether this object represents a network client
    //! [coding-naming-example#3]
    bool update_configs_mode_ =
        false; ///< --update-configs: refresh every config this binary owns (GParserBuilder update-in-place), then exit without optimizing
    std::string config_filename_ =
        GO2_DEF_DEFAULTCONFIGFILE; ///< Indicates where the configuration file is stored
    std::string consumer_name_ =
        GO2_DEF_NOCONSUMER; ///< The name of a consumer requested by the user on the command line

    //---------------------------------------------------------------------------
    // courtier routing (the DEFAULT submission path). setupChosenConsumer() builds the consumer for the
    // chosen mnemonic through the shared factory buildConsumerSetup(), which registers it as the
    // process's single consumer (GConsumerRegistry); every algorithm reads it from there -- no per-OA
    // injection. A fully custom consumer can be supplied directly via registerConsumer(); the GPU
    // consumer is selected by the "gpu" mnemonic and built by buildConsumerSetup() from the problem's
    // marshaller (registered into marshallerProviderStore()) like every other consumer.
    /** @brief The single server-backed/local courtier consumer, shared across all algorithms. Held here
     *  so it (and any listening server) outlives the run and is torn down by RAII at Go2 destruction.
     *  Null when this process is not a submitter (a role-at-runtime consumer placed it in the worker role). */
    std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> consumer_;
    /** @brief Set when a role-at-runtime consumer (see GConsumerProviderT::determinesRoleAtRuntime, e.g.
     *  the MPI worker rank) placed this process in the worker role: the worker loop clientRun_ runs instead
     *  of building a networked client. Type-erased so Go2.hpp needs no transport headers; the captured
     *  consumer shared_ptr keeps the worker node alive. Empty on a submitter / role-from-flag path. */
    std::move_only_function<void()> run_worker_;
    /** @brief The transport-agnostic spec for the chosen consumer, assembled from the command line in
     *  setupChosenConsumer(). Held so clientRun_() can build the matching networked client through the
     *  courtier setup layer (buildConsumerClient) without re-touching the command line or the
     *  concrete consumer types. */
    ConsumerSpec consumer_spec_;

    //---------------------------------------------------------------------------
    // Parameters for the random number generator
    std::uint16_t n_producer_threads_ =
        GO2_DEF_NPRODUCERTHREADS; ///< The number of threads that will simultaneously produce random numbers

    //---------------------------------------------------------------------------
    // Parameters for clients
    std::chrono::duration<double> max_client_duration_ = Gem::Common::duration_from_string(
        DEFAULTDURATION
    ); ///< Maximum time-frame for a client to run

    //---------------------------------------------------------------------------
    // Internal parameters
    bool sorted_ = false; ///< Indicates whether local individuals have been sorted
    std::uint32_t total_iterations_ =
        0; ///< Continuous iteration count accumulated across the algorithm chain (for getIteration_ reporting only)
    bool copy_best_individuals_only_ =
        GO2_DEF_COPYBESTINDIVIDUALSONLY; ///< Indicates whether only the best individuals of an optimization run are copied to the next algorithm
    //---------------------------------------------------------------------------
    // Name and path of a checkpoint file, if supplied by the user
    std::string cp_file_ = "empty";

    //---------------------------------------------------------------------------
    // The list of "chained" optimization algorithms
    std::vector<std::shared_ptr<GOABase>> algorithms_cnt_;
    // OA-owned adaption configs keyed by algorithm personality type ("PERSONALITY_EA", …). Installed on
    // the matching algorithm before it runs in the chain; an adapting algorithm that runs without a
    // registered config is a hard error (adaption intent is never inferred from the genome layout).
    std::map<std::string, std::shared_ptr<oa::GAdaptionConfigBase>> adaption_config_registry_;
    // The default algorithm (if any)
    std::shared_ptr<GOABase> default_algorithm_;
    // A string representation of the default algorithm
    const std::string default_algorithm_str_ = DEFAULTOPTALG; ///< This is the last fall-back
    // Holds an object capable of producing objects of the desired type
    std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> content_creator_ptr_;
    // Where content_creator_ptr_ came from (enforces one-individual-per-process, see claimContentCreator_)
    individualSource content_creator_source_ = individualSource::NONE;
    // Filesystem path to a runtime individual plugin (.so) to load; settable via config or --individual.
    // Empty (the default) means no plugin is loaded -- the individual is expected to be compiled in.
    std::string individual_plugin_path_;
    // Filesystem paths to runtime Geneva modules (.so) to load at startup; settable via the module_paths
    // config key and repeated --module options. Each may contribute optimization algorithms (usable by
    // mnemonic) and/or the optimization individual. Loaded before the algorithm mnemonics are resolved.
    std::vector<std::string> module_paths_;
    // A user-defined means for information retrieval. Shared ownership is deliberate: on
    // hand-over, every algorithm of a chain registers the SAME monitor object rather than a
    // clone of it, so a monitor accumulates across the whole chain (which is what a progress
    // plotter wants). Go2 and each algorithm are therefore co-owners for the run's duration.
    std::vector<std::shared_ptr<oa::GBasePluggableOM>> pluggable_monitors_cnt_;

    //---------------------------------------------------------------------------
    // Two-phase configuration (D3/D4): the constructor only parses the command line / config into the
    // members above; ensureConfigured_() then builds the consumer, loads programmatically-added modules and
    // resolves the algorithm chain once -- so a setter called after construction is honoured, and a value
    // given on the command line overrides a programmatic one.
    /** @brief Guard: ensureConfigured_() runs its work exactly once. */
    bool configured_ = false;
    /** @brief The parsed command line, retained so the consumer can be built later (ensureConfigured_) from
     *  the same options rather than re-touching argv. */
    boost::program_options::variables_map cl_vm_;
    /** @brief The consumer mnemonic explicitly given via --consumer, if any; it wins over a programmatic
     *  setConsumerName() when the run is configured. */
    std::optional<std::string> cli_consumer_name_;
    /** @brief The --optimizationAlgorithms list (comma-separated), captured if the option was given. */
    std::string cli_optimization_algorithms_;
    /** @brief Whether --optimizationAlgorithms was given on the command line; it then wins over
     *  setAlgorithmChain(). */
    bool cli_algorithms_explicit_ = false;
    /** @brief The algorithm-chain mnemonics set programmatically via setAlgorithmChain(), resolved in
     *  ensureConfigured_() unless a command-line list overrides them. */
    std::vector<std::string> programmatic_algorithm_mnemonics_;
    /** @brief Module paths already loaded, so ensureConfigured_() does not re-load a module already loaded
     *  during parsing (a re-load would collide on its registered OA mnemonics). */
    std::set<std::string> loaded_module_paths_;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */
