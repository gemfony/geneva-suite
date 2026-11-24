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
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "common/GProviderT.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GFactoryStore.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

namespace Gem::Geneva {

/******************************************************************************/
/**
 * A driver that only chains algorithms.
 */
Go2::Go2() = default;

/******************************************************************************/
/**
 * The driver of a program on the launcher, built from Geneva's description after the parse.
 */
Go2::Go2(const GOptimizationDescription &description)
  : copy_best_individuals_only_(description.copyBestIndividualsOnly())
  , cp_file_(description.checkpointFile())
  , cl_algorithm_mnemonics_(description.algorithms()) {
    if(auto individual = description.loadedIndividual()) {
        this->claimContentCreator_(individual, individualSource::LOADED);
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
        throw gem_exception(
            g_error_streamer() << "In Go2::registerDefaultAlgorithm(std::string): Error!" << '\n'
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
        throw gem_exception(
            g_error_streamer() << "In Go2::registerDefaultAlgorithm(): Error!" << '\n'
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
        throw gem_exception(
            g_error_streamer()
            << "In Go2::registerPluggableOM(): Tried to register empty pluggable optimization "
               "monitor\n"
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
        throw gem_exception(
            g_error_streamer() << "In Go2::addAlgorithm(): Error!" << '\n'
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

/******************************************************************************/
/**
 * @brief Sets the algorithm chain by mnemonic (programmatic equivalent of --optimizationAlgorithms).
 *
 * The mnemonics are resolved when optimize() runs. A --optimizationAlgorithms list on the command line
 * (the description's) overrides this.
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
 * @return A reference to this object, allowing call chaining
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
        throw gem_exception(
            g_error_streamer() << "In Go2::addAlgorithm(std::string): Error!" << '\n'
                               << "Got invalid algorithm mnemonic " << mn << '\n'
        );
    }

    this->addAlgorithm(p->provide()); // The provider's factory might add a monitor to the object
}

/***************************************************************************/
/**
 * @brief Makes it easier to add algorithms through their mnemonics.
 *
 * @param mn A small mnemonic identifying the optimization algorithm in the global factory store (e.g. "ea")
 * @return A reference to this object, allowing call chaining
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
void Go2::registerContentCreator(
    const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> &cc_ptr
) {
    // A user-compiled-in individual: claim the single slot, so a later plugin load (or a second
    // registration) is refused by the one-individual-per-process guard.
    this->claimContentCreator_(cc_ptr, individualSource::COMPILED_IN);
}

/******************************************************************************/
/**
 * Claims the single content-creator slot, enforcing that exactly one optimization problem (individual)
 * exists per process: whichever source registers first wins, and any second claim -- a compiled-in
 * registration when a plugin was already loaded, or a second plugin -- throws, naming both sources.
 *
 * @param cc_ptr The content-creator factory to install (must not be empty)
 * @param source Where this content creator came from (for the diagnostic on a double claim)
 */
void Go2::claimContentCreator_(
    const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> &cc_ptr,
    individualSource source
) {
    auto sourceStr = [](individualSource s) -> const char * {
        switch(s) {
        case individualSource::COMPILED_IN:
            return "compiled in (registerContentCreator)";
        case individualSource::LOADED:
            return "loaded from a plugin (--individual)";
        default:
            return "none";
        }
    };

    if(not cc_ptr) {
        throw gem_exception(
            g_error_streamer() << "In Go2::claimContentCreator_(): Error!" << '\n'
                               << "Tried to register an empty content creator (source: "
                               << sourceStr(source) << ")" << '\n'
        );
    }

    if(content_creator_source_ != individualSource::NONE) {
        throw gem_exception(
            g_error_streamer()
            << "In Go2::claimContentCreator_(): Error!" << '\n'
            << "An optimization problem (individual) is already provided ("
            << sourceStr(content_creator_source_) << ");" << '\n'
            << "refusing to add another (" << sourceStr(source) << ")." << '\n'
            << "Exactly one individual may exist per process -- compile one in OR load one, not "
               "both, and"
            << '\n'
            << "never two. (If you compiled an individual in, do not also pass --individual.)"
            << '\n'
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
    // The algorithm chain of -a (it wins) or of setAlgorithmChain(), once
    if(not chain_resolved_) {
        chain_resolved_ = true;
        this->resolveAlgorithmChain_(
            cl_algorithm_mnemonics_.empty() ? programmatic_algorithm_mnemonics_
                                            : cl_algorithm_mnemonics_
        );
    }

    // --update-configs: refresh every config this binary owns and end without optimizing.
    if(Gem::Common::GParserBuilder::updateInPlace()) {
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
 * With GParserBuilder in update-in-place mode (begun by the launcher), Go2.json and the consumers'
 * files were already rewritten by the launcher. Here we additionally produce one object from each
 * registered algorithm factory -- which parses (and thus rewrites) that algorithm's config file --
 * and one individual from the content creator, refreshing the individual's config. No optimization
 * is run. Per-config failures are warned about rather than aborting, so one missing/odd config does
 * not stop the rest from being refreshed.
 */
void Go2::refreshAllConfigs_() {
    // Every registered optimization-algorithm factory: producing one algorithm parses its config file.
    for(auto const &provider : oaFactoryStore()->getContentSnapshot()) {
        try {
            (void)provider->provide();
        }
        catch(std::exception const &e) {
            glogger << "In Go2::refreshAllConfigs_(): could not refresh the \""
                    << provider->getMnemonic() << "\" algorithm config: " << e.what() << '\n'
                    << GWARNING;
        }
    }

    // The individual (content-creator) config, if an individual is available.
    if(content_creator_ptr_) {
        try {
            (void)(*content_creator_ptr_)();
        }
        catch(std::exception const &e) {
            glogger << "In Go2::refreshAllConfigs_(): could not refresh the individual config: "
                    << e.what() << '\n'
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
 * A checkpoint brings the whole population: the individuals registered with this object are then
 * dropped, as they belong to the start of a new run.
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
        // The checkpoint holds the whole population of the run: the individuals registered with this
        // object (push_back) start a new run only, as the content creator does. Kept, they would be
        // added to the restored population (runAlgorithmChain()), beyond its size.
        this->clear();
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
        throw gem_exception(
            g_error_streamer() << "In Go2::optimize(): Error!" << '\n'
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
        throw gem_exception(
            g_error_streamer()
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
                throw gem_exception(
                    g_error_streamer()
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
 * start at 0 (full iteration budget). Each algorithm is told how many iterations of the chain ran
 * before it (GOptimizationAlgorithmBase::setChainOffset()), so that it can count its iterations
 * through the chain.
 *
 * @param first_algorithm_offset The iteration offset at which the first algorithm in the chain should start
 */
void Go2::runAlgorithmChain(std::uint32_t first_algorithm_offset) {
    total_iterations_ = 0;
    sorted_ = false;
    for(auto const &[alg_index, alg_ptr] : algorithms_cnt_ | std::views::enumerate) {
        // Every algorithm submits through the one process consumer (registered in GConsumerRegistry
        // by the launcher). A standalone algorithm with none set builds the default consumer on
        // first use.

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

        // The iterations of the chain before this algorithm. The first algorithm of a run resumed
        // from a checkpoint keeps the offset its checkpoint holds: that says where in the earlier
        // chain it stood. Go2 knows that case by resumedFromCheckpoint(), which loadCheckpoint()
        // set and which only the algorithm's optimize() clears.
        if(alg_index == 0 and alg_ptr->resumedFromCheckpoint()) {
            total_iterations_ = alg_ptr->getChainOffset();
        }
        else {
            alg_ptr->setChainOffset(total_iterations_);
        }

        // Do the actual optimization (see first_algorithm_offset above)
        if(alg_index == 0) {
            alg_ptr->optimize(first_algorithm_offset);
        }
        else {
            alg_ptr->optimize();
        }

        // Count on through the chain: the next algorithm's chain offset, and getIteration_(). An
        // algorithm ends one past its last iteration, so this is the number of chain iterations
        // so far. It is not the next algorithm's start iteration, which is 0.
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
    std::ranges::sort(this->begin(), this->end(), [](const auto &x_ptr, const auto &y_ptr) -> bool {
        return minOnly_transformed_fitness(*x_ptr) < minOnly_transformed_fitness(*y_ptr);
    });

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
        throw gem_exception(
            g_error_streamer() << "In Go2::getBestGlobalIndividual_(): Error!" << '\n'
                               << "No individuals found" << '\n'
        );
    }

    if(not sorted_) {
        throw gem_exception(
            g_error_streamer() << "In Go2::getBestGlobalIndividual_(): Error!" << '\n'
                               << "Tried to retrieve best individual" << '\n'
                               << "from an unsorted population." << '\n'
        );
    }

    // Check if the best individual is processed
    if(not this->front()->is_processed() && not this->front()->is_unprocessed()) {
        throw gem_exception(
            g_error_streamer() << "In Go2::getBestGlobalIndividual_(): Error!" << '\n'
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
        throw gem_exception(
            g_error_streamer() << "In Go2::getBestGlobalIndividuals_(): Error!" << '\n'
                               << "No individuals found" << '\n'
        );
    }

    std::vector<std::unique_ptr<gen::GGenome>> best_individuals;
    for(auto const &[pos, ind_ptr] : *this | std::views::enumerate) {
        if(ind_ptr->is_due_for_processing() || ind_ptr->has_errors()) {
            throw gem_exception(
                g_error_streamer() << "In Go2::getBestGlobalIndividuals_(): Error!" << '\n'
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
 * @return Never returns normally; always throws a gem_exception
 */
std::unique_ptr<gen::GGenome> Go2::getBestIterationIndividual_() const {
    throw gem_exception(
        g_error_streamer() << "In Go2::getBestIterationIndividual_(): Error!" << '\n'
                           << "This function should not be called" << '\n'
    );
}

/******************************************************************************/
/**
 * @brief Retrieves a list of the best individuals of the current iteration.
 *
 * This function is not meaningful for the algorithm-combiner Go2 and always throws when called.
 *
 * @return Never returns normally; always throws a gem_exception
 */
std::vector<std::unique_ptr<gen::GGenome>> Go2::getBestIterationIndividuals_() const {
    throw gem_exception(
        g_error_streamer() << "In Go2::getBestIterationIndividuals_(): Error!" << '\n'
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
 * @brief Retrieval of the current iteration.
 *
 * @return The iterations of the chain so far (in a resumed run including those before its checkpoint)
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
/**
 * @brief Returns one-word information about the type of optimization algorithm.
 *
 * @return A short string identifying the algorithm personality type
 */
std::string Go2::getAlgorithmPersonalityType_() const {
    return {"PERSONALITY_NONE"};
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
            throw gem_exception(
                g_error_streamer()
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
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */
