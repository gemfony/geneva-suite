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

// Standard headers
#include <cstddef>
#include <memory>

// Boost headers

// Geneva headers
#include "common/GMemberReflectionT.hpp"
#include "common/concurrency/GThreadPool.hpp"
#include "courtier/GSubmissionStatusT.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * @brief The meta-optimization evolutionary algorithm: an EA that tunes an optimization algorithm's own
 * parameters by treating each candidate parameter-set as an individual whose fitness is the quality the
 * tuned algorithm achieves.
 *
 * @details
 * This is a standard evolutionary algorithm whose population members are "umbrella" individuals
 * (GMetaOptimizerIndividualT), each encoding a vector of hyper-parameters \f$\boldsymbol{\theta}\f$ for an
 * inner algorithm (e.g. an EA's \f$\sigma\f$ bounds, \f$\sigma\f$-adaption strength, adaption probability,
 * parent/child counts). The meta-objective is the @e expected solution quality the inner algorithm reaches
 * with those parameters: evaluating an umbrella individual runs \f$R\f$ independent sub-optimizations of
 * the target problem configured with \f$\boldsymbol{\theta}\f$ and aggregates their results,
 * \f[
 *   F(\boldsymbol{\theta}) \;=\; \frac{1}{R}\sum_{r=1}^{R} q_r(\boldsymbol{\theta}),
 * \f]
 * where \f$q_r\f$ is the best fitness (or the solver-call count, per the configured target) of run
 * \f$r\f$. Averaging over \f$R\f$ runs damps the stochastic noise of a single optimization so the meta-EA
 * selects on robust, repeatable performance rather than a lucky seed. The outer loop is then the ordinary
 * EA: \f$(\mu,\lambda)\f$ / \f$(\mu+\lambda)\f$ selection, adaption and sorting over the
 * \f$\boldsymbol{\theta}\f$ genomes. It is the SINGLE facility for meta-optimization in Geneva.
 *
 * @par Two-tier evaluation (deadlock-free nesting)
 * The only thing it changes about GEvolutionaryAlgorithm is WHERE its population is evaluated: a meta-EA
 * evaluates its umbrella-individuals on its OWN orchestration thread pool, NOT through the process-wide
 * work consumer. Each umbrella-individual's evaluation in turn runs a sub-optimization whose fine-grained
 * work items DO go to the one work consumer. So the meta level is just an orchestration pool -- never a
 * second consumer -- which keeps the "one consumer per process" rule intact AND avoids deadlock: the
 * orchestration pool and the work consumer are distinct pools, so an umbrella-individual blocking on its
 * sub-optimization's results (handled by the work consumer) never starves the pool it is running on.
 *
 * It derives from GEvolutionaryAlgorithm to reuse all of the evolutionary machinery (selection, adaption,
 * sorting, personality traits -- its individuals are evolved exactly like an EA's, hence PERSONALITY_EA);
 * it overrides only the per-generation evaluation seam (evaluatePopulationRange_) and clone_().
 */
class GMetaEvolutionaryAlgorithm
  : public Gem::Common::GReflectiveInterfaceT<GMetaEvolutionaryAlgorithm, GEvolutionaryAlgorithm> {
protected:
    /***************************************************************************/
    /**
     * @brief Single declaration of this class'es local data members (mutable access). The transient
     * orchestration_pool_ is deliberately NOT part of this tuple. Defined before its inline callers
     * (serialize(), the copy constructor) so the deduced return type is available to them.
     *
     * @return A tuple of named, comparable/serializable references to this object's local data members
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("n_orchestration_threads_", self.n_orchestration_threads_)
        );
    }

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    // serialize(), load_(), compare_(), name_() and clone_() are all generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_(). The mixin's clone_() builds a
    // GMetaEvolutionaryAlgorithm (Derived), so it deep-clones the whole meta-EA with no slicing -- the
    // reason the class formerly hand-wrote clone_(). The transient orchestration_pool_ stays out of
    // localMembers_(), so it is neither serialized, compared, nor copied (rebuilt on first use).
    ///////////////////////////////////////////////////////////////////////

public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold. The personality type stays
    // PERSONALITY_EA: the umbrella-individuals are evolved exactly like an EA's and carry EA traits.
    static constexpr std::string_view class_name = "GMetaEvolutionaryAlgorithm";
    static constexpr std::string_view oa_algorithm_name = "Meta Evolutionary Algorithm";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_EA";

    /** @brief The default constructor */
    GMetaEvolutionaryAlgorithm() = default;
    /** @brief The copy constructor. The orchestration pool is transient run state, so a copy starts
     *  without one (it is re-established on first use); the local config members are copied through
     *  localMembers_(), the same single source serialize()/load_()/compare_() use.
     *  @param cp The meta-EA to copy */
    GMetaEvolutionaryAlgorithm(const GMetaEvolutionaryAlgorithm &cp)
      : Gem::Common::GReflectiveInterfaceT<GMetaEvolutionaryAlgorithm, GEvolutionaryAlgorithm>(cp) {
        Gem::Common::g_load_members(this->localMembers_(), cp.localMembers_());
        /* orchestration_pool_ deliberately left null -- rebuilt on first use */
    }
    /** @brief The standard destructor */
    ~GMetaEvolutionaryAlgorithm() override = default;

    /** @brief Sets the number of umbrella-individuals evaluated in parallel on the orchestration pool.
     *  @param n The orchestration-pool size; 0 means hardware concurrency */
    void setNOrchestrationThreads(unsigned int n) { n_orchestration_threads_ = n; }
    /** @brief Retrieves the configured orchestration-pool size (0 == hardware concurrency).
     *  @return The configured number of orchestration threads */
    [[nodiscard]] unsigned int getNOrchestrationThreads() const { return n_orchestration_threads_; }

protected:
    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceT base.

    /***************************************************************************/
    /**
     * @brief Evaluates the population's [start, end) range on this meta-EA's OWN orchestration thread
     * pool instead of the process-wide work consumer. Each item's process() runs an umbrella-individual
     * (which spawns a sub-optimization that uses the work consumer). A local evaluation never goes
     * MISSING -- process() either succeeds (PROCESSED) or records a caught exception -- so the returned
     * status is always "complete", with the error flag set if any item failed.
     *
     * @param start First population index to evaluate (inclusive)
     * @param end One past the last population index to evaluate
     * @return The executor status: always complete; error flag set if any umbrella-individual failed
     */
    Gem::Courtier::submission_status_t
    evaluatePopulationRange_(std::size_t start, std::size_t end) override;

private:
    /***************************************************************************/
    std::unique_ptr<Gem::Common::Concurrency::GThreadPool> orchestration_pool_; ///< Transient: built on first use
    unsigned int n_orchestration_threads_ = 0; ///< Orchestration-pool size (0 == hardware concurrency)
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

