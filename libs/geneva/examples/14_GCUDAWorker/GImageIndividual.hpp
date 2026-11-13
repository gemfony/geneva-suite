/**
 * @file GImageIndividual.hpp
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

// Includes check for correct Boost version(s)
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <algorithm> // for std::sort
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility> // For std::pair
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "common/GGlobalOptionsT.hpp"
#include "common/GParserBuilder.hpp"
#include "common/GSingletonT.hpp"
#include "common/GUnitTestFrameworkT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"

// Example-local headers
#include "GImageScalar.hpp"
#include "common/GSelfTestable.hpp"

namespace Gem::Geneva {

namespace OptimizationAlgorithms {
class GAdaptionConfigBase;
} // namespace OptimizationAlgorithms

constexpr std::size_t GII_DEF_NTRIANGLES = static_cast<std::size_t>(300);
constexpr double GII_DEF_STARTSIZE = 0.;
constexpr double GII_DEF_MINSIZE = 0.;
constexpr double GII_DEF_MAXSIZE = 0.3;
constexpr double GII_DEF_MINOPAQUENESS = 0.3;
constexpr double GII_DEF_MAXOPAQUENESS = 0.6;

constexpr double GII_DEF_ADPROB = 0.05;
constexpr double GII_DEF_ADAPTADPROB = 0.1;
constexpr double GII_DEF_MINADPROB = 0.05;
constexpr double GII_DEF_MAXADPROB = 0.2;
constexpr double GII_DEF_SIGMA = 0.1;
constexpr double GII_DEF_SIGMASIGMA = 0.8;
constexpr double GII_DEF_MINSIGMA = 0.05;
constexpr double GII_DEF_MAXSIGMA = 0.2;

constexpr double GII_DEF_LOC_ADPROB = 0.1;
constexpr double GII_DEF_LOC_ADAPTADPROB = 0.1;
constexpr double GII_DEF_LOC_MINADPROB = 0.1;
constexpr double GII_DEF_LOC_MAXADPROB = 0.3;
constexpr double GII_DEF_LOC_SIGMA = 0.2;
constexpr double GII_DEF_LOC_SIGMASIGMA = 0.8;
constexpr double GII_DEF_LOC_MINSIGMA = 0.1;
constexpr double GII_DEF_LOC_MAXSIGMA = 0.4;

constexpr std::size_t GII_DEF_COLORDEPTH = 8;
constexpr std::size_t GII_DEF_NCOLORS = Gem::Common::PowSmallPosInt<2, GII_DEF_COLORDEPTH>();
constexpr std::size_t GII_DEF_MAXCOLOR = GII_DEF_NCOLORS - 1;
constexpr double GII_DEF_BGRED = 0.9;
constexpr double GII_DEF_BGGREEN = 0.9;
constexpr double GII_DEF_BGBLUE = 0.9;
constexpr bool GII_DEF_ALPHASORT = true;
constexpr bool GII_DEF_CHBGCOLOR = false;
constexpr bool GII_DEF_MUTATE_ALPHA_CHANNEL = false;

constexpr int GII_DEF_IMAGE_WIDTH = 1024;
constexpr int GII_DEF_IMAGE_HEIGHT = 768;

typedef std::tuple<std::size_t, std::size_t> SCREENSIZE_t;

// Circle-based triangle model. A host-side inspection/representation (off the fitness/GPU path;
// populated by getTriangleData()). Its members follow the build's parameter precision
// (gimage_fp_t = double by default, float when GIMAGE_USE_FLOAT), so getTriangleData() does not
// narrow the genome values.
struct CircleTriangle {
    gimage_fp_t r, g, b, a;             // Colors & transparency (0..1)
    gimage_fp_t cx, cy;                 // Middle-coordinates of the circle
    gimage_fp_t radius;                 // Radius
    gimage_fp_t angle1, angle2, angle3; // Angles normalized (0..1)
};

// Add a comparison-operator for our tests
bool operator==(const CircleTriangle &, const CircleTriangle &);
// Output operator
std::ostream &operator<<(std::ostream &, const CircleTriangle &);

/******************************************************************************/
/**
     * This individual searches for a matching set of triangles
     * that most closely resembles a given picture. It was developed
     * for evaluation using CUDA on a GPU.
     */
class GImageIndividual final
  : public gen::GGenomeT<GImageIndividual>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    // The archive still default-constructs the concrete type on load; GReflectiveInterfaceAccess lets the mixin reach
    // the private localMembers_() below (from which serialize/load_/compare_/clone_/name_ are generated).
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
     * @brief Single declaration of this class'es local data members
     * @return A tuple of named member references driving the generated serialize()/load_()/compare_()
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("width_", self.width_),
            Gem::Common::make_member("height_", self.height_),
            Gem::Common::make_member("nTriangles_", self.nTriangles_),
            Gem::Common::make_member("alphaSort_", self.alphaSort_),
            Gem::Common::make_member("changeBGColor_", self.changeBGColor_),
            Gem::Common::make_member("mutateAlphaChannel_", self.mutateAlphaChannel_)
        );
    }

    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() and compare token. */
    static constexpr std::string_view class_name = "GImageIndividual";

    /******************************************************************************/
    /** @brief The default constructor. All real work is done in the init()-Function */
    GImageIndividual() = default;
    /** @brief A standard copy constructor */
    GImageIndividual(const GImageIndividual &) = default;
    /** @brief The standard destructor */
    ~GImageIndividual() override = default;

    //---------------------------------------------------------------------------
    // GIndividualFactory<GImageIndividual> hooks. The individual supplies the static hooks the generic
    // factory needs: describeConfig (the configurable values), buildGenome
    // (the labelled triangle + background genome structure), applyConfig (the per-object members + the
    // random init) and buildAdaptionConfig (the OA-owned main/location Gauss adaption config, authored
    // from the labelled genome layout -- NOT stored on the individual).

    /** @brief The configurable values parsed from the config file. */
    struct Config {
        std::size_t n_triangles = GII_DEF_NTRIANGLES;
        double bg_red = GII_DEF_BGRED;
        double bg_green = GII_DEF_BGGREEN;
        double bg_blue = GII_DEF_BGBLUE;
        double start_size = GII_DEF_STARTSIZE;
        double min_size = GII_DEF_MINSIZE;
        double max_size = GII_DEF_MAXSIZE;
        double min_opaqueness = GII_DEF_MINOPAQUENESS;
        double max_opaqueness = GII_DEF_MAXOPAQUENESS;
        bool alpha_sort = GII_DEF_ALPHASORT;
        bool change_bg_color = GII_DEF_CHBGCOLOR;
        bool mutate_alpha_channel = GII_DEF_MUTATE_ALPHA_CHANNEL;
        double sigma = GII_DEF_SIGMA;
        double sigma_sigma = GII_DEF_SIGMASIGMA;
        double min_sigma = GII_DEF_MINSIGMA;
        double max_sigma = GII_DEF_MAXSIGMA;
        double ad_prob = GII_DEF_ADPROB;
        double adapt_ad_prob = GII_DEF_ADAPTADPROB;
        double min_ad_prob = GII_DEF_MINADPROB;
        double max_ad_prob = GII_DEF_MAXADPROB;
        double loc_sigma = GII_DEF_LOC_SIGMA;
        double loc_sigma_sigma = GII_DEF_LOC_SIGMASIGMA;
        double loc_min_sigma = GII_DEF_LOC_MINSIGMA;
        double loc_max_sigma = GII_DEF_LOC_MAXSIGMA;
        double loc_ad_prob = GII_DEF_LOC_ADPROB;
        double loc_adapt_ad_prob = GII_DEF_LOC_ADAPTADPROB;
        double loc_min_ad_prob = GII_DEF_LOC_MINADPROB;
        double loc_max_ad_prob = GII_DEF_LOC_MAXADPROB;
    };

    /** @brief Registers the config-file options, binding them to the passed Config */
    static void describeConfig(Gem::Common::GParserBuilder &gpb, Config &c);
    /** @brief Builds the flat genome's labelled structure (per triangle: cx,cy,radius,3 angles,r,g,b,a,
     *  then 3 background colours), validating the configured ranges */
    static gen::GenomeData buildGenome(const Config &c);
    /** @brief Per-object post-config hook: sets the local members and random-inits the active parameters */
    static void applyConfig(GImageIndividual &ind, const Config &c);
    /** @brief The OA-owned main/location Gauss adaption config, authored from the labelled genome layout.
     *  Authored from the Config -- no adaptor data resides on the individual. */
    static std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
    buildAdaptionConfig(const gen::GGenome &sample, const Config &c);

    /** @brief Retrieves the number of triangles */
    std::size_t getNTriangles() const;
    /** @brief Retrieves an array with the triangle data, using the circular triangle definition */
    std::vector<CircleTriangle> getTriangleData() const;
    /** @brief Checks whether background colors shall be changed */
    bool getChangeBGColor() const;
    /** @brief Checks whether the alpha channel of triangles shall be mutated */
    bool getMutateAlphaChannel() const;

    /*******************************************************************************************/
    /**
         * Retrieves the background colors
         *
         * @return The background color used for the candidate image
         */
    template <typename fp_type = float>
    std::tuple<fp_type, fp_type, fp_type> getBackGroundColor() const {
        static_assert(
            std::is_same_v<fp_type, float> || std::is_same_v<fp_type, double>,
            "GImageIndividual::getBackGroundColor(): Error! Template argument must be either float "
            "or double"
        );

        // Background colors are the last three values of the flat genome (10 per triangle + 3 bg).
        const std::size_t offset = 10 * nTriangles_;

        std::vector<gimage_fp_t> parVec;
        this->streamline(parVec);

        const fp_type r = std::clamp(static_cast<fp_type>(parVec.at(offset + 0)), fp_type(0), fp_type(1));
        const fp_type g = std::clamp(static_cast<fp_type>(parVec.at(offset + 1)), fp_type(0), fp_type(1));
        const fp_type b = std::clamp(static_cast<fp_type>(parVec.at(offset + 2)), fp_type(0), fp_type(1));
        return {r, g, b};
    }

protected:
    /** @brief The evaluation hook: the CPU render+score of the genome's triangles against the process-wide
     *  target image (Gem::Geneva::MonaLisa::scoreAgainstTarget) -- the SAME render+score the GPU kernel uses,
     *  so it doubles as the device cross-check reference. Reads only the genome and the shared target store.
     *  @return The raw fitness (deviation from the target image) as a one-element vector */
    std::vector<double> evaluate() override;

private:
    /******************************************************************************/
    // Local parameters
    int width_{GII_DEF_IMAGE_WIDTH}, height_{GII_DEF_IMAGE_HEIGHT}; ///< Image dimensions
    std::size_t nTriangles_{GII_DEF_NTRIANGLES};                    ///< The number of triangles
    bool alphaSort_{GII_DEF_ALPHASORT};
    ///< Indicates whether triangles should be sorted according to their alpha channel
    bool changeBGColor_{GII_DEF_CHBGCOLOR}; ///< Whether the background color should be mutated
    ///< Indicates whether the alpha-channel of triangle colors shall be mutated
    bool mutateAlphaChannel_{GII_DEF_MUTATE_ALPHA_CHANNEL};

protected:
    /** @brief Applies modifications to this object. */
    bool modify_GUnitTests_() override;
    /** @brief Performs self tests that are expected to succeed. */
    void specificTestsNoFailureExpected_GUnitTests_() override;
};

/******************************************************************************/
//////////////////////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
     * A factory for GImageIndividual objects: an alias for the generic, config-driven
     * GIndividualFactory, for which GImageIndividual supplies the static describeConfig /
     * buildGenome / applyConfig / buildAdaptionConfig hooks. Call sites use ctor(path), get_as<>()
     * and the factory's getAdaptionConfig(sample).
     */
using GImageIndividualFactory = Gem::Geneva::Genome::GIndividualFactory<GImageIndividual>;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
} /* namespace Gem::Geneva */

#ifdef GEM_TESTING

template <>
std::shared_ptr<Gem::Geneva::GImageIndividual> TFactory_GUnitTests<Gem::Geneva::GImageIndividual>();

#endif /* GEM_TESTING */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

