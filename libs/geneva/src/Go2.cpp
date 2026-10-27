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

#include "geneva/Go2.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GConfigEmission.hpp"
#include "common/GExceptions.hpp"
#include "common/GFactoryT.hpp"
#include "weft/GArchivePolymorphic.hpp" // verifyArchiveRegistrations (boot-time completeness self-check)
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "common/GProviderT.hpp"
#include "courtier/GBaseClientT.hpp" // the networked client run by clientRun_ (built via the setup layer)
// courtier consumer construction, the per-mnemonic command-line spec, the consumer option surface and
// the networked client all live in the shared setup layer (buildConsumerSetup / specFromCommandLine /
// addConsumerOptions / buildConsumerClient); Go2 no longer touches the concrete consumer
// types or the consumer store at all.
#include "geneva/GConsumerSetup.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GFactoryStore.hpp"
#include "geneva/GModuleLoader.hpp"
#include "geneva/genome/GGenome.hpp"
#include "hap/GRandomFactory.hpp"
#include <boost/program_options.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <print>
#include <ranges>
#include <string>
#include <vector>

namespace Gem::Geneva {

/******************************************************************************/
/**
 * Set a number of parameters of the random number factory
 *
 * @param n_producer_threads The number of threads simultaneously producing random numbers
 */
void setRNFParameters(std::uint16_t n_producer_threads) {
    //--------------------------------------------
    // Random numbers are our most valuable good.
    // Set the number of threads. Gem::Hap::randomFactory() is
    // a singleton that will be initialized by this call.
    Gem::Hap::randomFactory()->setNProducerThreads(n_producer_threads);
}

std::once_flag fGo2; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::once_flag fArchiveCheck; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) -- boot self-check runs once

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A constructor that first parses the command line for relevant parameters and then
 * loads data from a configuration file. The user may pass additional configuration
 * parameters. This is the only allowed constructor.
 *
 * @param argc The number of command line arguments
 * @param argv An array with the arguments
 * @param config_filename The name of a configuration file
 * @param user_descriptions A vector of additional command line options (cmp. boost::program_options)
 */
Go2::Go2(
    int argc,
    char **argv,
    std::string const &config_filename,
    boost::program_options::options_description const &user_descriptions
)
  : config_filename_(config_filename) {
    //--------------------------------------------
    // --update-configs: detected here, before this constructor parses Go2.json (so Go2's own config is
    // refreshed too), through the shared config-emission facility -- the same switch, argv detection and
    // update-in-place engine every config-owning binary uses (Go2-based or not). It flips GParserBuilder
    // into update-in-place mode process-wide, so every config subsequently parsed -- Go2's, each
    // algorithm's, the individual's -- is rewritten in canonical form (stale keys dropped, existing values
    // preserved, new keys defaulted). optimize() then refreshes the remaining configs and returns without
    // running an optimization.
    update_configs_mode_ = Gem::Common::configEmissionRequested(argc, argv);
    if(update_configs_mode_) {
        Gem::Common::beginConfigEmission();
    }

    //--------------------------------------------
    // The known optimization algorithms register themselves with the global factory store at
    // library-load time (see the self-registration helpers in each factory's .cpp). Consumers are
    // built on demand by the courtier setup layer. The GenevaInitializer member gi_ performs the
    // required runtime initialization (random factory) via its constructor / destructor.

    //--------------------------------------------
    // Parse configuration file options
    this->parseConfigFile(config_filename);

    //--------------------------------------------
    // Load configuration options from the command line. Any requested runtime modules / individual plugin
    // are loaded from inside parseCommandLine (via loadRequestedModules_), after the command line is parsed
    // but before the algorithm mnemonics are resolved -- so a module's optimization algorithms are usable by
    // mnemonic, and a contributed individual claims the single content-creator slot (a compiled-in
    // registerContentCreator() or a second plugin then hits the one-individual-per-process guard). Server
    // and client are the same binary launched with different options, so both load identically.
    parseCommandLine(argc, argv, user_descriptions);

    //--------------------------------------------
    // GArchive registration completeness self-check. parseCommandLine (above) has loaded any
    // command-line-requested runtime modules / individual plugins, so every concrete polymorphic
    // type this process can (de)serialize is now registered. Assert that each identity-registered
    // type is also archive-dispatchable, turning a missing GEM_REGISTER_ARCHIVABLE into a loud
    // failure here at startup rather than an "unknown tag" at the first (possibly remote,
    // possibly hours-into-a-run) deserialization -- the enumerable replacement for Boost's
    // compiler-invisible void_cast gap. Runs once per process.
    std::call_once(fArchiveCheck, []() {
        Gem::Weft::verifyArchiveRegistrations();
        glogger << "GArchive registration self-check passed: "
                << Gem::Weft::archiveRegisteredHierarchyCount()
                << " polymorphic hierarchies are fully archive-dispatchable." << '\n'
                << GLOGGING;
    });

    //--------------------------------------------
    // Random numbers are our most valuable good.
    // Initialize all necessary variables
    std::call_once(fGo2, [this]() { setRNFParameters(this->n_producer_threads_); });
}

/******************************************************************************/
/**
 * @brief Destructor. Releases the process consumer this Go2 established so a networked consumer's server
 * threads are torn down by RAII at the end of the run, instead of lingering in the process-global
 * registry until process exit. Only the consumer THIS Go2 registered is cleared (a later
 * Go2 / registerConsumer may have replaced it), and only if the registry still holds it.
 */
Go2::~Go2() {
    this->releaseConsumer_();
}

/******************************************************************************/
/**
 * @brief Releases the process consumer this Go2 established: cleared from the registry (only if the
 * registry still holds it -- a later Go2 / registerConsumer may have replaced it) and the own
 * reference dropped, so a networked consumer's server threads are torn down by RAII. Idempotent;
 * called by the destructor and by derived classes that must sequence their own teardown (e.g.
 * MPI finalization) after the consumer is gone.
 */
void Go2::releaseConsumer_() {
    if(consumer_) {
        auto &registry = Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance();
        if(registry.consumer() == consumer_) {
            registry.clear();
        }
        consumer_.reset(); // drop our reference -> RAII teardown once no one else holds it
    }
}

/******************************************************************************/
/**
 * Allows to register a default algorithm to be used when no other algorithms
 * have been specified. When others have been specified, this algorithm will
 * not be used. Note that any individuals registered with the default algorithm
 * will be copied into the Go2 object. This function takes the algorithm from a
 * global algorithm factory store. The algorithm needs to be specified using a
 * small nickname, such as "ea" for "Evolutionary Algorithms". See the available
 * algorithms in the Geneva distribution for further information.
 *
 * @param mn A small mnemonic for the optimization algorithm
 */
void Go2::registerDefaultAlgorithm(std::string const &mn) {
    // Retrieve the algorithm from the global store
    std::shared_ptr<Gem::Common::GProviderT<GOABase>> p;
    if(not oaFactoryStore()->get(mn, p)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::registerDefaultAlgorithm(std::string): Error!" << '\n'
            << "Got invalid algorithm mnemonic " << mn << '\n'
        );
    }

    this->registerDefaultAlgorithm(p->provide());
}

/******************************************************************************/
/**
 * @brief Allows to register a default algorithm to be used when no other algorithms have been specified.
 *
 * When others have been specified, this algorithm will not be used. Note that any individuals registered
 * with the default algorithm will be copied into the Go2 object.
 *
 * @param default_algorithm A smart pointer to the optimization algorithm to use as the default
 */
void Go2::registerDefaultAlgorithm(const std::shared_ptr<GOABase> &default_algorithm) {
    // Check that the pointer isn't empty
    if(not default_algorithm) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::registerDefaultAlgorithm(): Error!" << '\n'
            << "Got empty algorithm." << '\n'
        );
    }

    // If any individuals have been storedn in the default algorithm, we assume
    // that the user wants us to use them and copy them over. Note that these are not cloned.
    if(not default_algorithm->empty()) { // Have individuals been registered ?
        for(const auto &ind_ptr : *default_algorithm) {
            this->push_back(ind_ptr->clone());
        }
        // Remove the individuals from the old algorithm
        default_algorithm->clear();
    }

    // Register the algorithm
    default_algorithm_ = default_algorithm;
}

/******************************************************************************/
/**
 * @brief Allows to register a pluggable optimization monitor.
 *
 * @param pluggable_om A smart pointer to the pluggable optimization monitor to register (must not be empty)
 */
void Go2::registerPluggableOM(const std::shared_ptr<oa::GBasePluggableOM> &pluggable_om) {
    if(pluggable_om) {
        pluggable_monitors_cnt_.push_back(pluggable_om);
    }
    else {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::registerPluggableOM(): Tried to register empty pluggable optimization monitor\n"
        );
    }
}

/******************************************************************************/
/**
 * @brief Allows resetting the local pluggable optimization monitors.
 */
void Go2::resetPluggableOM() {
    pluggable_monitors_cnt_.clear();
}

/******************************************************************************/
/**
 * @brief Allows to check whether pluggable optimization monitors were registered.
 *
 * @return true if at least one pluggable optimization monitor has been registered, false otherwise
 */
bool Go2::hasOptimizationMonitors() const {
    return not pluggable_monitors_cnt_.empty();
}

/******************************************************************************/
/**
 * @brief Allows to set the maximum running time for a client.
 *
 * A duration of 0 results in no time limit being set.
 *
 * @param max_duration The maximum running time for a client (in seconds); 0 means no time limit
 */
void Go2::setMaxClientTime(std::chrono::duration<double> max_duration) {
    max_client_duration_ = max_duration;
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the maximum running time for a client.
 *
 * @return The maximum running time for a client (in seconds); 0 means no time limit
 */
std::chrono::duration<double> Go2::getMaxClientTime() const {
    return max_client_duration_;
}

/******************************************************************************/
/**
 * @brief Triggers execution of the client loop.
 *
 * Note that it is up to you to terminate the program after calling this function.
 *
 * @return The exit status of the client loop (0 on normal completion)
 */
int Go2::clientRun() {
    // Finalize configuration (build the consumer, resolve modules/algorithms) before serving as a client;
    // idempotent if clientMode() already triggered it.
    this->ensureConfigured_();
    return this->clientRun_();
}

/**
 * @brief Implementation of the client loop.
 *
 * On an MPI worker rank routed through courtier this serves work through the courtier worker node;
 * otherwise it builds the networked client for the chosen consumer and runs its processing loop.
 *
 * @return The exit status of the client loop (always 0 here)
 */
int Go2::clientRun_() {
    // When a role-at-runtime consumer placed this process in the worker role (e.g. an MPI worker rank),
    // serve work through its worker loop (held type-erased from setupChosenConsumer) instead of a
    // networked client.
    if(run_worker_) {
        run_worker_();
        return 0;
    }

    // Build the networked client for the chosen consumer through the courtier setup layer, from the
    // spec assembled in setupChosenConsumer(). The client is wire-compatible with the courtier socket
    // server. Go2 thus stays free of the concrete consumer/client types and the consumer store.
    std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>> const p =
        Gem::Geneva::buildConsumerClient(consumer_spec_);

    if(not p) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::clientRun(): Error!" << '\n'
            << "Consumer \"" << consumer_name_ << "\" does not provide a networked client." << '\n'
        );
    }

    // Set the maximum runtime of the client
    p->setMaxTime(this->max_client_duration_);

    // Start the actual processing loop. This call will not return until run() is finished.
    p->run();

    return 0;
}

/******************************************************************************/
/**
 * Checks whether this object is running in client mode
 *
 * @return A boolean which indicates whether the client mode has been set for this object
 */
bool Go2::clientMode() const {
    // Most consumers take the client/server role from --client, so it is already known after construction
    // and this is a plain getter. A consumer that determines the role at runtime instead (see
    // GConsumerProviderT::determinesRoleAtRuntime -- e.g. the MPI consumer deriving it from the process
    // rank) only reveals the role once its setup runs: for such a consumer we must finalize configuration
    // here, before the caller dispatches on the result, or a worker process would read the default (false)
    // and wrongly call optimize() instead of clientRun(). We test the EFFECTIVE consumer (a --consumer on
    // the command line wins over a programmatic setConsumerName, mirroring ensureConfigured_). The trigger
    // is idempotent, so the later optimize()/clientRun() is a no-op; the const_cast reflects that this is a
    // lazy-init concern, not a change to observable state.
    const std::string &effective_consumer = cli_consumer_name_ ? *cli_consumer_name_ : consumer_name_;
    if(Gem::Geneva::consumerDeterminesRoleAtRuntime(effective_consumer)) {
        const_cast<Go2 *>(this)->ensureConfigured_();
    }
    return client_mode_;
}

/******************************************************************************/
/**
 * @brief Specifies whether only the best individuals of a population should be copied.
 *
 * @param copy_best_individuals_only If true, only the best individuals are carried over between algorithms
 */
void Go2::setCopyBestIndividualsOnly(bool copy_best_individuals_only) {
    copy_best_individuals_only_ = copy_best_individuals_only;
}

/******************************************************************************/
/**
 * @brief Checks whether only the best individuals are copied.
 *
 * @return true if only the best individuals are carried over between algorithms, false otherwise
 */
bool Go2::onlyBestIndividualsAreCopied() const {
    return copy_best_individuals_only_;
}

/******************************************************************************/
/**
 * @brief Retrieves the currently registered number of algorithms.
 *
 * @return The number of optimization algorithms currently registered with this object
 */
std::size_t Go2::getNAlgorithms() const {
    return algorithms_cnt_.size();
}

/******************************************************************************/
/**
 * Allows to add an optimization algorithm to the chain. If any individuals have
 * been stored in these algorithms, Go2 will unload them and store them for later
 * usage.
 *
 * @param alg A base pointer to another optimization algorithm
 */
void Go2::addAlgorithm(const std::shared_ptr<GOABase> &alg) {
    // Check that the pointer is not empty
    if(not alg) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::addAlgorithm(): Error!" << '\n'
            << "Tried to register an empty pointer" << '\n'
        );
    }

    // If any individuals have already been registered with alg, we assume
    // that the user wants us to add them to the optimization and copy them over.
    // Note that these are not cloned, as we will clear its vector anyway.
    if(not alg->empty()) { // Have individuals been registered?
        for(const auto &ind_ptr : *alg) {
            this->push_back(ind_ptr->clone());
        }
        // Remove the individuals from the old algorithm
        alg->clear();
    }

    algorithms_cnt_.push_back(alg);
}

/******************************************************************************/
/**
 * Retrieves the algorithms that were registered with this class. Note that
 * this function gives you access to the actual objects used for the
 * optimization, so altering their configuration will alter the course of the
 * optimization.
 *
 * @return The algorithms that were registered with this class
 */
std::vector<std::shared_ptr<GOABase>> Go2::getRegisteredAlgorithms() {
    return algorithms_cnt_;
}

/**
 * @brief Returns the name of the consumer currently in use.
 *
 * The consumer is set using the command line argument `--consumer`.
 *
 * @return The used consumer's name
 */
std::string Go2::getConsumerName() {
    return consumer_name_;
}

/******************************************************************************/
/**
 * @brief Selects the consumer by mnemonic (programmatic equivalent of --consumer). Takes effect when the
 * run is configured; a --consumer value on the command line overrides it.
 *
 * @param mnemonic The consumer mnemonic to use (e.g. "stc", "asio", "mpi", "gpu")
 */
void Go2::setConsumerName(std::string const &mnemonic) {
    consumer_name_ = mnemonic;
}

/******************************************************************************/
/**
 * @brief Appends a runtime module (.so) path to load at startup (programmatic equivalent of --module).
 *
 * @param path Filesystem path to the module to load (an empty path is ignored)
 */
void Go2::addModulePath(std::string const &path) {
    if(not path.empty()) {
        module_paths_.push_back(path);
    }
}

/******************************************************************************/
/**
 * @brief Replaces the list of runtime module (.so) paths to load at startup.
 *
 * @param paths The module paths to load
 */
void Go2::setModulePaths(std::vector<std::string> paths) {
    module_paths_ = std::move(paths);
}

/******************************************************************************/
/**
 * @brief Retrieves the runtime module (.so) paths configured for loading.
 *
 * @return The module paths (config + --module + programmatic)
 */
std::vector<std::string> Go2::getModulePaths() const {
    return module_paths_;
}

/******************************************************************************/
/**
 * @brief Sets the algorithm chain by mnemonic (programmatic equivalent of --optimizationAlgorithms).
 *
 * Resolution is deferred to run configuration, so a mnemonic contributed by a runtime module is available.
 * A --optimizationAlgorithms list on the command line overrides this.
 *
 * @param mnemonics The ordered algorithm mnemonics forming the chain
 */
void Go2::setAlgorithmChain(std::vector<std::string> const &mnemonics) {
    programmatic_algorithm_mnemonics_ = mnemonics;
}

/******************************************************************************/
/**
 * @brief Retrieves the programmatic algorithm-chain mnemonics set via setAlgorithmChain().
 *
 * @return The algorithm mnemonics (empty if none were set programmatically)
 */
std::vector<std::string> Go2::getAlgorithmChain() const {
    return programmatic_algorithm_mnemonics_;
}

/******************************************************************************/
/**
 * Makes it easier to add algorithms. The idea is to call this function like this:
 *
 * Go2 go2;
 * go2 & alg1 & alg2 & alg3;
 * go2.optimize();
 *
 * @param alg A base pointer to another optimization algorithm
 * @return A reference to this object
 */
Go2 &Go2::operator&(const std::shared_ptr<GOABase> &alg) {
    this->addAlgorithm(alg); // NOLINT
    return *this;
}

/***************************************************************************/
/**
 * @brief Allows to add an optimization algorithm through its mnemonic.
 *
 * @param mn A small mnemonic identifying the optimization algorithm in the global factory store (e.g. "ea")
 */
void Go2::addAlgorithm(std::string const &mn) {
    // Retrieve the algorithm from the global store
    std::shared_ptr<Gem::Common::GProviderT<GOABase>> p;
    if(not oaFactoryStore()->get(mn, p)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::addAlgorithm(std::string): Error!" << '\n'
            << "Got invalid algorithm mnemonic " << mn << '\n'
        );
    }

    this->addAlgorithm(
        p->provide()
    ); // The provider's factory might add a monitor to the object
}

/***************************************************************************/
/**
 * @brief Makes it easier to add algorithms through their mnemonics.
 *
 * @param mn A small mnemonic identifying the optimization algorithm in the global factory store (e.g. "ea")
 * @return A reference to this object
 */
Go2 &Go2::operator&(std::string const &mn) {
    this->addAlgorithm(mn);
    return *this;
}

/***************************************************************************/
/**
 * @brief Allows to register a content creator.
 *
 * A content creator creates individuals to be added to the population.
 *
 * @param cc_ptr A smart pointer to a factory that produces optimizable entities (must not be empty)
 */
void Go2::registerContentCreator(const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> &cc_ptr) {
    // A user-compiled-in individual: claim the single slot, so a later plugin load (or a second
    // registration) is refused by the one-individual-per-process guard.
    this->claimContentCreator_(cc_ptr, individualSource::COMPILED_IN);
}

/******************************************************************************/
/**
 * Claims the single content-creator slot, enforcing that exactly one optimization problem (individual)
 * exists per process: whichever source registers first wins, and any second claim -- a compiled-in
 * registration when a plugin was already loaded, or a second plugin -- throws, naming both sources.
 */
void Go2::claimContentCreator_(
    const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> &cc_ptr,
    individualSource source
) {
    auto sourceStr = [](individualSource s) -> const char * {
        switch(s) {
            case individualSource::COMPILED_IN: return "compiled in (registerContentCreator)";
            case individualSource::LOADED: return "loaded from a plugin (--individual)";
            default: return "none";
        }
    };

    if(not cc_ptr) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::claimContentCreator_(): Error!" << '\n'
            << "Tried to register an empty content creator (source: " << sourceStr(source) << ")" << '\n'
        );
    }

    if(content_creator_source_ != individualSource::NONE) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::claimContentCreator_(): Error!" << '\n'
            << "An optimization problem (individual) is already provided (" << sourceStr(content_creator_source_)
            << ");" << '\n'
            << "refusing to add another (" << sourceStr(source) << ")." << '\n'
            << "Exactly one individual may exist per process -- compile one in OR load one, not both, and"
            << '\n'
            << "never two. (If you compiled an individual in, do not also pass --individual.)" << '\n'
        );
    }

    content_creator_ptr_ = cc_ptr;
    content_creator_source_ = source;
}

/******************************************************************************/
/**
 * Perform the actual optimization cycle. Note that we assume that individuals
 * have either been registered with the Go2 object or with the first algorithm
 * which has been added to the object. When no algorithm was added to the Go2
 * object (either on the command line or by passing a mnemonic or smart pointer),
 * a default algorithm will be used. Check the @c \#define @c DEFAULTOPTALG for information
 * on the type of algorithm being used. The default algorithm may also be altered
 * by the user.
 *
 * @param offset An iteration offset at which the first algorithm should start
 *               (e.g. for checkpoint resume); subsequent algorithms in the chain
 *               always start at 0. Defaults to 0 through the GOptimizerIT interface.
 * @return A pointer to this object (after the optimization has run)
 */
Go2 const *Go2::optimize_(std::uint32_t offset) {
    // Finalize configuration first (build the consumer, load programmatically-added modules, resolve the
    // algorithm chain); idempotent if clientMode() already triggered it.
    this->ensureConfigured_();

    // --update-configs: refresh every config this binary owns and return without optimizing.
    if(update_configs_mode_) {
        this->refreshAllConfigs_();
        return this;
    }
    this->ensureAlgorithmPresent();
    std::uint32_t const first_algorithm_offset = this->prepareInitialPopulation(offset);
    this->runAlgorithmChain(first_algorithm_offset);
    this->sortIndividualsByFitness();
    return this;
}

/******************************************************************************/
/**
 * @brief --update-configs pass: refresh every configuration file this binary owns.
 *
 * With GParserBuilder in update-in-place mode (set in the constructor), Go2.json was already rewritten by
 * the constructor's parse. Here we additionally produce one object from each registered algorithm factory
 * -- which parses (and thus rewrites) that algorithm's config file -- and one individual from the content
 * creator, refreshing the individual's config. No optimization is run. Per-config failures are warned
 * about rather than aborting, so one missing/odd config does not stop the rest from being refreshed.
 */
void Go2::refreshAllConfigs_() {
    // Every registered optimization-algorithm factory: producing one algorithm parses its config file.
    for(auto const &provider : oaFactoryStore()->getContentSnapshot()) {
        try {
            (void) provider->provide();
        }
        catch(std::exception const &e) {
            glogger << "In Go2::refreshAllConfigs_(): could not refresh the \"" << provider->getMnemonic()
                    << "\" algorithm config: " << e.what() << '\n'
                    << GWARNING;
        }
    }

    // The individual (content-creator) config, if an individual is available.
    if(content_creator_ptr_) {
        try {
            (void) (*content_creator_ptr_)();
        }
        catch(std::exception const &e) {
            glogger << "In Go2::refreshAllConfigs_(): could not refresh the individual config: " << e.what()
                    << '\n'
                    << GWARNING;
        }
    }

    // --update-configs is a utility mode, not an optimization: finish HERE, via the shared config-emission
    // facility (log + exit), so the caller's boilerplate (optimize() then getBestGlobalIndividual()) is not
    // reached -- there is no population, so getBestGlobalIndividual() would have nothing to return. This lets
    // every example's main() run unchanged. The std::exit inside finishConfigEmission runs the registered
    // atexit / static teardown (flushing stdio and the logger, releasing the library's RNG factory guard);
    // nothing produced by this pass needs a Go2 destructor.
    Gem::Common::finishConfigEmission();
}

/******************************************************************************/
/**
 * @brief Adds the Geneva default algorithm if the user has registered none.
 */
void Go2::ensureAlgorithmPresent() {
    if(algorithms_cnt_.empty()) {
        if(not default_algorithm_) {
            // No algorithms given, no default algorithm specified by the user:
            // Simply add the Geneva-side default algorithm
            this->registerDefaultAlgorithm(default_algorithm_str_);

            glogger << "In Go2::optimize(): INFORMATION:" << '\n'
                    << "No user-defined optimization algorithm available." << '\n'
                    << "Using default algorithm \"" << default_algorithm_str_ << "\" instead."
                    << '\n'
                    << GLOGGING;
        }

        algorithms_cnt_.push_back(default_algorithm_->clone<GOABase>());
    }
}

/******************************************************************************/
/**
 * @brief Loads a checkpoint into the first algorithm, or fills the population from the content creator.
 *
 * Returns the iteration offset for the FIRST algorithm only (a checkpoint resume overrides the
 * passed-in offset); every subsequent algorithm in the chain starts at iteration 0 so it gets its
 * full iteration budget -- otherwise a chained algorithm would inherit the previous one's end
 * iteration and, with an absolute max-iteration halt criterion, stop immediately.
 *
 * @param offset The iteration offset requested for the first algorithm
 * @return The iteration offset the first algorithm should actually start at (offset, or one past the
 *         checkpoint's last iteration when resuming from a checkpoint)
 */
std::uint32_t Go2::prepareInitialPopulation(std::uint32_t offset) {
    this->validateCheckpointFitsFirstAlgorithm();

    // Load the checkpoint file or create individuals from the content creator
    if(cp_file_ != "empty") {
        this->loadFirstAlgorithmCheckpoint();
        // The first algorithm starts right after the iteration where the checkpoint file ended
        return algorithms_cnt_[0]->getIteration() + 1;
    }

    this->fillPopulationFromContentCreator();
    return offset;
}

/******************************************************************************/
/**
 * @brief Throws if a configured checkpoint file does not fit the first algorithm in the chain.
 */
void Go2::validateCheckpointFitsFirstAlgorithm() const {
    if(cp_file_ != "empty" &&
       not algorithms_cnt_[0]->cp_personality_fits(std::filesystem::path(cp_file_))) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::optimize(): Error!" << '\n'
            << "Checkpoint file " << cp_file_ << " does not" << '\n'
            << "fit requirements of first algorithm "
            << algorithms_cnt_[0]->getAlgorithmPersonalityType() << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Loads the configured checkpoint into the first algorithm in the chain.
 */
void Go2::loadFirstAlgorithmCheckpoint() {
    algorithms_cnt_[0]->loadCheckpoint(std::filesystem::path(cp_file_));
}

/******************************************************************************/
/**
 * @brief Fills the (empty) population from the registered content creator, throwing if no creator and no
 * individuals are available.
 */
void Go2::fillPopulationFromContentCreator() {
    if(not this->empty()) {
        return; // individuals were already registered directly -- nothing to create
    }
    if(not content_creator_ptr_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::optimize(): Error!" << '\n'
            << "No optimization problem (individual) is available: no content creator and no"
            << '\n'
            << "individuals have been registered. Provide exactly one of:" << '\n'
            << "  (1) compile an individual in and call registerContentCreator();" << '\n'
            << "  (2) add individuals directly via push_back();" << '\n'
            << "  (3) load one at runtime with --individual <path>.so (or the" << '\n'
            << "      individual_plugin_path config-file setting)." << '\n'
        );
    }
    createIndividualsFromContentCreator();
}

/******************************************************************************/
/**
 * @brief fillPopulationFromContentCreator() loop: fill up to the first algorithm's default population size
 * by calling the registered content creator, stopping early if it runs empty (but throwing if it never
 * produced a single individual).
 */
void Go2::createIndividualsFromContentCreator() {
    for(std::size_t ind = 0; ind < algorithms_cnt_.at(0)->getDefaultPopulationSize(); ind++) {
        std::shared_ptr<gen::GGenome> const p_ind = (*content_creator_ptr_)();
        if(p_ind) {
            this->push_back(p_ind);
        }
        else {                  // No valid item received, the factory has run empty
            if(this->empty()) { // Still empty?
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In Go2::optimize(): Error!" << '\n'
                    << "The content creator did not deliver any individuals" << '\n'
                    << "and none have been registered so far." << '\n'
                    << "No way to continue." << '\n'
                );
            }
            break;
        }
    }
}

/******************************************************************************/
/**
 * @brief Runs the registered algorithms in sequence, threading the individuals from one algorithm to the next.
 *
 * Only the first algorithm honours the offset (checkpoint resume / user offset); subsequent algorithms
 * start at 0 (full iteration budget).
 *
 * @param first_algorithm_offset The iteration offset at which the first algorithm in the chain should start
 */
void Go2::runAlgorithmChain(std::uint32_t first_algorithm_offset) {
    total_iterations_ = 0;
    sorted_           = false;
    for(auto const &[alg_index, alg_ptr] : algorithms_cnt_ | std::views::enumerate) {
        // No per-algorithm broker injection: every algorithm submits through the one process consumer
        // (registered in GConsumerRegistry by setupChosenConsumer / registerConsumer). A standalone
        // algorithm with none set builds a default local consumer on first use.

        // Add the pluggable optimization monitors to the algorithm
        for(auto const &pm_ptr : pluggable_monitors_cnt_) {
            alg_ptr->registerPluggableOM(pm_ptr);
        }

        // Add the individuals to the algorithm
        for(const auto &ind_ptr : *this) {
            alg_ptr->push_back(ind_ptr->clone());
        }

        // Remove our local copies
        this->clear();

        // If an OA-owned adaption config was registered for this algorithm's type, hand
        // it over now (before it runs). The algorithm adopts it -- validating it against the population's
        // genome -- in place of deriving a default from the genome layout.
        if(const auto it = adaption_config_registry_.find(alg_ptr->getAlgorithmPersonalityType());
           it != adaption_config_registry_.end()) {
            alg_ptr->setAdaptionConfig(it->second);
        }

        // Do the actual optimization (see first_algorithm_offset above)
        if(alg_index == 0) {
            alg_ptr->optimize(first_algorithm_offset);
        }
        else {
            alg_ptr->optimize();
        }

        // Accumulate a continuous iteration count for reporting (getIteration_) only,
        // WITHOUT feeding it back as the next algorithm's start offset.
        total_iterations_ += alg_ptr->getIteration();

        // Unload the individuals from the last algorithm and store them again in this object
        if(copy_best_individuals_only_) {
            for(auto &best_ind_ptr : alg_ptr->getBestGlobalIndividuals<gen::GGenome>()) {
                this->push_back(std::move(best_ind_ptr));
            }
        }
        else { // copy all individuals
            for(const auto &ind_ptr : *alg_ptr) {
                this->push_back(ind_ptr->clone());
            }
        }

        alg_ptr->clear();            // Get rid of local individuals in the algorithm
        alg_ptr->resetPluggableOM(); // Get rid of the algorithm's pluggable optimization monitors
    }
}

/******************************************************************************/
/**
 * @brief Registers an OA-owned adaption configuration for an algorithm type.
 *
 * Stored keyed by the algorithm's personality type and handed to the matching algorithm in
 * runAlgorithmChain() before it runs. A null config removes any existing entry.
 *
 * @param oa_personality_type The algorithm personality type the configuration applies to (registry key)
 * @param config A smart pointer to the adaption configuration to register; a null pointer removes any existing entry
 */
void Go2::registerAdaptionConfig(
    const std::string &oa_personality_type,
    std::shared_ptr<oa::GAdaptionConfigBase> config
) {
    if(config) {
        adaption_config_registry_[oa_personality_type] = std::move(config);
    }
    else {
        adaption_config_registry_.erase(oa_personality_type);
    }
}

/******************************************************************************/
/**
 * @brief Sorts the collected individuals by their (min-only transformed) fitness.
 *
 * Sorting makes the best individuals easy to extract afterwards.
 */
void Go2::sortIndividualsByFitness() {
    std::ranges::sort(
        this->begin(),
        this->end(),
        [](const auto &x_ptr, const auto &y_ptr) -> bool {
            return minOnly_transformed_fitness(*x_ptr) < minOnly_transformed_fitness(*y_ptr);
        }
    );

    sorted_ = true;
}

/******************************************************************************/
/**
 * @brief Retrieves the best individual found.
 *
 * This function returns a base pointer. Conversion is done through the clone<>() template in GCommonInterfaceT.
 *
 * @return The best individual found
 */
std::unique_ptr<gen::GGenome> Go2::getBestGlobalIndividual_() const {
    // Do some error checking
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::getBestGlobalIndividual_(): Error!" << '\n'
            << "No individuals found" << '\n'
        );
    }

    if(not sorted_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::getBestGlobalIndividual_(): Error!" << '\n'
            << "Tried to retrieve best individual" << '\n'
            << "from an unsorted population." << '\n'
        );
    }

    // Check if the best individual is processed
    if(not this->front()->is_processed() && not this->front()->is_unprocessed()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::getBestGlobalIndividual_(): Error!" << '\n'
            << "Best individual is unprocessed or has errors" << '\n'
        );
    }

    // Simply return the best individual. This will result in an implicit downcast
    return this->front()->clone<gen::GGenome>();
}

/******************************************************************************/
/**
 * @brief Retrieves a list of the best individuals found.
 *
 * This function returns base pointers. Conversion is done through the clone<>() template in GCommonInterfaceT.
 *
 * @return A vector holding the best individuals found
 */
std::vector<std::unique_ptr<gen::GGenome>> Go2::getBestGlobalIndividuals_() const {
    // Do some error checking
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::getBestGlobalIndividuals_(): Error!" << '\n'
            << "No individuals found" << '\n'
        );
    }

    std::vector<std::unique_ptr<gen::GGenome>> best_individuals;
    for(auto const &[pos, ind_ptr] : *this | std::views::enumerate) {
        if(ind_ptr->is_due_for_processing() || ind_ptr->has_errors()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In Go2::getBestGlobalIndividuals_(): Error!" << '\n'
                << "Found individual in position " << pos
                << " which is unprocessed or which has errors" << '\n'
            );
        }

        // This will result in an implicit downcast
        best_individuals.push_back(ind_ptr->clone<gen::GGenome>());
    }

    return best_individuals;
}

/******************************************************************************/
/**
 * @brief Retrieves the best individual of the current iteration.
 *
 * This function is not meaningful for the algorithm-combiner Go2 and always throws when called.
 *
 * @return Never returns normally; always throws a geneva_exception
 */
std::unique_ptr<gen::GGenome> Go2::getBestIterationIndividual_() const {
    throw geneva_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In Go2::getBestIterationIndividual_(): Error!" << '\n'
        << "This function should not be called" << '\n'
    );
}

/******************************************************************************/
/**
 * @brief Retrieves a list of the best individuals of the current iteration.
 *
 * This function is not meaningful for the algorithm-combiner Go2 and always throws when called.
 *
 * @return Never returns normally; always throws a geneva_exception
 */
std::vector<std::unique_ptr<gen::GGenome>> Go2::getBestIterationIndividuals_() const {
    throw geneva_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In Go2::getBestIterationIndividuals_(): Error!" << '\n'
        << "This function should not be called" << '\n'
    );
}

/******************************************************************************/
/**
 * @brief Satisfies a requirement of GOptimizerIT (no-op for the algorithm combiner).
 */
void Go2::evaluatePopulation_() { /* nothing */
}

/******************************************************************************/
/**
 * Adds local configuration options to a GParserBuilder object
 *
 * @param gpb The GParserBuilder object to which configuration options should be added
 */
void Go2::addConfigurationOptions(Gem::Common::GParserBuilder &gpb) {
    this->addConfigurationOptions_(gpb);
}

/******************************************************************************/
/**
 * Adds local configuration options to a GParserBuilder object
 *
 * @param gpb The GParserBuilder object to which configuration options should be added
 */
void Go2::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    using namespace Gem::Common;

    // Add local data only -- no options from parent classes
    gpb.registerFileParameter<std::uint16_t>(
        "n_producer_threads",
        GO2_DEF_NPRODUCERTHREADS,
        [this](std::uint16_t npt) { this->setNProducerThreads(npt); }
    ) << "The number of threads simultaneously producing random numbers";

    gpb.registerFileParameter<bool>(
        "copy_best_individuals_only",
        GO2_DEF_COPYBESTINDIVIDUALSONLY,
        [this](bool copy_best_individuals_only) {
            this->setCopyBestIndividualsOnly(copy_best_individuals_only);
        }
    ) << "Indicates whether only the best individuals should be copied when"
      << '\n'
      << "switching from one optimization algorithm to the next";

    gpb.registerFileParameter<std::string>(
        "individual_plugin_path",
        std::string(),
        [this](std::string const &p) { individual_plugin_path_ = p; }
    ) << "Filesystem path to a runtime individual (optimization-problem) plugin (.so) to load at startup."
      << '\n'
      << "Empty (the default) means the individual is compiled into this binary. The --individual"
      << '\n'
      << "command-line option overrides this setting.";

    gpb.registerFileParameter<std::string>(
        "module_paths",
        module_paths_,
        std::vector<std::string>(),
        Gem::Common::VAR_IS_SECONDARY,
        "Filesystem paths to runtime Geneva modules (.so) loaded at startup, each of which may contribute "
        "optimization algorithms (usable by their mnemonic) and/or the optimization individual. Repeated "
        "--module command-line options add to this list. Empty (the default) loads no modules."
    );

    gpb.registerFileParameter<std::string>(
        "consumer",
        GO2_DEF_CONSUMER,
        [this](std::string const &c) { consumer_name_ = c; }
    ) << "The consumer (parallelization backend) used when none is given on the command line."
      << '\n'
      << "One of: sc, stc, asio, beast, mpi, gpu (availability depends on build options and the"
      << '\n'
      << "problem; e.g. gpu requires a registered GPU consumer builder). The --consumer command-line"
      << '\n'
      << "option overrides this setting.";
}

/******************************************************************************/
/**
 * Allows marking this object as belonging to a client as opposed to a server
 *
 * @param client_mode Allows marking this object as belonging to a client as opposed to a server
 */
void Go2::setClientMode(bool client_mode) {
    // Note: a consumer that determines its client/server role at runtime (see
    // GConsumerProviderT::determinesRoleAtRuntime, e.g. the MPI consumer, from its process rank) overrides
    // this request when configuration is finalized -- see setupChosenConsumer(), which derives client_mode_
    // from the courtier setup result for such a consumer.
    client_mode_ = client_mode;
}

/******************************************************************************/
/**
 * Allows to set the number of threads that will simultaneously produce random numbers.
 *
 * @param n_producer_threads The number of threads that will simultaneously produce random numbers
 */
void Go2::setNProducerThreads(std::uint16_t n_producer_threads) {
    n_producer_threads_ = n_producer_threads;
}

/******************************************************************************/
/**
 * Allows to retrieve the number of threads that will simultaneously produce random numbers.
 *
 * @return The number of threads that will simultaneously produce random numbers
 */
std::uint16_t Go2::getNProducerThreads() const {
    return n_producer_threads_;
}

/******************************************************************************/
/**
 * @brief Retrieval of the current iteration.
 *
 * @return The accumulated iteration count across all algorithms run so far (for reporting only)
 */
uint32_t Go2::getIteration_() const {
    return total_iterations_;
}

/******************************************************************************/
/**
 * Returns the name of this optimization algorithm
 *
 * @return The name assigned to this optimization algorithm
 */
std::string Go2::getAlgorithmName_() const {
    return {"Algorithm Combiner"};
}

/******************************************************************************/
/** @brief Returns one-word information about the type of optimization algorithm. */
std::string Go2::getAlgorithmPersonalityType_() const {
    return {"PERSONALITY_NONE"};
}

/******************************************************************************/
/******************************************************************************/
namespace {

/**
 * @brief Builds a "mnemonic:  human-readable-name" listing of all entries in a store.
 *
 * @tparam StorePtr The (pointer-like) type of the store being iterated
 * @param store The store whose registered keys and human-readable names should be listed
 * @return A newline-separated listing of "mnemonic:  name" for every entry in the store
 */
template <typename StorePtr>
std::string listMnemonics(const StorePtr& store) {
    std::vector<std::string> keys;
    store->getKeyVector(keys);
    std::string result;
    for(auto const &key : keys) {
        result += std::format("{}:  {}\n", key, store->get(key)->getName());
    }
    return result;
}

} // anonymous namespace

/******************************************************************************/
/**
 * @brief Parses the command line for the algorithms, consumer, checkpoint file and client options.
 *
 * @param argc The number of command line arguments
 * @param argv An array with the arguments
 * @param user_options A program_options object for user-defined command line options
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent CLI-option registration sweep (PHASE 1-3: module preload, option-surface assembly, parse + record); splitting would scatter the option list and the ordering invariant between phases
void Go2::parseCommandLine(
    int argc,
    char **argv,
    boost::program_options::options_description const &user_options
) {
    namespace po = boost::program_options;

    try {
        // PHASE 1 -- load command-line/config modules up front. Their optimization algorithms register into
        // oaFactoryStore() here, BEFORE the help text and per-algorithm option surface are built below, so a
        // module's OAs appear in --help and contribute their own command-line options. (--module is extracted
        // by a permissive pre-parse because the full option surface it feeds does not exist yet.)
        // Programmatically-added modules are loaded later, in ensureConfigured_().
        this->extractEarlyModulePaths_(argc, argv);
        this->loadRequestedModules_();

        std::string max_client_duration = EMPTYDURATION; // 00:00:00

        std::string optimization_algorithms; // NOLINT(cppcoreguidelines-init-variables)
        std::string checkpoint_file = "empty";

        // PHASE 2 -- build the option surface (now reflecting any module-contributed OAs).
        // Help texts listing the registered algorithms / consumers
        std::string const oa_help = std::format(
            "A comma-separated list of optimization algorithms, e.g. \"arg1,arg2\". "
            "{} algorithms have been registered: \n{}",
            oaFactoryStore()->size(), listMnemonics(oaFactoryStore()));

        std::string const consumer_help = std::format(
            "The name of the process consumer used for work-item evaluation (see --help for "
            "the registered consumer mnemonics). "
            "{} consumers are available: \n{}",
            Gem::Geneva::consumerCount(), Gem::Geneva::consumerListing());

        auto usage_string = std::string("Usage: ") + argv[0] + " [options]";

        boost::program_options::options_description general(usage_string);
        boost::program_options::options_description basic("Basic options");

        // First add local options
        basic.add_options()
				("help,h", "Emit help message")
				("showAll", "Show all available options")
				("update-configs", "Refresh every configuration file this binary owns (Go2.json, each algorithm's config and the individual's config) in place -- drop keys no registered parameter consumes, keep existing values, add newly-registered keys with defaults -- then exit without optimizing. Runs on the local thread-pool consumer regardless of --consumer.")
				("optimizationAlgorithms,a", po::value<std::string>(&optimization_algorithms), oa_help.c_str())
				("cp_file,f", po::value<std::string>(&checkpoint_file)->default_value("empty"),
				 "A file (including its path) holding a checkpoint for a given optimization algorithm")
				("client", "Indicates that this program should run as a client or in server mode. Note that this setting will trigger an error unless called in conjunction with a consumer capable of dealing with clients. This option is ignored when working with the mpi consumer, because the mpi consumer will configure itself to be a client or server depending on its rank.")
				("max_client_duration", po::value<std::string>(&max_client_duration)->default_value(EMPTYDURATION),
				 R"(The maximum runtime for a client in the form "hh:mm:ss". Note that a client may run longer as this time-frame if its work load still runs. The default value "00:00:00" means: "no time limit")")
				("consumer,c", po::value<std::string>(&consumer_name_), consumer_help.c_str())
				("individual,i", po::value<std::string>(&individual_plugin_path_),
				 "Filesystem path to a runtime individual (optimization-problem) plugin (.so) to load at "
				 "startup. Overrides the individual_plugin_path config-file setting. Omit it to use an "
				 "individual compiled into this binary.")
				("module,m", po::value<std::vector<std::string>>()->composing(),
				 "Filesystem path to a runtime Geneva module (.so) to load at startup (repeatable). A module "
				 "may contribute optimization algorithms (usable by their mnemonic) and/or the optimization "
				 "individual. Adds to the module_paths config-file setting.");

        // Add additional options coming from the algorithms and consumers
        boost::program_options::options_description visible(
            "Global algorithm- and consumer-options"
        );
        boost::program_options::options_description hidden(
            "Hidden algorithm- and consumer-options"
        );

        // Register the consumer command-line options through the courtier setup layer (the single
        // owner of the consumer option surface) -- no consumer store iteration.
        Gem::Geneva::addConsumerOptions(visible, hidden);

        // Retrieve available command-line options from registered optimization
        // algorithm factories, if any (same snapshot pattern).
        if(not oaFactoryStore()->empty()) {
            for(auto const &factory : oaFactoryStore()->getContentSnapshot()) {
                factory->addCLOptions(visible, hidden);
            }
        }

        // Add the other options to "general"
        if(user_options.options().empty()) {
            general.add(basic).add(visible).add(hidden);
        }
        else {
            general.add(basic).add(user_options).add(visible).add(hidden);
        }

        // PHASE 3 -- parse the command line and record the results into members. The consumer and the
        // algorithm chain are NOT built here: that is deferred to ensureConfigured_() (D3), so a programmatic
        // setter called after construction is honoured. The parsed map is retained (cl_vm_) for that step.
        po::variables_map vm;
        po::store(
            po::parse_command_line<char>(argc, static_cast<const char *const *>(argv), general),
            vm
        );

        // Emit a help message and exit the process, if requested
        this->emitHelpIfRequested(vm, general, basic, visible, user_options, usage_string);

        po::notify(vm);

        // Record which settings were given explicitly on the command line, so ensureConfigured_() can let a
        // command-line value override a programmatic one (D4). --consumer bound consumer_name_ directly (so
        // getConsumerName() is correct right after construction, before any setConsumerName()); we remember
        // that value to restore it should a later setConsumerName() overwrite the member.
        if(vm.count("consumer") > 0) {
            cli_consumer_name_ = consumer_name_;
        }
        if(vm.contains("optimizationAlgorithms")) {
            cli_optimization_algorithms_ = optimization_algorithms;
            cli_algorithms_explicit_     = true;
        }

        if(vm.contains("client")) {
            client_mode_ = true;
        }

        // Set the name of a checkpoint file (if any)
        cp_file_ = checkpoint_file;

        // Set the maximum running time for the client (if any)
        max_client_duration_ = Gem::Common::duration_from_string(max_client_duration);

        // Retain the parsed command line; ensureConfigured_() builds the consumer from it later.
        cl_vm_ = std::move(vm);
    }
    catch(const po::error &e) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "Error parsing the command line:" << '\n'
            << e.what() << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Emits the help message and exits the process, if --help / --showAll was given.
 *
 * @param vm The parsed program_options variables map (checked for the "help" / "showAll" flags)
 * @param general The full options description (printed for --showAll)
 * @param basic The basic Go2 options (included in the selected --help output)
 * @param visible The visible algorithm- and consumer-options (included in the selected --help output)
 * @param user_options The user-defined command line options (included if non-empty)
 * @param usage_string The usage banner string used as the heading of the selected options listing
 */
void Go2::emitHelpIfRequested(
    boost::program_options::variables_map const &vm,
    boost::program_options::options_description const &general,
    boost::program_options::options_description const &basic,
    boost::program_options::options_description const &visible,
    boost::program_options::options_description const &user_options,
    std::string const &usage_string
) {
    if(not vm.contains("help") && not vm.contains("showAll")) {
        return;
    }
    if(vm.contains("showAll")) { // Show all options
        std::cout << general << '\n';
    }
    else { // Just show a selection
        boost::program_options::options_description selected(usage_string);
        if(user_options.options().empty()) {
            selected.add(basic).add(visible);
        }
        else {
            selected.add(basic).add(user_options).add(visible);
        }
        std::cout << selected << '\n';
    }
    exit(0); // NOLINT(concurrency-mt-unsafe) — --help path; no worker threads have started yet
}

/******************************************************************************/
/**
 * @brief Registers a ready-built custom consumer as the process's single consumer, overriding the
 * mnemonic-based default that setupChosenConsumer() established during construction.
 *
 * setupChosenConsumer() already announced and registered the mnemonic-based default (e.g. "stc"); this
 * replaces it in the GConsumerRegistry. We log the replacement so the earlier "Using consumer <name>"
 * line is not mistaken for the consumer that actually runs the work (e.g. a GPU consumer).
 *
 * @param consumer The ready-to-use consumer to register as the process consumer (ownership is moved in)
 */
void Go2::registerConsumer(
    std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> consumer) {
    consumer_ = consumer;
    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().setConsumer(
        std::move(consumer));
    std::println(
        "Using a custom registered consumer; it replaces the default \"{}\" as the process consumer",
        consumer_name_);
}

/******************************************************************************/
/**
 * @brief Validates, initialises, configures and enrols the consumer chosen on the command line.
 *
 * Operates on the consumer_name_ member; assembles the transport-agnostic consumer spec, builds the
 * courtier consumer (except on an MPI worker rank) and derives client_mode_ for the MPI consumer.
 *
 * @param vm The parsed program_options variables map (used to assemble the consumer spec)
 */
void Go2::setupChosenConsumer(boost::program_options::variables_map const &vm) {
    // The consumer may be chosen on the command line (--consumer) or in the configuration file (the
    // "consumer" key, defaulting to stc); the command-line value takes precedence. Only passing
    // --consumer more than once is ambiguous. consumer_name_ has already been resolved (config parse
    // followed by the optional command-line override) and is validated for known-ness just below.
    if(vm.count("consumer") > 1) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::setupChosenConsumer(): Error!" << '\n'
            << "You may specify at most one consumer on the command line. Found "
            << vm.count("consumer") << "." << '\n'
        );
    }

    // Check that the requested consumer is one the courtier setup layer can build.
    if(not Gem::Geneva::isKnownConsumer(consumer_name_)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::setupChosenConsumer(): Error!" << '\n'
            << "You have requested an unknown consumer \"" << consumer_name_ << "\"." << '\n'
            << "Available consumers are:" << '\n'
            << Gem::Geneva::consumerListing()
        );
    }

    // Client mode requires a consumer with a networked client form (asio/beast/mpi). A consumer that
    // determines the role at runtime (e.g. mpi, from the rank) is exempt from this up-front check: its role
    // is not decided from --client and is not yet known here.
    const bool role_at_runtime = Gem::Geneva::consumerDeterminesRoleAtRuntime(consumer_name_);
    if(client_mode_ && not role_at_runtime && not Gem::Geneva::consumerNeedsClient(consumer_name_)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Go2::setupChosenConsumer(): Error!" << '\n'
            << "Requested client mode even though consumer " << consumer_name_
            << " does not require a client" << '\n'
        );
    }

    std::println("Using consumer {}", consumer_name_);

    // courtier is the submission path. Assemble the transport-agnostic spec from the command line and
    // remember it, so clientRun_() can build the matching networked client without a second pass.
    consumer_spec_ = Gem::Geneva::specFromCommandLine(consumer_name_, vm);

    // Build the courtier consumer through the shared factory -- the single place that knows the concrete
    // consumer types -- which registers the resulting consumer as the process's single consumer
    // (GConsumerRegistry); every algorithm reads it from there. A role-at-runtime consumer builds on EVERY
    // process (it self-assigns submitter vs. worker: submitter -> consumer, worker -> run_worker); the
    // socket and local consumers build a server only when this process is not a client.
    if(role_at_runtime || not client_mode_) {
        auto setup = Gem::Geneva::buildConsumerSetup(consumer_spec_);
        consumer_    = setup.consumer;   // also registered as the process consumer (null in the worker role)
        run_worker_  = std::move(setup.run_worker); // worker role: clientRun_ serves through it

        // A role-at-runtime consumer assigns the role here: a worker process yields a run_worker loop (and a
        // null consumer). Reflect that in client_mode_ so the caller dispatches to clientRun_().
        if(role_at_runtime) {
            client_mode_ = static_cast<bool>(run_worker_);
        }

        if(consumer_) {
            std::println("Routing consumer \"{}\" through courtier", consumer_name_);
        }
    }
}

/******************************************************************************/
/******************************************************************************/
/**
 * @brief Loads every not-yet-loaded requested runtime module and dispatches its contributions.
 *
 * Walks module_paths_ (config + --module + programmatic) and, last, the individual plugin path (config +
 * --individual): each module's optimization algorithms are registered into oaFactoryStore() and any
 * contributed individual claims the single content-creator slot. Idempotent -- a path already loaded (tracked
 * in loaded_module_paths_) is skipped, so this may safely run during command-line parsing (for
 * command-line/config modules) and again from ensureConfigured_() (for programmatically-added modules)
 * without re-loading a module (which would collide on its already-registered OA mnemonics). A genuine
 * collision (a module's OA mnemonic already registered by another module or built-in, or a second individual)
 * throws from the loader / the content-creator guard.
 *
 * --individual states what the module is FOR, so it is checked against what the module turns out to carry:
 * a module named there that contributes no individual is refused by name rather than left to surface much
 * later as "no optimization problem was registered". --module makes no such promise (a module may carry any
 * mix of contributions), so it is not checked -- the loader has already refused a module that contributes
 * nothing at all.
 */
void Go2::loadRequestedModules_() {
    auto loadOne = [this](std::string const &path, bool must_carry_individual) {
        if(path.empty()) { return; }
        if(not loaded_module_paths_.insert(path).second) { return; } // already loaded -> skip
        LoadedModule const loaded = loadModule(path);
        if(must_carry_individual and not loaded.individual) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In Go2::loadRequestedModules_(): Error!" << '\n'
                << "'" << path << "' was given as the optimization problem (--individual), but the module"
                << " carries no individual." << '\n'
                << "It contributes " << loaded.oa_count << " optimization algorithm(s) and "
                << loaded.marshaller_count << " GPU marshaller(s)." << '\n'
                << "A module that contributes algorithms rather than a problem is loaded with --module."
                << '\n'
            );
        }
        if(loaded.individual) {
            this->claimContentCreator_(loaded.individual, individualSource::LOADED);
        }
    };

    for(auto const &path : module_paths_) { loadOne(path, /* must_carry_individual = */ false); }
    loadOne(individual_plugin_path_, /* must_carry_individual = */ true);
}

/******************************************************************************/
/**
 * @brief Extracts --module / --individual paths from the raw command line via a permissive pre-parse.
 *
 * A minimal options description recognising only --module / --individual is parsed with unregistered options
 * allowed, so this runs before the full option surface (which depends on the OAs a module contributes) exists.
 * The extracted --module paths are appended to module_paths_, and a --individual path overrides the
 * individual_plugin_path_ config setting. A malformed command line is ignored here -- the full parse in
 * parseCommandLine() then produces the user-facing diagnostic.
 *
 * @param argc The number of command line arguments
 * @param argv The array of command line argument strings
 */
void Go2::extractEarlyModulePaths_(int argc, char **argv) {
    namespace po = boost::program_options;

    po::options_description early("early module extraction");
    early.add_options()
        ("module,m", po::value<std::vector<std::string>>()->composing(), "")
        ("individual,i", po::value<std::string>(), "");

    po::variables_map early_vm;
    try {
        po::store(
            po::command_line_parser(argc, static_cast<const char *const *>(argv))
                .options(early)
                .allow_unregistered()
                .run(),
            early_vm
        );
        po::notify(early_vm);
    }
    catch(po::error const &) {
        // Ignore: parseCommandLine()'s full parse will report a malformed command line to the user.
        return;
    }

    if(early_vm.contains("module")) {
        for(auto const &p : early_vm["module"].as<std::vector<std::string>>()) {
            module_paths_.push_back(p);
        }
    }
    if(early_vm.contains("individual")) {
        individual_plugin_path_ = early_vm["individual"].as<std::string>(); // --individual overrides config
    }
}

/******************************************************************************/
/**
 * @brief Idempotently finalizes configuration: loads programmatically-added modules, builds the chosen
 * consumer and resolves the algorithm chain.
 *
 * Runs its body exactly once (guarded by configured_), at the first of optimize_() / clientRun() /
 * clientMode(). The constructor only parses the command line / config into members; nothing is built until
 * here, so a programmatic setter (setConsumerName / addModulePath / setAlgorithmChain) called after
 * construction is honoured. Where a setting was also given on the command line, the command-line value wins
 * (D4). This is the uniform lazy resolution for ALL consumers.
 */
void Go2::ensureConfigured_() {
    if(configured_) { return; }
    configured_ = true;

    // Load any modules added programmatically after construction (command-line/config modules were already
    // loaded during parsing); loadRequestedModules_() skips those via loaded_module_paths_.
    this->loadRequestedModules_();

    // Consumer precedence (D4): a --consumer value on the command line wins over a programmatic
    // setConsumerName() (which may have overwritten the member after construction).
    if(cli_consumer_name_) {
        consumer_name_ = *cli_consumer_name_;
    }
    // --update-configs only refreshes configuration files and never optimizes: force the local thread-pool
    // consumer so no networked / GPU / MPI consumer is built (it might try to connect or need a device). This
    // overrides any --consumer the user passed.
    if(update_configs_mode_) {
        consumer_name_ = "stc";
    }

    // Build the chosen consumer from the retained command line (registers it as the process consumer).
    this->setupChosenConsumer(cl_vm_);

    // Algorithm-chain precedence (D4): a --optimizationAlgorithms list on the command line wins over a
    // programmatic setAlgorithmChain(); either resolves against oaFactoryStore() (now including loaded OAs)
    // and appends to the chain. Algorithms added directly via addAlgorithm()/operator& remain in the chain.
    if(cli_algorithms_explicit_) {
        this->resolveAlgorithmChain_(Gem::Common::splitString(cli_optimization_algorithms_, ","));
    }
    else if(not programmatic_algorithm_mnemonics_.empty()) {
        this->resolveAlgorithmChain_(programmatic_algorithm_mnemonics_);
    }
}

/******************************************************************************/
/**
 * @brief Resolves a list of algorithm mnemonics against oaFactoryStore() and appends the produced algorithms.
 *
 * @param mnemonics The ordered algorithm mnemonics to instantiate and append to the chain
 */
void Go2::resolveAlgorithmChain_(std::vector<std::string> const &mnemonics) {
    for(const auto &alg_str : mnemonics) {
        // Retrieve the algorithm provider from the global store
        std::shared_ptr<Gem::Common::GProviderT<GOABase>> p;
        if(not oaFactoryStore()->get(alg_str, p)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In Go2::resolveAlgorithmChain_(): Error!" << '\n'
                << "Got invalid algorithm mnemonic \"" << alg_str << "\"." << '\n'
                << "No algorithm found for this string." << '\n'
            );
        }
        // Retrieve an algorithm from the provider and add it to the list
        algorithms_cnt_.push_back(p->provide());
    }
}

/******************************************************************************/
/**
 * Parses a configuration file for configuration options
 *
 * @param config_filename The name of a configuration file to be parsed
 */
void Go2::parseConfigFile(std::filesystem::path const &config_filename) {
    // Create a parser builder object. It will be destroyed at
    // the end of this scope and thus cannot cause trouble
    // due to registered call-backs and references
    Gem::Common::GParserBuilder gpb;

    // Add local configuration options
    this->addConfigurationOptions(gpb);

    // Do the actual parsing
    if(not gpb.parseConfigFile(config_filename)) {
        glogger << "In Go2::parseConfigFile: Error!" << '\n'
                << "Could not parse configuration file " << config_filename.string() << '\n'
                << LOGEXIT(EXIT_FAILURE);
    }
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */
