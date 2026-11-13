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

#include "GRandomSearch.hpp"

// Standard headers go here
#include <cstddef>
#include <memory>
#include <tuple>

// Geneva headers go here
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GenevaHelperFunctions.hpp" // isBetter
#include "geneva/genome/GGenome.hpp"

#include "GRandomSearch_PersonalityTraits.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/** @brief The default population size, used unless overridden by the "size" config option. */
namespace {
constexpr std::size_t GRSEARCH_DEF_POPSIZE = 100;
} // namespace

/******************************************************************************/
/**
 * @brief The default constructor. Establishes a default population size (config-overridable via "size").
 */
GRandomSearch::GRandomSearch() {
    this->setDefaultPopulationSize(GRSEARCH_DEF_POPSIZE);
}

/******************************************************************************/
/**
 * @brief Adds local configuration options to a GParserBuilder object.
 *
 * @param gpb The parser builder to which the configuration options are added
 */
void GRandomSearch::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our parent class'es function first
    GOptimizationAlgorithmBase::addConfigurationOptions_(gpb);

    gpb.registerFileParameter<std::size_t>(
        "size",
        GRSEARCH_DEF_POPSIZE,
        [this](std::size_t ps) { this->setDefaultPopulationSize(ps); }
    ) << "The size of the population (the number of random samples drawn per iteration)";
}

/******************************************************************************/
/**
 * @brief Random search cannot tolerate a missing/failed evaluation: submit under full-success-or-fatal.
 *
 * @return The full-success-or-fatal submission policy
 */
Gem::Courtier::GSubmissionPolicy GRandomSearch::getSubmissionPolicy_() const {
    return Gem::Courtier::GSubmissionPolicy::full_success_or_fatal();
}

/******************************************************************************/
/**
 * @brief The per-iteration business logic: re-randomize the whole population, evaluate it, report the best.
 *
 * @return A tuple holding the raw and transformed fitness of the best individual found this iteration
 */
std::tuple<double, double> GRandomSearch::cycleLogic_() {
    // Draw a fresh random point for every individual and mark it for (re-)evaluation.
    for(auto it = this->begin(); it != this->end(); ++it) {
        (*it)->randomInit(activityMode::ALLPARAMETERS);
        (*it)->set_processing_status(Gem::Courtier::processingStatus::DO_PROCESS);
    }

    // Evaluate the whole population through the process consumer.
    this->evaluatePopulation_();

    // Report this iteration's best; the base class tracks the best-ever across iterations.
    std::tuple<double, double> best_fitness =
        std::make_tuple(this->at(0)->getWorstCase(), this->at(0)->getWorstCase());
    const maxMode m = this->at(0)->getMaxMode(); // uniform across the population
    for(auto it = this->begin(); it != this->end(); ++it) {
        const std::tuple<double, double> new_eval = (*it)->getFitnessTuple();
        if(isBetter(
               std::get<G_TRANSFORMED_FITNESS>(new_eval),
               std::get<G_TRANSFORMED_FITNESS>(best_fitness),
               m)) {
            best_fitness = new_eval;
        }
    }
    return best_fitness;
}

/******************************************************************************/
/**
 * @brief Triggers fitness calculation of the whole population through the process consumer.
 */
void GRandomSearch::evaluatePopulation_() {
    const auto status = this->workOnPopulation(0, this->size());

    if(not status.is_complete || status.has_errors) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GRandomSearch::evaluatePopulation_(): Error!" << '\n'
            << "The population could not be fully evaluated (is_complete=" << status.is_complete
            << ", has_errors=" << status.has_errors << ")." << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Retrieves the number of processable items for the current iteration (the whole population).
 *
 * @return The population size (every individual is (re-)evaluated each iteration)
 */
std::size_t GRandomSearch::getNProcessableItems_() const {
    return this->size();
}

/******************************************************************************/
/**
 * @brief Resizes the population to the configured size by cloning the single seed individual.
 */
void GRandomSearch::adjustPopulation_() {
    const std::size_t n_start = this->size();

    if(n_start == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GRandomSearch::adjustPopulation_(): Error!" << '\n'
            << "You didn't add any individuals to the collection. We need at least one." << '\n'
        );
    }
    if(this->getDefaultPopulationSize() == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GRandomSearch::adjustPopulation_(): Error!" << '\n'
            << "The default population size is 0." << '\n'
        );
    }

    // Keep a single seed, then clone it up to the configured population size.
    if(n_start > 1) {
        this->resize(1);
    }
    for(std::size_t i = 1; i < this->getDefaultPopulationSize(); ++i) {
        this->push_back(this->at(0)->clone());
    }
}

/******************************************************************************/
/**
 * @brief Retrieves a fresh personality-traits object for this algorithm.
 *
 * @return A shared pointer to a new GRandomSearch_PersonalityTraits object
 */
std::shared_ptr<GPersonalityTraits> GRandomSearch::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
