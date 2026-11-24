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

// Boost headers go here

// Geneva headers go here
#include "common/GExceptions.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOptimizationAlgorithmT.hpp"
#include "geneva/oa/GSwarmAlgorithm_PersonalityTraits.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * @brief A particle-swarm optimizer (PSO): a population of candidate solutions ("particles") that move
 * through the parameter space pulled towards the best positions found so far.
 *
 * @details
 * The swarm is partitioned into a constant number of @e neighborhoods whose member counts may vary (so
 * late arrivals under networked execution can still join later iterations). Each particle \f$ i \f$ keeps
 * a velocity \f$ \mathbf{v}_i \f$ (a per-slot POD block in its OA scratch) and is attracted towards
 * three reference points: its own best-ever position \f$ \mathbf{p}_i \f$ (personal best), the best
 * position in its neighborhood \f$ \mathbf{p}_{N(i)} \f$, and -- only when there is more than one
 * neighborhood -- the global best \f$ \mathbf{g} \f$. Per iteration the velocity is updated and the
 * particle moved:
 * \f[
 *   \mathbf{v}_i \leftarrow c_v\,\mathbf{v}_i
 *     + c_p\,\mathbf{r}_p \odot (\mathbf{p}_i-\mathbf{x}_i)
 *     + c_n\,\mathbf{r}_n \odot (\mathbf{p}_{N(i)}-\mathbf{x}_i)
 *     + c_g\,\mathbf{r}_g \odot (\mathbf{g}-\mathbf{x}_i),
 *   \qquad
 *   \mathbf{x}_i \leftarrow \mathbf{x}_i + \mathbf{v}_i,
 * \f]
 * where \f$ \odot \f$ is the element-wise product, \f$ c_v \f$ is the inertia / velocity weight and
 * \f$ c_p, c_n, c_g \f$ are the personal / neighborhood / global acceleration constants (defaults
 * \f$ c_v=0.72,\ c_p=c_n=1.49,\ c_g=1.0 \f$). The random weights \f$ \mathbf{r}_\bullet \f$ are drawn
 * uniformly from \f$ [0,1) \f$; in the @e classic update rule (the default) each @e component gets its
 * own independent draw, while in the @e linear rule one scalar per term scales the whole difference
 * vector.
 *
 * @par Velocity clamping
 * After the update each velocity component is clamped (pruneVelocity()) so that it stays within a
 * per-dimension cap proportional to that parameter's value range (a configurable percentage,
 * velocity_range_percentage_); if any component overflows, the whole velocity vector is scaled down by
 * the largest overflow ratio, preserving its direction. A frozen (equal-bound) parameter has a zero
 * range and hence a zero velocity cap, which is handled without dividing by the zero range.
 *
 * @par Repulsion (stall escape)
 * With a non-zero repulsion threshold (setRepulsionThreshold()) the swarm flips the move to
 * \f$ \mathbf{x}_i \leftarrow \mathbf{x}_i - \mathbf{v}_i \f$ once the run has stalled for that many
 * iterations, so particles walk @e away from the known bests to escape a local optimum, reverting to
 * attraction once progress resumes.
 */
class GSwarmAlgorithm // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GSwarmAlgorithm> {
public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold.
    static constexpr std::string_view class_name = "GSwarmAlgorithm";
    static constexpr std::string_view oa_algorithm_name = "Swarm Algorithm";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_SWARM";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GSwarmAlgorithm_PersonalityTraits;

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of ALL this class's local data members, driving the
     * GReflectiveInterfaceBaseT-generated serialize(), load_(), compare_() and name_() from one source.
     *
     * The neighborhood bookkeeping (n_neighborhoods_ / n_neighborhood_members_cnt_) is copied by plain
     * assignment; the per-neighborhood bests (neighborhood_bests_cnt_) and the global best (global_best_ptr_)
     * are independent clone() SNAPSHOTS (never aliases into the population), so the cloneable-member policies
     * deep-clone them element-wise -- correctly reproducing every iteration state, including iteration 0
     * (a null best clones to null) and a fresh, differently-sized target (the container resizes on clone).
     * The former conditional reconstruction in load_()/compare_() was an efficiency (load-in-place) and
     * null-guarding optimisation the generic policies already subsume. */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. self_type is deduced as the (const) class type.
    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            Gem::Common::make_member(
                "default_n_neighborhood_members_",
                self.default_n_neighborhood_members_
            ),
            Gem::Common::make_member("c_personal_", self.c_personal_),
            Gem::Common::make_member("c_neighborhood_", self.c_neighborhood_),
            Gem::Common::make_member("c_global_", self.c_global_),
            Gem::Common::make_member("c_velocity_", self.c_velocity_),
            Gem::Common::make_member("update_rule_", self.update_rule_),
            Gem::Common::make_member("random_fill_up_", self.random_fill_up_),
            Gem::Common::make_member("repulsion_threshold_", self.repulsion_threshold_),
            Gem::Common::make_member("velocity_range_percentage_", self.velocity_range_percentage_),
            Gem::Common::make_member("n_neighborhoods_", self.n_neighborhoods_),
            Gem::Common::make_member(
                "n_neighborhood_members_cnt_",
                self.n_neighborhood_members_cnt_
            ),
            Gem::Common::make_cloneable_member("global_best_ptr_", self.global_best_ptr_),
            Gem::Common::make_cloneable_container_member(
                "neighborhood_bests_cnt_",
                self.neighborhood_bests_cnt_
            )
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The default constructor */
    GSwarmAlgorithm();
    /**
     * @brief Initialization with neighborhood sizes and amount of individuals in each neighborhood.
     */
    GSwarmAlgorithm(
        const std::size_t &n_neighborhoods,
        const std::size_t &default_n_neighborhood_members
    );
    /**
     * @brief A standard copy constructor
     */
    GSwarmAlgorithm(const GSwarmAlgorithm &cp);
    /** @brief The destructor */
    ~GSwarmAlgorithm() override = default;

    /**
     * @brief Sets the number of neighborhoods and the number of members in them.
     */
    void setSwarmSizes(std::size_t n_neighborhoods, std::size_t default_n_neighborhood_members);

    /**
     * @brief Allows to set a static multiplier for personal distances.
     */
    void setCPersonal(double c_personal);
    /**
     * @brief Allows to retrieve the static multiplier for personal distances.
     */
    double getCPersonal() const;

    /**
     * @brief Allows to set a static multiplier for neighborhood distances.
     */
    void setCNeighborhood(double c_neighborhood);
    /**
     * @brief Allows to retrieve the static multiplier for neighborhood distances.
     */
    double getCNeighborhood() const;

    /**
     * @brief Allows to set a static multiplier for global distances.
     */
    void setCGlobal(double c_global);
    /**
     * @brief Allows to retrieve the static multiplier for global distances.
     */
    double getCGlobal() const;

    /**
     * @brief Allows to set a static multiplier for velocities.
     */
    void setCVelocity(double c_velocity);
    /**
     * @brief Allows to retrieve the static multiplier for velocities.
     */
    double getCVelocity() const;

    /**
     * @brief Allows to set the velocity range percentage.
     */
    void setVelocityRangePercentage(double velocity_range_percentage);
    /**
     * @brief Allows to retrieve the velocity range percentage.
     */
    double getVelocityRangePercentage() const;

    /**
     * @brief Retrieves the number of neighborhoods.
     */
    std::size_t getNNeighborhoods() const;
    /**
     * @brief Retrieves the default number of individuals in each neighborhood.
     */
    std::size_t getDefaultNNeighborhoodMembers() const;
    /**
     * @brief Retrieves the current number of individuals in a given neighborhood.
     */
    std::size_t getCurrentNNeighborhoodMembers(const std::size_t &neighborhood) const;

    /**
     * @brief Allows to specify the update rule to be used by the swarm.
     */
    void setUpdateRule(updateRule ur);
    /**
     * @brief Allows to retrieve the update rule currently used by the swarm.
     */
    updateRule getUpdateRule() const;

    /**
     * @brief Allows to specify the number of stalls as of which the algorithm switches to repulsive mode.
     */
    void setRepulsionThreshold(std::uint32_t repulsion_threshold);
    /**
     * @brief Allows to retrieve the number of stalls as of which the algorithm switches to repulsive mode.
     */
    std::uint32_t getRepulsionThreshold() const;

    /** @brief All individuals automatically added to a neighborhood will have equal value */
    void setNeighborhoodsEqualFillUp();
    /**
     * @brief All individuals automatically added to a neighborhood will have a random value.
     */
    void setNeighborhoodsRandomFillUp(bool random_fill_up = true);
    /**
     * @brief Allows to check whether neighborhoods are filled up with random individuals.
     */
    bool neighborhoodsFilledUpRandomly() const;

    /***************************************************************************/
    /**
	  * Retrieves the best individual of a neighborhood and casts it to the desired type. The C++20
	  * `requires std::derived_from` constraint below makes this overload visible to the compiler only when
	  * individual_type is a derivative of GGenome.
	  *
	  * The individual handed back is a fresh copy, solely owned by the caller — like every other
	  * getBest* accessor, it cannot be used to modify the algorithm's own stored best.
	  *
	  * @tparam individual_type The target type to which the best individual is cast (must derive from gen::GGenome)
	  * @param neighborhood The neighborhood, whose best individual should be returned
	  * @return A copy of the best individual of a given neighborhood, as sole owner
	  */
    template <typename individual_type>
        requires std::derived_from<individual_type, gen::GGenome>
    std::unique_ptr<individual_type> getBestNeighborhoodIndividual(std::size_t neighborhood) {
#ifdef DEBUG
        // Check that the neighborhood is in a valid range
        if(neighborhood >= n_neighborhoods_) {
            throw gem_exception(
                g_error_streamer() << "Requested neighborhood which does not exist: "
                                   << neighborhood << " / " << n_neighborhoods_ << '\n'
            );
        }
#endif /* DEBUG */

        // clone<>() deep-copies and checks the conversion internally (throws on a type mismatch)
        return neighborhood_bests_cnt_[neighborhood]->template clone<individual_type>();
    }

protected:
    /***************************************************************************/
    // Virtual or overridden protected functions

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_(), compare_() and name_() are generated by the Gem::Common::GReflectiveInterfaceBaseT base from
    // class_name and the single localMembers_() declaration (which now carries the neighborhood
    // bookkeeping and the cloneable neighborhood / global bests). The former conditional reconstruction
    // is subsumed by the cloneable-member policies (see the localMembers_() comment above).

    /** @brief The swarm reuses late returns: a particle that returns asynchronously is re-attached to its
     *  neighborhood and folded into the next iteration (no age window -- the base default keeps all ages).
     *  @return true (the swarm algorithm reaps late returns) */
    bool reapsLateReturns() const override {
        return true;
    }

    /** @brief Resets the settings of this population to what was configured when the optimize()-call was issued */
    void resetToOptimizationStart_() override;

    /** @brief Does some preparatory work before the optimization starts */
    void init() override;
    /** @brief Does any necessary finalization work */
    void finalize() override;

    /**
     * @brief Updates the best individuals found.
     */
    virtual std::tuple<double, double> findBests();
    /** @brief findBests() phase 1: (re)computes every individual's personal best. */
    void updateAllPersonalBests();
    /** @brief findBests() phase 2: sorts each neighborhood by fitness and refreshes its neighborhood best. */
    void sortNeighborhoodsAndUpdateBests(maxMode mode);
    /** @brief findBests() phase 3: picks the best neighborhood best and updates the global best. */
    void updateGlobalBest(maxMode mode);
    /** @brief findBests() phase 4: returns the best fitness tuple seen in the current iteration. */
    [[nodiscard]] std::tuple<double, double> bestIterationFitness(maxMode mode) const;
    /** @brief Triggers an update of all individual's positions */
    virtual void updatePositions();

    /** @brief Fixes an incomplete population */
    virtual void adjustNeighborhoods();

    // name_(), clone_(), getAlgorithmName_(), getAlgorithmPersonalityType_()
    // are generated by the GOptimizationAlgorithmT scaffold from the oa_* identifiers above.

    /***************************************************************************/

    /**
     * @brief Checks whether each neighborhood has the default size.
     */
    bool neighborhoodsHaveNominalValues() const;

    /**
     * @brief Helper function that returns the id of the first individual of a neighborhood.
     */
    std::size_t getFirstNIPos(const std::size_t &neighborhood) const;
    /**
     * @brief Helper function that returns the id of the first individual of a neighborhood, using a vector of neighborhood sizes.
     */
    static std::size_t
    getFirstNIPosVec(const std::size_t &neighborhood, const std::vector<std::size_t> &vec);
    /**
     * @brief Helper function that returns the id of the last individual of a neighborhood.
     */
    std::size_t getLastNIPos(const std::size_t &neighborhood) const;

    /**
     * @brief Triggers an update of an individual's positions.
     *
     * The per-particle velocity lives on the slot's OA scratch.
     */
    void updateIndividualPositions(
        const std::size_t &neighborhood,
        const std::unique_ptr<gen::GGenome> &ind, // the population slot being moved (borrowed)
        const std::shared_ptr<gen::GGenome> &neighborhood_best, // neighborhood best
        const std::shared_ptr<gen::GGenome> &global_best,       // global best
        std::tuple<double, double, double, double>
            constants // c_personal / c_neighborhood / c_global / c_velocity
    );                // (velocity now lives on the slot's OA scratch)

    /**
     * @brief Adjusts the velocity vector so that its values don't exceed the allowed value range.
     */
    void pruneVelocity(std::vector<double> &vel_vec);

    /**
     * @brief Updates the personal best of an individual.
     */
    static void updatePersonalBest(const std::unique_ptr<gen::GGenome> &ind_ptr);
    /**
     * @brief Updates the personal best of an individual, if a better solution was found.
     */
    void updatePersonalBestIfBetter(const std::unique_ptr<gen::GGenome> &ind_ptr);

    std::size_t n_neighborhoods_ =
        (DEFAULTNNEIGHBORHOODS ? DEFAULTNNEIGHBORHOODS
                               : 1); ///< The number of neighborhoods in the population
    std::size_t default_n_neighborhood_members_ =
        ((DEFAULTNNEIGHBORHOODMEMBERS <= 1)
             ? 2
             : DEFAULTNNEIGHBORHOODMEMBERS); ///< The desired number of individuals belonging to each neighborhood
    std::vector<std::size_t> n_neighborhood_members_cnt_ = std::vector<std::size_t>(
        n_neighborhoods_,
        0
    ); ///< The current number of individuals belonging to each neighborhood

    std::shared_ptr<gen::GGenome> global_best_ptr_; ///< The globally best individual

    std::vector<std::shared_ptr<gen::GGenome>> neighborhood_bests_cnt_ =
        std::vector<std::shared_ptr<gen::GGenome>>(
            n_neighborhoods_
        ); ///< The collection of best individuals from each neighborhood
    // (Per-particle velocities now live on each individual's OA scratch as a POD double block --
    //  key AUXKEY_SWARM_VELOCITY in GSwarmAlgorithm.cpp -- not in a parallel vector here.)

    double c_personal_ =
        DEFAULTCPERSONAL; ///< A factor for multiplication of personal best distances
    double c_neighborhood_ =
        DEFAULTCNEIGHBORHOOD; ///< A factor for multiplication of neighborhood best distances
    double c_global_ = DEFAULTCGLOBAL;     ///< A factor for multiplication of global best distances
    double c_velocity_ = DEFAULTCVELOCITY; ///< A factor for multiplication of velocities

    updateRule update_rule_ = DEFAULTUPDATERULE; ///< Specifies how the parameters are updated
    bool random_fill_up_ =
        true; ///< Specifies whether neighborhoods are filled up with random values

    std::uint32_t repulsion_threshold_ =
        DEFREPULSIONTHRESHOLD; ///< The number of stalls until the swarm algorithm switches to repulsion instead of attraction

    double vel_max_ =
        0.; ///< The dimensionless velocity cap (a fraction of the normalized unit interval); recomputed in init() (transient)
    std::size_t n_fp_parms_ =
        0; ///< The number of active floating point parameters; recomputed in init() (transient)

    double velocity_range_percentage_ =
        DEFAULTVELOCITYRANGEPERCENTAGE; ///< Indicates the percentage of a value range used for the initialization of the velocity

    std::vector<std::shared_ptr<gen::GGenome>>
        last_iteration_individuals_cnt_; ///< A temporary copy of the last iteration's individuals

private:
    /***************************************************************************/
    // Virtual or overridden private functions

    /**
     * @brief The actual business logic to be performed during each iteration.
     */
    std::tuple<double, double> cycleLogic_() override;
    /** @brief Updates the fitness of all individuals */
    void evaluatePopulation_() override;

    /**
     * @brief Retrieve a GPersonalityTraits object belonging to this algorithm.
     */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;

    /** @brief Resizes the population to the desired level and does some error checks */
    void adjustPopulation_() override;

    /***************************************************************************/

    /** @brief Small helper function that helps to fill up a neighborhood, if there is just one entry in it */
    void fillUpNeighborhood1();
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
