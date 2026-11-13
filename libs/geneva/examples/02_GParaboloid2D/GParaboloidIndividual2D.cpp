/**
 * @file GParaboloidIndividual2D.cpp
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

#include "GParaboloidIndividual2D.hpp"

#include "geneva/oa/GAdaption.hpp"
//! [weft-register-archivable#1]
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
//! [weft-register-archivable#1]
#include "geneva/oa/GAdaptionConfig.hpp"

#include <algorithm>
#include <functional>
#include <ranges>

//! [weft-register-archivable#2]
GEM_REGISTER_ARCHIVABLE(Gem::Geneva::GParaboloidIndividual2D) // NOLINT
//! [weft-register-archivable#2]
namespace Gem::Geneva {

/********************************************************************************************/
/**
 * The default constructor. This function will add two double parameters to this individual,
 * each of which has a constrained value range [-10:10].
 */
//! [problem-constructor]
GParaboloidIndividual2D::GParaboloidIndividual2D()
  : M_PAR_MIN(-10.)
  , M_PAR_MAX(10.) {
    // Build a flat genome of two constrained doubles in [M_PAR_MIN, M_PAR_MAX[, each its own Gauss
    // group adapted with the default GDoubleGaussAdaptor configuration.
    gen::GGenomeBuilder b;
    for(std::size_t npar = 0; npar < 2; npar++) {
        b.addDouble(M_PAR_MIN, M_PAR_MIN, M_PAR_MAX); // structure only; the adaptor lives on the OA config
    }
    this->setGenome(b.build());

    // Per-parameter random initialization within bounds.
    this->randomInit(activityMode::ALLPARAMETERS);
}
//! [problem-constructor]

/********************************************************************************************/
/**
 * Builds the OA-owned adaption configuration for this genome: each of the two double parameters is its
 * own Gauss group, configured with the default GDoubleGaussAdaptor settings. The adaptor settings live
 * solely on the returned (OA-owned) config -- none reside on the individual.
 *
 * @param sample A sample flat genome whose group structure the config mirrors
 * @return A shared pointer to the populated OA-owned adaption config
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GParaboloidIndividual2D::buildAdaptionConfig(const gen::GGenome &sample) {
    auto cfg = OptimizationAlgorithms::makeAdaptionConfig<OptimizationAlgorithms::GAdaptionConfigBase>(sample);
    for(std::size_t npar = 0; npar < 2; npar++) {
        cfg->groupDouble(npar).gauss(
            DEFAULTSIGMA, DEFAULTSIGMASIGMA, DEFAULTMINSIGMA, DEFAULTMAXSIGMA, DEFAULTADPROB
        );
    }
    return cfg;
}

/********************************************************************************************/
/**
 * Registers the config-file options -- the lower and upper bound of each parameter's value range.
 *
 * @param gpb The parser builder the configuration options are registered on
 * @param c The Config instance the options are bound to (written on parse)
 */
void GParaboloidIndividual2D::describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
    gpb.registerFileParameter<double>(
        "par_min", c.par_min, -10., Gem::Common::VAR_IS_ESSENTIAL,
        "The lower boundary of each parameter's value range;"
    );
    gpb.registerFileParameter<double>(
        "par_max", c.par_max, 10., Gem::Common::VAR_IS_ESSENTIAL,
        "The upper boundary of each parameter's value range;"
    );
}

/********************************************************************************************/
/**
 * Builds the flat genome structure: two constrained doubles in [par_min, par_max[, each its own Gauss group
 * (the same structure the constructor builds; the adaptor settings live on the OA-owned adaption config).
 *
 * @param c The configuration providing the parameter bounds
 * @return The structure-only genome data
 */
gen::GenomeData GParaboloidIndividual2D::buildGenome(const Config &c) {
    gen::GGenomeBuilder b;
    for(std::size_t npar = 0; npar < 2; npar++) {
        b.addDouble(c.par_min, c.par_min, c.par_max); // structure only; the adaptor lives on the OA config
    }
    return b.build();
}

/********************************************************************************************/
/**
 * The factory's adaption-config hook. The Config carries no adaptor settings, so this delegates to the
 * one-argument form (single-sourcing the Gauss configuration).
 *
 * @param sample A sample flat genome whose group structure the config mirrors
 * @param c The configuration (unused; the Gauss settings are the individual's fixed defaults)
 * @return A shared pointer to the populated OA-owned adaption config
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GParaboloidIndividual2D::buildAdaptionConfig(const gen::GGenome &sample, [[maybe_unused]] const Config &c) {
    return buildAdaptionConfig(sample);
}

/********************************************************************************************/
/**
 * A standard copy constructor. All real work is done by the parent class.
 *
 * @param cp A copy of another GParaboloidIndividual2D
 */
GParaboloidIndividual2D::GParaboloidIndividual2D(const GParaboloidIndividual2D &cp)
  : gen::GGenomeT<GParaboloidIndividual2D>(cp)
  , M_PAR_MIN(-10.)
  , M_PAR_MAX(10) { /* nothing */
}

/********************************************************************************************/
/**
 * The standard destructor. The flat genome set in the constructor is managed automatically
 * by the base class; no manual cleanup is required.
 */
GParaboloidIndividual2D::~GParaboloidIndividual2D() { /* nothing */
}

/********************************************************************************************/
/**
 * The evaluation hook: the sum of squares of the genome's external parameters (single criterion).
 *
 * @return The raw fitness as a one-element vector
 */
//! [problem-evaluate]
std::vector<double> GParaboloidIndividual2D::evaluate() {
    std::vector<double> parVec; // Will hold the parameters

    this->streamline(parVec); // Retrieve the (external) parameters

    // Do the actual calculation (a single criterion -> a one-element result vector)
    return {std::ranges::fold_left(
        parVec | std::views::transform([](double d) { return d * d; }), 0., std::plus{})};
}
//! [problem-evaluate]

/********************************************************************************************/

}
