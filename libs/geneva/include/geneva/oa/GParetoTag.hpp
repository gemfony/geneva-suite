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

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * A small mixin carrying the pareto-front tag used by the pareto (NSGA-II style)
 * selection modes. It is shared by personality-traits classes that sit on
 * different serialization base chains (the EA traits derive from
 * GBaseParChildPersonalityTraits, the sep-CMA traits directly from
 * GPersonalityTraits), so it is inherited ALONGSIDE the polymorphic chain as a
 * plain, non-polymorphic mixin rather than spliced into either hierarchy.
 *
 * Deliberately not serialized here: each deriving class serializes the
 * inherited is_on_pareto_front_ member itself under its historical NVP name
 * (via its localMembers_() declaration), so archive layouts are unchanged by
 * this unification.
 */
class GParetoTag {
public:
    /**
     * @brief Allows to check whether this individual lies on the current pareto front
     * (only yields useful results after pareto-sorting in the owning algorithm).
     * @return true if the individual is currently tagged as lying on the pareto front, false otherwise
     */
    [[nodiscard]] bool isOnParetoFront() const {
        return is_on_pareto_front_;
    }

    /** @brief Allows to reset the pareto tag to "true" */
    void resetParetoTag() {
        is_on_pareto_front_ = true;
    }

    /** @brief Allows to specify that this individual does not lie on the pareto front of the current iteration */
    void setIsNotOnParetoFront() {
        is_on_pareto_front_ = false;
    }

protected:
    /** @brief Whether the individual lies on the current pareto front (serialized by each deriving class) */
    bool is_on_pareto_front_ = true;
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
