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
#include <ostream>
#include <string_view>
#include <tuple>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "common/GFixedSizePriorityQueueT.hpp" // the solution archive IS the bounded top-k queue
#include "common/GReflectiveInterfaceT.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOptimizationAlgorithmT.hpp"
#include "geneva/oa/GAntColonyOptimization_PersonalityTraits.hpp"

#ifdef GEM_TESTING
#include "geneva/individuals/GTestIndividual1.hpp"
#include "common/GSelfTestable.hpp"
#endif /* GEM_TESTING */

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * Default values for the continuous Ant Colony Optimization (ACOR).
 */
constexpr std::size_t DEFAULTACORARCHIVESIZE = 50; ///< Archive size k (the pheromone model)
constexpr std::size_t DEFAULTACORNANTS = 2;        ///< Number of ants m constructed per iteration
constexpr double DEFAULTACORQ = 1.e-4;             ///< Locality / intensification parameter q
constexpr double DEFAULTACORXI = 0.85;             ///< Evaporation / convergence-speed parameter xi

/******************************************************************************/
/**
 * @brief One archived ACOR solution: a candidate parameter vector plus the fitness it scored.
 *
 * The parameters are the individual's ACTIVE floating-point values in the normalized internal
 * coordinate; the fitness is the min-only transformed one, so the archive always ranks as a
 * minimization regardless of the problem's maxMode.
 */
struct ACOSolution {
    std::vector<double> parms; ///< The candidate's active floating-point parameters
    double fitness = 0.;       ///< Its min-only transformed fitness (smaller is better)

    /** @brief Value equality over both members.
     *  @param other The solution to compare against. @return true iff both records are equal. */
    bool operator==(const ACOSolution &other) const = default;
};

/** @brief Streams a solution, so the comparison DSL can report a differing archive entry.
 *  @param stream The stream to write to. @param sol The solution to write. @return The stream. */
inline std::ostream &operator<<(std::ostream &stream, const ACOSolution &sol) {
    stream << "ACOSolution{fitness=" << sol.fitness << ", n_parms=" << sol.parms.size() << "}";
    return stream;
}

/** @brief Non-intrusive GArchive (de)serializer for a solution record.
 *  @tparam Archive The GArchive codec type. @param ar The archive. @param sol The record. */
template <class Archive>
void gem_archive_serialize(Archive &ar, ACOSolution &sol) {
    Gem::Common::archive_named(ar, "parms", sol.parms);
    Gem::Common::archive_named(ar, "fitness", sol.fitness);
}

/******************************************************************************/
/**
 * @brief ACOR's solution archive: the k best solutions seen, ranked best-first.
 *
 * This is exactly the fixed-size priority queue's contract -- a runtime capacity, insertion that
 * keeps the container ordered, eviction of the worst on overflow, and a bulk add that merges a batch
 * and truncates in one call -- so it IS that queue rather than a second implementation of it. Value
 * storage is the natural holder: a solution record is plain data with no identity of its own.
 *
 * The archive is transient run state, rebuilt in init() and excluded from the algorithm's serialized
 * member list; the queue's own serialization support simply goes unused here.
 */
class GACOSolutionArchive // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Common::GReflectiveInterfaceT<
        GACOSolutionArchive,
        Gem::Common::GPodFixedSizePriorityQueueT<ACOSolution>> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief This class adds no own members; the explicit empty declaration is required
     *  (the mixin's deleted fallback rejects a missing one). */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GACOSolutionArchive";

    /** @brief The default constructor (an unsized archive; init() sets the capacity k). */
    GACOSolutionArchive() = default;
    /** @brief Initialization with the archive size k.
     *  @param max_size The maximum number of solutions the archive keeps. */
    explicit GACOSolutionArchive(std::size_t max_size)
      : Gem::Common::GReflectiveInterfaceT<
            GACOSolutionArchive,
            Gem::Common::GPodFixedSizePriorityQueueT<ACOSolution>>(max_size) {
    }
    /** @brief The copy constructor. @param cp The archive to copy. */
    GACOSolutionArchive(const GACOSolutionArchive &cp) = default;
    /** @brief The destructor. */
    ~GACOSolutionArchive() override = default;

protected:
    /** @brief The ranking criterion: the record's own (min-only) fitness.
     *  @param sol The archived solution. @return Its fitness. */
    [[nodiscard]] double evaluation(const ACOSolution &sol) const override {
        return sol.fitness;
    }
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * GAntColonyOptimization implements ACOR -- Ant Colony Optimization for
 * continuous domains, as introduced by Socha & Dorigo (2008). It is a
 * population-based, derivative-free global optimizer.
 *
 * @brief Continuous Ant Colony Optimization (ACOR, Socha & Dorigo 2008).
 *
 * @details
 *
 * <h3>The solution archive as a continuous pheromone model</h3>
 *
 * Classical (combinatorial) ant colony optimization deposits pheromone on the
 * edges of a discrete graph. In a continuous search space there are no
 * discrete edges, so ACOR replaces the discrete pheromone table by a
 * \em solution \em archive
 * \f[
 *   T = \{\, s_1, s_2, \dots, s_k \,\}, \qquad s_l \in \mathbb{R}^n ,
 * \f]
 * an ordered set of \f$k\f$ complete candidate solutions of dimension \f$n\f$,
 * kept sorted best-first by their (min-only transformed) fitness, so that
 * \f$s_1\f$ is the incumbent best. The archive \em is the pheromone: each
 * member spawns a Gaussian kernel from which new solutions ("ants") are
 * sampled, and intensification happens by ranking rather than by additive
 * deposition/evaporation on edges.
 *
 * <h3>Ranked Gaussian weights and selection probabilities</h3>
 *
 * Archive member \f$l\f$ (with rank \f$l=1\f$ being the best) is assigned a
 * weight drawn from a Gaussian centred on the top rank,
 * \f[
 *   w_l \;=\; \frac{1}{q\,k\,\sqrt{2\pi}}\,
 *             \exp\!\left( -\,\frac{(l-1)^2}{2\,q^2\,k^2} \right),
 *   \qquad l = 1,\dots,k .
 * \f]
 * The locality parameter \f$q>0\f$ controls intensification: a small \f$q\f$
 * concentrates the weights on the very best members (strong exploitation),
 * while a large \f$q\f$ flattens them towards a uniform preference
 * (exploration). A guiding member is then chosen by roulette-wheel selection
 * with probability
 * \f[
 *   p_l \;=\; \frac{w_l}{\displaystyle\sum_{j=1}^{k} w_j}.
 * \f]
 *
 * <h3>Per-dimension Gaussian kernel sampling</h3>
 *
 * One new solution (one ant) is built dimension by dimension and
 * independently. For each dimension \f$i = 1,\dots,n\f$:
 * <ol>
 *   <li>a guiding member \f$l\f$ is drawn from \f$\{p_l\}\f$ by roulette wheel;</li>
 *   <li>its bandwidth in dimension \f$i\f$ is the \f$\xi\f$-scaled average
 *       distance of the chosen member to all other archive members in that
 *       dimension,
 *       \f[
 *         \sigma_{l,i} \;=\; \xi \cdot \frac{1}{k-1}
 *           \sum_{\substack{e=1 \\ e \neq l}}^{k}
 *           \bigl|\, s_e[i] - s_l[i] \,\bigr| ;
 *       \f]
 *       the evaporation / convergence-speed parameter \f$\xi>0\f$ plays the
 *       role of pheromone evaporation: a smaller \f$\xi\f$ shrinks the sampling
 *       width and accelerates convergence;</li>
 *   <li>the new coordinate is drawn from the kernel
 *       \f[
 *         x_{\text{new}}[i] \;\sim\; \mathcal{N}\!\bigl( s_l[i],\, \sigma_{l,i} \bigr).
 *       \f]
 *       If \f$\sigma_{l,i}=0\f$ (a degenerate archive in that dimension) the
 *       coordinate is left at \f$s_l[i]\f$.</li>
 * </ol>
 * The sampled vector is written back through the genome, so Geneva's
 * constrained parameter objects fold/clamp it into the feasible box
 * automatically.
 *
 * <h3>Archive update (merge + truncate)</h3>
 *
 * Per iteration \f$m\f$ ants are constructed and evaluated. The newly
 * evaluated ants are merged with the current archive, the union is re-sorted
 * best-first, and the worst members are discarded to truncate it back to the
 * best \f$k\f$:
 * \f[
 *   T' \;=\; \operatorname{best}_k\bigl( T \cup \{x^{(1)},\dots,x^{(m)}\} \bigr).
 * \f]
 * The reported best solution is always \f$s_1\f$ (archive[0]).
 *
 * <h3>Contrast with particle swarm optimization</h3>
 *
 * ACOR is fundamentally different from particle swarm optimization. A swarm
 * carries per-particle \em velocities and moves each particle deterministically
 * (plus noise) towards a blend of its personal best and the neighbourhood/global
 * best. ACOR has \em no velocities and no per-individual memory: every new
 * solution is \em sampled afresh from a probabilistic model -- a ranked archive
 * of elite solutions, each defining a Gaussian kernel. Exploration is therefore
 * governed by the spread of the archive (and \f$q\f$, \f$\xi\f$), not by inertia
 * and cognitive/social acceleration terms.
 *
 * <h3>Reuse of Geneva ES/EA facilities</h3>
 *
 * The implementation deliberately reuses existing Geneva machinery where the
 * mathematics overlaps with evolutionary strategies (ES) and evolutionary
 * algorithms (EA):
 * <ul>
 *   <li>the solution archive is conceptually the EA's ranked parent set;
 *       ranking uses the same min-only transformed fitness helper
 *       (Gem::Geneva::minOnly_transformed_fitness) and isBetter()/getMaxMode()
 *       so that maximisation problems are handled transparently;</li>
 *   <li>the per-dimension Gaussian kernel sampling is an ES-style mutation; it
 *       reuses the algorithm's inherited random-number generator
 *       (GOptimizationAlgorithmBase::gr_) together with std::normal_distribution
 *       and the inherited uniform_real_distribution_ (the latter for the roulette
 *       wheel), exactly as the swarm and sep-CMA-ES strategies draw their
 *       randomness;</li>
 *   <li>candidate parameters are read and written through the standard flat
 *       genome FP channels (streamlineFPInternal, assignFPValueVectorInternal) with
 *       activityMode::ACTIVEONLY, and the population is evaluated through the one
 *       process consumer via workOnPopulation(), mirroring the other from-scratch
 *       flat-genome optimizers (e.g. GSepCmaEvolutionStrategy, GStandardPSO2011).</li>
 * </ul>
 *
 * ACOR is FP-only (continuous) and needs no adaption configuration: mutation is
 * performed by the algorithm's sampling, not by per-parameter adaptors.
 *
 * @par References
 * - K. Socha and M. Dorigo, "Ant colony optimization for continuous domains",
 *   European Journal of Operational Research, 185(3):1155-1173, 2008.
 */
class GAntColonyOptimization // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GAntColonyOptimization>
  , public Gem::Common::GSelfTestable {
public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold.
    static constexpr std::string_view class_name = "GAntColonyOptimization";
    static constexpr std::string_view oa_algorithm_name = "Ant Colony Optimization (continuous)";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_ACOR";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GAntColonyOptimization_PersonalityTraits;

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class'es local data members (drives serialize/load/compare).
     *  Only the scalar configuration (k, m, q, xi) is persisted; all per-iteration archive state
     *  (parameter vectors, fitnesses, bounds, selection probabilities) is transient and rebuilt in
     *  init(). */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("archive_size_", self.archive_size_),
            Gem::Common::make_member("n_ants_", self.n_ants_),
            Gem::Common::make_member("q_", self.q_),
            Gem::Common::make_member("xi_", self.xi_)
        );
    }

    // serialize(), load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base (via GOptimizationAlgorithmT) from class_name and localMembers_().
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The default constructor */
    GAntColonyOptimization();
    /** @brief Initialization with the archive size */
    explicit GAntColonyOptimization(std::size_t archive_size);
    /** @brief A standard copy constructor */
    GAntColonyOptimization(const GAntColonyOptimization &) = default;
    /** @brief The destructor */
    ~GAntColonyOptimization() override = default;

    /** @brief Sets the archive size k (the pheromone model size; at least 2) */
    void setArchiveSize(std::size_t archive_size);
    /** @brief Retrieves the archive size k */
    std::size_t getArchiveSize() const;

    /** @brief Sets the number of ants m constructed per iteration */
    void setNAnts(std::size_t n_ants);
    /** @brief Retrieves the number of ants m */
    std::size_t getNAnts() const;

    /** @brief Sets the locality / intensification parameter q (> 0) */
    void setQ(double q);
    /** @brief Retrieves the locality / intensification parameter q */
    double getQ() const;

    /** @brief Sets the evaporation / convergence-speed parameter xi (> 0) */
    void setXi(double xi);
    /** @brief Retrieves the evaporation / convergence-speed parameter xi */
    double getXi() const;

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

    /** @brief Constructs the m new ant solutions for this iteration */
    virtual void constructAnts();
    /** @brief Merges the freshly evaluated ants into the archive and truncates to k */
    virtual void updateArchive();

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
    /** @brief Triggers fitness calculation of the relevant individuals via the one process consumer */
    void evaluatePopulation_() override;

    /** @brief Retrieves the number of processable items for the current iteration */
    std::size_t getNProcessableItems_() const override;

    /** @brief Retrieve a GPersonalityTraits object belonging to this algorithm */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;

    /** @brief Resizes the population to the desired level and does some error checks */
    void adjustPopulation_() override;


    /***************************************************************************/
    // Algorithm-internal helpers
    /** @brief Computes the ranked selection probabilities p_l from the weights w_l */
    void computeSelectionProbabilities();
    /** @brief Seeds (and randomizes) the initial candidates into the first k population slots */
    void seedInitialArchive();
    /** @brief Reads an evaluated population slot back into an archivable solution record
     *  @param pos The population slot to read. @return The slot's parameters and min-only fitness. */
    ACOSolution solutionFromSlot(std::size_t pos);
    /** @brief Picks an archive rank by roulette wheel over the selection probabilities */
    std::size_t rouletteSelect();

    /***************************************************************************/
    // User-facing strategy parameters (configured by the user / factory)

    std::size_t archive_size_ = DEFAULTACORARCHIVESIZE; ///< Archive size k (the continuous pheromone model)
    std::size_t n_ants_ = DEFAULTACORNANTS;             ///< Number of ants m constructed per iteration
    double q_ = DEFAULTACORQ;                           ///< Locality / intensification parameter
    double xi_ = DEFAULTACORXI;                         ///< Evaporation / convergence-speed parameter

    /***************************************************************************/
    // Internal archive state (transient; rebuilt in init(), excluded from serialize/compare)

    std::size_t n_fp_parms_ = 0; ///< The number of active floating point parameters (set in init())

    GACOSolutionArchive
        archive_; ///< The k archived solutions (the pheromone), ranked best-first; sized in init()

    std::vector<double>
        selection_probabilities_; ///< Cached roulette-wheel probabilities p_l
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

