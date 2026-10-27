/**
 * @file GImageIndividual.cpp
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

#include "GImageIndividual.hpp"
#include "GMonaLisaProblem.hpp"

#include "geneva/oa/GAdaption.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#endif /* GEM_TESTING */

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::GImageIndividual) // NOLINT
namespace Gem::Geneva {
/******************************************************************************/
//////////////////////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
// Add a comparison-operator for our tests
bool operator==(const CircleTriangle &lhs, const CircleTriangle &rhs) {
    return (
        (lhs.r == rhs.r) && (lhs.g == rhs.g) && (lhs.b == rhs.b) && (lhs.a == rhs.a) &&
        (lhs.cx == rhs.cx) && (lhs.cy == rhs.cy) && (lhs.radius == rhs.radius) &&
        (lhs.angle1 == rhs.angle1) && (lhs.angle2 == rhs.angle2) && (lhs.angle3 == rhs.angle3)
    );
}

// Output operator for the CircleStruct
std::ostream &operator<<(std::ostream &os, const CircleTriangle &ct) {
    os << "CircleTriangle(" << '\n';
    os << "r=" << static_cast<int>(ct.r) << ", " << '\n';
    os << "g=" << static_cast<int>(ct.g) << ", " << '\n';
    os << "b=" << static_cast<int>(ct.b) << ", " << '\n';
    os << "a=" << static_cast<int>(ct.a) << ", " << '\n';
    os << "cx=" << ct.cx << ", " << '\n';
    os << "cy=" << ct.cy << ", " << '\n';
    os << "radius=" << ct.radius << ", " << '\n';
    os << "angle1=" << ct.angle1 << ", " << '\n';
    os << "angle2=" << ct.angle2 << ", " << '\n';
    os << "angle3=" << ct.angle3 << '\n';
    os << ")" << '\n' << '\n';
    return os;
}

/******************************************************************************/
//////////////////////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
	 * Builds the flat genome's STRUCTURE for one candidate image. The genome holds 10 contiguous values
	 * per triangle -- cx, cy, radius, angle1..3, r, g, b, a -- followed by the 3 background colours, all
	 * constrained gimage_fp_t in their own Gauss group; the centre (cx, cy) groups carry the "loc" label
	 * and everything else "main", so applyConfig() can author the matching OA-owned Gauss adaptor by label.
	 * The configured ranges are validated up front.
	 */
/******************************************************************************/
/**
 * @brief Validates the size / adaption-probability ranges of a GImageIndividual::Config, throwing a
 * geneva_exception on any inconsistency. Extracted from buildGenome() (the validation preamble).
 *
 * @param c The Config whose size and (local) adaption-probability ranges are validated
 */
static void validateBuildGenomeConfig(const GImageIndividual::Config &c) {
    if(c.min_size < 0. || c.max_size > 1. || c.min_size >= c.max_size) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GImageIndividual::buildGenome() : Error!" << '\n'
            << "Invalid values for minSize and maxSize provided: " << c.min_size << " / "
            << c.max_size << '\n'
        );
    }

    // A startSize < 0 means random initialization in the range [minSize, maxSize]
    if(c.start_size >= 0. && c.start_size < c.min_size) {
        // Cannot be < 0 as minSize may not be <= 0
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GImageIndividual::buildGenome() : Error!" << '\n'
            << "Invalid values for minSize and startSize provided: " << c.min_size << " / "
            << c.start_size << '\n'
        );
    }

    if(c.start_size > c.max_size) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GImageIndividual::buildGenome() : Error!" << '\n'
            << "Invalid values for maxSize and startSize provided: " << c.max_size << " / "
            << c.start_size << '\n'
        );
    }

    if(c.adapt_ad_prob < 0. || c.adapt_ad_prob > 1.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GImageIndividual::buildGenome() : Error!" << '\n'
            << "Invalid value for adaptAdProb provided: " << c.adapt_ad_prob << '\n'
        );
    }

    if(c.loc_adapt_ad_prob < 0. || c.loc_adapt_ad_prob > 1.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GImageIndividual::buildGenome() : Error!" << '\n'
            << "Invalid value for loc_adaptAdProb provided: " << c.loc_adapt_ad_prob << '\n'
        );
    }

    if(c.min_ad_prob >= c.max_ad_prob || c.min_ad_prob < 0. || c.max_ad_prob > 1. ||
       c.ad_prob < c.min_ad_prob || c.ad_prob > c.max_ad_prob) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GImageIndividual::buildGenome() : Error!" << '\n'
            << "Invalid values for minAdprob, maxAdProb or adProb provided: " << c.min_ad_prob
            << " / " << c.max_ad_prob << " / " << c.ad_prob << '\n'
        );
    }

    if(c.loc_min_ad_prob >= c.loc_max_ad_prob || c.loc_min_ad_prob < 0. || c.loc_max_ad_prob > 1. ||
       c.loc_ad_prob < c.loc_min_ad_prob || c.loc_ad_prob > c.loc_max_ad_prob) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GImageIndividual::buildGenome() : Error!" << '\n'
            << "Invalid values for loc_minAdprob, loc_maxAdProb or loc_adProb provided: "
            << c.loc_min_ad_prob << " / " << c.loc_max_ad_prob << " / " << c.loc_ad_prob << '\n'
        );
    }
}

gen::GenomeData GImageIndividual::buildGenome(const Config &c) {
    validateBuildGenomeConfig(c);

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Build the flat genome via GGenomeBuilder. The streamline order is a positional contract --
    // per triangle: cx, cy, radius, angle1..3, r, g, b, a (10 values), then the 3 background colours --
    // so getTriangleData() / getBackGroundColor() / the GPU marshaller read it positionally and the
    // rasteriser matches. Every value is a constrained gimage_fp_t with its own Gauss group; the
    // location params (cx, cy) use the "loc" adaptor config, everything else the main one.
    const std::size_t nTriangles = c.n_triangles;

    gen::GGenomeBuilder bld;

    // Adds one constrained gimage_fp_t parameter, dispatching to the matching builder channel.
    auto addParam = [&bld](gimage_fp_t init, gimage_fp_t lo, gimage_fp_t hi) {
        if constexpr(std::is_same_v<gimage_fp_t, float>) {
            return bld.addFloat(init, lo, hi);
        }
        else {
            return bld.addDouble(init, lo, hi);
        }
    };
    // The Gauss adaptors live on the OA-owned config (authored in applyConfig()), not the genome layout.
    // Tag each group with its adaptor class -- "main" (size / angles / colours / alpha / background) or
    // "loc" (centre x/y) -- so the config can author the matching adaptor onto them by label.
    auto mainGauss = [](gen::ParamHandle<gimage_fp_t> &h) { h.label("main"); };
    auto locGauss = [](gen::ParamHandle<gimage_fp_t> &h) { h.label("loc"); };

    for(std::size_t t_cnt = 0; t_cnt < nTriangles; t_cnt++) {
        // middle-x and -y: the location adaptor.
        auto cx = addParam(gimage_fp_t(0), gimage_fp_t(0), gimage_fp_t(1));
        locGauss(cx);
        auto cy = addParam(gimage_fp_t(0), gimage_fp_t(0), gimage_fp_t(1));
        locGauss(cy);

        // radius: startSize >= 0 seeds it (else random init below); the main adaptor.
        const gimage_fp_t radius_init =
            static_cast<gimage_fp_t>(c.start_size >= 0. ? c.start_size : c.min_size);
        auto rad = addParam(
            radius_init, static_cast<gimage_fp_t>(c.min_size), static_cast<gimage_fp_t>(c.max_size)
        );
        mainGauss(rad);

        // three angles.
        auto a1 = addParam(gimage_fp_t(0), gimage_fp_t(0), gimage_fp_t(1));
        mainGauss(a1);
        auto a2 = addParam(gimage_fp_t(0), gimage_fp_t(0), gimage_fp_t(1));
        mainGauss(a2);
        auto a3 = addParam(gimage_fp_t(0), gimage_fp_t(0), gimage_fp_t(1));
        mainGauss(a3);

        // three colours.
        auto cr = addParam(gimage_fp_t(0), gimage_fp_t(0), gimage_fp_t(1));
        mainGauss(cr);
        auto cg = addParam(gimage_fp_t(0), gimage_fp_t(0), gimage_fp_t(1));
        mainGauss(cg);
        auto cb = addParam(gimage_fp_t(0), gimage_fp_t(0), gimage_fp_t(1));
        mainGauss(cb);

        // alpha channel: frozen at maxOpaqueness unless alpha mutation is enabled.
        auto ca = addParam(
            static_cast<gimage_fp_t>(c.max_opaqueness),
            static_cast<gimage_fp_t>(c.min_opaqueness),
            static_cast<gimage_fp_t>(c.max_opaqueness)
        );
        mainGauss(ca);
        if(not c.mutate_alpha_channel) {
            ca.adaptionMode(adaptionMode::NEVER);
        }
    }

    //---------------------------------------------------------------------------
    // Add the three background colours (the last three values of the genome). A negative bg* means
    // random init; otherwise it seeds the value. When background colours are not adapted, the groups
    // are inactive (NEVER) and keep their seed across the whole population (randomInit skips them).
    auto addBg = [&](double bgVal) {
        const gimage_fp_t init = static_cast<gimage_fp_t>(bgVal >= 0. ? bgVal : 0.);
        auto h = addParam(init, gimage_fp_t(0), gimage_fp_t(1));
        mainGauss(h);
        if(not c.change_bg_color) {
            h.adaptionMode(adaptionMode::NEVER);
        }
    };
    addBg(c.bg_red);
    addBg(c.bg_green);
    addBg(c.bg_blue);

    std::cout << (c.change_bg_color ? "Background colors will be adapted"
                                    : "Background colors will not be adapted")
              << '\n';

    return bld.build();
}

/******************************************************************************/
/**
	 * Per-object post-config hook. The factory installs the shared, structure-only genome (above) on the
	 * produced individual; this then sets the individual's local members and randomly initialises the
	 * ACTIVE parameters within their bounds (inactive frozen-alpha / frozen-background groups keep their
	 * seeds). The Gauss adaption config is NOT stored on the individual -- it is authored separately by
	 * buildAdaptionConfig() and owned by the optimization algorithm.
	 */
void GImageIndividual::applyConfig(GImageIndividual &ind, const Config &c) {
    ind.nTriangles_ = c.n_triangles;
    ind.alphaSort_ = c.alpha_sort;
    ind.mutateAlphaChannel_ = c.mutate_alpha_channel;
    ind.changeBGColor_ = c.change_bg_color;

    ind.randomInit(activityMode::ACTIVEONLY);
}

/******************************************************************************/
/**
	 * Builds the OA-owned adaption configuration for a genome produced by this factory: the location
	 * adaptor on the "loc" (centre) groups and the main adaptor on every "main" group, authored from the
	 * labelled genome layout. The frozen alpha / background groups are labelled "main" too but were built
	 * adaptionMode::NEVER (inactive), so the adaption kernel skips them. The adaptor settings come from the
	 * Config and live solely on the returned (OA-owned) config -- none of them reside on the individual.
	 *
	 * @param sample A sample flat genome whose labelled group structure the config mirrors
	 * @param c The Config supplying the main / location Gauss adaptor parameters
	 * @return A shared pointer to the populated OA-owned adaption config
	 */
//! [adaptors-by-label]
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GImageIndividual::buildAdaptionConfig(const gen::GGenome &sample, const Config &c) {
    namespace oa = Gem::Geneva::OptimizationAlgorithms;
    auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(sample);
    cfg->forLabel("loc").gauss(
        c.loc_sigma, c.loc_sigma_sigma, c.loc_min_sigma, c.loc_max_sigma, c.loc_ad_prob,
        c.loc_adapt_ad_prob, 1, adaptionMode::WITHPROBABILITY, c.loc_min_ad_prob, c.loc_max_ad_prob
    );
    cfg->forLabel("main").gauss(
        c.sigma, c.sigma_sigma, c.min_sigma, c.max_sigma, c.ad_prob, c.adapt_ad_prob, 1,
        adaptionMode::WITHPROBABILITY, c.min_ad_prob, c.max_ad_prob
    );
    return cfg;
}
//! [adaptors-by-label]

/*******************************************************************************************/
/**
	 * Retrieves the number of triangles
	 */
std::size_t GImageIndividual::getNTriangles() const {
    return nTriangles_;
}

/*******************************************************************************************/
/**
	 * Checks whether background colors shall be changed
	 *
	 * @return A boolean indicating whether background colors shall be changed
	 */
bool GImageIndividual::getChangeBGColor() const {
    return changeBGColor_;
}

/*******************************************************************************************/
/**
	 * Checks whether the alpha channel of triangles shall be mutated
	 *
	 * @return A boolean indicating whether the alpha-channel of triangles shall be mutated
	 */
bool GImageIndividual::getMutateAlphaChannel() const {
    return mutateAlphaChannel_;
}

/*******************************************************************************************/
/**
	 * Retrieve an array with the triangles' data, using the circular triangle definition.
	 * Note that this array might be sorted in ascending order of opacity and might thus not be
	 * identical to the order in which triangles are sorted in this individual.
	 *
	 * @return An array with the triangle data
	 */
std::vector<CircleTriangle> GImageIndividual::getTriangleData() const {
    // The flat genome stores 10 contiguous values per triangle in the canonical render order, so each
    // triangle's fields are read positionally from the streamlined value array (10*nTriangles + 3 bg).
    std::vector<gimage_fp_t> parVec;
    this->streamline(parVec);

#ifdef DEBUG
    if(parVec.size() != 10 * nTriangles_ + 3) {
        // including background color
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GImageIndividual::getTriangleData(): Error!" << '\n'
            << "Invalid number of parameters in this class " << parVec.size() << " / "
            << (10 * nTriangles_ + 3) << '\n'
        );
    }
#endif /* DEBUG */

    std::size_t offset = 0;
    std::vector<CircleTriangle> circle_cnt(nTriangles_);
    for(std::size_t i = 0; i < nTriangles_; i++) {
        offset = i * 10;

        circle_cnt[i].cx = parVec[offset + 0];
        circle_cnt[i].cy = parVec[offset + 1];
        circle_cnt[i].radius = parVec[offset + 2];

        circle_cnt[i].angle1 = parVec[offset + 3];
        circle_cnt[i].angle2 = parVec[offset + 4];
        circle_cnt[i].angle3 = parVec[offset + 5];

        circle_cnt[i].r = parVec[offset + 6];
        circle_cnt[i].g = parVec[offset + 7];
        circle_cnt[i].b = parVec[offset + 8];
        circle_cnt[i].a = parVec[offset + 9];
    }

    if(alphaSort_) {
        // Sort circle_cnt so that items with higher opacity are in the front position
        // As a result, they will be drawn first
        std::ranges::sort(circle_cnt, [](const CircleTriangle &x, const CircleTriangle &y) {
            return x.a > y.a;
        });
    }

    return circle_cnt;
}

/******************************************************************************/
/**
 * The evaluation hook (a single criterion -> a one-element result vector).
 *
 * The host fitness is computed on the CPU via the SAME render+score the GPU kernel uses
 * (Gem::Geneva::MonaLisa::score, shared in GMonaLisaProblem.hpp). This makes the individual evaluable purely
 * on the CPU -- to cross-check the GPU result and compare speed -- while the GGPUConsumer path uses the device
 * kernel. Requires the target image to have been loaded (Gem::Geneva::MonaLisa::loadTarget) beforehand. The
 * genome scalar type is selected at compile time (gimage_fp_t); streamline<gimage_fp_t> is required --
 * streamline<float> collects nothing from a double genome and vice-versa.
 *
 * @return The raw fitness (deviation from the target image) as a one-element vector
 */
std::vector<double> GImageIndividual::evaluate() {
    std::vector<gimage_fp_t> parVec;
    this->streamline(parVec);
    return {Gem::Geneva::MonaLisa::scoreAgainstTarget(parVec.data(), static_cast<int>(parVec.size()))};
}

/******************************************************************************/
/**
	 * Applies modifications to this object. This is needed for testing purposes
	 *
	 * @return A boolean which indicates whether modifications were made
	 */
bool GImageIndividual::modify_GUnitTests_() {
#ifdef GEM_TESTING

    // Call the parent classes' functions. This already random-initialises every genome parameter, so
    // the object is changed. The Gauss adaptor configuration is OA-owned and not exercised here (the
    // individual carries no adaptor data).
    gen::GGenome::modify_GUnitTests_();

    return true;
#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset("GImageIndividual::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
/**
	 * Performs self tests that are expected to succeed. This is needed for testing purposes
	 */
void GImageIndividual::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING
    using namespace Gem::Geneva;

    // Call the parent classes' functions
    gen::GGenome::specificTestsNoFailureExpected_GUnitTests_();

    const std::size_t NTESTS = 100;

    //------------------------------------------------------------------------------

    {
        // Test that repeated extraction of an object's data results in the same output
        auto const p_test = this->clone<GImageIndividual>();

        const auto circles = p_test->getTriangleData();

        for(std::size_t i = 0; i < NTESTS; i++) {
            auto circles_new = p_test->getTriangleData();
            REQUIRE(circles_new == circles);
        }
    }

    //------------------------------------------------------------------------------
#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset("GImageIndividual::modify_GUnitTests", "GEM_TESTING");
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Registers the config-file options, binding them to the passed Config. The checkValueRange() guards
 * validate the defaults at registration time.
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent config-registration sweep (the Mona-Lisa problem's option list); splitting would scatter the option list across helper functions
void GImageIndividual::describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
    using namespace Gem::Common;

    // Describe our own options
    using namespace Gem::Courtier;
    using namespace Gem::Common;

    std::string comment;

    comment = "";
    comment += "The minimum size of the triangle in percent of the canvas;";
    comment += "The allowed value range is [0,maxSize[;";
    gpb.registerFileParameter<double>(
        "min_size",
        c.min_size,
        GII_DEF_MINSIZE,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(c.min_size, 0., 1., GFPLOWERCLOSED, GFPUPPEROPEN, GFNOWARNING, "min_size");

    comment = "";
    comment += "The maximum size of the triangle in percent of the canvas;";
    comment += "The allowed value range is ]minSize,1];";
    gpb.registerFileParameter<double>(
        "max_size",
        c.max_size,
        GII_DEF_MAXSIZE,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.max_size,
        c.min_size,
        1.,
        GFPLOWEROPEN,
        GFPUPPEROPEN,
        GFNOWARNING,
        "max_size"
    );

    comment = "";
    comment += "The start size of the triangle in percent of the canvas;";
    comment += "The allowed value range is [minSize,maxSize];";
    comment += "A value < 0 means random in the range [minSize,maxSize];";
    gpb.registerFileParameter<double>(
        "start_size",
        c.start_size,
        GII_DEF_MINSIZE,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );

    // A value < 0 means random in the range [minSize,maxSize]
    if(c.start_size >= 0.) {
        checkValueRange(
            c.start_size,
            c.min_size,
            c.max_size,
            GFPLOWERCLOSED,
            GFPUPPEROPEN,
            GFNOWARNING,
            "start_size"
        );
    }

    comment = "";
    comment += "The minimum allowed opaqueness of triangles;";
    comment += "The allowed value range is [0,maxOpaqueness];";
    gpb.registerFileParameter<double>(
        "min_opaqueness",
        c.min_opaqueness,
        GII_DEF_MINOPAQUENESS,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.min_opaqueness,
        0.,
        1.,
        GFPLOWERCLOSED,
        GFPUPPEROPEN,
        GFNOWARNING,
        "min_opaqueness"
    );

    comment = "";
    comment += "The maximum allowed opaqueness of triangles;";
    comment += "The allowed value range is [minOpaqueness,1];";
    gpb.registerFileParameter<double>(
        "max_opaqueness",
        c.max_opaqueness,
        GII_DEF_MAXOPAQUENESS,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.max_opaqueness,
        c.min_opaqueness,
        1.,
        GFPLOWEROPEN,
        GFPUPPEROPEN,
        GFNOWARNING,
        "max_opaqueness"
    );

    comment = "";
    comment +=
        "Determines the rate of adaption of adProb. Set to 0, if you do not need this feature;";
    gpb.registerFileParameter<double>(
        "adapt_ad_prob",
        c.adapt_ad_prob,
        GII_DEF_ADAPTADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );

    comment = "";
    comment += "Determines the rate of adaption of location-adProb. Set to 0, if you do not need "
               "this feature;";
    gpb.registerFileParameter<double>(
        "loc_adapt_ad_prob",
        c.loc_adapt_ad_prob,
        GII_DEF_LOC_ADAPTADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );

    comment = "";
    comment += "The lower allowed boundary for adProb-variation;";
    gpb.registerFileParameter<double>(
        "min_ad_prob",
        c.min_ad_prob,
        GII_DEF_MINADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.min_ad_prob,
        0.,
        1.,
        GFPLOWERCLOSED,
        GFPUPPERCLOSED,
        GFNOWARNING,
        "min_ad_prob"
    );

    comment = "";
    comment += "The upper allowed boundary for adProb-variation;";
    gpb.registerFileParameter<double>(
        "max_ad_prob",
        c.max_ad_prob,
        GII_DEF_MAXADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.max_ad_prob,
        c.min_ad_prob,
        1.,
        GFPLOWERCLOSED,
        GFPUPPERCLOSED,
        GFNOWARNING,
        "max_ad_prob"
    );

    comment = "";
    comment += "The lower allowed boundary for loc_adProb-variation;";
    gpb.registerFileParameter<double>(
        "loc_min_ad_prob",
        c.loc_min_ad_prob,
        GII_DEF_LOC_MINADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.loc_min_ad_prob,
        0.,
        1.,
        GFPLOWERCLOSED,
        GFPUPPERCLOSED,
        GFNOWARNING,
        "loc_min_ad_prob"
    );

    comment = "";
    comment += "The upper allowed boundary for loc_adProb-variation;";
    gpb.registerFileParameter<double>(
        "loc_max_ad_prob",
        c.loc_max_ad_prob,
        GII_DEF_LOC_MAXADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.loc_max_ad_prob,
        c.loc_min_ad_prob,
        1.,
        GFPLOWERCLOSED,
        GFPUPPERCLOSED,
        GFNOWARNING,
        "loc_max_ad_prob"
    );

    comment = "";
    comment += "The probability for random adaptions of values in evolutionary algorithms;";
    comment += "The allowed value range is [0,1];";
    gpb.registerFileParameter<double>(
        "ad_prob",
        c.ad_prob,
        GII_DEF_ADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.ad_prob,
        c.min_ad_prob,
        c.max_ad_prob,
        GFPLOWERCLOSED,
        GFPUPPERCLOSED,
        GFNOWARNING,
        "ad_prob"
    );

    comment = "";
    comment += "The probability for random adaptions of location parameters of values in "
               "evolutionary algorithms;";
    comment += "The allowed value range is [0,1];";
    gpb.registerFileParameter<double>(
        "loc_ad_prob",
        c.loc_ad_prob,
        GII_DEF_LOC_ADPROB,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.loc_ad_prob,
        c.loc_min_ad_prob,
        c.loc_max_ad_prob,
        GFPLOWERCLOSED,
        GFPUPPERCLOSED,
        GFNOWARNING,
        "loc_ad_prob"
    );

    comment = "";
    comment += "The sigma for gauss-adaption in ES;";
    comment += "sigma must be positive;";
    comment += "Recommended value range [0,1];";
    gpb.registerFileParameter<double>(
        "sigma",
        c.sigma,
        GII_DEF_SIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(c.sigma, 0., 1., GFPLOWERCLOSED, GFPUPPEROPEN, GFNOWARNING, "sigma");

    comment = "";
    comment += "The minimum value of sigma;";
    comment += "minSigma must be positive and smaller than maxSigma;";
    gpb.registerFileParameter<double>(
        "min_sigma",
        c.min_sigma,
        GII_DEF_MINSIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.min_sigma,
        0.,
        1.,
        GFPLOWERCLOSED,
        GFPUPPEROPEN,
        GFNOWARNING,
        "min_sigma"
    );

    comment = "";
    comment += "The maximum value of sigma;";
    comment += "maxSigma must be positive and larger than minSigma;";
    gpb.registerFileParameter<double>(
        "max_sigma",
        c.max_sigma,
        GII_DEF_MAXSIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.max_sigma,
        c.min_sigma,
        1.,
        GFPLOWEROPEN,
        GFPUPPEROPEN,
        GFNOWARNING,
        "max_sigma"
    );

    comment = "";
    comment += "Influences the self-adaption of gauss-mutation in ES;";
    comment += "sigmaSigma must be positive;";
    comment += "The allowed value range is [0,1];";
    gpb.registerFileParameter<double>(
        "sigma_sigma",
        c.sigma_sigma,
        GII_DEF_SIGMASIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.sigma_sigma,
        0.,
        1.,
        GFPLOWERCLOSED,
        GFPUPPERCLOSED,
        GFNOWARNING,
        "sigma_sigma"
    );

    comment = "";
    comment += "The sigma for gauss-adaption of location parameters in ES;";
    comment += "loc_sigma must be positive;";
    comment += "Recommended value range [0,1];";
    gpb.registerFileParameter<double>(
        "loc_sigma",
        c.loc_sigma,
        GII_DEF_LOC_SIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.loc_sigma,
        0.,
        1.,
        GFPLOWERCLOSED,
        GFPUPPEROPEN,
        GFNOWARNING,
        "loc_sigma"
    );

    comment = "";
    comment += "The minimum value of sigma for location parameters;";
    comment += "loc_minSigma must be positive and smaller than loc_maxSigma;";
    gpb.registerFileParameter<double>(
        "loc_min_sigma",
        c.loc_min_sigma,
        GII_DEF_LOC_MINSIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.loc_min_sigma,
        0.,
        1.,
        GFPLOWERCLOSED,
        GFPUPPEROPEN,
        GFNOWARNING,
        "loc_min_sigma"
    );

    comment = "";
    comment += "The maximum value of sigma for location parameters;";
    comment += "loc_maxSigma must be positive and larger than loc_minSigma;";
    gpb.registerFileParameter<double>(
        "loc_max_sigma",
        c.loc_max_sigma,
        GII_DEF_LOC_MAXSIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.loc_max_sigma,
        c.loc_min_sigma,
        1.,
        GFPLOWEROPEN,
        GFPUPPEROPEN,
        GFNOWARNING,
        "loc_max_sigma"
    );

    comment = "";
    comment += "Influences the self-adaption of gauss-mutation in ES for location parameters;";
    comment += "loc_sigmaSigma must be positive;";
    comment += "The allowed value range is [0,1];";
    gpb.registerFileParameter<double>(
        "loc_sigma_sigma",
        c.loc_sigma_sigma,
        GII_DEF_LOC_SIGMASIGMA,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(
        c.loc_sigma_sigma,
        0.,
        1.,
        GFPLOWERCLOSED,
        GFPUPPERCLOSED,
        GFNOWARNING,
        "loc_sigma_sigma"
    );

    comment = "";
    comment += "The initial background color (red channel);";
    comment += "Negative values mean random initialization;";
    comment += "Otherwise the allowed value range is [0.,1.];";
    gpb.registerFileParameter<double>(
        "bg_red",
        c.bg_red,
        GII_DEF_BGRED,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(c.bg_red, 0., 1., GFPLOWERCLOSED, GFPUPPEROPEN, GFNOWARNING, "bg_red");

    comment = "";
    comment += "The initial background color (green channel);";
    comment += "Negative values mean random initialization;";
    comment += "Otherwise the allowed value range is [0.,1.];";
    gpb.registerFileParameter<double>(
        "bg_green",
        c.bg_green,
        GII_DEF_BGGREEN,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(c.bg_green, 0., 1., GFPLOWERCLOSED, GFPUPPEROPEN, GFNOWARNING, "bg_green");

    comment = "";
    comment += "The initial background color (blue channel);";
    comment += "Negative values mean random initialization;";
    comment += "Otherwise the allowed value range is [0.,1.];";
    gpb.registerFileParameter<double>(
        "bg_blue",
        c.bg_blue,
        GII_DEF_BGBLUE,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    checkValueRange(c.bg_blue, 0., 1., GFPLOWERCLOSED, GFPUPPEROPEN, GFNOWARNING, "bg_blue");

    comment = "";
    comment += "The number of triangles that will constitute;";
    comment += "each candidate image;";
    comment += "Allowed value range [1,1000]";
    gpb.registerFileParameter<std::size_t>(
        "n_triangles",
        c.n_triangles,
        GII_DEF_NTRIANGLES,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
    // checkValueRange<std::size_t>(std::size_t(c.n_triangles), std::size_t(1), std::size_t(1000), GINTLOWERCLOSED, GINTUPPERCLOSED, GFNOWARNING, "n_triangles");

    comment = "";
    comment += "Whether triangles should be sorted according;";
    comment += "to their alpha channel;";
    gpb.registerFileParameter<bool>(
        "alpha_sort",
        c.alpha_sort,
        GII_DEF_ALPHASORT,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );

    comment = "";
    comment += "Whether the alpha channel shall be mutated;";
    gpb.registerFileParameter<bool>(
        "mutate_alpha_channel",
        c.mutate_alpha_channel,
        GII_DEF_MUTATE_ALPHA_CHANNEL,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );

    comment = "";
    comment += "Whether the background color shall be mutated;";
    gpb.registerFileParameter<bool>(
        "change_bg_color",
        c.change_bg_color,
        GII_DEF_CHBGCOLOR,
        Gem::Common::VAR_IS_ESSENTIAL,
        comment
    );
}

/******************************************************************************/
} // namespace Gem::Geneva

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

#ifdef GEM_TESTING

template <>
std::shared_ptr<Gem::Geneva::GImageIndividual>
TFactory_GUnitTests<Gem::Geneva::GImageIndividual>() {
    // Create an image individual factory and create the first individual
    Gem::Geneva::GImageIndividualFactory f("../../config/GImageIndividual.json");
    return f.get_as<Gem::Geneva::GImageIndividual>();
}

#endif /* GEM_TESTING */
