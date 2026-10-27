/**
 * @file GStarterIndividual.cpp
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

#include "GStarterIndividual.hpp"

#include "geneva/oa/GAdaption.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "geneva/oa/GAdaptionConfig.hpp"

#include <algorithm>
#include <any>
#include <functional>
#include <ranges>

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <utility>
#endif /* GEM_TESTING */

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::GStarterIndividual) // NOLINT
namespace Gem::Geneva {

/******************************************************************************/
/**
 * The default constructor. Note that some data members
 * may be initialized in the class body.
 */
GStarterIndividual::GStarterIndividual() { /* nothing */
}

/******************************************************************************/
/**
 * The standard constructor. The number of parameters is determined using the number of
 * entries in the startValues vector. Note that all vector arguments need to have the
 * same dimension.
 */
GStarterIndividual::GStarterIndividual(
    const std::size_t &prod_id,
    const std::vector<double> &startValues,
    const std::vector<double> &lowerBoundaries,
    const std::vector<double> &upperBoundaries
)
  : targetFunction_(targetFunction::PARABOLA) {
    try {
        // The following is a static function used both here
        // and in the factory, so setup code cannot diverge
        GStarterIndividual::addContent(
            *this,
            prod_id,
            startValues,
            lowerBoundaries,
            upperBoundaries
        );
    }
    catch(const geneva_exception &e) {
        glogger << e.what() << GTERMINATION;
    }
    catch(...) {
        glogger << "Unknown exception caught" << '\n' << GTERMINATION;
    }
}

/******************************************************************************/
/**
 * A standard copy constructor
 *
 * @param cp A copy of another GStarterIndividual
 */
GStarterIndividual::GStarterIndividual(const GStarterIndividual &cp)
  : gen::GGenomeT<GStarterIndividual>(cp)
  , targetFunction_(cp.targetFunction_) { /* nothing */
}

/******************************************************************************/
/**
 * The standard destructor
 */
GStarterIndividual::~GStarterIndividual() { /* nothing */
}

/*******************************************************************************************/
/**
 * Allows to set the demo function
 *
 * @param tF The id of the demo function
 */
void GStarterIndividual::setTargetFunction(targetFunction tF) {
    targetFunction_ = tF;
}

/*******************************************************************************************/
/**
 * Allows to retrieve the demo function
 *
 * @return The id of the currently selected demo function
 */
targetFunction GStarterIndividual::getTargetFunction() const {
    return targetFunction_;
}

/******************************************************************************/
/**
 * Builds the OA-owned adaption configuration for a genome produced by this factory: every parameter
 * group receives a Gauss adaptor with the configured parameters. The adaptor settings come from the
 * Config and live solely on the returned (OA-owned) config -- none of them reside on the individual.
 *
 * @param sample A sample flat genome whose group structure the config mirrors
 * @param c The Config supplying the Gauss adaptor parameters
 * @return A shared pointer to the populated OA-owned adaption config
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GStarterIndividual::buildAdaptionConfig(const gen::GGenome &sample, const Config &c) {
    auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(sample);
    for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
        cfg->groupDouble(i).gauss(c.sigma, c.sigma_sigma, c.min_sigma, c.max_sigma, c.ad_prob);
    }
    return cfg;
}

/******************************************************************************/
/**
 * Emit information about this individual
 */
//! [results-print]
std::string GStarterIndividual::print() {
    std::ostringstream result;

    // Retrieve the parameters
    std::vector<double> parVec;
    this->streamline(parVec);

    result << "GStarterIndividual with target function "
           << (targetFunction_ == targetFunction::PARABOLA ? " PARABOLA" : " NOISY PARABOLA")
           << '\n'
           << "and raw fitness " << this->raw_fitness(0)
           << " has the following parameter values:" << '\n';

    for(auto const& [i, x] : std::views::enumerate(parVec)) {
        result << i << ": " << x << '\n';
    }

    return result.str();
}
//! [results-print]

/******************************************************************************/
/**
 * The actual value calculation takes place here
 *
 * @return The value of this object, as calculated with the evaluation function
 */
//! [quickstart-evaluate]
std::vector<double> GStarterIndividual::evaluate() {
    // Retrieve the parameters
    std::vector<double> parVec;
    this->streamline(parVec);

    // Perform the actual calculation
    switch(targetFunction_) {
    //-----------------------------------------------------------
    // A simple, multi-dimensional parabola
    case targetFunction::PARABOLA:
        return {parabola(parVec)};

    //-----------------------------------------------------------
    // A "noisy" parabola, i.e. a parabola with a very large
    // number of overlaid local optima
    case targetFunction::NOISYPARABOLA:
        return {noisyParabola(parVec)};
        //-----------------------------------------------------------
    };

    // Make the compiler happy
    return {0.};
}
//! [quickstart-evaluate]

/******************************************************************************/
/**
 * A simple n-dimensional parabola
 */
double GStarterIndividual::parabola(const std::vector<double> &parVec) const {
    return std::ranges::fold_left(
        parVec | std::views::transform([](double x) { return x * x; }), 0., std::plus{});
}

/******************************************************************************/
/**
 * A "noisy" parabola
 */
double GStarterIndividual::noisyParabola(const std::vector<double> &parVec) const {
    const double xsquared = std::ranges::fold_left(
        parVec | std::views::transform([](double x) { return x * x; }), 0., std::plus{});

    return (cos(xsquared) + 2.) * xsquared;
}

/******************************************************************************/
/**
 * Applies modifications to this object. This function is only useful
 * if you wish to run unit tests with your individual.
 *
 * @return A boolean indicating whether
 */
bool GStarterIndividual::modify_GUnitTests_() {
#ifdef GEM_TESTING
    bool result = false;

    // Call the parent classes' functions
    if(gen::GGenome::modify_GUnitTests_()) {
        result = true;
    }

    // The parent's modify_GUnitTests_() already random-initialises every genome parameter, so the
    // object is changed. The Gauss adaptor configuration is OA-owned and is not exercised here (the
    // individual carries no adaptor data).
    result = true;

    // Let the audience know whether we have changed the content
    return result;

#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset("GStarterIndividual::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
/**
 * Performs self tests that are expected to succeed. This function is only useful
 * if you wish to run unit tests with your individual.
 */
void GStarterIndividual::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING
    using namespace Gem::Geneva;

    // Call the parent classes' functions
    gen::GGenome::specificTestsNoFailureExpected_GUnitTests_();

    //------------------------------------------------------------------------------

    {
        const std::size_t NENTRIES = 100;

        // Check standard construction and that the genome is built with one parameter per start value
        std::vector<double> startValues;
        std::vector<double> lowerBoundaries;
        std::vector<double> upperBoundaries;

        for(std::size_t n = 0; n < NENTRIES; n++) {
            startValues.push_back(1.);
            lowerBoundaries.push_back(0.);
            upperBoundaries.push_back(2.);
        }

        std::shared_ptr<GStarterIndividual> p_test;
        CHECK_NOTHROW(
            p_test = std::shared_ptr<GStarterIndividual>(new GStarterIndividual(
                0 // indicates the first individual
                ,
                startValues,
                lowerBoundaries,
                upperBoundaries
            ))
        );

        CHECK(p_test->countParameters<double>() == NENTRIES);
    }

    //------------------------------------------------------------------------------

    { // Test setting and retrieval of the target function valie
        auto p_test = this->clone<GStarterIndividual>();

        CHECK_NOTHROW(p_test->setTargetFunction(targetFunction::PARABOLA));
        CHECK(targetFunction::PARABOLA == p_test->getTargetFunction());

        CHECK_NOTHROW(p_test->setTargetFunction(targetFunction::NOISYPARABOLA));
        CHECK(targetFunction::NOISYPARABOLA == p_test->getTargetFunction());
    }

    //------------------------------------------------------------------------------

#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset(
        "GStarterIndividual::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
/**
 * Performs self tests that are expected to fail. This function is only useful
 * if you wish to run unit tests with your individual.
 */
void GStarterIndividual::specificTestsFailuresExpected_GUnitTests_() {
#ifdef GEM_TESTING
    using namespace Gem::Geneva;


    //------------------------------------------------------------------------------

    {
        /* Nothing. Add test cases here that are expected to fail.
			Enclose with a CHECK_THROWS_AS, using the expected
			exception type as an additional argument. See the
			Catch2 documentation for further information */
    }

    //------------------------------------------------------------------------------

#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset(
        "GStarterIndividual::specificTestsFailuresExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Allows to output a GStarterIndividual (or convert it to a string) via its operator<<
 */
std::ostream &operator<<(std::ostream &stream, const std::unique_ptr<GStarterIndividual> &gsi_ptr) {
    stream << gsi_ptr->print();
    return stream;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Registers the config-file options, binding them to the passed Config: the adaptor / bounds options
 * plus the target_function option.
 */
void GStarterIndividual::describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
    gpb.registerFileParameter<double>("ad_prob", c.ad_prob, GSI_DEF_ADPROB)
        << "The probability for random adaptions of values in evolutionary algorithms";

    gpb.registerFileParameter<double>("sigma", c.sigma, GSI_DEF_SIGMA)
        << "The sigma for gauss-adaption in ES";

    gpb.registerFileParameter<double>("sigma_sigma", c.sigma_sigma, GSI_DEF_SIGMASIGMA)
        << "Influences the self-adaption of gauss-mutation in ES";

    gpb.registerFileParameter<double>("min_sigma", c.min_sigma, GSI_DEF_MINSIGMA)
        << "The minimum amount value of sigma";

    gpb.registerFileParameter<double>("max_sigma", c.max_sigma, GSI_DEF_MAXSIGMA)
        << "The maximum amount value of sigma";

    std::vector<double> defStartValues{1., 1., 1.};
    gpb.registerFileParameter<double>("start_values", c.start_values, defStartValues)
        << "The start values for all parameters" << '\n'
        << "Note that the number of entries also determines" << '\n'
        << "The number of parameter used in the optimization" << '\n'
        << "The number of entries in the vector may be changed" << '\n'
        << "in the configuration file.";

    std::vector<double> defLowerBoundaries{0., 0., 0.};
    gpb.registerFileParameter<double>("lower_boundaries", c.lower_boundaries, defLowerBoundaries)
        << "The lower boundaries for all parameters" << '\n'
        << "Note that as many entries are needed as" << '\n'
        << "There are entries in the startValues vector";

    std::vector<double> defUpperBoundaries{2., 2., 2.};
    gpb.registerFileParameter<double>("upper_boundaries", c.upper_boundaries, defUpperBoundaries)
        << "The upper boundaries for all parameters" << '\n'
        << "Note that as many entries are needed as" << '\n'
        << "There are entries in the startValues vector";

    gpb.registerFileParameter<targetFunction>(
        "target_function", c.target_function, GO_DEF_TARGETFUNCTION
    ) << "Specifies which target function should be used:" << '\n'
      << "0: Parabola" << '\n'
      << "1: Berlich";
}

/******************************************************************************/
/**
 * Builds the flat genome's STRUCTURE only (the structure-building part of addContent()): one constrained
 * double per start value, in [lower, upper). The start value is the genome's initial value; the
 * optimization algorithm random-initialises within bounds. The Gauss adaptor settings live on the
 * OA-owned config (see getAdaptionConfig()), stamped onto the individual by applyConfig().
 */
//! [quickstart-buildgenome#1]
gen::GenomeData GStarterIndividual::buildGenome(const Config &c) {
//! [quickstart-buildgenome#1]
#ifdef DEBUG
    if(c.start_values.empty() || c.start_values.size() != c.lower_boundaries.size() ||
       c.start_values.size() != c.upper_boundaries.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GStarterIndividual::buildGenome(): Error!" << '\n'
            << "Invalid sizes " << c.start_values.size() << " / " << c.lower_boundaries.size()
            << " / " << c.upper_boundaries.size() << '\n'
        );
    }
#endif /* DEBUG */

    //! [quickstart-buildgenome#2]
    gen::GGenomeBuilder b;
    for(std::size_t i = 0; i < c.start_values.size(); i++) {
        b.addDouble(c.start_values.at(i), c.lower_boundaries.at(i), c.upper_boundaries.at(i));
    }
    return b.build();
}
    //! [quickstart-buildgenome#2]

/******************************************************************************/
/**
 * Per-object post-config hook (the per-object tail of addContent()): stamps the (non-genome) target
 * function. The Gauss adaptor parameters are NOT stored on the individual -- they live on the OA-owned
 * config authored by buildAdaptionConfig().
 */
void GStarterIndividual::applyConfig(GStarterIndividual &ind, const Config &c) {
    ind.setTargetFunction(c.target_function);
}

/******************************************************************************/

}
