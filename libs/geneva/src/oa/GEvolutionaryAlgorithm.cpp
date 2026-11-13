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

#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)

#include <atomic>
#include <memory>
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "common/concurrency/GThreadPool.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#ifdef GEM_TESTING
#include "geneva/individuals/GTestIndividual1.hpp"
#endif /* GEM_TESTING */
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GParChild.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GParetoTools.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeFixedSizePriorityQueue.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iterator>
#include <ostream>
#include <ranges>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#endif /* GEM_TESTING */

/******************************************************************************/

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GEvolutionaryAlgorithm) // NOLINT

/******************************************************************************/

namespace Gem::Geneva::OptimizationAlgorithms {

using GType = GEvolutionaryAlgorithm;

/******************************************************************************/
/**
 * @brief The default constructor. Sets a valid default population size.
 */
GEvolutionaryAlgorithm::GEvolutionaryAlgorithm() {
    this->setPopulationSizes(100, 1);
}

/******************************************************************************/

void GType::resetToOptimizationStart_() {
    GParChild::resetToOptimizationStart_();
}

/******************************************************************************/

void GType::setSortingScheme(const sortingMode smode) {
    sorting_mode_ = smode;
}

sortingMode GType::getSortingScheme() const {
    return sorting_mode_;
}

/******************************************************************************/

void GType::setStepControl(stepControl sc) {
    step_control_ = sc;
}

stepControl GType::getStepControl() const {
    return step_control_;
}

void GType::setLearningRateConstant(double c) {
    learning_rate_c_ = c;
}

double GType::getLearningRateConstant() const {
    return learning_rate_c_;
}

void GType::setSigmaRecombination(bool r) {
    recombine_sigma_ = r;
}

bool GType::getSigmaRecombination() const {
    return recombine_sigma_;
}

/******************************************************************************/

void GType::extractCurrentParetoIndividuals(
    std::vector<std::unique_ptr<gen::GGenome>> &pareto_inds
) {
    pareto_inds.clear();
    // An individual is on the (first) Pareto front iff no other individual strictly dominates it. Computed
    // directly here via the shared paretoDominates(), so the best-archive does not depend on any
    // selection-time tagging.
    const std::size_t sz = this->size();
    for(std::size_t i = 0; i < sz; ++i) {
        bool dominated = false;
        for(std::size_t j = 0; j < sz; ++j) {
            if(i != j && paretoDominates((*this->at(j)), (*this->at(i)))) {
                dominated = true;
                break;
            }
        }
        if(not dominated) {
            pareto_inds.push_back(this->at(i)->clone<gen::GGenome>());
        }
    }
}

/******************************************************************************/

Gem::Courtier::submission_status_t
GType::evaluatePopulationRange_(std::size_t start, std::size_t end) {
    if(inline_evaluation_) {
        end = std::min(end, this->size());
        bool has_errors = false;
        for(std::size_t i = start; i < end; ++i) {
            this->at(i)->process();
            if(this->at(i)->has_errors()) {
                has_errors = true;
            }
        }
        return Gem::Courtier::submission_status_t{.is_complete=true, .has_errors=has_errors};
    }
    return this->workOnPopulation(start, end);
}

/******************************************************************************/

std::vector<TunableParam> GType::tunableManifest() {
    namespace n = ea_tunable;
    return {
        {n::n_parents, true, 1., 1., 6.},
        {n::n_children, true, 100., 5., 250.},
        {n::amalgamation, false, 0., 0., 1.},
        {n::min_ad_prob, false, 0., 0., 0.1},
        {n::ad_prob_range, false, 0.9, 0.1, 0.9},
        {n::ad_prob_start_pct, false, 1., 0., 1.},
        {n::adapt_ad_prob, false, 0.1, 0., 1.},
        {n::min_sigma, false, 0.001, 0.001, 0.09999},
        {n::sigma_range, false, 0.2, 0.1, 0.9},
        {n::sigma_range_pct, false, 1., 0., 1.},
        {n::sigma_sigma, false, 0.1, 0., 1.}
    };
}

/******************************************************************************/

void GType::updateGlobalBestsPQ_(
    gen::GGenomeFixedSizePriorityQueue &best_individuals
) {
    constexpr bool donotreplace = false;
    constexpr bool replace = true;

#ifdef DEBUG
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GEvolutionaryAlgorithm::updateGlobalBestsPQ_() :" << '\n'
            << "Tried to retrieve the best individuals even though the population is empty." << '\n'
        );
    }
#endif /* DEBUG */

    switch(sorting_mode_) {
    case sortingMode::MUPLUSNU_SINGLEEVAL:
    case sortingMode::MUNU1PRETAIN_SINGLEEVAL:
    case sortingMode::MUCOMMANU_SINGLEEVAL:
        best_individuals.addClone(
            this->data_cnt_.begin(),
            this->data_cnt_.begin() + this->getNParents(),
            donotreplace
        );
        break;

    case sortingMode::MUPLUSNU_PARETO:
    case sortingMode::MUCOMMANU_PARETO: {
        // extractCurrentParetoIndividuals() already hands out fresh clones, so the archive takes
        // those over rather than cloning them a second time.
        std::vector<std::unique_ptr<gen::GGenome>> pareto_inds;
        this->extractCurrentParetoIndividuals(pareto_inds);
        best_individuals.add(std::move(pareto_inds), replace);
    } break;
    }
}

/******************************************************************************/

void GType::updateIterationBestsPQ_(
    gen::GGenomeFixedSizePriorityQueue &best_individuals
) {
    constexpr bool donotreplace = false;
    constexpr bool replace = true;

#ifdef DEBUG
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "GEvolutionaryAlgorithm::updateIterationBestsPQ_() :" << '\n'
            << "Tried to retrieve the best individuals even though the population is empty." << '\n'
        );
    }
#endif /* DEBUG */

    switch(sorting_mode_) {
    case sortingMode::MUPLUSNU_SINGLEEVAL:
    case sortingMode::MUNU1PRETAIN_SINGLEEVAL:
    case sortingMode::MUCOMMANU_SINGLEEVAL: {
        best_individuals.addClone(
            this->data_cnt_.begin(),
            this->data_cnt_.begin() + this->getNParents(),
            donotreplace
        );
    } break;

    case sortingMode::MUPLUSNU_PARETO:
    case sortingMode::MUCOMMANU_PARETO: {
        // See updateGlobalBestsPQ_(): the extracted individuals are already clones.
        std::vector<std::unique_ptr<gen::GGenome>> pareto_inds;
        this->extractCurrentParetoIndividuals(pareto_inds);
        best_individuals.add(std::move(pareto_inds), replace);
    } break;
    }
}

/******************************************************************************/

void GType::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    GParChild::addConfigurationOptions_(gpb);

    gpb.registerFileParameter<sortingMode>(
        "sorting_method",
        DEFAULTEASORTINGMODE,
        [this](sortingMode sm) { this->setSortingScheme(sm); }
    ) << "The sorting scheme. Options"
      << '\n'
      << "0: MUPLUSNU mode with a single evaluation criterion" << '\n'
      << "1: MUCOMMANU mode with a single evaluation criterion" << '\n'
      << "2: MUCOMMANU mode with single evaluation criterion," << '\n'
      << "   the best parent of the last iteration is retained" << '\n'
      << "   unless a better individual has been found" << '\n'
      << "3: MUPLUSNU mode for multiple evaluation criteria, pareto selection" << '\n'
      << "4: MUCOMMANU mode for multiple evaluation criteria, pareto selection";

    gpb.registerFileParameter<std::uint8_t>(
        "step_control",
        std::to_underlying(stepControl::SELF_ADAPT_SCALED),
        [this](std::uint8_t sc) { this->setStepControl(static_cast<stepControl>(sc)); }
    ) << "The step-size control strategy. Options"
      << '\n'
      << "0: SELF_ADAPT (classic mutative sigma self-adaption, the legacy \"ea\")" << '\n'
      << "1: SELF_ADAPT_SCALED (dimension-scaled tau = c/sqrt(2n)) [default]" << '\n'
      << "2: ONE_FIFTH (Rechenberg 1/5 success rule on a single global sigma)" << '\n'
      << "3: CSA (cumulative step-size adaptation on a single global sigma)";

    gpb.registerFileParameter<double>(
        "learning_rate_c",
        1.,
        [this](double c) { this->setLearningRateConstant(c); }
    ) << "The learning-rate constant c used by SELF_ADAPT_SCALED (tau = c/sqrt(2n))";

    gpb.registerFileParameter<bool>(
        "recombine_sigma",
        true,
        [this](bool r) { this->setSigmaRecombination(r); }
    ) << "Whether to intermediate-recombine the per-individual sigma after recombination";
}

/******************************************************************************/

void GType::populationSanityChecks_() const {
    if(this->n_parents_ == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GEvolutionaryAlgorithm::populationSanityChecks(): Error!" << '\n'
            << "Number of parents is set to 0"
        );
    }

    std::size_t const pop_size = this->getPopulationSize();
    if(
        ((sorting_mode_ == sortingMode::MUCOMMANU_SINGLEEVAL ||
          sorting_mode_ == sortingMode::MUNU1PRETAIN_SINGLEEVAL ||
          sorting_mode_ == sortingMode::MUCOMMANU_PARETO) &&
         (pop_size < 2 * this->n_parents_)) ||
        ((sorting_mode_ == sortingMode::MUPLUSNU_SINGLEEVAL ||
          sorting_mode_ == sortingMode::MUPLUSNU_PARETO) &&
         pop_size <= this->n_parents_)
    ) {
        std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)
        error << "In GEvolutionaryAlgorithm::populationSanityChecks() :" << '\n'
              << "Requested size of population is too small :" << pop_size << " "
              << this->n_parents_ << '\n';
        throw geneva_exception(g_error_streamer(DO_LOG, Gem::Common::timeAndPlace()) << error.str());
    }
}

/******************************************************************************/

void GType::evaluatePopulation_() {
    const std::tuple<std::size_t, std::size_t> range = getEvaluationRange_();

#ifdef DEBUG
    for(std::size_t i = this->getNParents(); i < this->size(); i++) {
        if(not this->at(i)->is_due_for_processing()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GEvolutionaryAlgorithm::evaluatePopulation_(): Error!" << '\n'
                << "Tried to evaluate children in range " << std::get<0>(range) << " - "
                << std::get<1>(range) << '\n'
                << "but found \"clean\" individual in position " << i << '\n'
            );
        }
    }

    if(this->size() != this->getDefaultPopulationSize()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GEvolutionaryAlgorithm::evaluatePopulation_(): Error!" << '\n'
            << "Size of data vector (" << this->size() << ") should be "
            << this->getDefaultPopulationSize() << '\n'
        );
    }
#endif

    auto status = this->evaluatePopulationRange_(std::get<0>(range), std::get<1>(range));

    this->discardUnusableItems_(status, "GEvolutionaryAlgorithm::evaluatePopulation_()");

    fixAfterJobSubmission();
}

/******************************************************************************/

void GType::selectBest_() {
#ifdef DEBUG
    if((this->size() - this->n_parents_) < this->default_n_children_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GEvolutionaryAlgorithm::select():" << '\n'
            << "Too few children. Got " << (this->size() - this->getNParents()) << "," << '\n'
            << "but was expecting at least " << this->getDefaultNChildren() << '\n'
        );
    }
#endif /* DEBUG */

    // Measure the offspring success rate (children vs. their OWN parent) NOW, while the population is
    // still laid out as parents [0, np) + children [np, size) -- i.e. before the sort below reorders
    // it. driveGlobalSigmaController() (called at the end, after selection) consumes the stored value.
    this->measureOffspringSuccess_();

    switch(sorting_mode_) {
    case sortingMode::MUPLUSNU_SINGLEEVAL: {
        this->sortMuPlusNuMode();
    } break;

    case sortingMode::MUNU1PRETAIN_SINGLEEVAL: {
        if(1 == n_parents_ || this->inFirstIteration()) {
            this->sortMuPlusNuMode();
        }
        else {
            this->sortMunu1pretainMode();
        }
    } break;

    case sortingMode::MUCOMMANU_SINGLEEVAL: {
        if(this->inFirstIteration()) {
            this->sortMuPlusNuMode();
        }
        else {
            this->sortMuCommaNuMode();
        }
    } break;

    case sortingMode::MUPLUSNU_PARETO:
        this->sortMuPlusNuParetoMode();
        break;

    case sortingMode::MUCOMMANU_PARETO: {
        if(this->inFirstIteration()) {
            this->sortMuPlusNuParetoMode();
        }
        else {
            this->sortMuCommaNuParetoMode();
        }
    } break;
    }

    this->markParents();

#ifdef DEBUG
    if(this->size() < this->getDefaultPopulationSize()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GEvolutionaryAlgorithm::selectBest(): Error!" << '\n'
            << "Size of population is smaller than expected: " << this->size() << " / "
            << this->getDefaultPopulationSize() << '\n'
        );
    }
#endif /* DEBUG */

    this->resize(this->getNParents() + this->getDefaultNChildren());

    // Drive the global-sigma controller (a no-op for the self-adaptive modes), now that the new parents
    // have been selected and sit at the front of the population.
    this->driveGlobalSigmaController();
}

/******************************************************************************/

std::shared_ptr<GPersonalityTraits> GType::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/
// Step-control machinery
/******************************************************************************/

void GType::init() {
    // GParChild::init() adopts + validates the provided adaption config and seeds each slot's scratch.
    GParChild::init();

    // Mirror the chosen knobs onto the OA-owned config so the kernels read them.
    installStepController();
}

/******************************************************************************/
/**
 * @brief Installs the chosen step controller onto the OA-owned adaption config (after GParChild::init()
 * adopted and seeded it). For SELF_ADAPT_SCALED the per-group sigma_sigma is rescaled to the textbook
 * dimension-aware value; for ONE_FIFTH / CSA the per-group log-normal self-adaption is suppressed (the
 * global controller owns sigma) and the global-sigma run scratch is seeded from the config's seed sigma.
 */
void GType::installStepController() {
    auto cfg = this->adaption_config_;
    if(not cfg) {
        return; // a non-adapting / non-flat individual: nothing to install
    }

    cfg->setStepControl(step_control_);
    cfg->setLearningRateConstant(learning_rate_c_);
    controller_dim_ = cfg->adaptedDimension();

    switch(step_control_) {
    case stepControl::SELF_ADAPT:
        // Classic behaviour -- leave the config exactly as authored.
        break;

    case stepControl::SELF_ADAPT_SCALED:
        cfg->applyScaledSelfAdaptionRate();
        break;

    case stepControl::ONE_FIFTH:
    case stepControl::CSA: {
        // The global controller owns sigma: stop the per-group log-normal self-adaption.
        cfg->suppressPerGroupSigmaSelfAdaption();
        // Seed the global sigma from a representative sigma in the slots' scratch. On a fresh run
        // that is the config's seed sigma (GParChild::init() just installed it); on a checkpoint
        // resume it is the EVOLVED sigma the interrupted run had reached (the scratch is
        // serialized), so the controller resumes where it left off instead of restarting at 1.0.
        global_sigma_ = 1.;
        p_sigma_ = 0.;
        last_p_success_ = 0.;
        controller_warmed_up_ = false;
        if(not this->empty()) {
            global_sigma_ = readRepresentativeSigma(this->at(0)->scratch(), *cfg, 1.);
            // Push the global sigma into every slot so the first adaption uses it uniformly.
            for(auto const &slot : *this) {
                writeGlobalSigma(slot->scratch(), *cfg, global_sigma_);
            }
        }
    } break;
    }
}

/******************************************************************************/
/**
 * @brief Drives the single-global-sigma controllers (ONE_FIFTH / CSA) once per generation, after the new
 * parents have been selected. A no-op for the self-adaptive modes (their sigma lives in the per-group
 * state and is recombined / log-normally self-adapted by the kernels).
 *
 * - ONE_FIFTH: estimate the success rate as the fraction of children that beat the previous best parent
 *   and apply the Rechenberg rule (sigma *= exp((p_success - 1/5) / d) with a damping d ~ 1/3). Since the
 *   number of trials per generation is large, this is a stable, well-behaved global step controller.
 * - CSA: a scalar evolution-path proxy accumulates the SIGN of the generation's improvement; sigma is
 *   then driven up when the path is consistently positive (improving) and down otherwise. This is the
 *   O(1) scalar analogue of the vector CSA evolution path, sufficient for an isotropic global sigma.
 *
 * The updated global sigma is clamped to a sane range and pushed into every slot's scratch so the next
 * generation's adaption (and the children cloned from these parents) uses it.
 */
void GType::measureOffspringSuccess_() {
    if(step_control_ != stepControl::ONE_FIFTH && step_control_ != stepControl::CSA) {
        return;
    }
    last_p_success_ = 0.;
    // The first generation has no meaningful parent-child lineage yet (comma mode even sorts as plus in
    // iteration 0); skip -- driveGlobalSigmaController() will hold sigma until we have a real measurement.
    if(this->inFirstIteration() || this->empty()) {
        return;
    }

    // At this point selection has NOT yet run: the parents that produced this generation's children sit
    // at [0, n_parents_) and the children at [n_parents_, size()). Rechenberg's success signal is the
    // fraction of children that IMPROVED ON THEIR OWN PARENT -- measured per offspring, pre-selection.
    // Each child records the population position of the parent it descended from (set during
    // recombination), so we compare the child's fitness against exactly that parent's fitness. This is a
    // monotone, well-posed signal in every sorting mode (unlike "survivors vs. last generation's best",
    // whose reference degrades when sigma overshoots under comma selection and drives sigma to run away).
    const std::size_t np = this->getNParents();
    std::size_t n_children = 0;
    std::size_t n_success = 0;
    for(std::size_t ci = np; ci < this->size(); ++ci) {
        const auto traits =
            this->at(ci)->template getPersonalityTraits<GBaseParChildPersonalityTraits>();
        if(not traits->parentIdSet()) {
            continue; // no recorded lineage (e.g. a cross-over child) -- leave it out of the estimate
        }
        const std::size_t parent_id = traits->getParentId();
        if(parent_id >= np) {
            continue; // defensive: a stale/out-of-range id cannot be scored against a current parent
        }
        ++n_children;
        if(minOnly_transformed_fitness(*this->at(ci)) < minOnly_transformed_fitness(*this->at(parent_id))) {
            ++n_success;
        }
    }
    if(n_children > 0) {
        last_p_success_ = static_cast<double>(n_success) / static_cast<double>(n_children);
    }
}

/******************************************************************************/

void GType::driveGlobalSigmaController() {
    if(step_control_ != stepControl::ONE_FIFTH && step_control_ != stepControl::CSA) {
        return;
    }
    auto cfg = this->adaption_config_;
    if(not cfg || this->empty()) {
        return;
    }

    // The success rate was measured before selection reordered the population (measureOffspringSuccess_):
    // the fraction of children that beat their own parent -- the textbook Rechenberg signal.
    const double p_success = last_p_success_;

    // Skip the very first measured generation (warm-up): measureOffspringSuccess_ leaves last_p_success_
    // at 0 in iteration 0, which would otherwise spuriously shrink sigma before any real signal exists.
    if(controller_warmed_up_) {
        if(step_control_ == stepControl::ONE_FIFTH) {
            // Rechenberg 1/5 success rule: grow sigma while more than 1/5 of the offspring improve on
            // their parent, shrink it otherwise. A responsive damping lets sigma track the success rate.
            constexpr double target = 1. / 5.;
            constexpr double damping = 0.2;
            global_sigma_ *= std::exp((p_success - target) / (1. + damping));
        }
        else { // CSA (scalar success-rate path)
            // A scalar evolution path that accumulates the success-rate deviation from the 1/5 target.
            // This is the O(1) analogue of the CSA evolution path: a persistently > 1/5 success rate
            // pushes the path positive (grow sigma); a persistently < 1/5 rate pushes it negative
            // (shrink). The cumulation constant gives it memory; the damping keeps each step mild.
            const double n = std::max<double>(1., static_cast<double>(controller_dim_));
            const double c_sigma = 1. / (1. + std::sqrt(n) / 4.); // O(1/sqrt(n)) time constant, floored
            constexpr double target = 1. / 5.;
            const double signal = (p_success - target) / (1. - target); // in [-0.25, +1]
            p_sigma_ = ((1. - c_sigma) * p_sigma_) + (std::sqrt(c_sigma * (2. - c_sigma)) * signal);
            global_sigma_ *= std::exp(0.3 * p_sigma_);
        }
    }

    // Clamp to the authored per-group sigma band so the controller can never explode or collapse. In the
    // normalized coordinate model sigma is a FRACTION of the parameter range, so the config's max_sigma is
    // the meaningful ceiling (the former hard-coded 10.0 was 10x the whole range -- no bound at all).
    const double max_sigma = readRepresentativeMaxSigma(*cfg, 0.5);
    global_sigma_ = std::clamp(global_sigma_, 1e-12, max_sigma);

    // Push the new global sigma into every slot (parents now at the front; children get it on the next
    // recombine via the whole-slot copy, but writing all slots keeps the state coherent for telemetry).
    for(auto const &slot : *this) {
        writeGlobalSigma(slot->scratch(), *cfg, global_sigma_);
    }

    controller_warmed_up_ = true;
}

/******************************************************************************/
/**
 * @brief Recombination, then (optionally) intermediate recombination of the per-individual sigma.
 *
 * The base recombine() copies the chosen parent's whole slot (sigma scratch included) into each child,
 * so each child inherits exactly ONE parent's sigma. For the per-individual-sigma modes (SELF_ADAPT /
 * SELF_ADAPT_SCALED) we additionally average the surviving parents' sigma into every child -- the
 * "intermediate recombination of sigma" the classic GParChild lacks, which reduces sigma's variance and
 * markedly improves fine convergence. A no-op for the global-sigma modes (every slot already shares one
 * sigma) and when sigma-recombination is disabled.
 */
void GType::recombine() {
    GParChild::recombine();

    if(not recombine_sigma_) {
        return;
    }
    if(step_control_ != stepControl::SELF_ADAPT && step_control_ != stepControl::SELF_ADAPT_SCALED) {
        return; // the global-sigma modes share one sigma already
    }
    auto cfg = this->adaption_config_;
    if(not cfg || this->empty()) {
        return;
    }

    const std::size_t np = this->getNParents();
    if(np < 2) {
        return; // nothing to average with a single parent
    }

    // Mean of the parents' representative sigma.
    double sum = 0.;
    std::size_t cnt = 0;
    for(std::size_t i = 0; i < np; ++i) {
        const double s = readRepresentativeSigma(this->at(i)->scratch(), *cfg, -1.);
        if(s >= 0.) {
            sum += s;
            ++cnt;
        }
    }
    if(cnt == 0) {
        return;
    }
    const double mean_sigma = sum / static_cast<double>(cnt);

    // Write the mean into every child's scratch (children are at [np, size())).
    for(auto const &child : GOptimizationAlgorithmBase::data_cnt_ | std::views::drop(np)) {
        writeGlobalSigma(child->scratch(), *cfg, mean_sigma);
    }
}

/******************************************************************************/
// Selection helpers (verbatim from GEvolutionaryAlgorithm).
/******************************************************************************/

void GType::sortMuPlusNuMode() {
    std::ranges::partial_sort(
        GOptimizationAlgorithmBase::data_cnt_.begin(),
        GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_,
        GOptimizationAlgorithmBase::data_cnt_.end(),
        std::ranges::less{},
        [](const auto &p) static { return minOnly_transformed_fitness(*p); }
    );
}

/******************************************************************************/

void GType::sortMuCommaNuMode() {
    //! [algo-ea:comma-selection]
    std::ranges::partial_sort(
        GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_,
        GOptimizationAlgorithmBase::data_cnt_.begin() + 2 * n_parents_,
        GOptimizationAlgorithmBase::data_cnt_.end(),
        std::ranges::less{},
        [](const auto &p) static { return minOnly_transformed_fitness(*p); }
    );

    std::swap_ranges(
        GOptimizationAlgorithmBase::data_cnt_.begin(),
        GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_,
        GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_
    );
    //! [algo-ea:comma-selection]
}

/******************************************************************************/

void GType::sortMunu1pretainMode() {
    std::ranges::partial_sort(
        GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_,
        GOptimizationAlgorithmBase::data_cnt_.begin() + 2 * n_parents_,
        GOptimizationAlgorithmBase::data_cnt_.end(),
        std::ranges::less{},
        [](const auto &p) static { return minOnly_transformed_fitness(*p); }
    );

    double const best_child = minOnly_transformed_fitness(
        (*(*(GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_)))
    );
    double const best_parent =
        minOnly_transformed_fitness((*(*(GOptimizationAlgorithmBase::data_cnt_.begin()))));

    if(best_child < best_parent) {
        std::swap_ranges(
            GOptimizationAlgorithmBase::data_cnt_.begin(),
            GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_,
            GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_
        );
    }
    else {
        std::swap_ranges(
            GOptimizationAlgorithmBase::data_cnt_.begin() + 1,
            GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_,
            GOptimizationAlgorithmBase::data_cnt_.begin() + n_parents_
        );
    }
}

/******************************************************************************/

void GType::selectParetoParents(bool include_parents) {
    const std::size_t sz = this->size();
    const std::size_t start = include_parents ? static_cast<std::size_t>(0) : this->n_parents_;

    // Rank the eligible individuals (the whole population for mu+nu, only the children for mu,nu) by the
    // shared NSGA-II order: non-dominated front first, ties within a front broken by DECREASING crowding
    // distance. The leading n_parents_ become the survivors. Crowding makes an over-full front keep a
    // well-SPREAD subset (the former implementation kept a RANDOM subset of the first front and ranked the
    // remainder by a single-objective scalar, which lost coverage of the trade-off surface).
    std::vector<const gen::GGenome *> eligible;
    eligible.reserve(sz - start);
    for(std::size_t i = start; i < sz; ++i) {
        eligible.push_back(&(*this->at(i)));
    }
    const std::vector<std::size_t> order = nonDominatedRank(eligible); // best-first, local to [start, sz)

    // Rebuild the population: eligible individuals in NSGA-II order (so [0, n_parents_) are the survivors),
    // then -- for mu,nu -- the discarded old parents at the tail (overwritten by the next recombination).
    std::vector<std::unique_ptr<gen::GGenome>> reordered;
    reordered.reserve(sz);
    for(std::size_t const local : order) {
        reordered.push_back(std::move(this->data_cnt_[start + local]));
    }
    if(not include_parents) {
        std::ranges::move(this->data_cnt_ | std::views::take(this->n_parents_), std::back_inserter(reordered));
    }
    this->data_cnt_ = std::move(reordered);

    // Order the surviving parent block by the min-only scalar fitness -- the EA convention (parent[0] is
    // the single-objective best for reporting, and the rank drives the recombination weighting). The
    // NSGA-II step already decided WHICH mu survive; this only orders that block.
    std::ranges::sort(
        this->begin(),
        this->begin() + this->n_parents_,
        std::ranges::less{},
        [](const auto &p) static { return minOnly_transformed_fitness(*p); }
    );
}

/******************************************************************************/

void GType::sortMuPlusNuParetoMode() {
    if(not(*this->begin())->hasMultipleFitnessCriteria()) {
        static std::atomic<bool> warned{false};
        if(not warned.exchange(true)) {
            glogger << "In GEvolutionaryAlgorithm::sortMuPlusNuParetoMode(): Warning!" << '\n'
                    << "A PARETO sorting mode was selected, but the individuals expose only a single" << '\n'
                    << "fitness criterion. Pareto selection therefore degenerates to single-objective" << '\n'
                    << "MUPLUSNU selection." << '\n'
                    << GWARNING;
        }
        this->sortMuPlusNuMode();
        return;
    }
    // mu+nu: parents AND children compete for the mu survivor slots.
    this->selectParetoParents(/* include_parents = */ true);
}

/******************************************************************************/

void GType::sortMuCommaNuParetoMode() {
    if(not(*this->begin())->hasMultipleFitnessCriteria()) {
        static std::atomic<bool> warned{false};
        if(not warned.exchange(true)) {
            glogger << "In GEvolutionaryAlgorithm::sortMuCommaNuParetoMode(): Warning!" << '\n'
                    << "A PARETO sorting mode was selected, but the individuals expose only a single" << '\n'
                    << "fitness criterion. Pareto selection therefore degenerates to single-objective" << '\n'
                    << "MUCOMMANU selection." << '\n'
                    << GWARNING;
        }
        this->sortMuCommaNuMode();
        return;
    }
    // mu,nu: only the children compete; the old parents are discarded.
    this->selectParetoParents(/* include_parents = */ false);
}

/******************************************************************************/

bool GType::modify_GUnitTests_() {
#ifdef GEM_TESTING
    bool result = false;

    if(GOptimizationAlgorithmBase::modify_GUnitTests_()) {
        result = true;
    }

    if(sortingMode::MUPLUSNU_SINGLEEVAL == this->getSortingScheme()) {
        this->setSortingScheme(sortingMode::MUCOMMANU_SINGLEEVAL);
    }
    else {
        this->setSortingScheme(sortingMode::MUPLUSNU_SINGLEEVAL);
    }
    result = true;

    return result;

#else /* GEM_TESTING */
    Gem::Common::condnotset("GEvolutionaryAlgorithm::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif                  /* GEM_TESTING */
}

/******************************************************************************/

void GType::fillWithObjects(const std::size_t &n_individuals) {
#ifdef GEM_TESTING
    CHECK_NOTHROW(this->clear());

    for(std::size_t i = 0; i < n_individuals; i++) {
        this->push_back(
            std::make_unique<Gem::Geneva::Individuals::GTestIndividual1>());
    }

    for(const auto &ind_ptr : *this) {
        ind_ptr->randomInit(activityMode::ALLPARAMETERS);
    }

#else /* GEM_TESTING */
    Gem::Common::condnotset("GEvolutionaryAlgorithm::fillWithObjects", "GEM_TESTING");
#endif                  /* GEM_TESTING */
}

/******************************************************************************/

// NOLINTNEXTLINE(readability-function-size) -- self-test entry point for GEvolutionaryAlgorithm: a sequence of independent, self-scoped CHECK blocks, one per EA-specific population/recombination scenario; same one-function-per-test-phase convention used identically across every OA self-test in this codebase
void GType::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING

    {
        auto const p_test = this->template clone<GType>();
        p_test->fillWithObjects(100);
    }

    {
        auto const p_test = this->template clone<GType>();
        for(std::size_t n_children = 5; n_children < 10; n_children++) {
            for(std::size_t n_parents = 1; n_parents < n_children; n_parents++) {
                CHECK_NOTHROW(p_test->clear());
                p_test->fillWithObjects(n_parents + n_children);
                CHECK_NOTHROW(p_test->setPopulationSizes(n_parents + n_children, n_parents));
                CHECK(p_test->getNParents() == n_parents);
                CHECK(p_test->getNChildren() == n_children);
            }
        }
    }

#else /* GEM_TESTING */
    Gem::Common::condnotset(
        "GEvolutionaryAlgorithm::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif                  /* GEM_TESTING */
}

/******************************************************************************/

/******************************************************************************/

std::ostream &operator<<(std::ostream &os, const GEvolutionaryAlgorithm &pop) {
    os << '\n' << '\n';
    for(auto it = pop.begin(); it != pop.begin() + pop.getNParents(); ++it) {
        os << (*it)->raw_fitness() << " " << (*it)->transformed_fitness() << '\n';
    }
    os << "***************************************" << '\n';

    return os;
}

/******************************************************************************/

} // namespace Gem::Geneva::OptimizationAlgorithms
