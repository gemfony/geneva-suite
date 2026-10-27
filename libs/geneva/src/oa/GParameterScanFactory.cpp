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

#include "geneva/oa/GParameterScanFactory.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GFactoryT.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOAFactoryT.hpp"
#include "geneva/oa/GParameterScan.hpp"
#include "geneva/oa/GParameterScan_PersonalityTraits.hpp"
#include "geneva/genome/GGenome.hpp"
#include <boost/program_options.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace Gem::Geneva::OptimizationAlgorithms {

/***************************************************************************/
/**
 * @brief Adds local command line options to a boost::program_options::options_description object.
 *
 * @param visible Command line options that should always be visible
 * @param hidden Command line options that should only be visible upon request
 */
void GParameterScanFactory::addCLOptions(
    boost::program_options::options_description &visible,
    boost::program_options::options_description &hidden
) {
    namespace po = boost::program_options;

    hidden.add_options()(
        "parameterSpec",
        po::value<std::string>(&parameter_spec_cl_)->default_value(std::string("empty")),
        "\t[GParameterScanFactory] Specification of parameters to be scanned. Syntax: \"d(0, -10., "
        "10., 100)\". Use a comma-separated list for more than one variable. A single entry "
        "\"s(1000)\" will lead to a random scan over all parameters of up to 1000 individuals"
    );

    // Add the parent class'es options
    GOAFactoryT<GOptimizationAlgorithmBase>::addCLOptions(visible, hidden);
}

/******************************************************************************/
/**
 * @brief Allows to specify the command line parameter manually for variables to be scanned.
 *
 * @param par_str The parameter specification string to store for later use by the created algorithm
 */
void GParameterScanFactory::setCLParameterSpecs(std::string par_str) {
    parameter_spec_cl_ = std::move(par_str);
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the command line parameter settings for variables to be scanned.
 *
 * @return The currently stored parameter specification string (or "empty" if none was set)
 */
std::string GParameterScanFactory::getCLParameterSpecs() const {
    return parameter_spec_cl_;
}

/******************************************************************************/
/**
 * @brief Allows to reset the command line parameter specs to the default ("empty").
 */
void GParameterScanFactory::resetCLParameterSpecs() {
    parameter_spec_cl_ = "empty";
}

/******************************************************************************/
/**
 * @brief Allows to act on the configuration options received from the configuration file. Here
 * we can add the options described in describeLocalOptions to the object.
 *
 * @param p_base A reference to the smart-pointer holding the optimization algorithm to be
 *               post-processed (expected to point at a GParameterScan); its parameter specs are
 *               applied if a non-"empty" command-line specification was set
 */
void GParameterScanFactory::postProcess_(std::shared_ptr<GOptimizationAlgorithmBase> &p_base) {
    if(parameter_spec_cl_ != "empty") {
        std::shared_ptr<GParameterScan> const p =
            Gem::Common::convertSmartPointer<GOptimizationAlgorithmBase, GParameterScan>(p_base);

        p->setParameterSpecs(parameter_spec_cl_);
    }

    // Call our parent class'es function
    GOAFactoryT<GOptimizationAlgorithmBase>::postProcess_(p_base);
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
