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

#include "geneva/oa/GSimulatedAnnealing.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "common/concurrency/GThreadPool.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GParChild.hpp"
#include "geneva/oa/GSimulatedAnnealing_PersonalityTraits.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/genome/GGenome.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <random>
#include <tuple>
#include <vector>

/******************************************************************************/

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GSimulatedAnnealing) // NOLINT

/******************************************************************************/

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/**
 * @brief The default constructor. All initialization work of member variables
 * is done in the class body.
 */
GSimulatedAnnealing::GSimulatedAnnealing() {
    // Make sure we start with a valid population size if the user does not supply these values
    this->setPopulationSizes(100, 1);
}

/******************************************************************************/
/**
 * @brief Resets the settings of this population to what was configured when
 * the optimize()-call was issued
 */
void GSimulatedAnnealing::resetToOptimizationStart_() {
    // Reset the temperature
    t_ = t0_;

    // There is no more work to be done here, so we simply call the
    // function of the parent class
    GParChild::resetToOptimizationStart_();
}

/******************************************************************************/
/**
  * @brief Adds local configuration options to a GParserBuilder object
  *
  * @param gpb The GParserBuilder object to which configuration options should be added
  */
void GSimulatedAnnealing::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our parent class'es function (registers the shared n_adaption_threads option)
    GParChild::addConfigurationOptions_(gpb);

    // Add local data
    gpb.registerFileParameter<double>(
        "t0" // The name of the variable
        ,
        SA_T0 // The default value
        ,
        [this](double sat0) { this->setT0(sat0); }
    ) << "The start temperature used in simulated annealing";

    gpb.registerFileParameter<double>(
        "alpha" // The name of the variable
        ,
        SA_ALPHA // The default value
        ,
        [this](double ds) { this->setTDegradationStrength(ds); }
    ) << "The degradation strength used in the cooling"
      << '\n'
      << "schedule in simulated annealing;";
}

/******************************************************************************/
/**
  * @brief Determines the strength of the temperature degradation. This function is used for simulated annealing.
  *
  * @param alpha The temperature degradation strength; must be in the open interval (0,1)
  */
void GSimulatedAnnealing::setTDegradationStrength(double alpha) {
    if(alpha <= 0. || alpha >= 1.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSimulatedAnnealing::setTDegradationStrength(const double&):" << '\n'
            << "alpha must be in (0,1) for SA cooling; got: " << alpha << '\n'
        );
    }

    alpha_ = alpha;
}

/******************************************************************************/
/**
  * @brief Retrieves the temperature degradation strength. This function is used for simulated annealing.
  *
  * @return The temperature degradation strength
  */
double GSimulatedAnnealing::getTDegradationStrength() const {
    return alpha_;
}

/******************************************************************************/
/**
  * @brief Sets the start temperature. This function is used for simulated annealing.
  *
  * @param t0 The start temperature; must be strictly positive
  */
void GSimulatedAnnealing::setT0(double t0) {
    if(t0 <= 0.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSimulatedAnnealing::setT0(const double&):" << '\n'
            << "Got negative start temperature: " << t0 << '\n'
        );
    }

    t0_ = t0;
}

/******************************************************************************/
/**
  * @brief Retrieves the start temperature. This function is used for simulated annealing.
  *
  * @return The start temperature
  */
double GSimulatedAnnealing::getT0() const {
    return t0_;
}

/******************************************************************************/
/**
  * @brief Retrieves the current temperature. This function is used for simulated annealing.
  *
  * @return The current temperature
  */
double GSimulatedAnnealing::getT() const {
    return t_;
}

/******************************************************************************/
/**
  * @brief Some error checks related to population sizes
  */
void GSimulatedAnnealing::populationSanityChecks_() const {
    // First check that we have been given a suitable value for the number of parents.
    // Note that a number of checks (e.g. population size != 0) has already been done
    // in the parent class.
    if(this->n_parents_ == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSimulatedAnnealing::populationSanityChecks(): Error!" << '\n'
            << "Number of parents is set to 0"
        );
    }

    // We need at least as many children as parents
    std::size_t const pop_size = this->getPopulationSize();
    if(pop_size <= this->n_parents_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSimulatedAnnealing::populationSanityChecks() :" << '\n'
            << "Requested size of population is too small :" << pop_size << " " << this->n_parents_
            << '\n'
        );
    }
}


/******************************************************************************/
/**
  * @brief We submit individuals to the process consumer and wait for processed items.
 */
void GSimulatedAnnealing::evaluatePopulation_() {
    //--------------------------------------------------------------------------------
    // Start by marking the work to be done in the individuals.
    // "range" will hold the start- and end-points of the range
    // to be worked on
    std::tuple<std::size_t, std::size_t> range = getEvaluationRange_();

#ifdef DEBUG
    // There should be no situation in which a "clean" child is submitted
    // through this function. There MAY be situations, where in the first iteration
    // parents are clean, e.g. when they were extracted from another optimization.
    for(std::size_t i = this->getNParents(); i < this->size(); i++) {
        if(not this->at(i)->is_due_for_processing()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GSimulatedAnnealing::evaluatePopulation_(): Error!" << '\n'
                << "Tried to evaluate children in range " << std::get<0>(range) << " - "
                << std::get<1>(range) << '\n'
                << "but found \"clean\" individual in position " << i << '\n'
            );
        }
    }
#endif

    //--------------------------------------------------------------------------------
    // Submit the [start, end) evaluation range and wait for results. courtier marks the span
    // DO_PROCESS and reconciles it in place -- no per-item flagging needed.
    auto status = this->workOnPopulation(std::get<0>(range), std::get<1>(range));

    //--------------------------------------------------------------------------------
    // Take care of unprocessed items, if these exist. We simply remove them and continue.
    this->discardUnusableItems_(status, "GSimulatedAnnealing::evaluatePopulation_()");

    //--------------------------------------------------------------------------------
    // Now fix the population -- it may be smaller than its nominal size
    fixAfterJobSubmission();
}


/******************************************************************************/
/**
  * @brief Choose new parents, based on the SA selection scheme.
  */
void GSimulatedAnnealing::selectBest_() {
    // Sort according to the "Simulated Annealing" scheme
    sortSAMode();

    // Let parents know they are parents
    this->markParents();

#ifdef DEBUG
    // Make sure our population is not smaller than its nominal size -- this
    // should have been taken care of in fixAfterJobSubmission() .
    if(this->size() < this->getDefaultPopulationSize()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSimulatedAnnealing::selectBest(): Error!" << '\n'
            << "Size of population is smaller than expected: " << this->size() << " / "
            << this->getDefaultPopulationSize() << '\n'
        );
    }
#endif /* DEBUG */

    ////////////////////////////////////////////////////////////
    // At this point we have a sorted list of individuals and can take care of
    // too many members, so the next iteration finds a "standard" population. This
    // function will remove the last items.
    this->resize(this->getNParents() + this->getDefaultNChildren());

    // Let children know they are children
    this->markChildren();

    // Everything should be back to normal ...
}

/******************************************************************************/
/**
  * @brief Retrieves the evaluation range in a given iteration and sorting scheme. Depending on the
  * iteration and sorting scheme, the start point will be different. The end-point is not meant
  * to be inclusive.
  *
  * @return A tuple holding the half-open [start, end) range of population positions to be evaluated
  */
/******************************************************************************/
/**
  * @brief Retrieve a GPersonalityTraits object belonging to this algorithm
  *
  * @return A shared pointer to a freshly created GSimulatedAnnealing_PersonalityTraits object
  */
std::shared_ptr<GPersonalityTraits> GSimulatedAnnealing::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/
/**
 * @brief Performs a simulated annealing style sorting and selection
 */
//! [sa-sort-mode]
void GSimulatedAnnealing::sortSAMode() {
    // Position the n_parents best children of the population right behind the parents
    std::ranges::partial_sort(
        this->begin() + this->n_parents_,
        this->begin() + 2 * this->n_parents_,
        this->end(),
        std::ranges::less{},
        [](const auto &p) static { return minOnly_transformed_fitness(*p); }
    );

    // Check for each parent whether it should be replaced by the corresponding child
    for(std::size_t np = 0; np < this->n_parents_; np++) {
        double const p_pass = saProb(
            minOnly_transformed_fitness((*this->at(np))),
            minOnly_transformed_fitness((*this->at(this->n_parents_ + np)))
        );
        if(p_pass >= 1.) {
            this->at(np)->load(this->at(this->n_parents_ + np));
        }
        else {
            double const challenge = this->uniform_real_distribution_(
                this->gr_,
                std::uniform_real_distribution<double>::param_type(0., 1.)
            );
            if(challenge < p_pass) {
                this->at(np)->load(this->at(this->n_parents_ + np));
            }
        }
    }

    // Sort the new parents -- it is possible that a child with a worse fitness has replaced a parent
    std::ranges::sort(
        this->begin(),
        this->begin() + this->n_parents_,
        std::ranges::less{},
        [](const auto &p) static { return minOnly_transformed_fitness(*p); }
    );

    // Make sure the temperature gets updated
    updateTemperature();
}
//! [sa-sort-mode]

/******************************************************************************/
/**
  * @brief Calculates the simulated annealing probability for a child to replace a parent.
  * Note that this function only sees minimization problems, as maximization problems
  * are transformed to minimization problems inside of GGenome.
  *
  * @param f_min_only_parent The "min only" (minimization-transformed) fitness of the parent
  * @param f_min_only_child The "min only" (minimization-transformed) fitness of the child
  * @return A double value representing the Boltzmann likelihood for the child to replace the parent
  */
//! [sa-cooling#1]
double
GSimulatedAnnealing::saProb(const double &f_min_only_parent, const double &f_min_only_child) const {
    // Minimisation: a worse child has f_child > f_parent, so the exponent is negative and the result
    // lies in (0, 1); a child that is at least as good gives a non-negative exponent, hence a result
    // >= 1, which the caller treats as "always accept". t_ is floored > 0 by updateTemperature(), so
    // there is no division by zero.
    return exp(-(f_min_only_child - f_min_only_parent) / t_);
}
//! [sa-cooling#1]

/******************************************************************************/
/**
  * @brief Updates the temperature. This function is used for simulated annealing.
  */
//! [sa-cooling#2]
void GSimulatedAnnealing::updateTemperature() {
    // Clamp to the smallest normalised double so t_ never enters the subnormal range or reaches 0.
    // With t_ == 0 and a fitness gap of 0, saProb() would compute 0/0 = NaN; the floor prevents that
    // without changing late-phase behaviour: exp(-(f_child - f_parent) / min()) is approximately 0 for
    // any positive gap, so worse candidates are effectively never accepted once the floor is reached.
    // (std::max applies the floor AFTER the multiply, so even if t_ * alpha_ underflows to 0 the result
    // is min(), never 0.)
    t_ = std::max(t_ * alpha_, std::numeric_limits<double>::min());
}
//! [sa-cooling#2]

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
