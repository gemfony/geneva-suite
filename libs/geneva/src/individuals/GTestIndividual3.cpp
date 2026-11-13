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

#include "geneva/individuals/GTestIndividual3.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <vector>

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#endif /* GEM_TESTING */

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GTestIndividual3) // NOLINT
namespace Gem::Geneva::Individuals {

constexpr std::size_t GTI_DEF_NITEMS = 300;
constexpr double GTI_DEF_SIGMA = 0.025;
constexpr double GTI_DEF_SIGMASIGMA = 0.1;
constexpr double GTI_DEF_MINSIGMA = 0.001;
constexpr double GTI_DEF_MAXSIGMA = 1.0;
constexpr double GTI_DEF_ADPROB = 0.05;

/******************************************************************************/
/**
 * @brief The default constructor
 *
 * Builds a flat genome that reproduces the historical nested structure (GTI_DEF_NITEMS records of ten
 * constrained doubles each) and random-initialises all parameters within their bounds.
 */
GTestIndividual3::GTestIndividual3() {
    using namespace Gem::Geneva;

    // Build a flat genome of GTI_DEF_NITEMS records, each holding 10 constrained doubles in the order
    // a[2], b, c[3], d[3], e. A multi-value field becomes one shared-sigma group; a standalone field
    // its own group. This streamline order is a positional contract, so getPlainData() reads it
    // positionally.
    // Structure only -- this individual is never adapted (it is a genome / slot-scratch test fixture), so
    // no adaptor is attached and no OA adaption config is authored for it.
    gen::GGenomeBuilder bld;

    for(std::size_t i_cnt = 0; i_cnt < GTI_DEF_NITEMS; i_cnt++) {
        bld.addDoubleGroup(2, 0., 1.); // a: middle of the circle
        bld.addDouble(0., 0., 0.3);    // b
        bld.addDoubleGroup(3, 0., 1.); // c: three angles
        bld.addDoubleGroup(3, 0., 1.); // d: three colors
        bld.addDouble(0.3, 0.3, 0.6);  // e: alpha channel
    }

    this->setGenome(bld.build());

    // Per-parameter random initialization within bounds.
    this->randomInit(activityMode::ALLPARAMETERS);
}

/******************************************************************************/
/**
 * @brief The copy constructor
 *
 * @param cp A constant reference to another GTestIndividual3 object
 */
GTestIndividual3::GTestIndividual3(const GTestIndividual3 &cp)
  : gen::GGenomeT<GTestIndividual3>(cp) { /* nothing */
}

/******************************************************************************/
/**
 * @brief The standard destructor
 */
GTestIndividual3::~GTestIndividual3() { /* nothing */
}

/******************************************************************************/
/**
 * @brief The actual fitness calculation takes place here.
 *
 * Computes the sum of the squares of all double parameters (a parabola).
 *
 * @return The value of this object (the parabola's value for the current parameters)
 */
std::vector<double> GTestIndividual3::evaluate() {
    // We just calculate the square of all double values
    std::vector<double> par_vec;
    this->streamline(par_vec);

    // Calculate the value of the parabola
    return {std::ranges::fold_left(
        par_vec | std::views::transform([](double x) { return Gem::Common::gsquared(x); }), 0., std::plus{})};
}

/******************************************************************************/
/**
 * @brief Get all data members of this class as a plain array
 *
 * Streamlines the genome's doubles and copies them, narrowed to float, into a freshly allocated array
 * (with an array deleter) in the same positional order the historical nested structure produced.
 *
 * @return A shared pointer to a float array of size 10 * GTI_DEF_NITEMS holding all parameter values
 */
std::shared_ptr<float[]> GTestIndividual3::getPlainData() const {
    using namespace Gem::Geneva;

    // The flat genome stores the 10 doubles of each record contiguously, in the order
    // a[0], a[1], b, c[0..2], d[0..2], e. So record i's field k sits at flat position i*10 + k --
    // a straight positional copy, no per-field decoding.
    std::vector<double> par_vec;
    this->streamline<double>(par_vec);

#ifdef DEBUG
    if(par_vec.size() != 10 * GTI_DEF_NITEMS) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GTestIndividual3::getPlainData(): Error!" << '\n'
            << "Invalid number of double parameters " << par_vec.size() << " / " << (10 * GTI_DEF_NITEMS)
            << '\n'
        );
    }
#endif /* DEBUG */

    // The array-aware shared_ptr<float[]> uses delete[] by itself -- no custom deleter needed
    std::shared_ptr<float[]> result(new float[10 * GTI_DEF_NITEMS]);
    for(std::size_t m = 0; m < 10 * GTI_DEF_NITEMS; m++) {
        result[m] = Gem::Common::narrow<float>(par_vec[m]);
    }

    // Let the audience know
    return result;
}

/******************************************************************************/
/**
 * @brief Applies modifications to this object. This is needed for testing purposes
 *
 * @return A boolean which indicates whether modifications were made
 */
bool GTestIndividual3::modify_GUnitTests_() {
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
    Gem::Common::condnotset("GTestIndividual3::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
/**
 * @brief Performs self tests that are expected to succeed. This is needed for testing purposes
 */
// NOLINTNEXTLINE(readability-function-size) -- self-test entry point for GTestIndividual3: a sequence of independent, self-scoped CHECK blocks, one per plain-data-extraction scenario; same one-function-per-test-phase convention used identically across every individual/OA self-test in this codebase
void GTestIndividual3::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING
    using namespace Gem::Geneva;

    // Call the parent classes' functions
    gen::GGenome::specificTestsNoFailureExpected_GUnitTests_();

    constexpr std::size_t ntests = 100;

    //------------------------------------------------------------------------------

    { // Test that repeated extraction of an object's data results in the same output
        std::shared_ptr<GTestIndividual3> p;
        std::shared_ptr<float[]> result_old;
        std::shared_ptr<float[]> result_new;

        CHECK_NOTHROW(p = std::make_shared<GTestIndividual3>());
        CHECK_NOTHROW(result_old = p->getPlainData());
        for(std::size_t i = 0; i < ntests; i++) {
            CHECK_NOTHROW(result_new = p->getPlainData());
            for(std::size_t m = 0; m < GTI_DEF_NITEMS * 10; m++) {
                CHECK((result_old.get())[m] == (result_new.get())[m]);
            }
        }
    }

    //------------------------------------------------------------------------------

#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset(
        "GTestIndividual3::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif                  /* GEM_TESTING */
}

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
