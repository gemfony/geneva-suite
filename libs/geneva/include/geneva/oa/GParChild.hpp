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

// Standard headers go here
#include <concepts>
#include <cstdint>
#include <memory>
#include <random>
#include <string_view>
#include <tuple>
#include <type_traits>

// Boost headers go here

// Geneva headers go here
#include "common/GChecks.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "common/GReflectiveInterfaceT.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GBaseParChildPersonalityTraits.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"

namespace Gem::Geneva::Genome {
class GGenome;
} // namespace Gem::Geneva::Genome

namespace Gem::Geneva::OptimizationAlgorithms {

// The OA-owned adaption configuration, built from the population's shared genome layout.
class GAdaptionConfigBase;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The GParChild class adds the notion of parents and children to
 * the GOptimizationAlgorithmBase class. The evolutionary adaptation is realized
 * through the cycle of adaption, evaluation, and sorting, as defined in this
 * class.
 *
 * It forms the base class for either multi populations (i.e. evolutionary algorithms
 * that may act on other optimization algorithms (including themselves), or a hierarchy of
 * algorithms acting on parameter objects.
 *
 * Populations are collections of individuals, which themselves are objects
 * exhibiting at least the GGenome class' API, most notably the GGenome::evaluate()
 * function.
 *
 * In order to add parents to an instance of this class use the default constructor,
 * then add at least one GGenome-derivative to it, and call setPopulationSizes().
 * The population will then be "filled up" with missing individuals as required, before the
 * optimization starts.
 */
class GParChild // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Common::GReflectiveInterfaceBaseT<GParChild, GOptimizationAlgorithmBase> {
    /////////////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class'es local data members. Drives the
     *  GReflectiveInterfaceBaseT-generated serialize()/load_()/compare_() from one place. */
    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            Gem::Common::make_member("n_parents_", self.n_parents_),
            Gem::Common::make_member("recombination_method_", self.recombination_method_),
            Gem::Common::make_member("default_n_children_", self.default_n_children_),
            Gem::Common::make_member("growth_rate_", self.growth_rate_),
            Gem::Common::make_member("max_population_size_", self.max_population_size_),
            Gem::Common::make_member("amalgamation_likelihood_", self.amalgamation_likelihood_)
        );
    }
    /////////////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_()/compare token. */
    static constexpr std::string_view class_name = "GParChild";

    /***************************************************************************/
    /** @brief The default constructor */
    GParChild();
    /** @brief A standard copy constructor
     *  @param cp Another GParChild object to be copied */
    GParChild(const GParChild &cp) = default;
    /** @brief The standard destructor */
    ~GParChild() override = default;

    /** @brief Specifies the default size of the population plus the number of parents */
    void setPopulationSizes(std::size_t pop_size, std::size_t n_parents);

    /** @brief Retrieve the number of parents as set by the user */
    std::size_t getNParents() const;
    /** @brief Calculates the current number of children from the number of parents and the size of the vector. */
    std::size_t getNChildren() const;
    /** @brief Retrieves the default_n_children_ parameter */
    std::size_t getDefaultNChildren() const;

    /** @brief Lets the user set the desired recombination method */
    void setRecombinationMethod(duplicationScheme recombination_method);

    /** @brief Retrieves the value of the recombination_method_ variable */
    duplicationScheme getRecombinationMethod() const;

    /** @brief Adds the option to increase the population by a given amount per iteration */
    void setPopulationGrowth(std::size_t growth_rate, std::size_t max_population_size);
    /** @brief Allows to retrieve the growth rate of the population */
    std::size_t getGrowthRate() const;
    /** @brief Allows to retrieve the maximum population size when growth is enabled */
    std::size_t getMaxPopulationSize() const;

    /** @brief Allows to set the likelihood for amalgamation of two units to be performed instead of "just" duplication. */
    void setAmalgamationLikelihood(double amalgamation_likelihood);
    /** @brief Allows to retrieve the likelihood for amalgamation of two units to be performed instead of "just" duplication. */
    double getAmalgamationLikelihood() const;

    /***************************************************************************/
    /**
     * Retrieves a specific parent individual and casts it to the desired type. The C++20
     * `requires std::derived_from` constraint below makes this overload visible to the compiler only when
     * parent_type is a derivative of GGenome.
     *
     * @tparam parent_type The concrete individual type to cast the parent to (must derive from GGenome)
     * @param parent_id The id of the parent that should be returned
     * @return A converted shared_ptr to the parent
     */
    template <typename parent_type>
        requires std::derived_from<parent_type, gen::GGenome>
    std::shared_ptr<parent_type> getParentIndividual(std::size_t parent_id) {
        Gem::Common::checkThat(
            parent_id < this->getNParents(),
            "no parent ",
            parent_id,
            " among ",
            this->getNParents()
        );

        // Does error checks on the conversion internally
        return Gem::Common::g_ptr_conversion<gen::GGenome, parent_type>(
            *(this->begin() + parent_id)
        );
    }

    /** @brief Hands this mu/lambda algorithm an OA-owned adaption config to use. When
     *  set, init() adopts it (after validating it matches the population's genome) instead of deriving a
     *  default from the genome layout. Transient run scratch -- NOT serialized. */
    void setAdaptionConfig(std::shared_ptr<GAdaptionConfigBase> config) override;

    /** @brief The OA-owned adaption config built at init() (or the externally-provided one), for telemetry.
     *  @return A shared_ptr to the adaption configuration currently in effect */
    std::shared_ptr<const GAdaptionConfigBase> getAdaptionConfig() const override {
        return adaption_config_;
    }

protected:
    /***************************************************************************/
    // Virtual or overridden protected functions

    /** @brief Adds local configuration options to a GParserBuilder object */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceBaseT base.

    /** @brief Retrieves the evaluation range in a given iteration and sorting scheme. Protected so
     *  the derived algorithms' evaluatePopulation_() can call it.
     *  @return A tuple holding the [start, end) index range of individuals to be evaluated */
    virtual std::tuple<std::size_t, std::size_t> getEvaluationRange_() const {
        // Default shared by the parent/child algorithms (EA / SA): evaluate everything in the
        // first iteration (so pluggable optimization monitors need not distinguish between
        // algorithms), and only the children afterwards.
        return std::tuple<std::size_t, std::size_t>{
            this->inFirstIteration() ? 0 : this->getNParents(),
            this->size()
        };
    }

private:
    /** @brief The function checks that the population size meets the requirements and resizes the population to the appropriate size, if required. */
    void adjustPopulation_() override;

protected:
    /** @brief Resets the settings of this population to what was configured when the optimize()-call was issued */
    void resetToOptimizationStart_() override;

    /** @brief performs initialization work before the optimization loop starts */
    void init() override;
    /** @brief Does any necessary finalization work atfer the optimization loop has ended */
    void finalize() override;

    /***************************************************************************/

    /** @brief This function is called from GOptimizationAlgorithmBase::optimize() and performs the actual recombination */
    virtual void recombine();

    /** @brief Retrieves the adaption range in a given iteration and sorting scheme. */
    std::tuple<std::size_t, std::size_t> getAdaptionRange() const;

    /** @brief This helper function marks parents as parents and children as children. */
    void markParents();
    /** @brief This helper function marks children as children */
    void markChildren();

    /** @brief Reconciles the population after a job submission: admits late (asynchronous) returns,
     *  re-orders parents to the front, refills to the nominal size with clones, and re-marks
     *  parents/children. Shared by all mu/lambda algorithms (EA, SA); the per-algorithm selection
     *  scheme runs afterwards in selectBest_(). */
    void fixAfterJobSubmission();

    /** @brief EA/SA reuse late returns: an asynchronously-returned child is admitted as an extra
     *  candidate and kept only if the subsequent selection finds it competitive.
     *  @return true (mu/lambda algorithms reap late returns) */
    bool reapsLateReturns() const override {
        return true;
    }
    /** @brief EA/SA admit a late return only from the current or immediately-preceding iteration: a child
     *  evaluated in iteration N typically returns during N+1, so a one-generation window catches exactly
     *  those late returns and drops staler ones.
     *  @return 1 (a one-generation age window) */
    std::uint32_t lateReturnMaxAge() const override {
        return 1;
    }

    /** @brief Increases the population size if requested by the user */
    void performScheduledPopulationGrowth();

    /** @brief This function implements the RANDOMDUPLICATIONSCHEME scheme */
    void randomRecombine(const std::unique_ptr<gen::GGenome> &child);
    /** @brief This function implements the VALUEDUPLICATIONSCHEME scheme */
    void valueRecombine(
        const std::unique_ptr<gen::GGenome> &child,
        const std::vector<double> &threshold
    );

    /***************************************************************************/

    std::size_t n_parents_ = DEFPARCHILDNPARENTS; ///< The number of parents
    duplicationScheme recombination_method_ =
        duplicationScheme::DEFAULTDUPLICATIONSCHEME;        ///< The chosen recombination method
    std::size_t default_n_children_ = DEFPARCHILDNCHILDREN; ///< Expected number of children
    std::size_t growth_rate_ = 0; ///< Specifies the amount of individuals added per iteration
    std::size_t max_population_size_ =
        0; ///< Specifies the maximum amount of individuals in the population if growth is enabled
    double amalgamation_likelihood_ =
        DEFAULTAMALGAMATIONLIKELIHOOD; ///< Likelihood for children to be created by cross-over rather than "just" duplication (note that they may nevertheless be mutated)

    /***************************************************************************/
    /**
     * @brief transient: the OA-owned adaption configuration in effect for this run: the
     * externally-provided config (validated and adopted at init()). Run scratch: NOT serialized,
     * NOT compared, NOT part of localMembers(); a clone re-adopts it at its own init(). Read-only
     * during the parallel adaptChildren_, so it is shared safely.
     */
    std::shared_ptr<GAdaptionConfigBase> adaption_config_;

    /**
     * @brief transient: an optionally externally-supplied adaption configuration (set via
     * setAdaptionConfig() -- Go2 installs the one it holds for this algorithm's type). When present, init()
     * uses it (validating it against the population's genome) in place of the layout-derived default.
     * Transient run scratch: NOT serialized, NOT compared, NOT part of localMembers().
     */
    std::shared_ptr<GAdaptionConfigBase> provided_adaption_config_;

private:
    /***************************************************************************/
    // Virtual or overridden private functions

    /** @brief Emits a name for this class / object */
    std::string name_() const override;
    /** @brief Creates a deep clone of this object (pure virtual here; implemented by concrete algorithms)
     *  @return A pointer to a freshly allocated deep copy of this object */
    GOptimizationAlgorithmBase *clone_() const override = 0;

    /** @brief This function implements the logic that constitutes evolutionary algorithms */
    std::tuple<double, double> cycleLogic_() override;
    /** @brief Calculates the fitness of all required individuals; to be re-implemented in derived classes */
    void evaluatePopulation_() override = 0;

    /** @brief Returns the name of this optimization algorithm
     *  @return The human-readable name of the algorithm */
    std::string getAlgorithmName_() const override = 0;
    /** @brief Returns information about the type of optimization algorithm
     *  @return A string describing the algorithm's personality type */
    std::string getAlgorithmPersonalityType_() const override = 0;

    /** @brief Retrieve the number of processible items in the current iteration. */
    std::size_t getNProcessableItems_() const override;

    /** @brief Gives individuals an opportunity to update their internal structures */
    void actOnStalls_() override;

    /** @brief Adapts all children of this population in parallel, driven by the OA-owned adaption
     *  config. Uniform across all mu/lambda algorithms, so it is concrete here (was duplicated
     *  verbatim in EA and SA). */
    virtual void adaptChildren_();
    /** @brief Choose new parents, based on the selection scheme set by the user */
    virtual void selectBest_() = 0;

    /** @brief Some error checks related to population sizes (implemented per algorithm, e.g.
     *  GEvolutionaryAlgorithm / GSimulatedAnnealing; invoked from GParChild::init). */
    virtual void populationSanityChecks_() const = 0;

    /***************************************************************************/

    /** @brief This function assigns a new value to each child individual */
    void doRecombine();
    /** @brief doRecombine() helper: builds the value-duplication weight (threshold) vector. */
    [[nodiscard]] std::vector<double> buildRecombinationThresholds() const;
    /** @brief doRecombine() fast path: sequential parent selection + parallel deep-copy (no cross-over). */
    void recombineParallel(const std::vector<double> &threshold);
    /** @brief recombineParallel() phase 1: pick each child's parent slot sequentially (on the orchestration thread). */
    [[nodiscard]] std::vector<std::size_t> selectParentsForChildren(
        std::size_t n_children,
        bool value_scheme,
        const std::vector<double> &threshold
    );
    /** @brief selectParentsForChildren() per-child draw: the parent-slot index for one child. */
    [[nodiscard]] std::size_t
    selectParentForChild(bool value_scheme, const std::vector<double> &threshold);
    /** @brief recombineParallel() phase 2: deep-copy each selected parent into its child slot in parallel. */
    void copyParentsIntoChildrenParallel(const std::vector<std::size_t> &parent_pos);
    /** @brief doRecombine() serial path: per-child cross-over / duplication (original behaviour). */
    void recombineSerial(const std::vector<double> &threshold);
    /** @brief recombineSerial() body for a single child slot: cross-over (amalgamation) or duplication. */
    void recombineOneSerial(
        const std::unique_ptr<gen::GGenome> &child,
        const std::vector<double> &threshold,
        std::bernoulli_distribution &amalgamation_wanted
    );
    /** @brief recombineOneSerial() duplication path: apply the recombination scheme to one child slot. */
    void duplicateForChild(
        const std::unique_ptr<gen::GGenome> &child,
        const std::vector<double> &threshold
    );

    /***************************************************************************/
    // Data

    /// transient: access to uniformly distributed random numbers, stateless between draws
    std::uniform_int_distribution<std::size_t> uniform_int_distribution_;

    /***************************************************************************/
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/******************************************************************************/
