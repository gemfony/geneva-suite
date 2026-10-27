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
#include "geneva/oa/GSwarmAlgorithm.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GSwarmAlgorithm_PersonalityTraits.hpp"
#include "geneva/genome/GGenome.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <random>
#include <ranges>
#include <string>
#include <span>
#include <tuple>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GSwarmAlgorithm) // NOLINT

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * The auxiliary-store key under which each particle keeps its velocity vector (one double per active
 * floating-point parameter) on its OA scratch (carried on the individual). The velocity
 * is per-particle OA scratch and rides on the slot rather than in a parallel vector on the algorithm,
 * so it stays coherent with its particle (and is a plain POD double block, GPU-upload-friendly). The
 * value is distinct from the adaption AuxKeys 1-7 (a swarm never installs adaption state and vice versa,
 * but distinct keys keep the scratch self-describing).
 */
constexpr Gem::Geneva::Genome::AuxKey AUXKEY_SWARM_VELOCITY = 8;

/******************************************************************************/
/**
 * The default constructor. All work is delegated to another constructor.
 */
GSwarmAlgorithm::GSwarmAlgorithm()
  : GSwarmAlgorithm(DEFAULTNNEIGHBORHOODS, DEFAULTNNEIGHBORHOODMEMBERS) { /* nothing */
}

/******************************************************************************/
/**
 * This constructor sets the number of neighborhoods and the number of individuals in them.
 *
 * @param n_neighborhoods The desired number of neighborhoods (hardwired to >= 1)
 * @param default_n_neighborhood_members The default number of individuals in each neighborhood (hardwired to >= 2)
 */
GSwarmAlgorithm::GSwarmAlgorithm(
    const std::size_t &n_neighborhoods,
    const std::size_t &default_n_neighborhood_members
)
  : n_neighborhoods_((n_neighborhoods >= 1) ? n_neighborhoods : 1)
  , default_n_neighborhood_members_(
        (default_n_neighborhood_members >= 2) ? default_n_neighborhood_members : 2
    ) {
    GOptimizationAlgorithmBase::setDefaultPopulationSize(
        n_neighborhoods_ * default_n_neighborhood_members_
    );
}

/******************************************************************************/
/**
 * A standard copy constructor.
 *
 * @param cp Another GSwarmAlgorithm object
 */
GSwarmAlgorithm::GSwarmAlgorithm(const GSwarmAlgorithm &cp)
  : GOptimizationAlgorithmT<GSwarmAlgorithm>(cp)
  , n_neighborhoods_(cp.n_neighborhoods_)
  , default_n_neighborhood_members_(cp.default_n_neighborhood_members_)
  , n_neighborhood_members_cnt_(cp.n_neighborhood_members_cnt_)
  , global_best_ptr_(
        (cp.afterFirstIteration()) ? (cp.global_best_ptr_)->clone<gen::GGenome>()
                                   : std::shared_ptr<gen::GGenome>()
    )
  , neighborhood_bests_cnt_(n_neighborhoods_) // We copy the smart pointers over later
  , c_personal_(cp.c_personal_)
  , c_neighborhood_(cp.c_neighborhood_)
  , c_global_(cp.c_global_)
  , c_velocity_(cp.c_velocity_)
  , update_rule_(cp.update_rule_)
  , random_fill_up_(cp.random_fill_up_)
  , repulsion_threshold_(cp.repulsion_threshold_)
  , vel_max_(cp.vel_max_)
  , n_fp_parms_(cp.n_fp_parms_)
  , velocity_range_percentage_(cp.velocity_range_percentage_) {
    // Note that this setting might differ from nCPIndividuals, as it is not guaranteed
    // that cp has, at the time of copying, all individuals present in each neighborhood.
    // Differences might e.g. occur if not all individuals return from their remote
    // evaluation. adjustPopulation will take care to resize the population appropriately
    // inside of the "optimize()" call.
    GOptimizationAlgorithmBase::setDefaultPopulationSize(
        n_neighborhoods_ * default_n_neighborhood_members_
    );

    // Clone cp's best individuals in each neighborhood
    if(cp.afterFirstIteration()) {
        for(std::size_t i = 0; i < n_neighborhoods_; i++) {
            neighborhood_bests_cnt_[i] = cp.neighborhood_bests_cnt_[i]->clone<gen::GGenome>();
        }
    }

    // Copying / setting of the optimization algorithm id is done by the parent class. The same
    // applies to the copying of the optimization monitor.
}

/******************************************************************************/
/**
 * Resets the settings of this population to what was configured when
 * the optimize()-call was issued
 */
void GSwarmAlgorithm::resetToOptimizationStart_() {
    n_neighborhood_members_cnt_ = std::vector<std::size_t>(
        n_neighborhoods_,
        0
    ); // The current number of individuals belonging to each neighborhood

    global_best_ptr_.reset(); // The globally best individual

    neighborhood_bests_cnt_ = std::vector<std::shared_ptr<gen::GGenome>>(
        n_neighborhoods_
    ); // The collection of best individuals from each neighborhood

    vel_max_ = 0.;     // The dimensionless maximum velocity (recomputed in init())
    n_fp_parms_ = 0;   // The active floating-point parameter count (recomputed in init())

    last_iteration_individuals_cnt_
        .clear(); // A temporary copy of the last iteration's individuals

    // There is no more work to be done here, so we simply call the
    // function of the parent class
    GOptimizationAlgorithmBase::resetToOptimizationStart_();
}

/******************************************************************************/
/**
 * Sets the number of neighborhoods and the default number of members in them. All work is done inside of
 * the adjustPopulation function, inside of the GOptimizationAlgorithmBase::optimize() function.
 *
 * @param n_neighborhoods The number of neighborhoods
 * @param default_n_neighborhood_members The default number of individuals in each neighborhood
 */
void GSwarmAlgorithm::setSwarmSizes(
    std::size_t n_neighborhoods,
    std::size_t default_n_neighborhood_members
) {
    // Enforce useful settings
    if(n_neighborhoods == 0) {
        glogger << "In GSwarmAlgorithm::setSwarmSizes(): Warning!" << '\n'
                << "Requested number of neighborhoods is 0. Setting to 1." << '\n'
                << GWARNING;
    }

    if(default_n_neighborhood_members <= 1) {
        glogger << "In GSwarmAlgorithm::setSwarmSizes(): Warning!" << '\n'
                << "Requested number of members in each neighborhood is too small. Setting to 2."
                << '\n'
                << GWARNING;
    }

    n_neighborhoods_ = (n_neighborhoods >= 1) ? n_neighborhoods : 1;
    default_n_neighborhood_members_ =
        (default_n_neighborhood_members >= 2) ? default_n_neighborhood_members : 2;

    // Keep the per-neighborhood bookkeeping vectors in lockstep with n_neighborhoods_. setSwarmSizes()
    // can GROW n_neighborhoods_ beyond the value the vectors were sized to at construction, and
    // adjustPopulation_() -> fillUpNeighborhood1() indexes n_neighborhood_members_cnt_ by neighborhood
    // BEFORE init()/resetToOptimizationStart_() would resize it -- a heap-buffer-overflow otherwise
    // (e.g. default-constructed swarm with 5 neighborhoods, then setSwarmSizes(8, ...)).
    n_neighborhood_members_cnt_.resize(n_neighborhoods_, default_n_neighborhood_members_);
    neighborhood_bests_cnt_.resize(n_neighborhoods_);

    // Update our parent class'es values
    GOptimizationAlgorithmBase::setDefaultPopulationSize(
        n_neighborhoods_ * default_n_neighborhood_members_
    );
}

/******************************************************************************/
/**
 * Helper function that returns the id of the first individual of a neighborhood. "NI" stands
 * for NeighborhoodIndividual. "neighborhood" is assumed to be a counter, starting at 0, with
 * a maximum value of (nNeighborhoods_-1).
 *
 * @param neighborhood The id of the neighborhood for which the id of the first individual should be calculated
 * @return The position of the first individual of a neighborhood
 */
std::size_t GSwarmAlgorithm::getFirstNIPos(const std::size_t &neighborhood) const {
    return getFirstNIPosVec(neighborhood, n_neighborhood_members_cnt_);
}

/******************************************************************************/
/**
 * Helper function that returns the id of the first individual of a neighborhood, using a vector of neighborhood
 * sizes. "NI" stands for NeighborhoodIndividual. "neighborhood" is assumed to be a counter, starting at 0 and assuming
 * a maximum value of (nNeighborhoods_-1).
 *
 * @brief Returns the population index of the first individual of a neighborhood, using an explicit size vector
 * @param neighborhood The id of the neighborhood for which the id of the first individual should be calculated
 * @param vec A vector holding the number of members of each neighborhood, used to sum up the preceding members
 * @return The position of the first individual of a neighborhood
 */
std::size_t GSwarmAlgorithm::getFirstNIPosVec(
    const std::size_t &neighborhood,
    const std::vector<std::size_t> &vec
) {
#ifdef DEBUG
    // The summation below reads vec[0 .. neighborhood-1], so the size vector must cover every
    // neighborhood up to (and including) the requested one. The callers pass one entry per
    // neighborhood, so this also catches the id of a non-existing neighborhood. (This is a static
    // function, so the check is expressed through the argument, not the n_neighborhoods_ member.)
    if(neighborhood >= vec.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::getFirstNIPosVec():" << '\n'
            << "Received id " << neighborhood << " of a neighborhood which does not exist:" << '\n'
            << "the neighborhood-size vector has " << vec.size() << " entries," << '\n'
            << "hence the maximum allowed id is " << vec.size() - 1 << "." << '\n'
        );
    }
#endif

    if(neighborhood == 0) {
        return 0;
    }
    // Sum up the number of members in each neighborhood
    std::size_t n_previous_members = 0;
    for(std::size_t n = 0; n < neighborhood; n++) {
        n_previous_members += vec[n];
    }

    return n_previous_members;
}

/******************************************************************************/
/**
 * Helper function that helps to determine the end of a neighborhood. "NI" stands
 * for NeighborhoodIndividual. "neighborhood" is assumed to be a counter, starting at 0, with a maximum
 * value of (nNeighborhoods_-1). The position returned is that right after the last individual, as is common
 * in C++ .
 *
 * @param neighborhood The id of the neighborhood for which the id of the last individual should be calculated
 * @return The position of the individual right after the last of a neighborhood
 */
std::size_t GSwarmAlgorithm::getLastNIPos(const std::size_t &neighborhood) const {
#ifdef DEBUG
    if(neighborhood >= n_neighborhoods_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::getLastNIPos():" << '\n'
            << "Received id " << neighborhood << " of a neighborhood which does not exist."
            << '\n'
            << "The number of neighborhoods is " << n_neighborhoods_ << " ." << '\n'
            << "hence the maximum allowed value of the id is " << n_neighborhoods_ - 1 << "."
            << '\n'
        );
    }
#endif

    return getFirstNIPos(neighborhood) + n_neighborhood_members_cnt_[neighborhood];
}

/******************************************************************************/
/**
 * Updates the personal best of an individual
 *
 * @param ind_ptr A reference to the unique_ptr-owned individual whose personal best should be (re)registered
 */
void GSwarmAlgorithm::updatePersonalBest(const std::unique_ptr<gen::GGenome> &ind_ptr) {
#ifdef DEBUG
    if(not ind_ptr) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::updatePersonalBest():" << '\n'
            << "Got empty ind_ptr" << '\n'
        );
    }

    if(ind_ptr->is_due_for_processing() || ind_ptr->has_errors()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::updatePersonalBest():" << '\n'
            << "ind_ptr is unprocessed or has errors: " << '\n'
            << "is_due_for_processing() == " << ind_ptr->is_due_for_processing()
            << ", has_errors() == " << ind_ptr->has_errors() << '\n'
        );
    }
#endif /* DEBUG */

    // First-iteration seed: each particle's personal best is initialised unconditionally from its own
    // freshly-evaluated position (there is no prior best to compare against yet). The steady-state,
    // guarded update -- only re-register when strictly better -- lives in updatePersonalBestIfBetter().
    // The archive (personal_best_) keeps its own shared_ptr copy; the population owns the live
    // individual by unique_ptr, so we hand registerPersonalBest a clone across the ownership boundary.
    ind_ptr->getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()->registerPersonalBest(
        ind_ptr->clone<gen::GGenome>()
    );
}

/******************************************************************************/
/**
 * Updates the personal best of an individual, if a better solution was found
 *
 * @param ind_ptr A reference to the unique_ptr-owned individual whose personal best should be updated when its current position is better
 */
void GSwarmAlgorithm::updatePersonalBestIfBetter(const std::unique_ptr<gen::GGenome> &ind_ptr) {
#ifdef DEBUG
    if(not ind_ptr) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::updatePersonalBestIfBetter():" << '\n'
            << "Got empty ind_ptr" << '\n'
        );
    }

    if(ind_ptr->is_due_for_processing() || ind_ptr->has_errors()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::updatePersonalBestIfBetter(): Error!" << '\n'
            << "dirty flag of individual is set." << '\n'
        );
    }
#endif /* DEBUG */

    auto m =
        this->at(0)->getMaxMode(); // We assume that the maxMode is the same for all individuals
    // Update personal best only when the current position is better than the stored best. By design
    // this swarm is single-objective: it compares on transformed_fitness(0) (the master criterion).
    // init() warns once if the individuals expose multiple criteria; the others are not considered here.
    if(isBetter(
           ind_ptr->transformed_fitness(0),
           std::get<G_TRANSFORMED_FITNESS>(
               ind_ptr->getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()
                   ->getPersonalBestQuality()
           ),
           m
       )) {
        ind_ptr->getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()->registerPersonalBest(
            ind_ptr->clone<gen::GGenome>()
        );
    }
}

/******************************************************************************/
/**
 * Adds local configuration options to a GParserBuilder object
 *
 * @param gpb The GParserBuilder object to which configuration options should be added
 */
void GSwarmAlgorithm::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our parent class'es function
    GOptimizationAlgorithmBase::addConfigurationOptions_(gpb);

    // Add local data
    gpb.registerFileParameter<std::size_t, std::size_t>(
        "n_neighborhoods" // The name of the first variable
        ,
        "n_neighborhood_members" // The name of the second variable
        ,
        DEFAULTNNEIGHBORHOODS // The default value for the first variable
        ,
        DEFAULTNNEIGHBORHOODMEMBERS // The default value for the second variable
        ,
        [this](std::size_t nh, std::size_t nhm) { this->setSwarmSizes(nh, nhm); },
        "swarm_size"
    ) << "The desired number of neighborhoods in the population"
      << Gem::Common::nextComment() << "The desired number of members in each neighborhood";

    gpb.registerFileParameter<double>(
        "c_personal" // The name of the variable
        ,
        DEFAULTCPERSONAL // The default value
        ,
        [this](double cp) { this->setCPersonal(cp); }
    ) << "A constant to be multiplied with the personal direction vector";

    gpb.registerFileParameter<double>(
        "c_neighborhood" // The name of the variable
        ,
        DEFAULTCNEIGHBORHOOD // The default value
        ,
        [this](double cn) { this->setCNeighborhood(cn); }
    ) << "A constant to be multiplied with the neighborhood direction vector";

    gpb.registerFileParameter<double>(
        "c_global" // The name of the variable
        ,
        DEFAULTCGLOBAL // The default value
        ,
        [this](double cg) { this->setCGlobal(cg); }
    ) << "A constant to be multiplied with the global direction vector";

    gpb.registerFileParameter<double>(
        "c_velocity" // The name of the variable
        ,
        DEFAULTCVELOCITY // The default value
        ,
        [this](double cv) { this->setCVelocity(cv); }
    ) << "A constant to be multiplied with the old velocity vector";

    gpb.registerFileParameter<double>(
        "velocity_range_percentage" // The name of the variable
        ,
        DEFAULTVELOCITYRANGEPERCENTAGE // The default value
        ,
        [this](double vrp) { this->setVelocityRangePercentage(vrp); }
    ) << "Sets the velocity-range percentage";

    gpb.registerFileParameter<updateRule>(
        "update_rule" // The name of the variable
        ,
        DEFAULTUPDATERULE // The default value
        ,
        [this](updateRule ur) { this->setUpdateRule(ur); }
    ) << "Specifies whether a linear (0) or classical (1)"
      << '\n'
      << "update rule should be used";

    gpb.registerFileParameter<bool>(
        "random_fill_up" // The name of the variable
        ,
        true // The default value
        ,
        [this](bool nhrf) { this->setNeighborhoodsRandomFillUp(nhrf); }
    ) << "Specifies whether neighborhoods should be filled up"
      << '\n'
      << "randomly (true) or start with equal values (false)";

    gpb.registerFileParameter<std::uint32_t>(
        "repulsion_threshold" // The name of the variable
        ,
        DEFREPULSIONTHRESHOLD // The default value
        ,
        [this](std::uint32_t rt) { this->setRepulsionThreshold(rt); }
    ) << "The number of stalls as of which the algorithm switches to repulsive mode"
      << '\n'
      << "Set this to 0 in order to disable this feature";
}

/******************************************************************************/
/**
 * This function does some preparatory work and tagging required by swarm algorithms. It is called
 * from within GOptimizationAlgorithmBase::optimize(), immediately before the actual optimization cycle starts.
 */
void GSwarmAlgorithm::init() {
    // To be performed before any other action
    GOptimizationAlgorithmBase::init();

    // The number of active floating point parameters fixes the per-particle velocity block size; the
    // swarm works in the normalized internal coordinate and needs no per-parameter bounds.
    n_fp_parms_ = this->at(0)->countFPParameters(activityMode::ACTIVEONLY);

    // The maximum velocity is a single dimensionless fraction of the normalized unit interval (the same
    // for every parameter), used to stabilize the velocities (pruneVelocity); positions are contained by
    // the genome's write-fold, not by clamping here.
    vel_max_ = getVelocityRangePercentage();

    // Each particle's velocity is a per-slot POD double block (one double per active floating-point
    // parameter) installed + randomised lazily by updatePositions() (which guarantees every slot --
    // including any spliced in by adjustNeighborhoods() -- carries one before it is read). The velocity
    // thus lives on the individual's OA scratch and travels coherently with its particle, rather
    // than in a parallel vector on the algorithm.

    // Make sure neighborhood_bests_cnt_ has the correct size
    // It will only hold empty smart pointers. However, new ones
    // will be assigned in findBests()
    neighborhood_bests_cnt_.resize(n_neighborhoods_);

    // Make sure the n_neighborhood_members_cnt_ vector has the correct size
    n_neighborhood_members_cnt_.resize(n_neighborhoods_, default_n_neighborhood_members_);

    // This swarm is a SINGLE-objective optimizer: personal-, neighborhood- and global-best are all
    // compared on transformed_fitness(0) (the master criterion). Multi-objective PSO (Pareto archive /
    // dominance) is not implemented. If the individuals expose more than one fitness criterion, only the
    // first is optimized and the others are silently ignored -- warn once so this is not mistaken for a
    // multi-objective run.
    if(this->at(0)->hasMultipleFitnessCriteria()) {
        static std::atomic<bool> warned{false};
        if(not warned.exchange(true)) {
            glogger << "In GSwarmAlgorithm::init(): Warning!" << '\n'
                    << "The individuals expose more than one fitness criterion, but the swarm" << '\n'
                    << "algorithm is single-objective: it optimizes only the first criterion" << '\n'
                    << "(transformed_fitness(0)) and ignores the rest. Multi-objective PSO is not" << '\n'
                    << "implemented. Use an evolutionary algorithm in a PARETO sorting mode for true" << '\n'
                    << "multi-objective optimization." << '\n'
                    << GWARNING;
        }
    }
}

/******************************************************************************/
/**
 * Does any necessary finalization work
 */
void GSwarmAlgorithm::finalize() {
    // The per-particle velocity blocks live on the slots' OA scratch and are dropped at the
    // optimization-algorithm boundary (resetIndividualPersonalities -> clearScratch); nothing to do here.

    // Last action
    GOptimizationAlgorithmBase::finalize();
}

/******************************************************************************/
/**
 * Retrieve a GPersonalityTraits object belonging to this algorithm
 */
std::shared_ptr<GPersonalityTraits> GSwarmAlgorithm::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/
/**
 * This function implements the logic that constitutes each cycle of a swarm algorithm. The
 * function is called by GOptimizationAlgorithmBase::optimize() for each iteration of
 * the optimization,
 *
 * @brief Performs one iteration of the swarm algorithm (position update, evaluation, best-search, topology fix)
 * @return A tuple holding the raw and transformed fitness of the best individual found
 */
//! [swarm-cycle]
std::tuple<double, double> GSwarmAlgorithm::cycleLogic_() {
    std::tuple<double, double> best_individual_fitness;

    // First update the positions and neighborhood ids
    updatePositions();

    // Now update each individual's fitness
    evaluatePopulation_();

    // Search for the personal, neighborhood and globally best individuals and
    // update the lists of best solutions, if necessary.
    best_individual_fitness = findBests();

    // The population might be in a bad state. Check and fix.
    adjustNeighborhoods();

    // Return the result to the audience
    return best_individual_fitness;
}
//! [swarm-cycle]

/******************************************************************************/
/**
 * Repairs the neighborhood topology after a job submission (e.g. splicing fresh slots into short
 * neighborhoods after partial returns). It was introduced to avoid adding a separate cycleLogic to
 * GSwarmAlgorithm.
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent per-iteration repair pass over the neighborhood topology (surplus-trim / after-first-iteration backfill / first-iteration random backfill are mutually exclusive per-neighborhood branches sharing the sequentially-recomputed first_ni_pos), bookended by DEBUG invariant checks
void GSwarmAlgorithm::adjustNeighborhoods() {
    std::size_t first_ni_pos = 0; // Will hold the expected first position of a neighborhood

#ifdef DEBUG
    // Check that last_iteration_individuals_cnt_ has the desired size in iterations other than the first
    if(afterFirstIteration() && last_iteration_individuals_cnt_.size() !=
                                    default_n_neighborhood_members_ * n_neighborhoods_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::adjustNeighborhoods(): Error!" << '\n'
            << "last_iteration_individuals_cnt_ has incorrect size! Expected" << '\n'
            << "default_n_neighborhood_members_*n_neighborhoods_ = "
            << default_n_neighborhood_members_ * n_neighborhoods_ << '\n'
            << "but found " << last_iteration_individuals_cnt_.size() << '\n'
        );
    }
#endif /* DEBUG */

    // Add missing items to neighborhoods that are too small. We use stored copies from the
    // last iteration to fill in the missing items, or add random items in the first iteration.
    // Neighborhoods with too many items are pruned. findBests() has sorted each neighborhood
    // according to its fitness, so we know that the best items are in the front position of each
    // neighborhood. We thus simply remove items at the end of neighborhoods that are too large.
    for(std::size_t n = 0; n < n_neighborhoods_; n++) { // Loop over all neighborhoods
        // Calculate the desired position of our own first individual in this neighborhood
        // As we start with the first neighborhood and add or remove surplus or missing items,
        // getFirstNIPos() will return a valid position.
        first_ni_pos = getFirstNIPos(n);

        if(n_neighborhood_members_cnt_[n] == default_n_neighborhood_members_) {
            continue;
        }
        if(n_neighborhood_members_cnt_[n] >
                default_n_neighborhood_members_) { // Remove surplus items from the end of the neighborhood
            // Find out, how many surplus items there are
            std::size_t const n_surplus =
                n_neighborhood_members_cnt_[n] -
                default_n_neighborhood_members_; // NOLINT(cppcoreguidelines-init-variables)

            // Remove n_surplus items from the position (n+1)*default_n_neighborhood_members_
            data_cnt_.erase(
                data_cnt_.begin() + (n + 1) * default_n_neighborhood_members_,
                data_cnt_.begin() + (((n + 1) * default_n_neighborhood_members_) + n_surplus)
            );
        }
        else { // n_neighborhood_members_cnt_[n] < default_n_neighborhood_members_
            // The number of missing items. This covers an entirely empty neighborhood
            // (n_neighborhood_members_cnt_[n] == 0, e.g. when every work item of this neighborhood
            // timed out on a networked transport): n_missing == default_n_neighborhood_members_, and
            // the backfill below restores the whole neighborhood. Because the preceding neighborhoods
            // were already repaired to the default size, first_ni_pos == n * default_n_neighborhood_members_,
            // which aligns with neighborhood n in the (full, default-sized) last-iteration snapshot, so
            // the [first_ni_pos + i] reads are correctly aligned and in bounds (i < n_missing <= default).
            std::size_t const n_missing =
                default_n_neighborhood_members_ -
                n_neighborhood_members_cnt_[n]; // NOLINT(cppcoreguidelines-init-variables)

            if(afterFirstIteration()) { // The most likely case
                // Copy the best items of this neighborhood over from the last_iteration_individuals_cnt_ vector.
                // Each neighborhood there should have been sorted according to the individuals
                // fitness, with the best individuals in the front of each neighborhood.
                for(std::size_t i = 0; i < n_missing; i++) {
                    data_cnt_.insert(
                        data_cnt_.begin() + first_ni_pos,
                        (*(last_iteration_individuals_cnt_.begin() + first_ni_pos + i))
                            ->clone()
                    );
                }
            }
            else { // first iteration
                // At least one individual must have returned: the first-iteration backfill seeds missing
                // slots from this->front(), so a completely empty population is unrecoverable. Guard in
                // all builds (not just DEBUG) -- otherwise this->front() below is undefined behaviour in
                // a release build when every first-iteration work item failed to return.
                if(this->empty()) {
                    throw geneva_exception(
                        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                        << "In GSwarmAlgorithm::adjustNeighborhoods(): Error!" << '\n'
                        << "No items returned in the first iteration; cannot seed the swarm." << '\n'
                    );
                }

                // Fill up with random items.
                for(std::size_t n_m = 0; n_m < n_missing; n_m++) {
                    // Insert a clone of the first individual of the collection
                    data_cnt_.insert(
                        data_cnt_.begin() + first_ni_pos,
                        (this->front())->clone()
                    );

                    // Randomly initialize the item and prevent position updates
                    (*(data_cnt_.begin() + first_ni_pos))->randomInit(activityMode::ACTIVEONLY);
                    (*(data_cnt_.begin() + first_ni_pos))
                        ->getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()
                        ->setNoPositionUpdate();

                    // Set the neighborhood as required
                    (*(data_cnt_.begin() + first_ni_pos))
                        ->getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()
                        ->setNeighborhood(n);
                }
            }
        }

        // Finally adjust the number of entries in this neighborhood
        n_neighborhood_members_cnt_[n] = default_n_neighborhood_members_;
    }

#ifdef DEBUG
    // Check that the population has the expected size
    if(this->size() != n_neighborhoods_ * default_n_neighborhood_members_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::adjustNeighborhoods(): Error!" << '\n'
            << "The population has an incorrect size of " << this->size() << ", expected "
            << n_neighborhoods_ * default_n_neighborhood_members_ << '\n'
        );
    }
#endif

    last_iteration_individuals_cnt_.clear(); // Get rid of the copies
}

/******************************************************************************/
/**
 * Checks whether each neighborhood has the default size
 *
 * @return A boolean which indicates whether all neighborhoods have the default size
 */
bool GSwarmAlgorithm::neighborhoodsHaveNominalValues() const {
    for(std::size_t n = 0; n < n_neighborhoods_; n++) {
        if(n_neighborhood_members_cnt_[n] != default_n_neighborhood_members_) {
            return false;
        }
    }
    return true;
}

/******************************************************************************/
/**
 * Triggers an update of all individual's positions. Also makes sure each
 * individual has the correct neighborhood id. Creates a copy of the last iteration's
 * individuals, if this is not the first iteration, then performs the standard position
 * update using GSwam::updatePositions(). We use the old individuals to fill in missing
 * returns in adjustNeighborhoods. This doesn't make sense for the first iteration though,
 * as individuals have not generally been evaluated then, and we do not want to fill
 * up with "dirty" individuals.
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent per-iteration position-update driver (lazily seed missing per-slot velocity blocks, snapshot the previous iteration, then dispatch updateIndividualPositions() per neighborhood member), interleaved with the DEBUG invariant checks that guard each state read before use
void GSwarmAlgorithm::updatePositions() {
    std::size_t neighborhood_offset = 0;
    auto start = this->begin();

    // Make sure every slot carries a velocity block before any position update reads it. init() seeds
    // the original population, but adjustNeighborhoods() may have spliced in fresh slots (cloned
    // individuals wrapped in empty slots) to fill short neighborhoods after lost returns -- give those a
    // freshly randomised velocity here so the velocity always travels with its particle.
    for(const auto &slot : *this) {
        if(not slot->scratch().hasAux(AUXKEY_SWARM_VELOCITY)) {
            slot->scratch().installAuxBlock<double>(
                AUXKEY_SWARM_VELOCITY, n_fp_parms_, gen::AuxScope::PerIndividual
            );
            std::span<double> const vel = slot->scratch().metaRecords<double>(AUXKEY_SWARM_VELOCITY);
            for(std::size_t i = 0; i < n_fp_parms_; i++) {
                vel[i] = GOptimizationAlgorithmBase::uniform_real_distribution_(
                    gr_,
                    std::uniform_real_distribution<double>::param_type(-vel_max_, vel_max_)
                );
            }
        }
    }

#ifdef DEBUG
    // Check that all neighborhoods have the default size
    for(std::size_t n = 0; n < n_neighborhoods_; n++) {
        if(n_neighborhood_members_cnt_[n] != default_n_neighborhood_members_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GSwarmAlgorithm::updatePositions(): Error!" << '\n'
                << "n_neighborhood_members_cnt_[" << n << "] has invalid size "
                << n_neighborhood_members_cnt_[n] << '\n'
                << "but expected size " << default_n_neighborhood_members_ << '\n'
            );
        }

        if(this->size() != n_neighborhoods_ * default_n_neighborhood_members_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GSwarmAlgorithm::updatePositions(): Error!" << '\n'
                << "The population has an incorrect size of " << this->size() << ", expected "
                << n_neighborhoods_ * default_n_neighborhood_members_ << '\n'
            );
        }
    }
#endif

    last_iteration_individuals_cnt_.clear();
    if(afterFirstIteration()) {
        // Clone the individuals and copy them over
        for(const auto &ind_ptr : *this) {
            last_iteration_individuals_cnt_.push_back(ind_ptr->clone<gen::GGenome>());
        }
    }

#ifdef DEBUG
    // Cross-check that we have the nominal amount of individuals
    if(this->size() != n_neighborhoods_ * default_n_neighborhood_members_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::updatePositions(): Error!" << '\n'
            << "Invalid number of individuals found." << '\n'
            << "Expected " << n_neighborhoods_ * default_n_neighborhood_members_ << " but got "
            << this->size() << '\n'
        );
    }
#endif /* DEBUG */

    // First update all positions
    for(std::size_t n = 0; n < n_neighborhoods_; n++) {
#ifdef DEBUG
        if(afterFirstIteration()) {
            if(not neighborhood_bests_cnt_[n]) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GSwarmAlgorithm::updatePositions():" << '\n'
                    << "neighborhood_bests_cnt_[" << n << "] is empty." << '\n'
                );
            }

            if(n == 0 && not global_best_ptr_) { // Only check for the first n
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GSwarmAlgorithm::updatePositions():" << '\n'
                    << "global_best_ptr_ is empty." << '\n'
                );
            }
        }

        // Check that the number if individuals in each neighborhoods has the expected value
        if(n_neighborhood_members_cnt_[n] != default_n_neighborhood_members_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GSwarmAlgorithm::updatePositions(): Error!" << '\n'
                << "Invalid number of members in neighborhood " << n << ": "
                << n_neighborhood_members_cnt_[n] << '\n'
            );
        }
#endif /* DEBUG */

        for(std::size_t member = 0; member < n_neighborhood_members_cnt_[n]; member++) {
            auto current = start + neighborhood_offset;

            // Update the neighborhood ids
            this->at(neighborhood_offset)
                ->getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()
                ->setNeighborhood(n);

            // Note: global/n bests and velocities haven't been determined yet in the first iteration and are not needed there
            if(afterFirstIteration() &&
               not(*current)
                      ->getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()
                      ->checkNoPositionUpdateAndReset()) {
                // Update the swarm positions:
                updateIndividualPositions(
                    n,
                    (*current),
                    neighborhood_bests_cnt_[n],
                    global_best_ptr_,
                    std::make_tuple(
                        getCPersonal(),
                        getCNeighborhood(),
                        getCGlobal(),
                        getCVelocity()
                    )
                );
            }

            neighborhood_offset++;
        }
    }
}

/******************************************************************************/
/**
 * Update the individual's positions. Note that we use a std::tuple as an argument,
 * so that we do not have to pass too many parameters. The particle's velocity is not
 * passed in: it lives as a per-slot POD double block in the slot's OA scratch (AUXKEY_SWARM_VELOCITY)
 * and is read/written there.
 *
 * @param neighborhood The id of the neighborhood the individual belongs to (currently unused)
 * @param ind The slot whose wrapped individual's position should be updated
 * @param neighborhood_best The best data set of the individual's neighborhood
 * @param global_best The globally best individual so far
 * @param constants A std::tuple holding the c_personal, c_neighborhood, c_global and c_velocity constants needed for the position update
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent numeric kernel: the classic PSO velocity/position update (personal/neighborhood/global attraction terms scaled by the update-rule switch, velocity scaling, pruning, then the repulsion/attraction position step), all operating on the same extracted vectors
void GSwarmAlgorithm::updateIndividualPositions(
    [[maybe_unused]] const std::size_t & neighborhood
    ,
    const std::unique_ptr<gen::GGenome> &ind,
    const std::shared_ptr<gen::GGenome>& neighborhood_best,
    const std::shared_ptr<gen::GGenome>& global_best,
    std::tuple<double, double, double, double> constants
) {
    // Extract the constants from the tuple
    double const c_personal = std::get<0>(constants);
    double const c_neighborhood = std::get<1>(constants);
    double const c_global = std::get<2>(constants);
    double const c_velocity = std::get<3>(constants);

#ifdef DEBUG
    // Do some error checking
    if(not ind) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::updateIndividualPositions():" << '\n'
            << "Found empty individual \"ind\"" << '\n'
        );
    }
#endif /* DEBUG */

    // Extract the personal best
    std::shared_ptr<gen::GGenome> const personal_best =
        ind->getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()->getPersonalBest();

    // Further error checks
#ifdef DEBUG
    if(not personal_best) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::updateIndividualPositions():" << '\n'
            << "Found empty individual \"personal_best\"" << '\n'
        );
    }

    if(not neighborhood_best) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::updateIndividualPositions():" << '\n'
            << "Found empty individual \"neighborhood_best\"" << '\n'
        );
    }

    if(not global_best) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::updateIndividualPositions():" << '\n'
            << "Found empty individual \"global_best\"" << '\n'
        );
    }

#endif /* DEBUG */

    // The particle's velocity is a per-slot POD double block in its OA scratch (installed in init()).
    std::span<double> const velocity = ind->scratch().metaRecords<double>(AUXKEY_SWARM_VELOCITY);

    // Extract the vectors for the individual, the personal, neighborhood and global bests,
    // as well as the velocity
    std::vector<double> ind_vec;
    std::vector<double> personal_best_vec;
    std::vector<double> nbh_best_vec;
    std::vector<double> glb_best_vec;
    ind->streamlineFPInternal(ind_vec, activityMode::ACTIVEONLY);
    personal_best->streamlineFPInternal(personal_best_vec, activityMode::ACTIVEONLY);
    neighborhood_best->streamlineFPInternal(nbh_best_vec, activityMode::ACTIVEONLY);
    global_best->streamlineFPInternal(glb_best_vec, activityMode::ACTIVEONLY);
    std::vector<double> vel_vec(velocity.begin(), velocity.end());

    // Subtract the individual vector from the personal, neighborhood and global bests
    Gem::Common::subtractVec<double>(personal_best_vec, ind_vec);
    Gem::Common::subtractVec<double>(nbh_best_vec, ind_vec);
    Gem::Common::subtractVec<double>(glb_best_vec, ind_vec);

    switch(update_rule_) {
    case updateRule::SWARM_UPDATERULE_CLASSIC:
        // Multiply each floating point value with a random fp number in the range [0,1[, times a constant
        for(std::size_t i = 0; i < personal_best_vec.size(); i++) {
            personal_best_vec[i] *=
                (c_personal * GOptimizationAlgorithmBase::uniform_real_distribution_(
                                  gr_,
                                  std::uniform_real_distribution<double>::param_type(0., 1.)
                              ));
            nbh_best_vec[i] *=
                (c_neighborhood * GOptimizationAlgorithmBase::uniform_real_distribution_(
                                      gr_,
                                      std::uniform_real_distribution<double>::param_type(0., 1.)
                                  ));
            glb_best_vec[i] *=
                (c_global * GOptimizationAlgorithmBase::uniform_real_distribution_(
                                gr_,
                                std::uniform_real_distribution<double>::param_type(0., 1.)
                            ));
        }
        break;

    case updateRule::SWARM_UPDATERULE_LINEAR:
        // Multiply each position with the same random floating point number times a constant
        Gem::Common::multVecConst<double>(
            personal_best_vec,
            c_personal * GOptimizationAlgorithmBase::uniform_real_distribution_(
                             gr_,
                             std::uniform_real_distribution<double>::param_type(0., 1.)
                         )
        );
        Gem::Common::multVecConst<double>(
            nbh_best_vec,
            c_neighborhood * GOptimizationAlgorithmBase::uniform_real_distribution_(
                                 gr_,
                                 std::uniform_real_distribution<double>::param_type(0., 1.)
                             )
        );
        Gem::Common::multVecConst<double>(
            glb_best_vec,
            c_global * GOptimizationAlgorithmBase::uniform_real_distribution_(
                           gr_,
                           std::uniform_real_distribution<double>::param_type(0., 1.)
                       )
        );
        break;
    }

    // Scale the velocity
    Gem::Common::multVecConst<double>(vel_vec, c_velocity);

    // Add the personal and neighborhood parameters to the velocity
    Gem::Common::addVec<double>(vel_vec, personal_best_vec);
    Gem::Common::addVec<double>(vel_vec, nbh_best_vec);

    // Adding a velocity component towards the global best only
    // makes sense if there is more than one neighborhood
    if(getNNeighborhoods() > 1) {
        Gem::Common::addVec<double>(vel_vec, glb_best_vec);
    }

    // Prune the velocity vector so that we can
    // be sure it is inside of the allowed range
    pruneVelocity(vel_vec);

    // Add or subtract the velocity parameters to the individual's parameters, depending on
    // the number of stalls and the value of the repulsion_threshold_ variable. This allows
    // the algorithm to escape local optima, if repulsion_threshold_ is > 0.
    if(0 < repulsion_threshold_ && this->getStallCounter() >= repulsion_threshold_) {
        Gem::Common::subtractVec<double>(
            ind_vec,
            vel_vec
        ); // repulsion -- walk away from best known individuals
    }
    else {
        Gem::Common::addVec<double>(
            ind_vec,
            vel_vec
        ); // attraction - walk towards best known individuals
    }

    // Write the updated velocity back into the slot's POD block
    std::copy(vel_vec.begin(), vel_vec.end(), velocity.begin());

    // Update the candidate solution
    ind->assignFPValueVectorInternal(ind_vec, activityMode::ACTIVEONLY);
}

/******************************************************************************/
/**
 * Adjusts the velocity vector so that its parameters don't exceed the allowed value range.
 *
 * @param vel_vec the velocity vector to be adjusted
 */
//! [swarm-prune-velocity]
void GSwarmAlgorithm::pruneVelocity(std::vector<double> &vel_vec) {
    // The velocity cap vel_max_ is a single dimensionless fraction of the normalized unit interval, the
    // same for every parameter. A zero cap (velocity range percentage of 0) freezes all velocities.
    if(vel_max_ <= 0.) {
        std::fill(vel_vec.begin(), vel_vec.end(), 0.);
        return;
    }

    // Find the component that exceeds the cap by the largest factor; if any does, scale the whole velocity
    // vector down uniformly so its largest component sits exactly at the cap (preserving its direction).
    double max_percentage = 1.;
    bool overflow_found = false;
    for(double const v : vel_vec) {
        if(std::abs(v) > vel_max_) {
            overflow_found = true;
            max_percentage = std::max(max_percentage, std::abs(v) / vel_max_);
        }
    }

    if(overflow_found) {
        for(double &i : vel_vec) {
            i /= max_percentage;
        }
    }
}
//! [swarm-prune-velocity]

/******************************************************************************/
/**
 * Triggers the fitness calculation of all individuals
 */
void GSwarmAlgorithm::evaluatePopulation_() {
    using namespace Gem::Courtier;

    //--------------------------------------------------------------------------------
    // Submit work items and wait for results (courtier marks + reconciles the whole population).
    auto status = this->workOnPopulation(0, this->data_cnt_.size());

    // Retrieve a vector of old work items. getOldWorkItems() has already applied the universal gate:
    // only clean successes survive and each lineage (submission UUID) appears at most once, de-duplicated
    // against the live population -- so the errored/unprocessed sweep below only ever acts on the FRESH
    // batch, never on these late returns.
    auto old_work_items = this->getOldWorkItems();

    // Update the iteration of older individuals (they will keep their old neighborhood id)
    // and attach them to the data vector
    for(auto &item_ptr : old_work_items) {
        item_ptr->setAssignedIteration(this->getIteration());
        this->push_back(std::move(item_ptr));
    }
    old_work_items.clear();

    //--------------------------------------------------------------------------------
    // Take care of unprocessed items, if these exist
    this->discardUnusableItems_(status, "GSwarmAlgorithm::evaluatePopulation_()");

    //--------------------------------------------------------------------------------
    // Sort according to the individuals' neighborhoods
    sort(
        data_cnt_.begin(),
        data_cnt_.end(),
        [](const auto &x, const auto &y) -> bool {
            return x->template getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()->getNeighborhood() <
                   y->template getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()->getNeighborhood();
        }
    );

    // Now update the number of items in each neighborhood: First reset the number of members of each neighborhood
    Gem::Common::assignVecConst(n_neighborhood_members_cnt_, static_cast<std::size_t>(0));
    // Then update the number of individuals in each neighborhood
    for(const auto &item_ptr : *this) {
        n_neighborhood_members_cnt_[item_ptr
                                         ->getPersonalityTraits<GSwarmAlgorithm_PersonalityTraits>()
                                         ->getNeighborhood()] += 1;
    }

    // The population is repaired per-iteration in GSwarmAlgorithm::adjustNeighborhoods().
}

/******************************************************************************/
/**
 * Updates the best individuals found. This function assumes that the population already contains individuals
 * and that the neighborhood and global bests have been initialized (possibly with dummy values). This should have
 * happened in the adjustPopulation() function. It also assumes that all individuals have already been evaluated.
 *
 * @return The best evaluation found in this iteration
 */
std::tuple<double, double> GSwarmAlgorithm::findBests() {
    auto const mode =
        this->at(0)->getMaxMode(); // We assume that the maxMode is the same for all individuals

#ifdef DEBUG
    for(auto const &[pos, ind_ptr] : *this | std::views::enumerate) {
        if(ind_ptr->is_due_for_processing() || ind_ptr->has_errors()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GSwarmAlgorithm::findBests(): Error!" << '\n'
                << "Found individual in position " << pos << " in iteration "
                << this->getIteration() << '\n'
                << "which is unprocessed or has errors" << '\n'
                << "is_due_for_processing() == " << ind_ptr->is_due_for_processing()
                << ", has_errors() == " << ind_ptr->has_errors() << '\n'
            );
        }
    }
#endif /* DEBUG */

    updateAllPersonalBests();
    sortNeighborhoodsAndUpdateBests(mode);
    updateGlobalBest(mode);
    return bestIterationFitness(mode);
}

/******************************************************************************/
/**
 * findBests() phase 1: (re)computes every individual's personal best -- unconditionally in the first
 * iteration, otherwise only when the current position improves on the stored personal best.
 */
void GSwarmAlgorithm::updateAllPersonalBests() {
    if(inFirstIteration()) {
        for(const auto &ind_ptr : *this) {
            updatePersonalBest(ind_ptr);
        }
    }
    else {
        for(const auto &ind_ptr : *this) {
            updatePersonalBestIfBetter(ind_ptr);
        }
    }
}

/******************************************************************************/
/**
 * findBests() phase 2: sorts every neighborhood by (min-only transformed) fitness and refreshes each
 * neighborhood's stored best -- cloning it in the first iteration, else loading over it when improved.
 *
 * @param m The optimization mode (shared by all individuals)
 */
void GSwarmAlgorithm::sortNeighborhoodsAndUpdateBests(maxMode m) {
    for(std::size_t n = 0; n < n_neighborhoods_; n++) {
        // identify the first and last id of the individuals in the current neighborhood
        std::size_t const first_counter = getFirstNIPos(n);
        std::size_t const last_counter = getLastNIPos(n);

        // Only partially sort the arrays
        std::ranges::sort(
            this->begin() + first_counter,
            this->begin() + last_counter,
            std::ranges::less{},
            [](const auto &x_ptr) { return minOnly_transformed_fitness((*x_ptr)); }
        );

        // Check whether the best individual of the neighborhood is better than
        // the best individual found so far in this neighborhood
        if(inFirstIteration()) {
            neighborhood_bests_cnt_.at(n) =
                (*(this->begin() + first_counter))->clone<gen::GGenome>();
        }
        else {
            if(isBetter(
                   (*(this->begin() + first_counter))->transformed_fitness(0),
                   neighborhood_bests_cnt_.at(n)->transformed_fitness(0),
                   m
               )) {
                (neighborhood_bests_cnt_.at(n))->load((*(this->begin() + first_counter)));
            }
        }
    }
}

/******************************************************************************/
/**
 * findBests() phase 3: identifies the best individual among all neighborhood bests and updates the
 * globally best individual -- initializing it in the first iteration, else loading over it when improved.
 *
 * @param m The optimization mode (shared by all individuals)
 */
void GSwarmAlgorithm::updateGlobalBest(maxMode m) {
    std::size_t best_local_id = 0;
    std::tuple<double, double> best_local_fitness =
        std::make_tuple(this->at(0)->getWorstCase(), this->at(0)->getWorstCase());

    // Identify the best individuals among all neighborhood bests
    for(std::size_t n = 0; n < n_neighborhoods_; n++) {
        if(isBetter(
               (neighborhood_bests_cnt_.at(n))->transformed_fitness(0),
               std::get<G_TRANSFORMED_FITNESS>(best_local_fitness),
               m
           )) {
            best_local_id = n;
            best_local_fitness = (neighborhood_bests_cnt_.at(n))->getFitnessTuple();
        }
    }

    // Compare the best neighborhood individual with the globally best individual and
    // update it, if necessary. Initialize it in the first generation.
    if(inFirstIteration()) {
        global_best_ptr_ = (neighborhood_bests_cnt_.at(best_local_id))->clone<gen::GGenome>();
    }
    else {
        if(isBetter(
               std::get<G_TRANSFORMED_FITNESS>(best_local_fitness),
               global_best_ptr_->transformed_fitness(0),
               m
           )) {
            global_best_ptr_->load(neighborhood_bests_cnt_.at(best_local_id));
        }
    }
}

/******************************************************************************/
/**
 * findBests() phase 4: identifies the best fitness among all individuals of the current iteration.
 *
 * @param m The optimization mode (shared by all individuals)
 * @return The best raw/transformed fitness tuple found in the current iteration
 */
std::tuple<double, double> GSwarmAlgorithm::bestIterationFitness(maxMode m) const {
    std::tuple<double, double> best_iteration_fitness =
        std::make_tuple(this->at(0)->getWorstCase(), this->at(0)->getWorstCase());

    for(auto const & i : *this) {
        if(isBetter(
               std::get<G_TRANSFORMED_FITNESS>(i->getFitnessTuple()),
               std::get<G_TRANSFORMED_FITNESS>(best_iteration_fitness),
               m
           )) {
            best_iteration_fitness = i->getFitnessTuple();
        }
    }

    return best_iteration_fitness;
}

/******************************************************************************/
/**
 * Resizes the population to the desired level and does some error checks. This function implements
 * the purely virtual function GOptimizationAlgorithmBase::adjustPopulation() .
 */
// NOLINTNEXTLINE(readability-function-size) -- switch-driven dispatcher over the initial-population-size cases (empty / single / exactly n_neighborhoods / exactly default_pop_size / arbitrary under or over capacity), each branch already delegating to fillUpNeighborhood1() or a short local loop; the case selection itself is the coherent unit
void GSwarmAlgorithm::adjustPopulation_() {
    const std::size_t current_size = this->size();
    const std::size_t default_pop_size = getDefaultPopulationSize();
    const std::size_t n_neighborhoods = getNNeighborhoods();

    if(current_size == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::adjustPopulation() :" << '\n'
            << "No individuals found in the population." << '\n'
            << "You need to add at least one individual before" << '\n'
            << "the call to optimize<>()" << '\n'
        );
    }
    if(current_size == 1) {
        // Fill up with random items to the number of neighborhoods
        for(std::size_t i = 1; i < n_neighborhoods_; i++) {
            this->push_back(this->front()->clone());
            this->back()->randomInit(activityMode::ACTIVEONLY);
        }

        // Fill in remaining items in each neighborhood. This will
        // also take care of the above case, where only one individual
        // has been added.
        fillUpNeighborhood1();
    }
    else if(current_size == n_neighborhoods) {
        // Fill in remaining items in each neighborhood.
        fillUpNeighborhood1();
    }
    else if(current_size == default_pop_size) {
        // Update the number of individuals in each neighborhood
        for(std::size_t n = 0; n < n_neighborhoods_; n++) {
            n_neighborhood_members_cnt_[n] = default_n_neighborhood_members_;
        }
    }
    else {
        if(current_size < n_neighborhoods_) {
            // First fill up the neighborhoods, if required
            for(std::size_t m = 0; m < (n_neighborhoods_ - current_size); m++) {
                this->push_back(this->front()->clone());
                this->back()->randomInit(activityMode::ACTIVEONLY);
            }

            // Now follow the procedure used for the "n_neighborhoods_" case
            fillUpNeighborhood1();
        }
        else { // current_size > n_neighborhoods_ and != default_pop_size
            // The user supplied more than n_neighborhoods_ start individuals but not exactly the default
            // population size. Normalise to EXACTLY default_pop_size without discarding usable individuals
            // or corrupting the neighborhood topology. (The old code either resized down to n_neighborhoods_
            // -- silently dropping the user's extra individuals -- or dumped ALL surplus into the last
            // neighborhood, leaving the others short. Both were marked "MUST FIX".)
            //
            // adjustPopulation_() runs ONCE at setup; neighborhoods are assigned POSITIONALLY by the
            // subsequent setIndividualPersonalities(), so here we only need the correct overall size with
            // every neighborhood at its default member count.
            if(current_size > default_pop_size) {
                // Keep as many of the user's individuals as the swarm can hold (the first default_pop_size,
                // one full set of neighborhoods); drop only the genuine surplus over capacity.
                this->resize(default_pop_size);
            }
            else {
                // n_neighborhoods_ < current_size < default_pop_size: clone-fill up to capacity, cycling
                // round-robin over the user's individuals so their diversity is spread rather than a single
                // one being duplicated.
                for(std::size_t k = current_size; k < default_pop_size; k++) {
                    this->push_back((*(this->begin() + (k % current_size)))->clone());
                    if(random_fill_up_) {
                        this->back()->randomInit(activityMode::ACTIVEONLY);
                    }
                }
            }

            // Every neighborhood now holds exactly its default member count.
            for(std::size_t n = 0; n < n_neighborhoods_; n++) {
                n_neighborhood_members_cnt_[n] = default_n_neighborhood_members_;
            }
        }
    }

#ifdef DEBUG
    // As the above switch statement is quite complicated, cross check that we now
    // indeed have at least the required number of individuals
    if(this->size() < default_pop_size) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::adjustPopulation() :" << '\n'
            << "Expected at least a population size of " << default_pop_size << '\n'
            << "but found a size of " << this->size() << ", which is too small." << '\n'
        );
    }
#endif /* DEBUG */

    // We do not initialize the neighborhood and global bests here, as this requires the value of
    // all individuals to be calculated.

    // Division of responsibility (the "split" once envisioned here already exists):
    //   - adjustPopulation_()    runs ONCE at setup and only SIZES the initial population to
    //                            default_pop_size; neighborhood ids are then assigned by
    //                            setIndividualPersonalities().
    //   - adjustNeighborhoods()  runs per-iteration and REPAIRS the topology mid-run (splices fresh
    //                            slots into short neighborhoods after partial returns, re-tags ids).
    // adjustPopulation_() therefore never touches the population mid-run and cannot discard evolved
    // state.
}

/******************************************************************************/
/**
 * Small helper function that helps to fill up a neighborhood, if there is just one entry in it.
 */
void GSwarmAlgorithm::fillUpNeighborhood1() {
    // Do some error checking
    if(this->size() != n_neighborhoods_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::fillUpNeighborhood1():" << '\n'
            << "Invalid size: " << this->size() << " Expected " << n_neighborhoods_ << '\n'
        );
    }

    if(default_n_neighborhood_members_ == 1) {
        return; // nothing to do
    }

    // Starting with the last item, loop over all neighborhoods
    for(std::size_t i = 0; i < n_neighborhoods_; i++) {
        std::size_t const n = n_neighborhoods_ - 1 - i; // Calculate the correct neighborhood

        // Insert the required number of clones after the existing individual
        for(std::size_t m = 1; m < default_n_neighborhood_members_;
            m++) { // m stands for "missing"
            // Add a clone of the first individual in the neighborhood to the next position
            this->insert(this->begin() + n, (*(this->begin() + n))->clone());
            // Make sure it has a unique value, if requested
            if(random_fill_up_) {
#ifdef DEBUG
                if(not(*(this->begin() + n + 1))) {
                    throw geneva_exception(
                        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                        << "In GSwarmAlgorithm::fillUpNeighborhood1():" << '\n'
                        << "Found empty position " << n << '\n'
                    );
                }
#endif /* DEBUG */

                (*(this->begin() + n + 1))->randomInit(activityMode::ACTIVEONLY);
            }
        }

        // Update the number of individuals in each neighborhood
        n_neighborhood_members_cnt_[n] = default_n_neighborhood_members_;
    }
}

/******************************************************************************/
/**
 * Allows to set a static multiplier for personal distances.
 *
 * @param c_personal A static multiplier for personal distances
 */
void GSwarmAlgorithm::setCPersonal(double c_personal) {
    c_personal_ = c_personal;
}

/******************************************************************************/
/**
 * Allows to retrieve the static multiplier for personal distances
 *
 * @return The static multiplier for personal distances
 */
double GSwarmAlgorithm::getCPersonal() const {
    return c_personal_;
}

/******************************************************************************/
/**
 * Allows to set a static multiplier for neighborhood distances.
 *
 * @param c_neighborhood A static multiplier for neighborhood distances
 */
void GSwarmAlgorithm::setCNeighborhood(double c_neighborhood) {
    c_neighborhood_ = c_neighborhood;
}

/******************************************************************************/
/**
 * Allows to retrieve the static multiplier for neighborhood distances
 *
 * @return A static multiplier for neighborhood distances
 */
double GSwarmAlgorithm::getCNeighborhood() const {
    return c_neighborhood_;
}

/******************************************************************************/
/**
 * Allows to set a static multiplier for global distances
 *
 * @param c_global A static multiplier for global distances
 */
void GSwarmAlgorithm::setCGlobal(double c_global) {
    c_global_ = c_global;
}

/******************************************************************************/
/**
 * Allows to retrieve the static multiplier for global distances
 *
 * @return The static multiplier for global distances
 */
double GSwarmAlgorithm::getCGlobal() const {
    return c_global_;
}

/******************************************************************************/
/**
 * Allows to set a static multiplier for velocities
 *
 * @param c_velocity A static multiplier for velocities
 */
void GSwarmAlgorithm::setCVelocity(double c_velocity) {
    c_velocity_ = c_velocity;
}

/******************************************************************************/
/**
 * Allows to retrieve the static multiplier for velocities
 *
 * @return The static multiplier for velocities
 */
double GSwarmAlgorithm::getCVelocity() const {
    return c_velocity_;
}

/******************************************************************************/
/**
 * Allows to set the velocity range percentage
 *
 * @param velocity_range_percentage The velocity range percentage
 */
void GSwarmAlgorithm::setVelocityRangePercentage(double velocity_range_percentage) {
    // Do some error checking
    if(velocity_range_percentage <= 0. || velocity_range_percentage > 1.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GSwarmAlgorithm::setVelocityRangePercentage()" << '\n'
            << "Invalid velocity_range_percentage: " << velocity_range_percentage << '\n'
        );
    }

    velocity_range_percentage_ = velocity_range_percentage;
}

/******************************************************************************/
/**
 * Allows to retrieve the velocity range percentage
 *
 * @return The velocity range percentage
 */
double GSwarmAlgorithm::getVelocityRangePercentage() const {
    return velocity_range_percentage_;
}

/******************************************************************************/
/**
 * Retrieves the number of neighborhoods
 *
 * @return The number of neighborhoods in the population
 */
std::size_t GSwarmAlgorithm::getNNeighborhoods() const {
    return n_neighborhoods_;
}

/******************************************************************************/
/**
 * Retrieves the default number of individuals in each neighborhood
 *
 * @return The default number of individuals in each neighborhood
 */
std::size_t GSwarmAlgorithm::getDefaultNNeighborhoodMembers() const {
    return default_n_neighborhood_members_;
}

/******************************************************************************/
/**
 * Retrieves the current number of individuals in a given neighborhood
 *
 * @param neighborhood The id of the neighborhood whose current member count should be retrieved
 * @return The current number of individuals in a given neighborhood
 */
std::size_t GSwarmAlgorithm::getCurrentNNeighborhoodMembers(const std::size_t &neighborhood) const {
    return n_neighborhood_members_cnt_[neighborhood];
}

/******************************************************************************/
/**
 * Allows to specify the update rule to be used by the swarm.
 *
 * @param ur The desired update rule
 */
void GSwarmAlgorithm::setUpdateRule(updateRule ur) {
    update_rule_ = ur;
}

/******************************************************************************/
/**
 * Allows to retrieve the update rule currently used by the swarm.
 *
 * @return The current update rule
 */
updateRule GSwarmAlgorithm::getUpdateRule() const {
    return update_rule_;
}

/******************************************************************************/
/**
 * Allows to specify the number of stalls as of which the algorithm switches to
 * repulsive mode. Set this value to 0 in order to disable repulsive mode.
 *
 * @param repulsion_threshold The threshold as of which the algorithm switches to repulsive mode
 */
void GSwarmAlgorithm::setRepulsionThreshold(std::uint32_t repulsion_threshold) {
    repulsion_threshold_ = repulsion_threshold;
}

/******************************************************************************/
/**
 * Allows to retrieve the number of stalls as of which the algorithm switches
 * to repulsive mode.
 *
 * @return The value of the repulsionThreshold_ variable
 */
std::uint32_t GSwarmAlgorithm::getRepulsionThreshold() const {
    return repulsion_threshold_;
}

/******************************************************************************/
/**
 * All individuals automatically added to a neighborhood will have equal value
 */
void GSwarmAlgorithm::setNeighborhoodsEqualFillUp() {
    random_fill_up_ = false;
}

/******************************************************************************/
/**
 * All individuals automatically added to a neighborhood will have a random value
 */
void GSwarmAlgorithm::setNeighborhoodsRandomFillUp(bool random_fill_up) {
    random_fill_up_ = random_fill_up;
}

/******************************************************************************/
/**
 * Allows to check whether neighborhoods are filled up with random individuals
 *
 * @return A boolean indicating whether neighborhoods are filled up with random values
 */
bool GSwarmAlgorithm::neighborhoodsFilledUpRandomly() const {
    return random_fill_up_;
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
