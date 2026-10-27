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

#include "geneva/genome/GProblemPolicy.hpp"

// The constraint reads the genome's parameter values, so its full definition is needed here.
#include "geneva/genome/GGenome.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
/**
 * @brief The copy constructor. Deep-clones the constraint object.
 * @param cp The policy to copy from
 */
GProblemPolicy::GProblemPolicy(GProblemPolicy const &cp)
  : maxmode_(cp.maxmode_)
  , eval_policy_(cp.eval_policy_)
  , sigmoid_steepness_(cp.sigmoid_steepness_)
  , sigmoid_extremes_(cp.sigmoid_extremes_) {
    Gem::Common::copyCloneableSmartPointer(cp.constraint_ptr_, constraint_ptr_);
}

/******************************************************************************/
/**
 * @brief The copy assignment operator. Deep-clones the constraint object.
 * @param cp The policy to copy from
 * @return A reference to this object
 */
GProblemPolicy &GProblemPolicy::operator=(GProblemPolicy const &cp) {
    // Copy-construct (the ONE place the member list is stated, including the constraint deep-clone)
    // and move-assign the result, so the two copy paths cannot drift apart when a member is added.
    if(this != &cp) {
        *this = GProblemPolicy(cp);
    }
    return *this;
}

/******************************************************************************/
/**
 * @brief Registers a constraint with this policy (the constraint is cloned, so the policy owns its own
 * copy). Throws on an empty pointer.
 * @param c_ptr The validity-check constraint to register (must not be empty)
 */
void GProblemPolicy::registerConstraint(
    const std::shared_ptr<GPreEvaluationValidityCheckT<GGenome>>& c_ptr
) {
    if(not c_ptr) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GProblemPolicy::registerConstraint(): Error!" << '\n'
            << "Tried to register an empty constraint object" << '\n'
        );
    }

    // We store a clone, so two policies never share one constraint object.
    constraint_ptr_ = c_ptr->clone<GPreEvaluationValidityCheckT<GGenome>>();
}

/******************************************************************************/
/**
 * @brief Checks whether a candidate fulfils the registered constraint. With no constraint registered,
 * the candidate is always valid and the validity level is 0.
 * @param genome The genome whose feasibility is checked
 * @param validity_level Out-parameter receiving the computed validity level
 * @return true if the candidate satisfies the constraint (or none is registered)
 */
bool GProblemPolicy::fulfillsConstraints(const GGenome &genome, double &validity_level) const {
    if(constraint_ptr_) {
        return constraint_ptr_->isValid(&genome, validity_level);
    }
    validity_level = 0.;
    return true;
}

/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
