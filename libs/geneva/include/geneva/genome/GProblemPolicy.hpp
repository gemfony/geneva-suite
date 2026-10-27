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
#include <limits>
#include <memory>

// Boost header files go here

// Geneva header files go here
#include "common/GArchiveNamed.hpp"               // archive_named (named-member emitter)
#include "common/GCommonHelperFunctionsT.hpp"     // copyCloneableSmartPointer
#include "common/GCommonMathHelperFunctionsT.hpp"  // grational_sigmoid
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "geneva/genome/GMultiConstraintT.hpp"            // GPreEvaluationValidityCheckT
#include "geneva/GOptimizationEnums.hpp"           // maxMode, evaluationPolicy, the sigmoid constants

namespace Gem::Geneva::Genome {

/******************************************************************************/
// Forward declaration: the constraint inspects an entity's parameter values via the value API
// (streamlineFP) declared on the algorithm-facing base GGenome.
class GGenome;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The shared, problem-uniform feasibility and ranking policy.
 *
 * In a Geneva population every candidate solves the SAME problem, so the rules that turn a raw fitness
 * into a ranked, feasibility-aware evaluation -- the optimization direction (maxMode), the policy for
 * invalid solutions (evaluationPolicy), the sigmoid transform parameters, and the constraint object --
 * are identical across the whole population. GProblemPolicy holds that shared 1:N policy in one place;
 * each GGenome references a single policy instance, so the policy travels (and checkpoints)
 * once rather than once per individual.
 *
 * The per-individual state that genuinely differs (the computed validity level and the stored results)
 * stays on GGenome. Only the population-uniform rules live here.
 *
 * The constraint is parameterised on GGenome (the algorithm-facing base) because a concrete
 * constraint reads parameter values via the genome value API (e.g. streamlineFP) declared there.
 * fulfillsConstraints() therefore takes a GGenome; it is defined in the .cpp, where
 * GGenome is a complete type.
 *
 * This is a plain serialisable holder (not a GCommonInterfaceT category root): it is shared, not
 * deep-cloned per individual, so it needs no clone/compare category machinery -- only value semantics
 * (the copy constructor deep-clones the constraint so two policies never share one constraint object)
 * and Boost serialisation for checkpointing.
 */
class GProblemPolicy {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /**
     * @brief Serialises the shared policy: the four scalar rules plus the (polymorphic) constraint.
     * @tparam Archive The GArchive codec type
     * @param ar The archive to (de)serialise with
     * @param version The (unused) serialization version number
     */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        archive_named(ar, "maxmode_", maxmode_);
        archive_named(ar, "eval_policy_", eval_policy_);
        archive_named(ar, "sigmoid_steepness_", sigmoid_steepness_);
        archive_named(ar, "sigmoid_extremes_", sigmoid_extremes_);
        archive_named(ar, "constraint_ptr_", constraint_ptr_);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The default constructor (USESIMPLEEVALUATION, MINIMIZE, default sigmoid, no constraint) */
    GProblemPolicy() = default;

    /**
     * @brief The copy constructor. Deep-clones the constraint object so the copy and the original never
     * share one constraint instance. Defined in the .cpp, where the constraint type is complete.
     * @param cp The policy to copy from
     */
    GProblemPolicy(GProblemPolicy const &cp);

    /** @brief The move constructor */
    GProblemPolicy(GProblemPolicy &&) noexcept = default;

    /**
     * @brief The copy assignment operator. Deep-clones the constraint object. Defined in the .cpp.
     * @param cp The policy to copy from
     * @return A reference to this object
     */
    GProblemPolicy &operator=(GProblemPolicy const &cp);

    /** @brief The move assignment operator */
    GProblemPolicy &operator=(GProblemPolicy &&) noexcept = default;

    /** @brief The destructor */
    ~GProblemPolicy() = default;

    /***************************************************************************/
    // Optimization direction

    /** @brief Sets the optimization direction. @param mode MAXIMIZE or MINIMIZE */
    void setMaxMode(maxMode const &mode) { maxmode_ = mode; }
    /** @brief @return The optimization direction (MAXIMIZE or MINIMIZE) */
    [[nodiscard]] maxMode getMaxMode() const { return maxmode_; }

    /** @brief @return The worst-case evaluation value for the current direction
     *  (lowest representable double in MAXIMIZE mode, highest in MINIMIZE mode) */
    [[nodiscard]] double getWorstCase() const {
        return (maxMode::MAXIMIZE == maxmode_) ? std::numeric_limits<double>::lowest()
                                               : std::numeric_limits<double>::max();
    }
    /** @brief @return The best-case evaluation value for the current direction
     *  (highest representable double in MAXIMIZE mode, lowest in MINIMIZE mode) */
    [[nodiscard]] double getBestCase() const {
        return (maxMode::MAXIMIZE == maxmode_) ? std::numeric_limits<double>::max()
                                               : std::numeric_limits<double>::lowest();
    }

    /***************************************************************************/
    // Evaluation policy for invalid solutions

    /** @brief Sets the policy applied to invalid solutions. @param eval_policy The evaluation policy */
    void setEvaluationPolicy(evaluationPolicy eval_policy) { eval_policy_ = eval_policy; }
    /** @brief @return The evaluation policy applied to invalid solutions */
    [[nodiscard]] evaluationPolicy getEvaluationPolicy() const { return eval_policy_; }

    /***************************************************************************/
    // Sigmoid transform parameters

    /** @brief @return The sigmoid steepness value */
    [[nodiscard]] double getSteepness() const { return sigmoid_steepness_; }
    /**
     * @brief Sets the sigmoid steepness (must be > 0).
     * @param steepness The new sigmoid steepness value
     */
    void setSteepness(double steepness) {
        if(steepness <= 0.) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GProblemPolicy::setSteepness(): Error!" << '\n'
                << "Invalid value of steepness parameter: " << steepness << '\n'
            );
        }
        sigmoid_steepness_ = steepness;
    }

    /** @brief @return The sigmoid barrier (extreme) value */
    [[nodiscard]] double getBarrier() const { return sigmoid_extremes_; }
    /**
     * @brief Sets the sigmoid barrier / extreme value (must be > 0).
     * @param barrier The new sigmoid barrier (extreme) value
     */
    void setBarrier(double barrier) {
        if(barrier <= 0.) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GProblemPolicy::setBarrier(): Error!" << '\n'
                << "Invalid value of barrier parameter: " << barrier << '\n'
            );
        }
        sigmoid_extremes_ = barrier;
    }

    /**
     * @brief Applies the rational sigmoid transform to a raw fitness value, using this policy's barrier
     * and steepness. Used to map a valid solution's raw fitness to its transformed (ranked) value when
     * the evaluation policy is USESIGMOID.
     * @param raw_value The raw fitness value to transform
     * @return The sigmoid-transformed fitness value
     */
    [[nodiscard]] double sigmoidTransform(double raw_value) const {
        return Gem::Common::grational_sigmoid(raw_value, sigmoid_extremes_, sigmoid_steepness_);
    }

    /***************************************************************************/
    // Constraint

    /**
     * @brief Registers a constraint with this policy. The constraint is cloned, so the policy owns its
     * own copy and distinct policies never share one constraint object. Throws on an empty pointer.
     * @param c_ptr The validity-check constraint to register (must not be empty)
     */
    void registerConstraint(const std::shared_ptr<GPreEvaluationValidityCheckT<GGenome>>& c_ptr);

    /** @brief @return true if a constraint object is registered with this policy */
    [[nodiscard]] bool hasConstraint() const { return static_cast<bool>(constraint_ptr_); }

    /**
     * @brief Checks whether a candidate fulfils the registered constraint. If no constraint is
     * registered, the candidate is always valid and the validity level is 0. Defined in the .cpp, where
     * GGenome is a complete type (the constraint reads its parameter values).
     * @param genome The genome whose feasibility is checked
     * @param validity_level Out-parameter receiving the computed validity level
     * @return true if the candidate satisfies the constraint (or none is registered), false otherwise
     */
    bool fulfillsConstraints(const GGenome &genome, double &validity_level) const;

private:
    /***************************************************************************/
    // Data -- the population-uniform feasibility / ranking rules.

    /** @brief The optimization direction (the algorithms always see a minimization problem) */
    maxMode maxmode_ = maxMode::MINIMIZE;
    /** @brief What to do with a solution flagged invalid by the constraint */
    evaluationPolicy eval_policy_ = Gem::Geneva::evaluationPolicy::USESIMPLEEVALUATION;
    /** @brief The steepness of the sigmoid transform at its centre */
    double sigmoid_steepness_ = Gem::Geneva::FITNESSSIGMOIDSTEEPNESS;
    /** @brief The extreme (barrier) values of the sigmoid transform */
    double sigmoid_extremes_ = Gem::Geneva::WORSTALLOWEDVALIDFITNESS;

    /** @brief The shared constraint-check applied to every candidate (empty == always valid) */
    std::unique_ptr<GPreEvaluationValidityCheckT<GGenome>> constraint_ptr_;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
