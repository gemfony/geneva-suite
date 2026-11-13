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

#include "geneva/individuals/GLineFitIndividual.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GLineFitIndividual) // NOLINT
namespace Gem::Geneva::Individuals {

/******************************************************************************/
/**
 * @brief The default constructor. Produces a genome-less shell -- the generic factory (loadable / config
 * path) installs the genome via buildGenome() afterwards, and (de-)serialization restores it. Public so the
 * factory can default-construct the individual before installing its genome.
 */
GLineFitIndividual::GLineFitIndividual() { /* nothing */
}

/******************************************************************************/
/**
 * @brief The standard constructor (the compile-in / test path); builds the two-parameter (offset/slope)
 * genome structure and stores the data points to be fitted, in memory.
 *
 * @param data_points The set of (x, y) data points a line should be fitted through; stored for the
 *        fitness calculation
 */
GLineFitIndividual::GLineFitIndividual(const std::vector<std::tuple<double, double>> &data_points)
  : data_points_(data_points) {
    // The genome structure is data-independent, so it is single-sourced in buildGenome() (Inv 14) and
    // shared with the config-driven factory path.
    this->setGenome(buildGenome(Config{}));
}

/******************************************************************************/
/**
 * @brief Builds the OA-owned adaption configuration: the line's offset a and slope b are each their own
 * Gauss group, configured with the settings below.
 *
 * @return A shared pointer to a freshly built adaption config whose double groups each carry the Gauss
 *         adaptor settings (sigma 0.025, sigma_sigma 0.1, min_sigma 0.0001, max_sigma 0.4, ad_prob 1.0)
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase> GLineFitIndividual::getAdaptionConfig() const {
    // Single-sourced with the factory hook (Inv 14): both build the identical line-fit Gauss config.
    return buildAdaptionConfig(*this, Config{});
}

/******************************************************************************/
/**
 * @brief Registers the config-file options: the path of the (x,y) data-point file.
 *
 * @param gpb The GParserBuilder the configurable values are registered on
 * @param c The Config instance whose members are bound to the parser (written on parse)
 */
void GLineFitIndividual::describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
    gpb.registerFileParameter<std::string>(
        "data_file", c.data_file, std::string{}, Gem::Common::VAR_IS_ESSENTIAL,
        "The path of a whitespace-separated \"x y\" data-point file (one point per line, '#' comments);"
    );
}

/******************************************************************************/
/**
 * @brief Builds the flat genome structure: two unbounded double parameters (the line's offset a and slope
 * b), each its own Gauss group, with a default unbounded init range of [0, 1]. The Gauss adaptor settings
 * live on the OA-owned config (see buildAdaptionConfig()), not the genome layout. Data-independent.
 *
 * @param c The configuration (unused for the genome structure)
 * @return The structure-only genome data
 */
gen::GenomeData GLineFitIndividual::buildGenome([[maybe_unused]] const Config &c) {
    gen::GGenomeBuilder b;
    for(std::size_t i = 0; i < 2; i++) {
        b.addDouble(0.);
    }
    return b.build();
}

/******************************************************************************/
/**
 * @brief Builds the OA-owned adaption configuration: the line's offset a and slope b are each their own
 * Gauss group, configured with the fixed line-fit settings.
 *
 * @param sample A sample flat genome whose group structure the config mirrors
 * @param c The configuration (unused; the Gauss settings are the individual's fixed defaults)
 * @return A shared pointer to a freshly built adaption config whose double groups each carry the Gauss
 *         adaptor settings (sigma 0.025, sigma_sigma 0.1, min_sigma 0.0001, max_sigma 0.4, ad_prob 1.0)
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GLineFitIndividual::buildAdaptionConfig(const gen::GGenome &sample, [[maybe_unused]] const Config &c) {
    auto cfg = OptimizationAlgorithms::makeAdaptionConfig<OptimizationAlgorithms::GAdaptionConfigBase>(sample);
    for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
        // sigma, sigma_sigma, min_sigma, max_sigma, ad_prob
        cfg->groupDouble(i).gauss(0.025, 0.1, 0.0001, 0.4, 1.);
    }
    return cfg;
}

/******************************************************************************/
/**
 * @brief Per-object post-config hook: opens the config-named data file at runtime and loads its (x,y)
 * points into the produced individual. This is where a loaded module reads its data from disk -- the
 * location comes from the config, the file is opened here. A streaming individual would instead retain a
 * file handle and read lazily in evaluate(); the line fit's point sets are small, so they are
 * loaded into memory. An empty path leaves the point set empty (e.g. a materialize-config dry run).
 *
 * @param ind The freshly produced individual to load the data points into
 * @param c The configuration providing the data-file path
 */
void GLineFitIndividual::applyConfig(GLineFitIndividual &ind, const Config &c) {
    ind.data_points_.clear();
    if(c.data_file.empty()) {
        return;
    }

    std::ifstream in(c.data_file);
    if(not in) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GLineFitIndividual::applyConfig(): Error!" << '\n'
            << "Could not open the data-point file \"" << c.data_file << "\"" << '\n'
        );
    }

    std::string line;
    while(std::getline(in, line)) {
        // Skip blank lines and '#' comments.
        const std::size_t first = line.find_first_not_of(" \t\r\n");
        if(first == std::string::npos or line[first] == '#') {
            continue;
        }
        std::istringstream iss(line);
        double x = 0.;
        double y = 0.;
        if(not(iss >> x >> y)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GLineFitIndividual::applyConfig(): Error!" << '\n'
                << "Malformed data-point line (expected \"x y\"): \"" << line << "\"" << '\n'
            );
        }
        ind.data_points_.emplace_back(x, y);
    }
}

/******************************************************************************/
/**
 * @brief The copy constructor.
 *
 * @param cp A constant reference to another GLineFitIndividual object to be copied
 */
GLineFitIndividual::GLineFitIndividual(const GLineFitIndividual &cp)
  : gen::GGenomeT<GLineFitIndividual>(cp)
  , data_points_(cp.data_points_) { /* nothing */
}

/******************************************************************************/
/**
 * @brief The standard destructor.
 */
GLineFitIndividual::~GLineFitIndividual() { /* nothing */
}

/******************************************************************************/
/**
 * @brief Retrieves the tuple (a, b) of the line represented by this object.
 *
 * @return A tuple holding the line's offset a (first parameter) and slope b (second parameter)
 */
std::tuple<double, double> GLineFitIndividual::getLine() const {
    std::vector<double> par_vec;
    this->streamline(par_vec);
    return std::tuple<double, double>(par_vec.at(0), par_vec.at(1));
}

/******************************************************************************/
/**
 * @brief The actual fitness calculation takes place here; computes the root of the summed squared
 * deviation between the fitted line (a + b*x) and the stored data points.
 *
 * @return The fitness of this object: sqrt of the summed squared deviations of line and data points
 */
std::vector<double> GLineFitIndividual::evaluate() {
    double result = 0.;

    // Compute the root of the summed squared deviations between the line (a + b*x) and the data points
    std::vector<double> par_vec;
    this->streamline(par_vec);

    double const a = par_vec.at(0);
    double const b = par_vec.at(1);

    // Sum up the square deviation of line and data points
    double deviation = 0.;
    for(const auto &data_point : data_points_) {
        deviation = (a + b * std::get<0>(data_point)) - std::get<1>(data_point);
        result += Gem::Common::gsquared(deviation);
    }

    return {sqrt(result)};
}

/******************************************************************************/
/**
 * @brief Applies modifications to this object. This is needed for testing purposes.
 *
 * @return A boolean which indicates whether modifications were made
 */
bool GLineFitIndividual::modify_GUnitTests_() {
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
    Gem::Common::condnotset("GLineFitIndividual::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
/**
 * @brief Performs self tests that are expected to succeed. This is needed for testing purposes.
 */
void GLineFitIndividual::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING
    using namespace Gem::Geneva;

    // Call the parent classes' functions
    gen::GGenome::specificTestsNoFailureExpected_GUnitTests_();

    //------------------------------------------------------------------------------
    //------------------------------------------------------------------------------
#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset(
        "GLineFitIndividual::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
