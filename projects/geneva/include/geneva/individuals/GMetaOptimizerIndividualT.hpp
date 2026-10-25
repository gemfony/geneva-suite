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
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GFactoryT.hpp"
#include "common/GParserBuilder.hpp"
#include "dietrich/GPlotDesigner.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GOptimizationAlgorithmFactoryT.hpp"
#include "geneva/oa/GTunableManifest.hpp"
#include "common/GSelfTestable.hpp"

namespace Gem::Geneva::Individuals {

/******************************************************************************/
// Different types of optimization targets
enum class metaOptimizationTarget : Gem::Common::ENUMBASETYPE {
    BESTFITNESS = 0,
    MINSOLVERCALLS = 1,
    MC_MINSOLVER_BESTFITNESS =
        2 // Multi-criterion optimization with least number of solver calls and best average fitness as targets
};

// Numeric streaming opt-in for metaOptimizationTarget; must precede its first
// streaming use (see numeric_enum_io_v in GCommonEnums.hpp).
} /* namespace Gem::Geneva::Individuals */
namespace Gem::Common {
template <> inline constexpr bool numeric_enum_io_v<Gem::Geneva::Individuals::metaOptimizationTarget> = true;
} /* namespace Gem::Common */
namespace Gem::Geneva::Individuals {

/******************************************************************************/
// metaOptimizationTarget streams as its underlying numeric value through the
// shared machinery in GCommonEnums.hpp (marker specialization at the end of
// this header). Re-export the operators so ADL finds them in this namespace.
using Gem::Common::operator<<;
using Gem::Common::operator>>;

/******************************************************************************/
// A number of default settings for the factory and individual

// Pertaining to the population
constexpr std::size_t GMETAOPT_DEF_INITNPARENTS = 1; ///< The initial number of parents
constexpr std::size_t GMETAOPT_DEF_NPARENTS_LB =
    1; ///< The lower boundary for variations of the number of parents
constexpr std::size_t GMETAOPT_DEF_NPARENTS_UB =
    6; ///< The upper boundary for variations of the number of parents

constexpr std::size_t GMETAOPT_DEF_INITNCHILDREN = 100; ///< The initial number of children
constexpr std::size_t GMETAOPT_DEF_NCHILDREN_LB =
    5; ///< The lower boundary for the variation of the number of children
constexpr std::size_t GMETAOPT_DEF_NCHILDREN_UB =
    250; ///< The upper boundary for the variation of the number of children

constexpr double GMETAOPT_DEF_INITAMALGLKLHOOD =
    0.; ///< The initial likelihood for an individual being created from cross-over rather than "just" duplication
constexpr double GMETAOPT_DEF_AMALGLKLHOOD_LB =
    0.; ///< The lower boundary for the variation of the amalgamation likelihood
constexpr double GMETAOPT_DEF_AMALGLKLHOOD_UB =
    1.; ///< The upper boundary for the variation of the amalgamation likelihood

// Concerning the individual
constexpr double GMETAOPT_DEF_INITMINADPROB =
    0.; ///< The initial lower boundary for the variation of ad_prob
constexpr double GMETAOPT_DEF_MINADPROB_LB = 0.; ///< The lower boundary for min_ad_prob
constexpr double GMETAOPT_DEF_MINADPROB_UB =
    0.1; ///< The upper boundary for min_ad_prob -- 0.1, effectively

constexpr double GMETAOPT_DEF_INITADPROBRANGE =
    0.9; ///< The initial upper boundary for the variation of ad_prob
constexpr double GMETAOPT_DEF_ADPROBRANGE_LB = 0.1; ///< The lower boundary for ad_prob_range
constexpr double GMETAOPT_DEF_ADPROBRANGE_UB = 0.9; ///< The upper boundary for ad_prob_range

constexpr double GMETAOPT_DEF_INITADPROBSTARTPERCENTAGE =
    1.; ///< Defines the place inside of the allowed value range where ad_prob starts. Boundaries are 0./1.

constexpr double GMETAOPT_DEF_INITADAPTADPROB =
    0.1; ///< The initial value of the strength of adProb_ adaption
constexpr double GMETAOPT_DEF_ADAPTADPROB_LB =
    0.; ///< The lower boundary for the variation of the strength of adProb_ adaption
constexpr double GMETAOPT_DEF_ADAPTADPROB_UB =
    1.; ///< The upper boundary for the variation of the strength of adProb_ adaption

constexpr double GMETAOPT_DEF_INITMINSIGMA = 0.001; ///< The initial lower boundary for sigma
constexpr double GMETAOPT_DEF_MINSIGMA_LB =
    0.001; ///< The lower boundary for the variation of the lower boundary of sigma
constexpr double GMETAOPT_DEF_MINSIGMA_UB =
    0.09999; ///< The upper boundary for the variation of the lower boundary of sigma, means ~0.1

constexpr double GMETAOPT_DEF_INITSIGMARANGE =
    0.2; ///< The initial maximum range for sigma --> note that the initial start value for sigma will always be set to the upper boundary of its variation limits
constexpr double GMETAOPT_DEF_SIGMARANGE_LB =
    0.1; ///< The lower boundary for the variation of the maximum range of sigma --> max_sigma is 0.2
constexpr double GMETAOPT_DEF_SIGMARANGE_UB =
    0.9; ///< The upper boundary for the variation of the maximum range of sigma --> max_sigma is 1.

constexpr double GMETAOPT_DEF_INITSIGMARANGEPERCENTAGE =
    1.; ///< The initial percentage of the sigma range as a start value

constexpr double GMETAOPT_DEF_INITSIGMASIGMA = 0.1; ///< The initial strength of sigma adaption
constexpr double GMETAOPT_DEF_SIGMASIGMA_LB =
    0.; ///< The lower boundary for the variation of the strength of sigma adaption
constexpr double GMETAOPT_DEF_SIGMASIGMA_UB =
    1.; ///< The upper boundary for the variation of the strength of sigma adaption

// General meta-optimization parameters
constexpr std::size_t GMETAOPT_DEF_NRUNSPEROPT = 10; ///< The number of successive optimization runs
constexpr double GMETAOPT_DEF_FITNESSTARGET = 0.001; ///< The fitness target
constexpr std::uint32_t GMETAOPT_DEF_ITERATIONTHRESHOLD =
    10000; ///< The maximum allowed number of iterations
const metaOptimizationTarget GMETAOPT_DEF_MOTARGET =
    metaOptimizationTarget::BESTFITNESS; ///< The target used for the meta optimization

const std::string GMETAOPT_DEF_INDCONFIG =
    "./config/GFunctionIndividual.json"; ///< The default configuration file for our individuals -- we follow the default template argument
const std::string GMETAOPT_DEF_SUBEACONFIG =
    "./config/GSubEvolutionaryAlgorithm.json"; ///< The default configuration file for the (sub-)evolutionary algorithms


/******************************************************************************/
/**
 * This individual performs "meta optimization" in the sense that parameters of
 * an optimization algorithm are optimized alongside a given "sub-"individual.
 * The individual is meant for tuning the parameters of Evolutionary Algorithms,
 * but may be used to find better optima as well.
 *
 * @tparam ind_type The type of sub-individual whose optimization is being tuned
 */
template <typename ind_type = Gem::Geneva::Individuals::GFunctionIndividual>
class GMetaOptimizerIndividualT // NOLINT(cppcoreguidelines-special-member-functions)
  : public gen::GGenomeT<GMetaOptimizerIndividualT<ind_type>>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    // The archive still default-constructs the concrete type on load; GReflectiveInterfaceAccess lets the mixin reach
    // the private localMembers_() below (load_/compare_/clone_/name_ are generated from it). serialize()
    // is kept BY HAND: it also emits ind_factory_, which localMembers_ deliberately omits (the factory is
    // kept-not-copied on load and ignored by compare) -- an asymmetry serialize_members() cannot express.
    friend struct Gem::Weft::access;
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <class Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        Gem::Common::archive_named_base<gen::GGenome>(ar, "gen::GGenome", *this);
        archive_named(ar, "n_runs_per_optimization_", n_runs_per_optimization_);
        archive_named(ar, "fitness_target_", fitness_target_);
        archive_named(ar, "iteration_threshold_", iteration_threshold_);
        archive_named(ar, "mo_target_", mo_target_);
        archive_named(ar, "sub_ea_config_", sub_ea_config_);
        archive_named(ar, "ind_factory_", ind_factory_);
    }

    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() and compare token. */
    static constexpr std::string_view class_name = "GMetaOptimizerIndividualT<ind_type>";
    /***************************************************************************/
    /**
     * The default constructor.
     */
    GMetaOptimizerIndividualT()
      : gen::GGenomeT<GMetaOptimizerIndividualT<ind_type>>()
      , n_runs_per_optimization_(GMETAOPT_DEF_NRUNSPEROPT)
      , fitness_target_(GMETAOPT_DEF_FITNESSTARGET)
      , iteration_threshold_(GMETAOPT_DEF_ITERATIONTHRESHOLD)
      , mo_target_(GMETAOPT_DEF_MOTARGET)
      , sub_ea_config_(GMETAOPT_DEF_SUBEACONFIG)
      , ind_factory_() { /* nothing */
    }

    /***************************************************************************/
    /**
     * A standard copy constructor
     *
     * @param cp A constant reference to another GMetaOptimizerIndividualT object
     */
    GMetaOptimizerIndividualT(const GMetaOptimizerIndividualT<ind_type> &cp)
      : gen::GGenomeT<GMetaOptimizerIndividualT<ind_type>>(cp)
      , n_runs_per_optimization_(cp.n_runs_per_optimization_)
      , fitness_target_(cp.fitness_target_)
      , iteration_threshold_(cp.iteration_threshold_)
      , mo_target_(cp.mo_target_)
      , sub_ea_config_(cp.sub_ea_config_)
      , ind_factory_(
            // A default-constructed meta-optimizer has no factory yet; deep-clone
            // it only when present, otherwise copy the (null) factory as-is. The
            // previous unconditional (cp.ind_factory_)->clone() dereferenced a null
            // pointer when copying/cloning a default-constructed object.
            cp.ind_factory_
                ? Gem::Common::convertSmartPointer<
                      Gem::Common::GFactoryT<gen::GGenome>,
                      typename ind_type::FACTORYTYPE>((cp.ind_factory_)->clone())
                : std::shared_ptr<typename ind_type::FACTORYTYPE>()
        ) { /* nothing */
    }

    /***************************************************************************/
    /**
     * The standard destructor
     */
    ~GMetaOptimizerIndividualT() override = default;

    /***************************************************************************/
    /**
     * Allows to specify the path and name of a configuration file passed to
     * the (sub-)evolutionary algorithm
     *
     * @param sub_ea_config The path and name of the (sub-)EA configuration file
     */
    void setSubEAConfig(std::string sub_ea_config) {
        sub_ea_config_ = std::move(sub_ea_config);
    }

    /***************************************************************************/
    /**
     * Allows to retrieve the path and name of a configuration file passed to
     * the (sub-)evolutionary algorithm
     *
     * @return The path and name of the (sub-)EA configuration file
     */
    std::string getSubEAConfig() const {
        return sub_ea_config_;
    }

    /***************************************************************************/
    /**
     * Allows to specify how many optimizations should be performed for each (sub-)optimization
     *
     * @param n_runs_per_optimization The number of optimization runs per sub-optimization (must be > 0)
     */
    void setNRunsPerOptimization(std::size_t n_runs_per_optimization) {
#ifdef DEBUG
        if(0 == n_runs_per_optimization) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMetaOptimizerIndividualT<ind_type>::setNRunsPerOptimization(): Error!"
                << '\n'
                << "Requested number of sub-optimizations is 0" << '\n'
            );
        }
#endif

        n_runs_per_optimization_ = n_runs_per_optimization;
    }

    /***************************************************************************/
    /**
     * Allows to retrieve the number of optimizations to be performed for each (sub-)optimization
     *
     * @return The number of optimization runs per sub-optimization
     */
    std::size_t getNRunsPerOptimization() const {
        return n_runs_per_optimization_;
    }

    /***************************************************************************/
    /**
     * Allows to set the fitness target for each optimization
     *
     * @param fitness_target The fitness value below which a sub-optimization may stop
     */
    void setFitnessTarget(double fitness_target) {
        fitness_target_ = fitness_target;
    }

    /***************************************************************************/
    /**
     * Retrieves the fitness target for each optimization
     *
     * @return The fitness target for each sub-optimization
     */
    double getFitnessTarget() const {
        return fitness_target_;
    }

    /***************************************************************************/
    /**
     * Allows to set the iteration threshold
     *
     * @param iteration_threshold The maximum allowed number of iterations per sub-optimization
     */
    void setIterationThreshold(std::uint32_t iteration_threshold) {
        iteration_threshold_ = iteration_threshold;
    }

    /***************************************************************************/
    /**
     * Allows to retrieve the iteration threshold
     *
     * @return The maximum allowed number of iterations per sub-optimization
     */
    std::uint32_t getIterationThreshold() const {
        return iteration_threshold_;
    }

    /***************************************************************************/
    /**
     * Allows to set the desired target of the meta-optimization
     *
     * @param mo_target The optimization target (best fitness, fewest solver calls, or multi-criterion)
     */
    void setMetaOptimizationTarget(metaOptimizationTarget mo_target) {
        mo_target_ = mo_target;

        // multi-criterion optimization. We need to set the number of fitness criteria
        if(metaOptimizationTarget::MC_MINSOLVER_BESTFITNESS == mo_target_) {
            this->setNStoredResults(2);
        }
    }

    /***************************************************************************/
    /**
     * Allows to retrieve the current target of the meta-optimization
     *
     * @return The current meta-optimization target
     */
    metaOptimizationTarget getMetaOptimizationTarget() const {
        return mo_target_;
    }

    /***************************************************************************/
    /**
     * Retrieves the current number of parents. Needed for the optimization monitor.
     *
     * @return The number of parents currently encoded in the genome
     */
    std::size_t getNParents() const {
        return static_cast<std::size_t>(readTuned().at(oa::ea_tunable::n_parents));
    }

    /***************************************************************************/
    /**
     * Retrieves the current number of children. Needed for the optimization monitor.
     *
     * @return The number of children currently encoded in the genome
     */
    std::size_t getNChildren() const {
        return static_cast<std::size_t>(readTuned().at(oa::ea_tunable::n_children));
    }

    /***************************************************************************/
    /**
     * Retrieves the adaption probability. Needed for the optimization monitor.
     *
     * @return The adaption probability derived from min_ad_prob and the ad_prob range/start percentage
     */
    double getAdProb() const {
        const auto v = readTuned();
        return v.at(oa::ea_tunable::min_ad_prob) +
               (v.at(oa::ea_tunable::ad_prob_start_pct) * v.at(oa::ea_tunable::ad_prob_range));
    }

    /***************************************************************************/
    /**
     * Retrieves the lower sigma boundary. Needed for the optimization monitor.
     *
     * @return The lower sigma boundary currently encoded in the genome
     */
    double getMinSigma() const {
        return readTuned().at(oa::ea_tunable::min_sigma);
    }

    /***************************************************************************/
    /**
     * Retrieves the sigma range. Needed for the optimization monitor.
     *
     * @return The sigma range currently encoded in the genome
     */
    double getSigmaRange() const {
        return readTuned().at(oa::ea_tunable::sigma_range);
    }

    /***************************************************************************/
    /**
     * Retrieves the sigma-sigma parameter. Needed for the optimization monitor.
     *
     * @return The sigma-sigma (sigma self-adaption strength) currently encoded in the genome
     */
    double getSigmaSigma() const {
        return readTuned().at(oa::ea_tunable::sigma_sigma);
    }

    /***************************************************************************/
    /**
     * Builds the flat meta genome for the passed individual from a tunable manifest: one labelled group
     * per knob (the label IS the knob name), integer knobs in the int32 channel and the rest in the double
     * channel, each in manifest order. Reading then happens by name (readTuned()), so the build and the
     * readers stay in sync through the single manifest -- there is no positional index to keep aligned.
     * The genome is structure-only; the adaptors live on the OA-owned config authored by
     * getAdaptionConfig().
     *
     * @param p The individual whose genome is being built (modified in place)
     * @param manifest The tunable parameters to encode (name, channel, init value and search bounds)
     */
    //! [meta.addcontent]
    static void addContent(
        const std::shared_ptr<GMetaOptimizerIndividualT<ind_type>>& p,
        const std::vector<oa::TunableParam> &manifest
    ) {
        gen::GGenomeBuilder b;

        // Integer knobs first (int32 channel), then the real knobs (double channel); within each channel
        // the order is the manifest order, which readTuned() walks identically.
        for(const auto &tp : manifest) {
            if(tp.is_integer) {
                b.addInt32(
                     static_cast<std::int32_t>(tp.init),
                     static_cast<std::int32_t>(tp.lower),
                     static_cast<std::int32_t>(tp.upper)
                 )
                    .label(tp.name);
            }
        }
        for(const auto &tp : manifest) {
            if(not tp.is_integer) {
                b.addDouble(tp.init, tp.lower, tp.upper).label(tp.name);
            }
        }

        p->setGenome(b.build());
    }
    //! [meta.addcontent]

    /***************************************************************************/
    /**
     * @brief Builds the OA-owned adaption configuration for the meta genome: n_parents gets a flip adaptor,
     * n_children an integer-Gauss adaptor, and every meta double a Gauss adaptor. Used by the outer EA
     * (via Go2 / setAdaptionConfig) and by the self-driven modify hook.
     *
     * @return A shared pointer to the populated OA-owned adaption config
     */
    std::shared_ptr<oa::GAdaptionConfigBase> getAdaptionConfig() const {
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
        cfg->groupInt32(0).flip(1.);                            // n_parents
        cfg->groupInt32(1).intGauss(0.025, 0.2, 0.001, 0.5, 1.); // n_children
        for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
            cfg->groupDouble(i).gauss(0.025, 0.2, 0.001, 0.5, 1.);
        }
        return cfg;
    }

    /***************************************************************************/
    /**
     * Emit information about this individual
     *
     * @param with_fitness Whether the fitness line should be included in the output
     * @return A human-readable, multi-line description of this individual's parameters
     */
    std::string print(bool with_fitness = true) const {
        std::ostringstream result; // NOLINT(cppcoreguidelines-init-variables)
        // Retrieve the parameters from the flat genome by name (see readTuned()).
        namespace n = oa::ea_tunable;
        const auto v = readTuned();
        const auto npar = static_cast<std::int32_t>(v.at(n::n_parents));
        const auto nch = static_cast<std::int32_t>(v.at(n::n_children));
        const double amalgamation = v.at(n::amalgamation);
        const double min_ad_prob = v.at(n::min_ad_prob);
        const double ad_prob_range = v.at(n::ad_prob_range);
        const double ad_prob_start_percentage = v.at(n::ad_prob_start_pct);
        const double adapt_adprob = v.at(n::adapt_ad_prob);
        const double minsigma = v.at(n::min_sigma);
        const double sigmarange = v.at(n::sigma_range);
        const double sigma_range_percentage = v.at(n::sigma_range_pct);
        const double sigmasigma = v.at(n::sigma_sigma);

        // Stream the results

        bool const unprocessed = (not this->is_processed() || this->has_errors());
        double const transformed_primary_fitness =
            unprocessed ? this->getWorstCase() : this->transformed_fitness(0);

        result << "================================================================================"
                  "============"
               << '\n';

        if(with_fitness) {
            result << "Fitness = " << transformed_primary_fitness
                   << (unprocessed ? " // unprocessed or error" : "") << '\n';
        }

        result << "Optimization target: " << getClearTextMOT(mo_target_) << '\n'
               << '\n'
               << "population::population size = " << npar + nch << '\n'
               << "population::n_parents = " << npar << '\n'
               << "population::n_children = " << nch << '\n'
               << "population::amalgamation_likelihood = " << amalgamation << '\n'
               << "individual::ad_prob_range = " << ad_prob_range << '\n'
               << "individual::ad_prob_start_percentage_ptr = "
               << ad_prob_start_percentage << '\n'
               << "individual::ad_prob = "
               << min_ad_prob + (ad_prob_range * ad_prob_start_percentage)
               << '\n'
               << "individual::min_ad_prob = " << min_ad_prob << '\n'
               << "individual::max_ad_prob = "
               << min_ad_prob + ad_prob_range << '\n'
               << "individual::adapt_ad_prob = " << adapt_adprob << '\n'
               << "individual::sigmarange_ptr = " << sigmarange << '\n'
               << "individual::sigma_range_percentage_ptr = " << sigma_range_percentage
               << '\n'
               << "individual::sigma1 = "
               << minsigma + (sigmarange * sigma_range_percentage)
               << '\n'
               << "individual::min_sigma1 = " << minsigma << '\n'
               << "individual::max_sigma1 = " << minsigma + sigmarange
               << '\n'
               << "individual::sigma_sigma1 = " << sigmasigma << '\n'
               << "================================================================================"
                  "============"
               << '\n'
               << '\n';

        return result.str();
    }

    /***************************************************************************/
    /**
     * Registers a factory class with this object. This function clones the factory,
     * so the individual can be sure to have a unique factory.
     *
     * @param factory The sub-individual factory to clone and store (must not be empty)
     */
    void registerIndividualFactory(const std::shared_ptr<typename ind_type::FACTORYTYPE>& factory) {
        if(not factory) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMetaOptimizerIndividualT<T>::registerIndividualFactory(): Error!"
                << '\n'
                << "Individual is empty" << '\n'
            );
        }

        ind_factory_ = Gem::Common::convertSmartPointer<
            Gem::Common::GFactoryT<gen::GGenome>,
            typename ind_type::FACTORYTYPE>(factory->clone());
    }

protected:
    /***************************************************************************/
    /**
     * Adds local configuration options to a GParserBuilder object
     *
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override {
        // Call our parent class'es function
        gen::GGenome::addConfigurationOptions_(gpb);

        // Add local data
        gpb.registerFileParameter<std::size_t>(
            "n_runs_per_optimization" // The name of the variable
            ,
            GMETAOPT_DEF_NRUNSPEROPT // The default value
            ,
            [this](std::size_t nrpo) { this->setNRunsPerOptimization(nrpo); }
        ) << "Specifies the number of optimizations performed";

        gpb.registerFileParameter<double>(
            "fitness_target" // The name of the variable
            ,
            GMETAOPT_DEF_FITNESSTARGET // The default value
            ,
            [this](double ft) { this->setFitnessTarget(ft); }
        ) << "The fitness below which optimization should stop";

        gpb.registerFileParameter<std::uint32_t>(
            "iteration_threshold" // The name of the variable
            ,
            GMETAOPT_DEF_ITERATIONTHRESHOLD // The default value
            ,
            [this](std::uint32_t dit) { this->setIterationThreshold(dit); }
        ) << "The maximum number of iterations per sub-optimization";

        gpb.registerFileParameter<metaOptimizationTarget>(
            "meta_optimization_target" // The name of the variable
            ,
            GMETAOPT_DEF_MOTARGET // The default value
            ,
            [this](metaOptimizationTarget mot) { this->setMetaOptimizationTarget(mot); }
        ) << "The target for the meta-optimization: best fitness (0),"
          << '\n'
          << "minimum number of solver calls (1), multi-criterion with best fitness" << '\n'
          << "and smallest number of solver calls as target (2);";

        gpb.registerFileParameter<std::string>(
            "sub_ea_config" // The name of the variable
            ,
            GMETAOPT_DEF_SUBEACONFIG // The default value
            ,
            [this](std::string seac) { this->setSubEAConfig(std::move(seac)); }
        ) << "Path and name of the configuration file used for the (sub-)evolutionary algorithm";
    }

    /***************************************************************************/
    /**
     * The single declaration of this class'es local data members. load_() and
     * compare_() are derived from it, so the member list lives in one place.
     * Note: ind_factory_ is deliberately NOT listed here -- load_() keeps the
     * object's own factory (see the comment there) and compare_() ignores it, so
     * it is not part of the copy/compare semantics. serialize() handles it
     * separately; do not derive serialize() from localMembers() for this class.
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("n_runs_per_optimization_", self.n_runs_per_optimization_),
            Gem::Common::make_member("fitness_target_", self.fitness_target_),
            Gem::Common::make_member("iteration_threshold_", self.iteration_threshold_),
            Gem::Common::make_member("mo_target_", self.mo_target_),
            Gem::Common::make_member("sub_ea_config_", self.sub_ea_config_)
        );
    }

    /***************************************************************************/
    /**
     * The evaluation hook: runs the nested optimization(s) and returns the meta-fitness. For the
     * multi-criterion target (MC_MINSOLVER_BESTFITNESS) it returns {best-fitness, average-solver-calls};
     * otherwise a single-element vector.
     *
     * @return The raw result vector (size == getNStoredResults())
     */
    /***************************************************************************/
    /** @brief The per-run measurements evaluate() accumulates across the sub-optimizations and then
     *  reduces to the raw result (see runOneSubOptimization / assembleEvaluationResult). */
    struct SubRunMeasurements {
        std::vector<double> solver_calls;
        std::vector<double> iterations;
        std::vector<double> best_evaluations;
    };

    /***************************************************************************/
    /** @brief Runs one sub-optimization (build population, drive adaption, optimize) and appends this
     *  run's solver-calls / iterations / best-evaluation to @p measurements. */
    void runOneSubOptimization(
        oa::GOptimizationAlgorithmFactoryT<oa::GEvolutionaryAlgorithm> &ea,
        std::uint32_t pop_size,
        std::uint32_t n_parents,
        std::uint32_t n_children,
        const std::shared_ptr<oa::GAdaptionConfigBase> &sub_adaption_config,
        double amalgamation_likelihood,
        SubRunMeasurements &measurements
    ) {
        // The inner optimization submits to the one process-wide work consumer (the default). This is
        // safe because this individual is evaluated by GMetaEvolutionaryAlgorithm on its own orchestration
        // pool -- distinct from that work consumer -- so the inner submission never starves the pool this
        // evaluation runs on.
        std::shared_ptr<oa::GEvolutionaryAlgorithm> const ea_ptr = ea.get<oa::GEvolutionaryAlgorithm>();

        populateSubEA(*ea_ptr, pop_size, n_parents, sub_adaption_config, amalgamation_likelihood);
        configureSubEAStopCriteria(*ea_ptr);

        // Make sure the optimization is quiet, then run it.
        ea_ptr->setReportIteration(0);
        ea_ptr->optimize();

        // Book-keeping from the completed run.
        auto const best_individual =
            ea_ptr->getBestGlobalIndividual<gen::GGenome>();
        const std::uint32_t iterations_consumed = ea_ptr->getIteration();
        measurements.solver_calls.push_back(
            static_cast<double>(((iterations_consumed + 1) * n_children) + n_parents)
        );
        measurements.iterations.push_back(static_cast<double>(iterations_consumed + 1));
        measurements.best_evaluations.push_back(
            best_individual->transformed_fitness(0)
        ); // We use the transformed fitness to avoid MAX_DOUBLE
    }

    /***************************************************************************/
    /**
     * @brief runOneSubOptimization() setup: size the sub-EA, fill it with fresh individuals from the
     * factory, and hand it the OA-owned adaption config and the cross-over likelihood.
     */
    void populateSubEA(
        oa::GEvolutionaryAlgorithm &ea,
        std::uint32_t pop_size,
        std::uint32_t n_parents,
        const std::shared_ptr<oa::GAdaptionConfigBase> &sub_adaption_config,
        double amalgamation_likelihood
    ) {
        //! [meta.populatesubea]
        ea.setPopulationSizes(pop_size, n_parents);
        for(std::size_t ind = 0; ind < pop_size; ind++) {
            std::shared_ptr<gen::GGenome> const gi_ptr = ind_factory_->get();
            ea.push_back(gi_ptr->clone());
        }
        // Drive the sub-individuals' adaption through the OA-owned config, and set the likelihood for work
        // items to be produced through cross-over rather than mutation alone.
        ea.setAdaptionConfig(sub_adaption_config);
        ea.setAmalgamationLikelihood(amalgamation_likelihood);
        //! [meta.populatesubea]
    }

    /***************************************************************************/
    /**
     * @brief runOneSubOptimization() setup: configure the sub-EA's stop criteria for the current
     * meta-optimization target. MINSOLVERCALLS also stops on the quality threshold and disables stalls;
     * the fitness / multi-criterion targets stop on max-iterations with a lenient stall threshold. Neither
     * emits a termination reason.
     */
    void configureSubEAStopCriteria(oa::GEvolutionaryAlgorithm &ea) const {
        ea.setEmitTerminationReason(false);
        ea.setMaxIteration(iteration_threshold_);
        if(metaOptimizationTarget::MINSOLVERCALLS == mo_target_) {
            ea.setQualityThreshold(fitness_target_, true);
            ea.setMaxStallIteration(0); // do not stop due to stalls (the EA-config default)
        }
        else { // BESTFITNESS / MC_MINSOLVER_BESTFITNESS
            ea.setMaxStallIteration(50);
        }
    }

    std::vector<double> evaluate() override {
        // Retrieve the parameters from the flat genome by name (see readTuned()).
        namespace n = oa::ea_tunable;
        const auto v = readTuned();

        // Check that we have been given a factory. This guard runs in release builds too: without it the
        // ind_factory_->get() below would dereference a null factory pointer (undefined behaviour) rather
        // than report the missing registration.
        if(not ind_factory_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMetaOptimizerIndividualT<T>::evaluate(): Error!" << '\n'
                << "No factory class for individuals has been registered" << '\n'
            );
        }

        // Derive the sub-individuals' adaptor config from the meta-optimised knobs.
        auto sub_adaption_config = buildSubAdaptionConfig(v);

        // Set up a population factory for serial execution
        oa::GOptimizationAlgorithmFactoryT<oa::GEvolutionaryAlgorithm> ea(sub_ea_config_);

        // Run the required number of optimizations
        auto n_children = static_cast<std::uint32_t>(v.at(n::n_children));
        auto n_parents = static_cast<std::uint32_t>(v.at(n::n_parents));
        std::uint32_t const pop_size = n_parents + n_children;
        double const amalgamation_likelihood = v.at(n::amalgamation);

        SubRunMeasurements measurements;

        for(std::size_t opt = 0; opt < n_runs_per_optimization_; opt++) {
            std::cout << "Starting measurement " << opt + 1 << " / " << n_runs_per_optimization_
                      << '\n';
            runOneSubOptimization(
                ea, pop_size, n_parents, n_children, sub_adaption_config, amalgamation_likelihood,
                measurements
            );
        }

        return assembleEvaluationResult(measurements);
    }

    /***************************************************************************/
    /**
     * @brief evaluate() phase: derive the sub-individuals' Gauss adaptor config from the meta-optimised
     * knobs. The genome carries RAW knobs (min + range + start percentage) so it always holds valid values;
     * the gauss bounds are derived here (max = min + range; start = min + percentage * range).
     *
     * The config is built from a sample genome rather than via the factory because GIndividualFactory
     * re-applies its config file on every get_(), so programmatic setters on the factory would not stick.
     *
     * @param v The meta-optimised tunable parameters (by name; see readTuned())
     * @return An OA-owned single-Gauss adaption config for the sub-individuals
     */
    auto buildSubAdaptionConfig(const std::map<std::string, double> &v) {
        namespace n = oa::ea_tunable;
        double const min_sigma = v.at(n::min_sigma);
        double const sigma_range = v.at(n::sigma_range);
        double const max_sigma = min_sigma + sigma_range;
        double const sigma_range_percentage = v.at(n::sigma_range_pct);
        double const start_sigma = min_sigma + (sigma_range_percentage * sigma_range);
        double const sigma_sigma = v.at(n::sigma_sigma);

        double const min_ad_prob = v.at(n::min_ad_prob);
        double const ad_prob_range = v.at(n::ad_prob_range);
        double const max_ad_prob = min_ad_prob + ad_prob_range;
        double const ad_prob_start_percentage = v.at(n::ad_prob_start_pct);
        double const start_ad_prob = min_ad_prob + (ad_prob_start_percentage * ad_prob_range);

        double const adapt_ad_prob = v.at(n::adapt_ad_prob);

        auto sub_adaption_config = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(
            dynamic_cast<const gen::GGenome &>(*ind_factory_->get())
        );
        for(std::size_t i = 0; i < sub_adaption_config->doubleGroups().size(); i++) {
            sub_adaption_config->groupDouble(i).gauss(
                start_sigma, sigma_sigma, min_sigma, max_sigma, start_ad_prob, adapt_ad_prob, 1,
                Gem::Geneva::adaptionMode::WITHPROBABILITY, min_ad_prob, max_ad_prob
            );
        }
        return sub_adaption_config;
    }

    /***************************************************************************/
    /**
     * @brief evaluate() phase: reduce the per-run measurements to the raw result vector, reporting the
     * summary statistics. The secondary (average solver calls) is present only for the multi-criterion
     * target, matching getNStoredResults() (2 vs 1).
     *
     * @param measurements The per-run solver-call / iteration / best-evaluation accumulators
     * @return The raw result vector for this meta-individual
     */
    std::vector<double> assembleEvaluationResult(const SubRunMeasurements &measurements) {
        std::tuple<double, double> sd =
            Gem::Common::GStandardDeviation(measurements.solver_calls);
        std::tuple<double, double> itmean =
            Gem::Common::GStandardDeviation(measurements.iterations);
        std::tuple<double, double> best_mean =
            Gem::Common::GStandardDeviation(measurements.best_evaluations);

        double evaluation = 0.;
        if(metaOptimizationTarget::MINSOLVERCALLS == mo_target_) {
            evaluation = std::get<0>(sd);
        }
        else if(metaOptimizationTarget::BESTFITNESS == mo_target_) {
            evaluation = std::get<0>(best_mean);
        }
        else if(metaOptimizationTarget::MC_MINSOLVER_BESTFITNESS == mo_target_) {
            evaluation = std::get<0>(best_mean);
        }

        // Emit some information
        std::cout << '\n'
                  << std::get<0>(sd) << " +/- " << std::get<1>(sd) << " solver calls with " << '\n'
                  << std::get<0>(itmean) << " +/- " << std::get<1>(itmean) << " average iterations "
                  << '\n'
                  << "and a best evaluation of " << std::get<0>(best_mean) << " +/- "
                  << std::get<1>(best_mean) << '\n'
                  << "out of " << n_runs_per_optimization_ << " consecutive runs" << '\n'
                  << "evaluate() will return the value " << evaluation << '\n'
                  << this->print(false)
                  << '\n' // print without fitness -- not defined at this stage
                  << '\n';

        if(metaOptimizationTarget::MC_MINSOLVER_BESTFITNESS == mo_target_) {
            return {evaluation, std::get<0>(sd)};
        }
        return {evaluation};
    }

    /***************************************************************************/
    /**
     * Retrieves a clear-text description of the optimization target
     *
     * @param mot The meta-optimization target to describe
     * @return A human-readable description of the target
     */
    std::string getClearTextMOT(const metaOptimizationTarget &mot) const {
        switch(mot) {
        case metaOptimizationTarget::BESTFITNESS:
            return std::string("\"best fitness\"");

        case metaOptimizationTarget::MINSOLVERCALLS:
            return std::string("\"minimum number of solver calls\"");

        case metaOptimizationTarget::MC_MINSOLVER_BESTFITNESS:
            return std::string(
                "\"multi-criterion target with best fitness, minimum number of solver calls\""
            );
        }

        // Make the compiler happy
        return {};
    }

    /***************************************************************************/
    /**
     * Applies modifications to this object.
     *
     * @return A boolean indicating whether any modifications were made
     */
    bool modify_GUnitTests_() override {
#ifdef GEM_TESTING

        bool result = false;

        // Call the parent classes' functions
        if(gen::GGenome::modify_GUnitTests_()) {
            result = true;
        }

        // Change the parameter settings (only when the genome has actually been built). The adaption
        // state + logic are OA-owned; a standalone individual drives them via a self-owned
        // scratch + config (StandaloneAdapter).
        if(this->template countParameters<std::int32_t>() + this->template countParameters<double>() > 0) {
            // The genome is structure-only; drive the self-owned adaption via the authored meta config.
            Gem::Geneva::OptimizationAlgorithms::StandaloneAdapter(*this, getAdaptionConfig()).adapt(*this);
            result = true;
        }

        // Let the audience know whether we have changed the content
        return result;

#else  /* GEM_TESTING */
        Gem::Common::condnotset(
            "GMetaOptimizerIndividualT<ind_type>::modify_GUnitTests()",
            "GEM_TESTING"
        );
        return false;
#endif /* GEM_TESTING */
    }

    /***************************************************************************/
    /**
     * Performs self tests that are expected to succeed.
     */
    void specificTestsNoFailureExpected_GUnitTests_() override {
#ifdef GEM_TESTING
        using namespace Gem::Geneva;

        // Call the parent classes' functions
        gen::GGenome::specificTestsNoFailureExpected_GUnitTests_();

        //------------------------------------------------------------------------------

        {
            /* nothing. Add test cases here that are expected to succeed. */
        }

        //------------------------------------------------------------------------------
#else  /* GEM_TESTING */
        Gem::Common::condnotset(
            "GMetaOptimizerIndividualT<ind_type>::specificTestsNoFailureExpected_GUnitTests()",
            "GEM_TESTING"
        );
#endif /* GEM_TESTING */
    }
private:
    /***************************************************************************/
    /**
     * @brief Reads the meta-optimised parameters from the flat genome into a name -> value map, keyed by
     * the tunable-manifest names. The genome is built from oa::GEvolutionaryAlgorithm::tunableManifest()
     * (one labelled group per knob, integer knobs first), so walking that manifest in the same order
     * recovers each value by name -- no positional index that could drift if a knob is added or reordered.
     *
     * @return A map from each manifest knob name to its current value in the genome
     */
    std::map<std::string, double> readTuned() const {
        std::vector<std::int32_t> iv;
        this->template streamline<std::int32_t>(iv);
        std::vector<double> dv;
        this->template streamline<double>(dv);

        std::map<std::string, double> out;
        std::size_t ii = 0;
        std::size_t di = 0;
        for(const auto &tp : oa::GEvolutionaryAlgorithm::tunableManifest()) {
            if(tp.is_integer) {
                out[tp.name] = static_cast<double>(iv.at(ii++));
            }
            else {
                out[tp.name] = dv.at(di++);
            }
        }
        return out;
    }

    /***************************************************************************/

    std::size_t n_runs_per_optimization_; ///< The number of runs performed for each (sub-)optimization
    double fitness_target_;             ///< The quality target to be reached by
    std::uint32_t iteration_threshold_; ///< The maximum allowed number of iterations
    metaOptimizationTarget mo_target_;  ///< The target used for the meta-optimization
    std::string
        sub_ea_config_; ///< Path and name of the configuration file needed for (sub-)evolutionary algorithms

    std::shared_ptr<typename ind_type::FACTORYTYPE>
        ind_factory_; ///< Holds a factory for our individuals
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Allows to output a GMetaOptimizerIndividualT<ind_type> (or convert it to a string) via its operator<<
 *
 * @tparam ind_type The type of sub-individual whose optimization is being tuned
 * @param stream The output stream to write to
 * @param gsi The meta-optimizer individual whose content is written
 * @return A reference to the output stream
 */
template <typename ind_type>
std::ostream &operator<<(std::ostream &stream, const GMetaOptimizerIndividualT<ind_type> &gsi) {
    stream << gsi.print();
    return stream;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A factory for GMetaOptimizerIndividualT<ind_type> objects.
 *
 * This is intentionally a hand-written factory rather than the generic
 * Gem::Geneva::Genome::GIndividualFactory used by every other individual: a meta-optimizer
 * COMPOSES a sub-individual's own factory (registerIndividualFactory() clones and stores an
 * ind_type::FACTORYTYPE, which is then injected into each produced meta-individual so it can spawn the
 * inner population it optimises). That is per-factory-instance state handed to each product -- a capability
 * the generic factory, whose hooks are static and whose only per-instance state is a plain Config, does
 * not (and is not meant to) provide. It is therefore the deliberate exception to the factory unification,
 * not an un-migrated leftover.
 *
 * @tparam ind_type The type of sub-individual whose optimization is being tuned
 */
template <typename ind_type>
class GMetaOptimizerIndividualFactoryT : public Gem::Common::GFactoryT<gen::GGenome> {
public:
    /***************************************************************************/
    /**
     * A constructor with the ability to switch the parallelization mode. It initializes a
     * target item as needed.
     *
     * @param config_file The name of the configuration file
     */
    GMetaOptimizerIndividualFactoryT(std::filesystem::path const &config_file)
      : Gem::Common::GFactoryT<gen::GGenome>(config_file) { /* nothing */
    }

    /***************************************************************************/
    /**
     * The destructor
     */
    ~GMetaOptimizerIndividualFactoryT() override = default;

    /***************************************************************************/
    /**
     * Registers a factory class with this object. This function clones the factory,
     * so the individual can be sure to have a unique factory.
     *
     * @param factory The sub-individual factory to clone and store (must not be empty)
     */
    void registerIndividualFactory(const std::shared_ptr<typename ind_type::FACTORYTYPE>& factory) {
        if(not factory) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GMetaOptimizerIndividualFactoryT<T>::registerIndividualFactory(): Error!"
                << '\n'
                << "Individual is empty" << '\n'
            );
        }

        ind_factory_ = Gem::Common::convertSmartPointer<
            Gem::Common::GFactoryT<gen::GGenome>,
            typename ind_type::FACTORYTYPE>(factory->clone());
    }

protected:
    /***************************************************************************/
    /**
     * Allows to describe local configuration options for the meta-optimizer factory
     *
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    // NOLINTNEXTLINE(readability-function-size) -- one coherent config-registration sweep for the meta-optimizer factory's option list; splitting would scatter the option list
    void describeLocalOptions_(Gem::Common::GParserBuilder &gpb) override {
        // Describe our own options
        using namespace Gem::Courtier;

        std::string comment; // NOLINT(cppcoreguidelines-init-variables)
        comment = "";
        comment += "The initial number of parents in a population;";
        gpb.registerFileParameter<std::size_t>(
            "init_n_parents",
            init_n_parents_,
            GMETAOPT_DEF_INITNPARENTS,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The lower boundary for variations of the number of parents;";
        gpb.registerFileParameter<std::size_t>(
            "n_parents_lb",
            n_parents_lb_,
            GMETAOPT_DEF_NPARENTS_LB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The upper boundary for variations of the number of parents;";
        gpb.registerFileParameter<std::size_t>(
            "n_parents_ub",
            n_parents_ub_,
            GMETAOPT_DEF_NPARENTS_UB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The initial number of children in a population;";
        gpb.registerFileParameter<std::size_t>(
            "init_n_children",
            init_n_children_,
            GMETAOPT_DEF_INITNCHILDREN,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The lower boundary for the variation of the number of children;";
        gpb.registerFileParameter<std::size_t>(
            "n_children_lb",
            n_children_lb_,
            GMETAOPT_DEF_NCHILDREN_LB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The upper boundary for the variation of the number of children;";
        gpb.registerFileParameter<std::size_t>(
            "n_children_ub",
            n_children_ub_,
            GMETAOPT_DEF_NCHILDREN_UB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The initial likelihood for an individual being created from cross-over rather "
                   "than just duplication;";
        gpb.registerFileParameter<double>(
            "init_amalgamation_lklh",
            init_amalgamation_lklh_,
            GMETAOPT_DEF_INITAMALGLKLHOOD,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The lower boundary for the variation of the amalgamation likelihood ;";
        gpb.registerFileParameter<double>(
            "amalgamation_lklh_lb",
            amalgamation_lklh_lb_,
            GMETAOPT_DEF_AMALGLKLHOOD_LB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The upper boundary for the variation of the amalgamation likelihood ;";
        gpb.registerFileParameter<double>(
            "amalgamation_lklh_ub",
            amalgamation_lklh_ub_,
            GMETAOPT_DEF_AMALGLKLHOOD_UB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The initial lower boundary for the variation of ad_prob;";
        gpb.registerFileParameter<double>(
            "init_min_ad_prob",
            init_min_ad_prob_,
            GMETAOPT_DEF_INITMINADPROB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The lower boundary for min_ad_prob;";
        gpb.registerFileParameter<double>(
            "min_ad_prob_lb",
            min_ad_prob_lb_,
            GMETAOPT_DEF_MINADPROB_LB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The upper boundary for min_ad_prob;";
        gpb.registerFileParameter<double>(
            "min_ad_prob_ub",
            min_ad_prob_ub_,
            GMETAOPT_DEF_MINADPROB_UB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The initial range for the variation of ad_prob;";
        gpb.registerFileParameter<double>(
            "init_ad_prob_range",
            init_ad_prob_range_,
            GMETAOPT_DEF_INITADPROBRANGE,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The lower boundary for ad_prob_range;";
        gpb.registerFileParameter<double>(
            "ad_prob_range_lb",
            ad_prob_range_lb_,
            GMETAOPT_DEF_ADPROBRANGE_LB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The upper boundary for ad_prob_range;";
        gpb.registerFileParameter<double>(
            "ad_prob_range_ub",
            ad_prob_range_ub_,
            GMETAOPT_DEF_ADPROBRANGE_UB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The start value for ad_prob relative to the allowed value range;";
        gpb.registerFileParameter<double>(
            "init_ad_prob_start_percentage",
            init_ad_prob_start_percentage_,
            GMETAOPT_DEF_INITADPROBSTARTPERCENTAGE,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The initial value of the strength of adProb_ adaption;";
        gpb.registerFileParameter<double>(
            "init_adapt_ad_prob",
            init_adapt_ad_prob_,
            GMETAOPT_DEF_INITADAPTADPROB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The lower boundary for the variation of the strength of adProb_ adaption;";
        gpb.registerFileParameter<double>(
            "adapt_ad_prob_lb",
            adapt_ad_prob_lb_,
            GMETAOPT_DEF_ADAPTADPROB_LB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The upper boundary for the variation of the strength of adProb_ adaption;";
        gpb.registerFileParameter<double>(
            "adapt_ad_prob_ub",
            adapt_ad_prob_ub_,
            GMETAOPT_DEF_ADAPTADPROB_UB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The initial minimum sigma for gauss-adaption in ES;";
        gpb.registerFileParameter<double>(
            "init_min_sigma",
            init_min_sigma_,
            GMETAOPT_DEF_INITMINSIGMA,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The lower boundary for the variation of the lower boundary of sigma;";
        gpb.registerFileParameter<double>(
            "min_sigma_lb",
            min_sigma_lb_,
            GMETAOPT_DEF_MINSIGMA_LB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The upper boundary for the variation of the lower boundary of sigma;";
        gpb.registerFileParameter<double>(
            "min_sigma_ub",
            min_sigma_ub_,
            GMETAOPT_DEF_MINSIGMA_UB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The initial maximum range for sigma;";
        gpb.registerFileParameter<double>(
            "init_sigma_range",
            init_sigma_range_,
            GMETAOPT_DEF_INITSIGMARANGE,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The lower boundary for the variation of the maximum range of sigma;";
        gpb.registerFileParameter<double>(
            "sigma_range_lb",
            sigma_range_lb_,
            GMETAOPT_DEF_SIGMARANGE_LB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The upper boundary for the variation of the maximum range of sigma;";
        gpb.registerFileParameter<double>(
            "sigma_range_ub",
            sigma_range_ub_,
            GMETAOPT_DEF_SIGMARANGE_UB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The initial percentage of the sigma range as a start value;";
        gpb.registerFileParameter<double>(
            "init_sigma_range_percentage",
            init_sigma_range_percentage_,
            GMETAOPT_DEF_INITSIGMARANGEPERCENTAGE,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The initial strength of self-adaption of gauss-mutation in ES;";
        gpb.registerFileParameter<double>(
            "init_sigma_sigma",
            init_sigma_sigma_,
            GMETAOPT_DEF_INITSIGMASIGMA,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The lower boundary for the variation of the strength of sigma adaption;";
        gpb.registerFileParameter<double>(
            "sigma_sigma_lb",
            sigma_sigma_lb_,
            GMETAOPT_DEF_SIGMASIGMA_LB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        comment = "";
        comment += "The upper boundary for the variation of the strength of sigma adaption;";
        gpb.registerFileParameter<double>(
            "sigma_sigma_ub",
            sigma_sigma_ub_,
            GMETAOPT_DEF_SIGMASIGMA_UB,
            Gem::Common::VAR_IS_ESSENTIAL,
            comment
        );

        // Allow our parent class to describe its options
        Gem::Common::GFactoryT<gen::GGenome>::describeLocalOptions_(gpb);
    }

    /***************************************************************************/
    /**
     * Allows to act on the configuration options received from the configuration file. Here
     * we can add the options described in describeLocalOptions to the object. In practice,
     * we will usually add the parameter objects here. Note that a very similar constructor
     * exists for GMetaOptimizerIndividualT<ind_type>, so it may be used independently of the factory.
     *
     * @param p_base A smart-pointer to be acted on during post-processing
     */
    void postProcess_(std::shared_ptr<gen::GGenome> &p_base) override {
        // Convert the base pointer to our local type
        std::shared_ptr<GMetaOptimizerIndividualT<ind_type>> const p =
            Gem::Common::convertSmartPointer<gen::GGenome, GMetaOptimizerIndividualT<ind_type>>(
                p_base
            );

        // Build the search genome from the EA's tunable manifest, overriding the default search ranges
        // with this factory's configured values (read from the config file). Addressing each knob by name
        // keeps the genome build and the readers in sync through the single manifest -- no positional
        // coupling, so a config option can be added or reordered without silently shifting another knob.
        namespace n = oa::ea_tunable;
        std::vector<oa::TunableParam> manifest = oa::GEvolutionaryAlgorithm::tunableManifest();
        auto override_knob =
            [&manifest](const char *name, double init, double lower, double upper) {
                for(auto &tp : manifest) {
                    if(tp.name == name) {
                        tp.init = init;
                        tp.lower = lower;
                        tp.upper = upper;
                    }
                }
            };
        override_knob(n::n_parents, double(init_n_parents_), double(n_parents_lb_), double(n_parents_ub_));
        override_knob(n::n_children, double(init_n_children_), double(n_children_lb_), double(n_children_ub_));
        override_knob(n::amalgamation, init_amalgamation_lklh_, amalgamation_lklh_lb_, amalgamation_lklh_ub_);
        override_knob(n::min_ad_prob, init_min_ad_prob_, min_ad_prob_lb_, min_ad_prob_ub_);
        override_knob(n::ad_prob_range, init_ad_prob_range_, ad_prob_range_lb_, ad_prob_range_ub_);
        override_knob(n::ad_prob_start_pct, init_ad_prob_start_percentage_, 0., 1.);
        override_knob(n::adapt_ad_prob, init_adapt_ad_prob_, adapt_ad_prob_lb_, adapt_ad_prob_ub_);
        override_knob(n::min_sigma, init_min_sigma_, min_sigma_lb_, min_sigma_ub_);
        override_knob(n::sigma_range, init_sigma_range_, sigma_range_lb_, sigma_range_ub_);
        override_knob(n::sigma_range_pct, init_sigma_range_percentage_, 0., 1.);
        override_knob(n::sigma_sigma, init_sigma_sigma_, sigma_sigma_lb_, sigma_sigma_ub_);

        GMetaOptimizerIndividualT<ind_type>::addContent(p, manifest);

        // Finally add the individual factory to p
        p->registerIndividualFactory(ind_factory_);
    }

private:
    /***************************************************************************/
    /** @brief The default constructor. Only needed for (de-)serialization */
    GMetaOptimizerIndividualFactoryT() = default;

    /***************************************************************************/
    /**
     * Creates items of this type
     *
     * @param gpb The GParserBuilder to which the new object's configuration options are added
     * @return Items of the desired type
     */
    std::shared_ptr<gen::GGenome>
    getObject_(Gem::Common::GParserBuilder &gpb) override {
        // Will hold the result
        std::shared_ptr<GMetaOptimizerIndividualT<ind_type>> target(
            new GMetaOptimizerIndividualT<ind_type>()
        );

        // Make the object's local configuration options known
        target->addConfigurationOptions(gpb);

        return target;
    }

    /***************************************************************************/
    // Data

    // Parameters pertaining to the ea population
    std::size_t init_n_parents_ = GMETAOPT_DEF_INITNPARENTS; ///< The initial number of parents
    std::size_t n_parents_lb_ =
        GMETAOPT_DEF_NPARENTS_LB; ///< The lower boundary for variations of the number of parents
    std::size_t n_parents_ub_ =
        GMETAOPT_DEF_NPARENTS_UB; ///< The upper boundary for variations of the number of parents

    std::size_t init_n_children_ = GMETAOPT_DEF_INITNCHILDREN; ///< The initial number of children
    std::size_t n_children_lb_ =
        GMETAOPT_DEF_NCHILDREN_LB; ///< The lower boundary for the variation of the number of children
    std::size_t n_children_ub_ =
        GMETAOPT_DEF_NCHILDREN_UB; ///< The upper boundary for the variation of the number of children

    double init_amalgamation_lklh_ =
        GMETAOPT_DEF_INITAMALGLKLHOOD; ///< The initial likelihood for an individual being created from cross-over rather than "just" duplication
    double amalgamation_lklh_lb_ =
        GMETAOPT_DEF_AMALGLKLHOOD_LB; ///< The lower boundary for the variation of the amalgamation likelihood
    double amalgamation_lklh_ub_ =
        GMETAOPT_DEF_AMALGLKLHOOD_UB; ///< The upper boundary for the variation of the amalgamation likelihood

    double init_min_ad_prob_ =
        GMETAOPT_DEF_INITMINADPROB; ///< The initial lower boundary for the variation of ad_prob
    double min_ad_prob_lb_ = GMETAOPT_DEF_MINADPROB_LB; ///< The lower boundary for min_ad_prob
    double min_ad_prob_ub_ = GMETAOPT_DEF_MINADPROB_UB; ///< The upper boundary for min_ad_prob

    double init_ad_prob_range_ =
        GMETAOPT_DEF_INITADPROBRANGE; ///< The initial range for the variation of ad_prob
    double ad_prob_range_lb_ = GMETAOPT_DEF_ADPROBRANGE_LB; ///< The lower boundary for ad_prob_range
    double ad_prob_range_ub_ = GMETAOPT_DEF_ADPROBRANGE_UB; ///< The upper boundary for ad_prob_range

    double init_ad_prob_start_percentage_ =
        GMETAOPT_DEF_INITADPROBSTARTPERCENTAGE; ///< The start value for ad_prob relative to the allowed value range

    double init_adapt_ad_prob_ =
        GMETAOPT_DEF_INITADAPTADPROB; ///< The initial value of the strength of adProb_ adaption
    double adapt_ad_prob_lb_ =
        GMETAOPT_DEF_ADAPTADPROB_LB; ///< The lower boundary for the variation of the strength of adProb_ adaption
    double adapt_ad_prob_ub_ =
        GMETAOPT_DEF_ADAPTADPROB_UB; ///< The upper boundary for the variation of the strength of adProb_ adaption

    double init_min_sigma_ = GMETAOPT_DEF_INITMINSIGMA; ///< The initial minimal value of sigma
    double min_sigma_lb_ =
        GMETAOPT_DEF_MINSIGMA_LB; ///< The lower boundary for the variation of the lower boundary of sigma
    double min_sigma_ub_ =
        GMETAOPT_DEF_MINSIGMA_UB; ///< The upper boundary for the variation of the lower boundary of sigma

    double init_sigma_range_ =
        GMETAOPT_DEF_INITSIGMARANGE; ///< The initial range of sigma (beyond min_sigma
    double sigma_range_lb_ =
        GMETAOPT_DEF_SIGMARANGE_LB; ///< The lower boundary for the variation of the maximum range of sigma
    double sigma_range_ub_ =
        GMETAOPT_DEF_SIGMARANGE_UB; ///< The upper boundary for the variation of the maximum range of sigma

    double init_sigma_range_percentage_ =
        GMETAOPT_DEF_INITSIGMARANGEPERCENTAGE; ///< The initial percentage of the sigma range as a start value

    double init_sigma_sigma_ =
        GMETAOPT_DEF_INITSIGMASIGMA; ///< The initial strength of sigma adaption
    double sigma_sigma_lb_ =
        GMETAOPT_DEF_SIGMASIGMA_LB; ///< The lower boundary for the variation of the strength of sigma adaption
    double sigma_sigma_ub_ =
        GMETAOPT_DEF_SIGMASIGMA_UB; ///< The upper boundary for the variation of the strength of sigma adaption

    std::shared_ptr<typename ind_type::FACTORYTYPE>
        ind_factory_; ///< Holds a factory for our individuals. It will be added to the individuals when needed
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

constexpr std::size_t P_XDIM = 1200;
constexpr std::size_t P_YDIM = 1400;

/******************************************************************************/
/**
 * This class implements a pluggable optimization monitor for Evolutionary Algorithms. Its main purpose
 * is to find out information about the development of various properties (e.g. sigma, best evaluation)
 * over the course of the optimization for the best individuals. This monitor is thus targeted at a specific
 * type of individual. Note that the class uses ROOT scripts for the output of its results.
 *
 * TODO: templatize this class on executor_type, like is being done for the other optimization monitors
 *
 * @tparam ind_type The sub-individual type wrapped by the GMetaOptimizerIndividualT being monitored
 */
template <typename ind_type>
class GOptOptMonitorT // NOLINT(cppcoreguidelines-special-member-functions)
  : public oa::GBasePluggableOM
  , public Gem::Common::GSelfTestable {
    // Make sure this class can only be instantiated if individual_type is an optimizable entity
    // (the monitor reads individuals only through the genome-agnostic interface, so any genome model
    // -- tree or flat -- qualifies).
    static_assert(
        std::is_base_of_v<gen::GGenome, ind_type>,
        "GGenome is no base class of ind_type"
    );

    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;
    // The GArchive registry factory reconstructs this (private default ctor) through the
    // GReflectiveInterfaceAccess construct shim, so it needs reach to the private constructor.
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<oa::GBasePluggableOM>(ar, "GBasePluggableOM", *this);
        // All local members, derived from the single localMembers() declaration (same NVP tags as before).
        Gem::Common::serialize_members(ar, this->localMembers_());
    }
    ///////////////////////////////////////////////////////////////////////

    /** @brief Single declaration of this monitor's local data members, feeding serialize() / load_() /
     *  compare_() from one source. Only file_name_ is state; the eight progress curves are declared into
     *  a transient GDataLog at INFOINIT, filled in INFOPROCESSING and written at INFOEND. */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("file_name_", self.file_name_)
        );
    }

public:
    /***************************************************************************/
    /**
     * Initialization with the name of the output file
     *
     * @param file_name The name of the file the recorded plots are written to
     */
    GOptOptMonitorT(const std::string& file_name)
      : file_name_(file_name) { /* nothing -- the progress curves live in a transient data log */
    }

    /***************************************************************************/
    /**
     * The copy constructor
     *
     * @param cp A copy of another GOptOptMonitorT object
     */
    GOptOptMonitorT(const GOptOptMonitorT<ind_type> &cp)
      : oa::GBasePluggableOM(cp)
      , file_name_(cp.file_name_) {
        // The transient data log (progress information) is deliberately not copied -- it is
        // rebuilt per run in INFOINIT.
    }

    /***************************************************************************/
    /**
     * The destructor
     */
    ~GOptOptMonitorT() override { /* nothing */
    }

    /***************************************************************************/
    /**
     * Sets the file name
     *
     * @param file_name The name of the file the recorded plots are written to
     */
    void setFileName(std::string file_name) {
        file_name_ = std::move(file_name);
    }

    /***************************************************************************/
    /**
     * Retrieves the current file name
     *
     * @return The name of the output file
     */
    [[nodiscard]] std::string getFileName() const {
        return file_name_;
    }

protected:
    /***************************************************************************/
    /**
       * Loads the data of another object
       *
       * @param cp A copy of another GOptOptMonitorT object, camouflaged as a GBasePluggableOM
       */
    void load_(const oa::GBasePluggableOM *cp) override {
        // Check that we are dealing with a GOptOptMonitorT<ind_type> reference independent of this object and convert the pointer
        const GOptOptMonitorT<ind_type> *p_load =
            Gem::Common::g_convert_and_compare<oa::GBasePluggableOM, GOptOptMonitorT<ind_type>>(cp, this);

        // Trigger loading of our parent class'es data
        // Load the parent classes' data ...
        oa::GBasePluggableOM::load_(cp);

        // Load local data, derived from the single localMembers() declaration (the cloneable plotter
        // pointers are deep-cloned, the plain members assigned).
        Gem::Common::g_load_members(this->localMembers_(), p_load->localMembers_());
    }

    /***************************************************************************/
    /** @brief Allow access to this classes compare_ function */
    friend void Gem::Common::compare_base_t<GOptOptMonitorT<ind_type>>(
        GOptOptMonitorT<ind_type> const &,
        GOptOptMonitorT<ind_type> const &,
        Gem::Common::GToken &
    );

    /***************************************************************************/
    /**
     * Searches for compliance with expectations with respect to another object
     * of the same type
     *
     * @param cp A constant reference to another GBasePluggableOM object
     * @param e The expected outcome of the comparison
     * @param limit The limit for allowed deviations of floating point types
     */
    void compare_(
        const oa::GBasePluggableOM &cp,
        const Gem::Common::expectation &e,
        [[maybe_unused]] const double & limit
    ) const override {
        using namespace Gem::Common;

        // Check that we are dealing with a GOptOptMonitorT<ind_type> reference independent of this object and convert the pointer
        const GOptOptMonitorT<ind_type> *p_load =
            Gem::Common::g_convert_and_compare<oa::GBasePluggableOM, GOptOptMonitorT<ind_type>>(cp, this);

        GToken token("GOptOptMonitorT", e);

        // Compare our parent data ...
        Gem::Common::compare_base_t<oa::GBasePluggableOM>(*this, *p_load, token);

        // ... and then our local data, derived from the single localMembers() declaration
        Gem::Common::g_compare_members(this->localMembers_(), p_load->localMembers_(), token);

        // React on deviations from the expectation
        token.evaluate();
    }

    /***************************************************************************/
    /**
     * Applies modifications to this object. This is needed for testing purposes
     *
     * @return A boolean which indicates whether modifications were made
     */
    bool modify_GUnitTests_() override {
#ifdef GEM_TESTING

        bool result = false;

        // Call the parent classes' functions
        if(oa::GBasePluggableOM::modify_GUnitTests_()) {
            result = true;
        }

        // no local data -- nothing to change

        return result;

#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
        Gem::Common::condnotset("GOptOptMonitorT<ind_type>::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif                  /* GEM_TESTING */
    }

    /***************************************************************************/

private:
    /***************************************************************************/
    /**
     * Emits a name for this class / object
     *
     * @return The name of this class
     */
    [[nodiscard]] std::string name_() const override {
        return std::string("GOptOptMonitorT<>");
    }

    /***************************************************************************/
    /**
       * Creates a deep clone of this object
       *
       * @return A deep clone of this object
       */
    [[nodiscard]] oa::GBasePluggableOM *clone_() const override {
        return new GOptOptMonitorT<ind_type>(*this);
    }

    /***************************************************************************/
    /**
     * Allows to emit information in different stages of the information cycle
     * (initialization, during each cycle and during finalization)
     *
     * @param im The current stage of the information cycle (init, processing or end)
     * @param goa A pointer to the optimization algorithm the monitor is attached to
     */
    void informationFunction_(infoMode im, oa::GOptimizationAlgorithmBase const *const goa) override {
        using namespace Gem::Common;
        using namespace Gem::Dietrich; // GPlotSpec / plotKind / graphPlotMode

        switch(im) {
        case Gem::Geneva::infoMode::INFOINIT: {
            // Initialize the plots we want to record
            // Declare the eight progress curves into a fresh data log, in the same order they were
            // registered on the 2x4 canvas. Each is a CURVE graph (x = iteration).
            const struct {
                const char *plot_label;
                const char *y_label;
            } curves[] = {
                {"Number of solver calls", "Best Result (lower is better)"},
                {"Number of parents as a function of the iteration", "Number of parents"},
                {"Number of children as a function of the iteration", "Number of children"},
                {"Adaption probability as a function of the iteration", "Adaption probability"},
                {"Lower sigma boundary as a function of the iteration", "Lower sigma boundary"},
                {"Upper sigma boundary as a function of the iteration", "Upper sigma boundary"},
                {"Development of the sigma range as a function of the iteration", "Sigma range"},
                {"Development of the adaption strength as a function of the iteration", "Sigma-Sigma"}
            };

            data_log_.emplace("Progress information", 2, 4);
            data_log_->setCanvasDimensions(P_XDIM, P_YDIM);
            ids_.clear();
            for(const auto &curve : curves) {
                GPlotSpec spec(plotKind::graph_2d);
                spec.plot_mode = graphPlotMode::CURVE;
                spec.name = curve.plot_label;
                spec.x_label = "Iteration";
                spec.y_label = curve.y_label;
                spec.columns = {"x", "y"};
                ids_.push_back(data_log_->declareSeries(spec));
            }
        } break;

        case Gem::Geneva::infoMode::INFOPROCESSING: {
            // Convert the base pointer to the target type
            auto const *const ea =
                static_cast<oa::GEvolutionaryAlgorithm const *const>(
                    goa
                ); // NOLINT(cppcoreguidelines-init-variables)

            // Extract the requested data. First retrieve the best individual.
            // It can always be found in the first position with evolutionary algorithms
            std::shared_ptr<GMetaOptimizerIndividualT<ind_type>> const p =
                ea->at(0)->clone<GMetaOptimizerIndividualT<ind_type>>();

            // Retrieve the best fitness and average sigma value and append them to the data log,
            // in the same { progress, n_parent, n_children, ad_prob, min_sigma, max_sigma,
            // sigma_range, sigma_sigma } order the series were declared.
            const auto iteration = static_cast<double>(ea->getIteration());
            const double min_sigma = p->getMinSigma();
            const double sigma_range = p->getSigmaRange();
            const double max_sigma = min_sigma + sigma_range;

            const double values[] = {
                p->raw_fitness(0),
                static_cast<double>(p->getNParents()),
                static_cast<double>(p->getNChildren()),
                p->getAdProb(),
                min_sigma,
                max_sigma,
                sigma_range,
                p->getSigmaSigma()
            };
            // The data log is created in INFOINIT, which precedes every per-iteration call.
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            auto &data_log = *data_log_;
            for(std::size_t s = 0; s < ids_.size(); ++s) {
                data_log.append(ids_[s], iteration, values[s]);
            }
        } break;

        case Gem::Geneva::infoMode::INFOEND: {
            // Write out the result, then drop the transient run state.
            if(data_log_.has_value()) {
                data_log_->writeToFile(file_name_);
            }
            data_log_.reset();
            ids_.clear();
        } break;

        default: {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GOptOptMonitorT<ind_type>>: Received invalid infoMode " << im << '\n'
            );
        }
        };
    }

    /***************************************************************************/

    GOptOptMonitorT() =
        default; ///< Default constructor; Intentionally private (only needed for serialization)

    std::string file_name_; ///< The name of the output file

    // Transient run state: the eight progress curves are declared into this data log at INFOINIT,
    // filled in INFOPROCESSING and written at INFOEND. It is NOT part of the serialized config.
    std::optional<Gem::Dietrich::GDataLog> data_log_;
    std::vector<Gem::Dietrich::GDataLog::SeriesId>
        ids_; ///< the eight declared series, in registration order
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
