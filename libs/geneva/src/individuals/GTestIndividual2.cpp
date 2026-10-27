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

#include "geneva/individuals/GTestIndividual2.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GCommonEnums.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <ranges>
#include <utility>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GTestIndividual2) // NOLINT
namespace Gem::Geneva::Individuals {

/******************************************************************************/
/**
 * @brief The default constructor -- private, as it is only needed for (de-)serialization purposes
 */
GTestIndividual2::GTestIndividual2() { /* nothing */
}

/******************************************************************************/
/**
 * @brief The standard constructor
 *
 * Builds a flat genome holding n_objects double parameters, laid out according to the requested
 * parameter-object type so that the various flat-genome group representations can be performance-compared.
 *
 * @param n_objects The number of double parameters the genome should hold
 * @param otype The flavour of double parameter representation to build the genome from
 */
GTestIndividual2::GTestIndividual2(const std::size_t &n_objects, const PERFOBJECTTYPE &otype) {
    using namespace Gem::Geneva;

    gen::GGenomeBuilder b;

    // Fill with the requested amount of data of the requested type
    switch(otype) {
    case PERFOBJECTTYPE::PERFGDOUBLEOBJECT: {
        // n unbounded double scalars, each with its own Gauss adaptor (GDoubleObject + adaptor).
        for(std::size_t i = 0; i < n_objects; i++) {
            b.addDouble(0.);
        }
    } break;

    case PERFOBJECTTYPE::PERFGCONSTRDOUBLEOBJECT: {
        // n constrained double scalars [-10, 10], each with its own Gauss adaptor.
        for(std::size_t i = 0; i < n_objects; i++) {
            b.addDouble(0., -10., 10.);
        }
    } break;

    case PERFOBJECTTYPE::PERFGCONSTRAINEDDOUBLEOBJECTCOLLECTION: {
        // n constrained double parameters [-10, 10], each its own adaption group
        // (GConstrainedDoubleObjectCollection of GConstrainedDoubleObject, each with own adaptor).
        b.addDoubleArray(n_objects, -10., 10.);
    } break;

    case PERFOBJECTTYPE::PERFGDOUBLECOLLECTION: {
        // n unbounded double parameters sharing one adaptor (GDoubleCollection).
        b.addDoublePlainGroup(n_objects, -10., 10.);
    } break;

    case PERFOBJECTTYPE::PERFGCONSTRAINEDDOUBLECOLLECTION: {
        // n constrained double parameters [-10, 10] sharing one adaptor (GConstrainedDoubleCollection).
        b.addDoubleGroup(n_objects, -10., 10.);
    } break;

    default: {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GTestIndividual2::GTestIndividual2(): Error!" << '\n'
            << "Invalid object type requested: " << otype << '\n'
        );
    }
    }

    this->setGenome(b.build());
}

/******************************************************************************/
/**
 * @brief The copy constructor
 *
 * @param cp A constant reference to another GTestIndividual2 object
 */
GTestIndividual2::GTestIndividual2(const GTestIndividual2 &cp)
  : gen::GGenomeT<GTestIndividual2>(cp) { /* nothing */
}

/******************************************************************************/
/**
 * @brief The standard destructor
 */
GTestIndividual2::~GTestIndividual2() { /* nothing */
}

/******************************************************************************/
/**
 * @brief Builds the OA-owned adaption configuration for this individual
 *
 * Every double group of this genome gets a Gauss adaptor (sigma 0.025 / sigma_sigma 0.1 / [0, 1] /
 * ad_prob 1).
 *
 * @return A shared pointer to the freshly built adaption configuration covering all double groups
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase> GTestIndividual2::getAdaptionConfig() const {
    namespace oa = Gem::Geneva::OptimizationAlgorithms;
    auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
    for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
        cfg->groupDouble(i).gauss(0.025, 0.1, 0., 1., 1.);
    }
    return cfg;
}

/******************************************************************************/
/**
 * @brief The actual fitness calculation takes place here.
 *
 * Computes the sum of the squares of all double parameters (a parabola).
 *
 * @return The value of this object (the parabola's value for the current parameters)
 */
std::vector<double> GTestIndividual2::evaluate() {
    // We just calculate the square of all double values
    std::vector<double> par_vec;
    this->streamline(par_vec);

    // Calculate the value of the parabola
    return {std::ranges::fold_left(
        par_vec | std::views::transform([](double x) { return Gem::Common::gsquared(x); }), 0., std::plus{})};
}

/******************************************************************************/
/**
 * @brief Applies modifications to this object. This is needed for testing purposes
 *
 * @return A boolean which indicates whether modifications were made
 */
bool GTestIndividual2::modify_GUnitTests_() {
#ifdef GEM_TESTING

    bool result = false;

    // Call the parent classes' functions
    if(gen::GGenome::modify_GUnitTests_()) {
        result = true;
    }

    // Change the parameter settings
    result = true;

    return result;

#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset("GTestIndividual2::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
/**
 * @brief Performs self tests that are expected to succeed. This is needed for testing purposes
 */
void GTestIndividual2::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING
    using namespace Gem::Geneva;

    // Call the parent classes' functions
    gen::GGenome::specificTestsNoFailureExpected_GUnitTests_();

    //------------------------------------------------------------------------------
    //------------------------------------------------------------------------------

#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset(
        "GTestIndividual2::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif                  /* GEM_TESTING */
}

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
