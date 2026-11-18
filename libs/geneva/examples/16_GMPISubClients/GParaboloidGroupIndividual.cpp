/**
 * @file GParaboloidGroupIndividual.cpp
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

#include "GParaboloidGroupIndividual.hpp"

// Standard header files go here
#include <span>

// Geneva header files go here
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE

#include "GParaboloidPart.hpp"

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::GParaboloidGroupIndividual) // NOLINT

namespace Gem::Geneva {

/******************************************************************************/
/**
 * The default constructor: NPARAMETERS double parameters in [-10, 10], randomly initialized.
 */
GParaboloidGroupIndividual::GParaboloidGroupIndividual() {
    constexpr double PARMIN = -10.;
    constexpr double PARMAX = 10.;
    gen::GGenomeBuilder builder;
    for(std::size_t npar = 0; npar < NPARAMETERS; npar++) {
        builder.addDouble(PARMIN, PARMIN, PARMAX); // structure only; the adaptor is the algorithm's
    }
    this->setGenome(builder.build());
    this->randomInit(activityMode::ALLPARAMETERS);
}

/******************************************************************************/
/**
 * Builds the adaption configuration of the algorithms: every parameter is its own Gauss group,
 * with the default settings of GDoubleGaussAdaptor. Nothing of it lives on the individual.
 *
 * @param sample A genome whose group structure the configuration mirrors
 * @return The adaption configuration
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GParaboloidGroupIndividual::buildAdaptionConfig(const gen::GGenome &sample) {
    auto config =
        OptimizationAlgorithms::makeAdaptionConfig<OptimizationAlgorithms::GAdaptionConfigBase>(
            sample
        );
    for(std::size_t npar = 0; npar < config->doubleGroups().size(); npar++) {
        config->groupDouble(npar).gauss(
            DEFAULTSIGMA,
            DEFAULTSIGMASIGMA,
            DEFAULTMINSIGMA,
            DEFAULTMAXSIGMA,
            DEFAULTADPROB
        );
    }
    return config;
}

/******************************************************************************/
/**
 * The sum of the squares of the parameter values, computed in this process alone.
 *
 * @return The fitness
 */
std::vector<double> GParaboloidGroupIndividual::evaluate() {
    std::vector<double> parameters;
    this->streamline(parameters);
    return {sumOfSquares(parameters, DELAYPERPARAMETER)};
}

/******************************************************************************/

} /* namespace Gem::Geneva */

// The individual stays free of MPI (CMakeLists.txt). Every mpi.h defines MPI_VERSION, so an include
// of an MPI header, directly or through another header, fails the build here also where mpi.h is
// on the compiler's own search path.
#ifdef MPI_VERSION
#error "GParaboloidGroupIndividual must stay free of MPI"
#endif
