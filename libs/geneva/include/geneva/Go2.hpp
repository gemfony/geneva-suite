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
#include "geneva/GNamespaceAliases.hpp"

// Standard header files go here
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GExceptions.hpp"
#include "common/GFactoryT.hpp"
#include "common/GParserBuilder.hpp"
#include "common/GSigHupHandler.hpp"
#include "geneva/GFaultInjector.hpp"
#include "geneva/GOptimizationDescription.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GenevaInitializer.hpp"
#include "geneva/Interface/GOptimizerIT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GFactoryStore.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "hap/GRandomFactory.hpp"
#include "hap/GRandomT.hpp"

namespace Gem::Geneva {

/******************************************************************************/
// Default values for the variables used by the optimizer
constexpr bool GO2_DEF_COPYBESTINDIVIDUALSONLY = true;

/******************************************************************************/
/** Syntactic sugar -- make the code easier to read */
using GOABase = oa::GOptimizationAlgorithmBase;

/******************************************************************************/
/**
 * The optimization driver: it chains a number of optimization algorithms so that a given set of
 * individuals can be optimized using more than one algorithm in sequence. It knows nothing of the
 * command line, of consumers or of the role of a process: courtier's launcher owns those, and
 * every algorithm submits to the one process consumer the launcher established (Invariant 5). A
 * program builds Go2 on the submitter from Geneva's description (runOptimization(), or
 * Go2(const GOptimizationDescription &) inside the launcher's producing function); Go2() serves
 * code that only chains algorithms.
 */
class Go2 // NOLINT(cppcoreguidelines-special-member-functions)
  : public Interface::GOptimizerIT<Go2>
  , public Gem::Common::GPtrContainerT<gen::GGenome> {
public:
    /** @brief A driver that only chains algorithms: the defaults of the configuration file, no
     *  algorithms from the command line, no checkpoint, no loaded individual */
    Go2();
    /**
     * @brief The driver of a program on the launcher: the algorithms of -a (they win over
     * setAlgorithmChain()), the checkpoint of --cp_file, the key copy_best_individuals_only and
     * the individual a module contributed (it claims the one content-creator slot)
     * @param description Geneva's description, after the launcher's parse
     */
    explicit Go2(const GOptimizationDescription &description);
    /** @brief Deleted copy constructor */
    Go2(Go2 const &) = delete;
    /** @brief The (defaulted) destructor */
    ~Go2() override = default;

    /**
     * @brief Specifies whether only the best individuals of a population should be copied to the next algorithm.
     */
    //! [coding-naming-example#2]
    void setCopyBestIndividualsOnly(bool copy_best_individuals_only);
    //! [coding-naming-example#2]
    /**
     * @brief Checks whether only the best individuals are copied.
     */
    //! [coding-naming-example#1]
    bool onlyBestIndividualsAreCopied() const;
    //! [coding-naming-example#1]

    /**
     * @brief Allows to add an optimization algorithm to the chain.
     */
    void addAlgorithm(const std::shared_ptr<GOABase> &alg);
    /**
     * @brief Makes it easier to add algorithms (operator form of addAlgorithm).
     */
    Go2 &operator&(const std::shared_ptr<GOABase> &alg);
    /**
     * @brief Allows to add an optimization algorithm through its mnemonic.
     */
    void addAlgorithm(std::string const &mn);
    /**
     * @brief Makes it easier to add algorithms by mnemonic (operator form of addAlgorithm).
     */
    Go2 &operator&(std::string const &mn);

    /** @brief Sets the chain of optimization algorithms by mnemonic (e.g. {"ea", "sa"}) -- the programmatic
     *  equivalent of --optimizationAlgorithms. The mnemonics are resolved when optimize() runs. A
     *  --optimizationAlgorithms list on the command line (the description's) overrides this. */
    void setAlgorithmChain(std::vector<std::string> const &mnemonics);
    /** @brief @return The programmatic algorithm-chain mnemonics set via setAlgorithmChain() (empty if none). */
    std::vector<std::string> getAlgorithmChain() const;

    /**
     * @brief Retrieves the currently registered number of algorithms.
     */
    std::size_t getNAlgorithms() const;

    /**
     * @brief Allows to register a content creator (a factory producing the individuals to be optimized).
     */
    void
    registerContentCreator(const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> &cc_ptr);

    /**
     * @brief @return The registered content-creator factory (compiled-in or loaded from a plugin), or an
     *  empty pointer if none. A generic optimizer program uses this to pull e.g. the OA-owned adaption config from a
     *  runtime-loaded individual it does not know the concrete type of.
     */
    std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> getContentCreator() const {
        return content_creator_ptr_;
    }

    /***************************************************************************/
    /**
     * @brief Allows to register a default algorithm via a shared pointer.
     */
    void registerDefaultAlgorithm(const std::shared_ptr<GOABase> &default_algorithm);
    /**
     * @brief Allows to register a default algorithm via its mnemonic.
     */
    void registerDefaultAlgorithm(std::string const &mn);

    /**
     * @brief Allows to register a pluggable optimization monitor.
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
     */
    bool hasOptimizationMonitors() const;

    /**
     * @brief Retrieves the algorithms that were registered with this class.
     */
    std::vector<std::shared_ptr<GOABase>> getRegisteredAlgorithms();

    /**
     * @brief Registers an OA-owned adaption configuration for an algorithm type. When an
     * algorithm of the given personality type (e.g. "PERSONALITY_EA") runs in the chain, Go2 hands it this
     * config, which it adopts (after validating it matches the population's genome) instead of deriving a
     * default from the genome layout. This separates adaption authoring (OA-owned, optionally from a config
     * file) from genome authoring (the individual's factory). Passing a null config clears the entry.
     */
    void registerAdaptionConfig(
        const std::string &oa_personality_type,
        std::shared_ptr<oa::GAdaptionConfigBase> config
    );

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
     *  registerContentCreator() (COMPILED_IN) and the plugin load (LOADED). */
    void claimContentCreator_(
        const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> &cc_ptr,
        individualSource source
    );

    // GOptimizerIT NVI hooks: keep these overrides private (do not
    // widen access -- matches oa::GOptimizationAlgorithmBase and the base's NVI contract).
    /**
     * @brief Retrieves the best individual found globally across the whole run.
     */
    std::unique_ptr<gen::GGenome> getBestGlobalIndividual_() const final;
    /**
     * @brief Retrieves a list of the globally best individuals found.
     */
    std::vector<std::unique_ptr<gen::GGenome>> getBestGlobalIndividuals_() const final;
    /**
     * @brief Retrieves the best individual found in the current iteration.
     */
    std::unique_ptr<gen::GGenome> getBestIterationIndividual_() const final;
    /**
     * @brief Retrieves a list of the best individuals found in the current iteration.
     */
    std::vector<std::unique_ptr<gen::GGenome>> getBestIterationIndividuals_() const final;

    /**
     * @brief Returns one-word information about the type of optimization algorithm.
     */
    std::string getAlgorithmPersonalityType_() const final;
    /**
     * @brief Returns the name of this optimization algorithm.
     */
    std::string getAlgorithmName_() const final;

    /** @brief Satisfies a requirement of GOptimizerIT */
    void evaluatePopulation_() final;

    /**
     * @brief Retrieval of the current iteration.
     */
    uint32_t getIteration_() const final;

    /**
     * @brief Performs the actual optimization cycle.
     */
    Go2 const *optimize_(std::uint32_t offset) final;

    /** @brief --update-configs pass: with GParserBuilder in update-in-place mode, produce one object from
     *  each registered algorithm factory and one individual from the content creator, so every algorithm
     *  config and the individual config is parsed and rewritten in canonical form (Go2.json and the
     *  consumers' files were already refreshed by the launcher), then end the process. Runs no
     *  optimization. */
    void refreshAllConfigs_();

    // --- optimize_ sub-steps (decomposition) ---
    /** @brief Adds the Geneva default algorithm if none have been registered */
    void ensureAlgorithmPresent();
    /**
     * @brief Loads a checkpoint or fills the population from the content creator.
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
     */
    void runAlgorithmChain(std::uint32_t first_algorithm_offset);
    /** @brief Sorts the collected individuals by their (min-only transformed) fitness */
    void sortIndividualsByFitness();

    /**
     * @brief Resolves a list of algorithm mnemonics against oaFactoryStore() and appends the produced
     * algorithms to the chain. An unknown mnemonic throws.
     */
    void resolveAlgorithmChain_(std::vector<std::string> const &mnemonics);
    /***************************************************************************/
    // Initialization code for the Geneva library
    GenevaInitializer gi_;

    //---------------------------------------------------------------------------
    // Internal parameters
    bool sorted_ = false; ///< Indicates whether local individuals have been sorted
    std::uint32_t total_iterations_ =
        0; ///< Continuous iteration count accumulated across the algorithm chain (for getIteration_ reporting only)
    /// Whether only the best individuals of an algorithm are copied to the next one
    //! [coding-naming-example#3]
    bool copy_best_individuals_only_ = GO2_DEF_COPYBESTINDIVIDUALSONLY;
    //! [coding-naming-example#3]
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
    // A user-defined means for information retrieval. Shared ownership is deliberate: on
    // hand-over, every algorithm of a chain registers the SAME monitor object rather than a
    // clone of it, so a monitor accumulates across the whole chain (which is what a progress
    // plotter wants). Go2 and each algorithm are therefore co-owners for the run's duration.
    std::vector<std::shared_ptr<oa::GBasePluggableOM>> pluggable_monitors_cnt_;

    //---------------------------------------------------------------------------
    /** @brief Whether the algorithm chain of -a or setAlgorithmChain() was resolved (at the first
     *  optimize()) */
    bool chain_resolved_ = false;
    /** @brief The -a list of the description; it wins over setAlgorithmChain() */
    std::vector<std::string> cl_algorithm_mnemonics_;
    /** @brief The algorithm-chain mnemonics set programmatically via setAlgorithmChain() */
    std::vector<std::string> programmatic_algorithm_mnemonics_;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */
