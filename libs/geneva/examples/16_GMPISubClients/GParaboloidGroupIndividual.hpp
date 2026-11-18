/**
 * @file GParaboloidGroupIndividual.hpp
 */

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
#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>
#include <tuple>
#include <vector>

// Geneva header files go here
#include "geneva/genome/GGenomeT.hpp"

namespace Gem::Geneva {

namespace OptimizationAlgorithms {
class GAdaptionConfigBase;
} /* namespace OptimizationAlgorithms */

/******************************************************************************/
/**
 * The individual of example 16: a paraboloid of NPARAMETERS double parameters in [-10, 10]. Its
 * fitness is the sum of the squares of the parameter values, and every square stands for an
 * expensive calculation of DELAYPERPARAMETER. evaluate() computes it in one process (with
 * --consumer stc, say). A lead of an MPI cascade shares the calculation out over its group instead
 * (GParaboloidGroup.hpp, GMPISubClients.cpp). The individual itself knows nothing of MPI.
 */
class GParaboloidGroupIndividual // NOLINT(cppcoreguidelines-special-member-functions)
  : public gen::GGenomeT<GParaboloidGroupIndividual> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief No state of its own beyond the genome */
    template <typename self_type>
    auto localMembers_(this self_type &) {
        return std::make_tuple();
    }

public:
    /** @brief The class name, consumed by the generated name_() and compare token */
    static constexpr std::string_view class_name = "GParaboloidGroupIndividual";
    /** @brief The number of parameters */
    static constexpr std::size_t NPARAMETERS = 64;
    /** @brief The simulated time of the calculation of the square of one parameter */
    static constexpr std::chrono::milliseconds DELAYPERPARAMETER{50};

    /** @brief The default constructor: NPARAMETERS parameters, randomly initialized */
    GParaboloidGroupIndividual();
    /** @brief The copy constructor */
    GParaboloidGroupIndividual(const GParaboloidGroupIndividual &) = default;
    /** @brief The destructor */
    ~GParaboloidGroupIndividual() override = default;

    /** @brief The adaption configuration of the algorithms: one Gauss group per parameter */
    static std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
    buildAdaptionConfig(const gen::GGenome &);

protected:
    /** @brief The sum of the squares of the parameter values, computed in this process alone */
    std::vector<double> evaluate() final;
};

/******************************************************************************/

} /* namespace Gem::Geneva */
