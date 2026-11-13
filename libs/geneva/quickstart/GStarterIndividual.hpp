/**
 * @file GStarterIndividual.hpp
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
#include <cmath>
#include <iostream>
#include <sstream>
#include <tuple>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "common/GSelfTestable.hpp"
#include <filesystem>
#include <memory>
#include <string>

namespace Gem {
namespace Geneva {

namespace OptimizationAlgorithms {
class GAdaptionConfigBase;
} // namespace OptimizationAlgorithms

/******************************************************************************/
/**
 * This enum denotes the possible demo function types
 */
enum class targetFunction : Gem::Common::ENUMBASETYPE {
    PARABOLA = 0,
    NOISYPARABOLA = 1
};

// Numeric streaming opt-in for targetFunction; must precede its first streaming
// use (see numeric_enum_io_v in GCommonEnums.hpp).
} /* namespace Geneva */
} /* namespace Gem */
namespace Gem::Common {
template <> inline constexpr bool numeric_enum_io_v<Gem::Geneva::targetFunction> = true;
} /* namespace Gem::Common */
namespace Gem {
namespace Geneva {

// targetFunction streams as its underlying numeric value through the shared
// machinery in GCommonEnums.hpp (marker specialization at the end of this
// header); the operators are already re-exported into Gem::Geneva by
// GOptimizationEnums.hpp.

/******************************************************************************/
// A number of default settings for the factory
const double GSI_DEF_ADPROB = 1.0;
const double GSI_DEF_SIGMA = 0.025;
const double GSI_DEF_SIGMASIGMA = 0.2;
const double GSI_DEF_MINSIGMA = 0.001;
const double GSI_DEF_MAXSIGMA = 1;
const targetFunction GO_DEF_TARGETFUNCTION = targetFunction::PARABOLA;

/******************************************************************************/
/**
 * This individual searches for a minimum of a number of predefined functions, each capable
 * of processing their input in multiple dimensions.
 */
//! [quickstart-individual-decl#1]
class GStarterIndividual
  : public gen::GGenomeT<GStarterIndividual>
  , public Gem::Common::GSelfTestable {
//! [quickstart-individual-decl#1]
    ///////////////////////////////////////////////////////////////////////
    // The archive still default-constructs the concrete type on load; GReflectiveInterfaceAccess lets the mixin
    // reach the private localMembers_() below (from which serialize/load_/compare_/clone_/name_ derive).
    //! [weft-local-members#1]
    friend struct Gem::Common::GReflectiveInterfaceAccess;
    //! [weft-local-members#1]

    /**
     * @brief Single declaration of this class'es local data members. The GReflectiveInterfaceT mixin (via
     * GGenomeT) generates serialize() / load_() / compare_() / clone_() / name_() from this one list
     * (plus the GGenome base slice), so a stateful flat individual lives in exactly one place -- no
     * hand-written quartet, no silently-dropped member.
     * @return A tuple of named member references
     */
    //! [weft-local-members#2]
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(Gem::Common::make_member("targetFunction_", self.targetFunction_));
    }
    //! [weft-local-members#2]

    ///////////////////////////////////////////////////////////////////////

//! [quickstart-individual-decl#2]
public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() and the compare token. */
    //! [weft-local-members#3]
    static constexpr std::string_view class_name = "GStarterIndividual";
    //! [weft-local-members#3]
//! [quickstart-individual-decl#2]

    /** @brief The default constructor */
    GStarterIndividual();
    /** @brief A constructor that receives the genome's start values and bounds */
    GStarterIndividual(
        const std::size_t &,
        const std::vector<double> &,
        const std::vector<double> &,
        const std::vector<double> &
    );
    /** @brief A standard copy constructor */
    GStarterIndividual(const GStarterIndividual &);
    /** @brief The standard destructor */
    virtual ~GStarterIndividual();

    /** @brief Allows to set the demo function */
    void setTargetFunction(targetFunction);
    /** @brief Allows to retrieve the current demo function */
    targetFunction getTargetFunction() const;

    /** @brief Emit information about this individual */
    std::string print();

    //---------------------------------------------------------------------------
    // GIndividualFactory<GStarterIndividual> hooks. The individual supplies the static hooks the
    // generic factory needs: describeConfig (the configurable values,
    // including the target function), buildGenome (one constrained double per start value), applyConfig
    // (the per-object, non-genome target function) and buildAdaptionConfig (the OA-owned Gauss adaption
    // config, authored from the Config -- NOT stored on the individual). The full ctor + addContent()
    // below remain as a standalone (factory-less) construction path, used by the unit tests.

    //! [problem-factory-hooks#1]
    /** @brief The configurable values parsed from the config file. */
    struct Config {
        double ad_prob = GSI_DEF_ADPROB;
        double sigma = GSI_DEF_SIGMA;
        double sigma_sigma = GSI_DEF_SIGMASIGMA;
        double min_sigma = GSI_DEF_MINSIGMA;
        double max_sigma = GSI_DEF_MAXSIGMA;
        std::vector<double> start_values{1., 1., 1.};
        std::vector<double> lower_boundaries{0., 0., 0.};
        std::vector<double> upper_boundaries{2., 2., 2.};
        targetFunction target_function = GO_DEF_TARGETFUNCTION;
    };
    //! [problem-factory-hooks#1]

    //! [problem-factory-hooks#2]
    /** @brief Registers the config-file options, binding them to the passed Config */
    static void describeConfig(Gem::Common::GParserBuilder &gpb, Config &c);
    /** @brief Builds the flat genome's structure: one constrained double per start value */
    static gen::GenomeData buildGenome(const Config &c);
    /** @brief Per-object post-config hook: stamps the (non-genome) target function */
    static void applyConfig(GStarterIndividual &ind, const Config &c);
    /** @brief The OA-owned Gauss adaption config: every parameter group gets the configured adaptor.
     *  Authored from the Config -- no adaptor data resides on the individual. */
    static std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
    buildAdaptionConfig(const gen::GGenome &sample, const Config &c);
    //! [problem-factory-hooks#2]

    /***************************************************************************/
    /**
	  * This function is used to unify the setup from within the constructor
	  * and factory.
	  */
    static void addContent(
        GStarterIndividual &p,
        const std::size_t &prod_id,
        const std::vector<double> &startValues,
        const std::vector<double> &lowerBoundaries,
        const std::vector<double> &upperBoundaries
    ) {
        // Some error checking
#ifdef DEBUG
        // Check whether values have been provided
        if(startValues.empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GStarterIndividual::addContent(): Error!" << '\n'
                << "No parameters given" << '\n'
            );
        }

        // Check whether all sizes match
        if(startValues.size() != lowerBoundaries.size() ||
           startValues.size() != upperBoundaries.size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GStarterIndividual::addContent(): Error!" << '\n'
                << "Invalid sizes" << startValues.size() << " / " << lowerBoundaries.size()
                << " / " << upperBoundaries.size() << '\n'
            );
        }

        // Check that start values and boundaries have valid values
        for(std::size_t i = 0; i < startValues.size(); i++) {
            Gem::Common::checkValueRange( // We expect the start value to be in the range [lower, upper[
				 startValues.at(i)
				 , lowerBoundaries.at(i)
				 , upperBoundaries.at(i)
				 , false // closed lower boundary
				 , true  // open upper boundary
			 );
        }

#endif /* DEBUG */

        // Build the flat genome's STRUCTURE: one constrained double per parameter. The Gauss adaptor lives
        // solely on the OA-owned config (buildAdaptionConfig()); none of its parameters reside here.
        gen::GGenomeBuilder b;
        for(std::size_t i = 0; i < startValues.size(); i++) {
            b.addDouble(startValues.at(i), lowerBoundaries.at(i), upperBoundaries.at(i));
        }
        p.setGenome(b.build());

        // The first individual (prod_id == 0) keeps the supplied start values; all others start
        // randomly within bounds.
        if(prod_id != 0) {
            p.randomInit(Gem::Geneva::activityMode::ALLPARAMETERS);
        }
    }

protected:
    /***************************************************************************/
    // load_(), compare_(), clone_() and name_() are generated by the Gem::Common::GReflectiveInterfaceT base
    // (via GGenomeT) from localMembers_() -- no hand-written quartet.

    //! [quickstart-individual-decl#3]
    /** @brief The evaluation hook: the selected target function on the genome (single criterion). */
    std::vector<double> evaluate() final;
    //! [quickstart-individual-decl#3]

    /** @brief Applies modifications to this object. */
    bool modify_GUnitTests_() override;
    /** @brief Performs self tests that are expected to succeed. */
    void specificTestsNoFailureExpected_GUnitTests_() override;
    /** @brief Performs self tests that are expected to fail. */
    void specificTestsFailuresExpected_GUnitTests_() override;

private:
    /***************************************************************************/

    targetFunction targetFunction_ =
        GO_DEF_TARGETFUNCTION; ///< Specifies which demo function should be used

    /***************************************************************************/
    /** @brief A simple n-dimensional parabola */
    double parabola(const std::vector<double> &parVec) const;
    /** @brief A "noisy" parabola */
    double noisyParabola(const std::vector<double> &parVec) const;

    /***************************************************************************/
};

/** @brief Allows to output a GStarterIndividual (or convert it to a string) via its operator<< */
std::ostream &operator<<(std::ostream &, const std::unique_ptr<GStarterIndividual> &);

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A factory for GStarterIndividual objects: an alias for the generic, config-driven
 * GIndividualFactory, for which GStarterIndividual supplies the static describeConfig /
 * buildGenome / applyConfig / buildAdaptionConfig hooks. Call sites use ctor(path), get_as<>(),
 * registerContentCreator() and the factory's getAdaptionConfig(sample).
 */
//! [problem-factory-hooks#3]
using GStarterIndividualFactory = Gem::Geneva::Genome::GIndividualFactory<GStarterIndividual>;
//! [problem-factory-hooks#3]

/******************************************************************************/

} /* namespace Geneva */
} /* namespace Gem */

