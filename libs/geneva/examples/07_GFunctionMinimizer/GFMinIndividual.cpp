/**
 * @file GFMinIndividual.cpp
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

#include "GFMinIndividual.hpp"

#include "geneva/oa/GAdaption.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "geneva/oa/GAdaptionConfig.hpp"

#include <algorithm>
#include <any>
#include <functional>
#include <ranges>
#include <utility>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::GFMinIndividual) // NOLINT
namespace Gem::Geneva {

/******************************************************************************/
/**
 * The default constructor. Data members may be initialized in the class body.
 */
GFMinIndividual::GFMinIndividual() { /* nothing */
}

/******************************************************************************/
/**
 * A standard copy constructor
 *
 * @param cp A copy of another GFMinIndividual
 */
GFMinIndividual::GFMinIndividual(const GFMinIndividual &cp)
  : gen::GGenomeT<GFMinIndividual>(cp)
  , targetFunction_(cp.targetFunction_)
  , seed_sigma_(cp.seed_sigma_) { /* nothing */
}

/******************************************************************************/
/**
 * The standard destructor
 */
GFMinIndividual::~GFMinIndividual() { /* nothing */
}

/*******************************************************************************************/
/**
 * Allows to set the demo function
 *
 * @param tF The id of the demo function
 */
void GFMinIndividual::setTargetFunction(targetFunction tF) {
    targetFunction_ = tF;
}

/*******************************************************************************************/
/**
 * Allows to retrieve the demo function
 *
 * @return The id of the currently selected demo function
 */
targetFunction GFMinIndividual::getTargetFunction() const {
    return targetFunction_;
}

/*******************************************************************************************/
/**
 * Retrieves the average value of sigma used in Gauss adaptors. Note: This function is highly
 * dependent on the parameter object loaded into this class. It is not meant for general
 * consumption, but has been added here to allow an optimization monitor demo to extract
 * further information.
 *
 * @return The average value of sigma used in Gauss adaptors
 */
double GFMinIndividual::getAverageSigma() const {
    // The Gauss adaptor configuration now lives on the OA-owned config (the genome is structure-only), and
    // the live evolving sigma is OA-owned scratch carried on the individual itself (its GAuxiliaryStore). An
    // individual queried in isolation (as here) is detached from its slot, so this reports the configured
    // SEED sigma the factory stamped at construction.
    return seed_sigma_;
}

/******************************************************************************/
/**
 * The actual value calculation takes place here
 *
 * @return The value of this object, as calculated with the evaluation function
 */
std::vector<double> GFMinIndividual::evaluate() {
    // Retrieve the parameters
    std::vector<double> parVec;
    this->streamline(parVec);

    // Perform the actual calculation
    switch(targetFunction_) {
    //-----------------------------------------------------------
    // A simple, multi-dimensional parabola
    case targetFunction::GFM_PARABOLA:
        return {parabola(parVec)};

    //-----------------------------------------------------------
    // A "noisy" parabola, i.e. a parabola with a very large
    // number of overlaid local optima
    case targetFunction::GFM_NOISYPARABOLA:
        return {noisyParabola(parVec)};
        //-----------------------------------------------------------
    };

    // Make the compiler happy
    return {0.};
}

/******************************************************************************/
/**
 * A simple n-dimensional parabola
 */
double GFMinIndividual::parabola(const std::vector<double> &parVec) {
    return std::ranges::fold_left(
        parVec | std::views::transform([](double x) { return Gem::Common::gsquared(x); }), 0., std::plus{});
}

/******************************************************************************/
/**
 * A "noisy" parabola
 */
double GFMinIndividual::noisyParabola(const std::vector<double> &parVec) {
    const double xsquared = std::ranges::fold_left(
        parVec | std::views::transform([](double x) { return Gem::Common::gsquared(x); }), 0., std::plus{});

    return (cos(xsquared) + 2.) * xsquared;
}

/******************************************************************************/
/**
 * Provide an easy way to print the individual's content
 */
std::ostream &operator<<(std::ostream &s, const Gem::Geneva::GFMinIndividual &f) {
    std::vector<double> parVec;
    f.streamline(parVec);

    std::vector<double>::iterator it;
    for(it = parVec.begin(); it != parVec.end(); ++it) {
        std::cout << (it - parVec.begin()) << ": " << *it << '\n';
    }

    return s;
}

/******************************************************************************/
/**
 * Provide an easy way to print the individual's content through a smart-pointer
 */
std::ostream &operator<<(std::ostream &s, const std::unique_ptr<Gem::Geneva::GFMinIndividual>& f_ptr) {
    return operator<<(s, *f_ptr);
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Registers the config-file options, binding them to the passed Config: the parameter-dimension /
 * bounds / adaptor options plus the target_function option.
 */
void GFMinIndividual::describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
    std::string comment;

    comment = "";
    comment += "The probability for random adaptions of values in evolutionary algorithms;";
    gpb.registerFileParameter<double>(
        "ad_prob", c.ad_prob, GFI_DEF_ADPROB, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "The sigma for gauss-adaption in ES;";
    gpb.registerFileParameter<double>(
        "sigma", c.sigma, GFI_DEF_SIGMA, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "Influences the self-adaption of gauss-mutation in ES;";
    gpb.registerFileParameter<double>(
        "sigma_sigma", c.sigma_sigma, GFI_DEF_SIGMASIGMA, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "The minimum amount value of sigma;";
    gpb.registerFileParameter<double>(
        "min_sigma", c.min_sigma, GFI_DEF_MINSIGMA, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "The maximum amount value of sigma;";
    gpb.registerFileParameter<double>(
        "max_sigma", c.max_sigma, GFI_DEF_MAXSIGMA, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "The number of dimensions used for the demo function;";
    gpb.registerFileParameter<std::size_t>(
        "par_dim", c.par_dim, GFI_DEF_PARDIM, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "The lower boundary of the initialization range for parameters;";
    gpb.registerFileParameter<double>(
        "min_var", c.min_var, GFI_DEF_MINVAR, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "The upper boundary of the initialization range for parameters;";
    gpb.registerFileParameter<double>(
        "max_var", c.max_var, GFI_DEF_MAXVAR, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "Specifies which target function should be used:;0: Parabola;1: Berlich;";
    gpb.registerFileParameter<targetFunction>(
        "target_function", c.target_function, GO_DEF_TARGETFUNCTION, Gem::Common::VAR_IS_ESSENTIAL, comment
    );
}

/******************************************************************************/
/**
 * Builds the flat genome's STRUCTURE only: one constrained-double group of par_dim values sharing a
 * single sigma. The adaptor settings live on the OA-owned config (see buildAdaptionConfig()), not in
 * the structure-only genome.
 */
gen::GenomeData GFMinIndividual::buildGenome(const Config &c) {
    gen::GGenomeBuilder b;
    b.addDoubleGroup(c.par_dim, c.min_var, c.max_var); // structure only; the adaptor lives on the OA config
    return b.build();
}

/******************************************************************************/
/**
 * Builds the OA-owned adaption configuration for a genome produced by this factory: the single shared
 * double group gets a Gauss adaptor with the configured parameters.
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GFMinIndividual::buildAdaptionConfig(const gen::GGenome &sample, const Config &c) {
    namespace oa = Gem::Geneva::OptimizationAlgorithms;
    auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(sample);
    for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
        cfg->groupDouble(i).gauss(c.sigma, c.sigma_sigma, c.min_sigma, c.max_sigma, c.ad_prob);
    }
    return cfg;
}

/******************************************************************************/
/**
 * Per-object post-config hook: applies the target function and stamps the seed sigma for the
 * getAverageSigma() telemetry hook.
 */
void GFMinIndividual::applyConfig(GFMinIndividual &ind, const Config &c) {
    ind.setTargetFunction(c.target_function);
    ind.seed_sigma_ = c.sigma;
}

/******************************************************************************/

}
