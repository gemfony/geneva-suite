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

// Standard headers go here
#include <cstddef>
#include <string_view>
#include <tuple>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOptimizationAlgorithmT.hpp"
#include "geneva/oa/GStandardPSO2011_PersonalityTraits.hpp"

#ifdef GEM_TESTING
#include "geneva/individuals/GTestIndividual1.hpp"
#include "common/GSelfTestable.hpp"
#endif /* GEM_TESTING */

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * Default values for the Standard PSO 2011 algorithm.
 */
constexpr std::size_t DEFAULTSPSOSWARMSIZE = 40; ///< The default number of particles in the swarm
constexpr std::size_t DEFAULTSPSOK = 3;          ///< The default number of particles each particle informs

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A from-scratch implementation of Maurice Clerc's "Standard PSO 2011" (SPSO-2011).
 *
 * @details
 * SPSO-2011 is a rotation-invariant variant of particle swarm optimization that draws the next
 * candidate position from a hypersphere built around a center of gravity, rather than updating each
 * coordinate independently. This removes the coordinate-system dependence of classical PSO.
 *
 * @par Swarm state
 * The swarm consists of \f$S\f$ particles (default \f$S = 40\f$). Particle \f$i\f$ is described by three
 * \f$n\f$-dimensional vectors, where \f$n\f$ is the number of active floating-point parameters of the
 * individual:
 * \f[
 *   x_i \in \mathbb{R}^n \quad\text{(position)},\qquad
 *   v_i \in \mathbb{R}^n \quad\text{(velocity)},\qquad
 *   p_i \in \mathbb{R}^n \quad\text{(personal best)}.
 * \f]
 * The position \f$x_i\f$ is held in the Geneva population (one individual per particle); the velocity
 * \f$v_i\f$ and personal best \f$p_i\f$ are kept in parallel arrays owned by the algorithm.
 *
 * @par Adaptive random informant topology
 * The swarm communicates through an adaptive random topology. Each particle \f$i\f$ informs \f$K\f$
 * other particles drawn uniformly at random <em>with replacement</em>, plus itself, giving up to
 * \f$K+1\f$ informants (default \f$K = 3\f$). This induces a directed adjacency: particle \f$j\f$ is an
 * informant of particle \f$i\f$ if \f$j\f$ chose \f$i\f$ (and every particle is its own informant). The
 * <em>local best</em> of particle \f$i\f$ is
 * \f[
 *   l_i = \arg\min_{p_j,\; j \in \mathrm{informants}(i)} f(p_j),
 * \f]
 * i.e. the best personal best among all particles that inform \f$i\f$.
 *
 * The topology is regenerated at the start of an iteration <em>only if</em> the swarm's global best did
 * not improve during the previous iteration (and it is generated once on the first iteration). A
 * stagnating swarm therefore reshuffles its communication graph, which helps it escape local optima.
 *
 * @par Velocity and position update
 * SPSO-2011 uses the constants
 * \f[
 *   w = \frac{1}{2 \ln 2} \approx 0.721347, \qquad
 *   c = \frac{1}{2} + \ln 2 \approx 1.193147 .
 * \f]
 * For particle \f$i\f$ with personal best \f$p_i\f$ and local best \f$l_i\f$, two auxiliary points are
 * formed,
 * \f[
 *   P = x_i + c\,(p_i - x_i), \qquad
 *   L = x_i + c\,(l_i - x_i),
 * \f]
 * and the center of gravity \f$G_i\f$ is built. If the particle is its own best informant
 * (\f$p_i = l_i\f$) the 2-point center is used, otherwise the 3-point center:
 * \f[
 *   G_i =
 *   \begin{cases}
 *     \dfrac{x_i + P}{2}, & p_i = l_i, \\[2ex]
 *     \dfrac{x_i + P + L}{3}, & p_i \neq l_i .
 *   \end{cases}
 * \f]
 * A trial point \f$x'\f$ is then sampled uniformly from the hypersphere centered at \f$G_i\f$ with radius
 * \f$r = \lVert G_i - x_i \rVert\f$. A uniform direction on the unit sphere is obtained from a standard
 * normal vector \f$u \sim \mathcal{N}(0, I_n)\f$ via \f$d = u / \lVert u \rVert\f$, and a uniform radius
 * factor \f$\rho = U(0,1)^{1/n}\f$ gives a uniform draw from the solid ball:
 * \f[
 *   x' = G_i + r\,\rho\,d .
 * \f]
 * Finally the velocity and position are updated:
 * \f[
 *   v_i \leftarrow w\,v_i + (x' - x_i), \qquad
 *   x_i \leftarrow x_i + v_i .
 * \f]
 *
 * @par Boundary handling
 * The swarm works in the normalized internal coordinate and is boundary-agnostic: it never queries or
 * clamps parameter bounds. A position that overshoots a bounded parameter's range is folded back into
 * range by the genome on assignment (continuous reflection); an unbounded parameter roams freely. Only
 * the velocity is stabilized, by capping each component to a fixed fraction (SPSO_VMAX_FACTOR) of the
 * normalized unit interval.
 *
 * @par Velocity clamping (deviation from strict SPSO-2011)
 * After the velocity update each component is clamped to \f$\pm\,k\,(\mathrm{upper}_d-\mathrm{lower}_d)\f$
 * (\f$k=0.2\f$). Strict SPSO-2011 omits a \f$V_{\max}\f$ and relies on the constriction (\f$w<1\f$) to
 * contract the swarm. That contraction works well in low dimension, but in HIGH dimension the
 * hypersphere radius factor \f$U(0,1)^{1/n}\to 1\f$, so every trial point sits at distance
 * \f$\approx\lVert\mathbf{G}-\mathbf{x}\rVert\f$ from the centre: the step never becomes small relative to
 * the swarm spread, the velocity does not decay, and the swarm fails to contract (it stagnates,
 * coordinates pinned near the box walls). The \f$V_{\max}\f$ clamp curbs this runaway and roughly halves
 * the stalled fitness. It is, however, only a mitigation: SPSO-2011 has a well-known high-dimensional
 * weakness and is out-scaled at large \f$n\f$ by CMA-ES and archive methods (e.g. ACOR) -- prefer those
 * for high-dimensional problems.
 *
 * @par Initialization
 * Position \f$x_i\f$ is drawn uniformly from the box \f$[\mathrm{lower}_d, \mathrm{upper}_d]\f$ (particle
 * 0 keeps the registered start individual). The velocity uses the SPSO-2011 "half-diff" rule,
 * \f[
 *   v_{i,d} = \tfrac{1}{2}\bigl(U(\mathrm{lower}_d, \mathrm{upper}_d) - x_{i,d}\bigr),
 * \f]
 * and the personal best is initialized to the start position, \f$p_i = x_i\f$.
 *
 * @note Clean-room implementation: derived solely from the published SPSO-2011 specification, not adapted
 * from any existing PSO codebase, keeping it free of copyleft entanglement with Geneva's Apache-2.0
 * licence.
 *
 * @par References
 * - M. Clerc, "Standard Particle Swarm Optimisation", 2012 (the SPSO-2011 specification).
 */
class GStandardPSO2011 // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GStandardPSO2011>
  , public Gem::Common::GSelfTestable {
public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold.
    static constexpr std::string_view class_name = "GStandardPSO2011";
    static constexpr std::string_view oa_algorithm_name = "Standard PSO 2011";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_SPSO2011";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GStandardPSO2011_PersonalityTraits;

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class'es local data members (drives serialize/load/compare).
     *  Only the scalar configuration is persisted; all per-particle swarm state (velocities, personal
     *  bests, topology, bounds, ...) is transient and rebuilt in init(). */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("swarm_size_", self.swarm_size_),
            Gem::Common::make_member("n_informants_", self.n_informants_)
        );
    }

    // serialize(), load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base (via GOptimizationAlgorithmT) from class_name and localMembers_().
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The default constructor */
    GStandardPSO2011();
    /** @brief Initialization with the swarm size */
    explicit GStandardPSO2011(std::size_t swarm_size);
    /** @brief A standard copy constructor */
    GStandardPSO2011(const GStandardPSO2011 &) = default;
    /** @brief The destructor */
    ~GStandardPSO2011() override = default;

    /** @brief Retrieves the number of particles in the swarm */
    std::size_t getSwarmSize() const;
    /** @brief Allows to set the number of particles in the swarm (at least 2) */
    void setSwarmSize(std::size_t swarm_size);

    /** @brief Retrieves the number of particles each particle informs (K) */
    std::size_t getNInformants() const;
    /** @brief Allows to set the number of particles each particle informs (K, at least 1) */
    void setNInformants(std::size_t n_informants);

protected:
    /***************************************************************************/
    // Virtual or overridden protected functions

    /** @brief Adds local configuration options to a GParserBuilder object */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceT base.

    /** @brief Resets the settings of this population to what was configured when optimize() was issued */
    void resetToOptimizationStart_() override;

    /** @brief Does some preparatory work before the optimization starts */
    void init() override;
    /** @brief Does any necessary finalization work */
    void finalize() override;

    /** @brief Applies modifications to this object. This is needed for testing purposes */
    bool modify_GUnitTests_() override;
    /** @brief Performs self tests that are expected to succeed. This is needed for testing purposes */
    void specificTestsNoFailureExpected_GUnitTests_() override;
    /***************************************************************************/

private:
    /***************************************************************************/
    // Overloaded or virtual base functions. name_(), clone_(), getAlgorithmName_(),
    // getAlgorithmPersonalityType_() are generated by the GOptimizationAlgorithmT scaffold.

    /** @brief The actual business logic performed during each iteration */
    std::tuple<double, double> cycleLogic_() override;
    /** @brief Triggers fitness calculation of all individuals via the one process consumer */
    void evaluatePopulation_() override;


    /** @brief Retrieve a GPersonalityTraits object belonging to this algorithm */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;

    /** @brief Resizes the population to the desired level and does some error checks */
    void adjustPopulation_() override;


    /***************************************************************************/
    // Algorithm-internal helpers

    /** @brief (Re-)builds the random informant topology */
    void buildTopology();
    /** @brief Updates the velocity and position of every particle */
    void updatePositions();
    /** @brief Updates the personal bests and the global best; returns the best fitness of this iteration */
    std::tuple<double, double> updateBests();
    /** @brief Determines the local best position of a particle from its informants */
    std::vector<double> localBest(std::size_t particle) const;

    /***************************************************************************/
    // User-facing strategy parameters (configured by the user / factory)

    std::size_t swarm_size_ = DEFAULTSPSOSWARMSIZE; ///< The number of particles in the swarm
    std::size_t n_informants_ = DEFAULTSPSOK;       ///< The number of particles each particle informs (K)

    /***************************************************************************/
    // Internal swarm state (transient; rebuilt in init(), excluded from serialize/compare)

    std::size_t n_fp_parms_ = 0; ///< The number of active floating point parameters (set in init())

    std::vector<std::vector<double>>
        velocities_; ///< Per-particle velocity vectors
    std::vector<std::vector<double>>
        personal_bests_; ///< Per-particle personal-best positions
    std::vector<double>
        personal_best_fitness_; ///< Per-particle personal-best (minimization) fitness

    std::vector<double> global_best_; ///< The globally best position found so far
    double global_best_fitness_ = 0.; ///< The globally best (minimization) fitness

    std::vector<std::vector<bool>>
        informs_; ///< informs_[i][j] == true iff particle i informs particle j

    bool global_best_improved_ =
        true; ///< Whether the global best improved in the previous iteration (gates topology rebuild)
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

