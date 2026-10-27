/**
 * @file GSigmaSigmaAdaptionTest.cpp
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

// Standard header files go here
#include <math.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// Boost header files go here

// Geneva header files go here
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "dietrich/GPlotDesigner.hpp"
#include "hap/GRandomT.hpp"

using namespace Gem::Common;
using namespace Gem::Dietrich; // plotting types live here now
using namespace Gem::Hap;

const std::size_t NPOINTS = 10000;
const std::size_t NBINS = 100;

const double sigmaStart = 0.025; // The start value
const double minSigma = 0.;
const double maxSigma = 1.;

std::bernoulli_distribution uniform_bool;
std::normal_distribution<double> normal_distribution;

/******************************************************************************/
/**
 * First distribution:
 * std::exp(normal_distribution(*gr_ptr, std::normal_distribution<double>::param_type(0., std::abs(sigmaSigma)))*(uniform_bool(*gr_ptr)?1.:-1.));
 */
double dist1(const std::shared_ptr<Gem::Hap::GRandomBase>& gr_ptr, const double &sigmaSigma) {
    return std::exp(
        normal_distribution(
            *gr_ptr,
            std::normal_distribution<double>::param_type(0., std::abs(sigmaSigma))
        ) *
        (uniform_bool(*gr_ptr) ? 1. : -1.)
    );
}

/**
 * Second distribution:
 * std::exp(normal_distribution(*gr_ptr, std::normal_distribution<double>::param_type(0.,std::abs(sigmaSigma))));
 */
double dist2(const std::shared_ptr<Gem::Hap::GRandomBase>& gr_ptr, const double &sigmaSigma) {
    return std::exp(normal_distribution(
        *gr_ptr,
        std::normal_distribution<double>::param_type(0., std::abs(sigmaSigma))
    ));
}

/******************************************************************************/
/**
 * This program plots a number of random distributions used to adapt sigma
 * in gauss mutation. It also shows the development of a fixed value over
 * time, as it is repeatedly mutated without selection pressure.
 */
// NOLINTNEXTLINE(readability-function-size) -- main() of a manual plotting demo: drives the same sigma-adaption random-walk loop over eight (distribution, sigmaSigma) parameter combinations and plots the results; splitting would scatter one linear demo script
int main() {
    double sigma_1_02 = sigmaStart;
    double sigma_1_04 = sigmaStart;
    double sigma_1_06 = sigmaStart;
    double sigma_1_08 = sigmaStart;
    double sigma_2_02 = sigmaStart;
    double sigma_2_04 = sigmaStart;
    double sigma_2_06 = sigmaStart;
    double sigma_2_08 = sigmaStart;

    double fact_1_02 = NAN;
    double fact_1_04 = NAN;
    double fact_1_06 = NAN;
    double fact_1_08 = NAN;
    double fact_2_02 = NAN;
    double fact_2_04 = NAN;
    double fact_2_06 = NAN;
    double fact_2_08 = NAN;

    std::shared_ptr<Gem::Hap::GRandomBase> const gr_ptr =
        std::shared_ptr<GRandomT<randomSource::QUEUE>>(
            new GRandomT<randomSource::QUEUE>()
        );

    std::string const caption_dist =
        "Different random distributions for the adaption of sigma, varying sigmaSigma";
    GPlotDesigner gpd_dist(caption_dist, 2, 4);
    gpd_dist.setCanvasDimensions(1600, 1200);

    std::string const caption_devel =
        "Development of a single sigma upon repeated calls, varying sigmaSigma";
    GPlotDesigner gpd_devel(caption_devel, 2, 4);
    gpd_devel.setCanvasDimensions(1600, 1200);

    std::shared_ptr<GHistogram1D> const dist1_02_ptr(new GHistogram1D(NBINS, 0., 3.));
    std::shared_ptr<GHistogram1D> const dist1_04_ptr(new GHistogram1D(NBINS, 0., 3.));
    std::shared_ptr<GHistogram1D> const dist1_06_ptr(new GHistogram1D(NBINS, 0., 3.));
    std::shared_ptr<GHistogram1D> const dist1_08_ptr(new GHistogram1D(NBINS, 0., 3.));

    std::shared_ptr<GHistogram1D> const dist2_02_ptr(new GHistogram1D(NBINS, 0., 3.));
    std::shared_ptr<GHistogram1D> const dist2_04_ptr(new GHistogram1D(NBINS, 0., 3.));
    std::shared_ptr<GHistogram1D> const dist2_06_ptr(new GHistogram1D(NBINS, 0., 3.));
    std::shared_ptr<GHistogram1D> const dist2_08_ptr(new GHistogram1D(NBINS, 0., 3.));

    dist1_02_ptr->setXAxisLabel("Distribution 1 (random sign)");
    dist1_02_ptr->setYAxisLabel("Number of Entries");
    dist1_02_ptr->setPlotLabel("sigmaSigma = 0.2");
    dist1_04_ptr->setXAxisLabel("Distribution 1 (random sign)");
    dist1_04_ptr->setYAxisLabel("Number of Entries");
    dist1_04_ptr->setPlotLabel("sigmaSigma = 0.4");
    dist1_06_ptr->setXAxisLabel("Distribution 1 (random sign)");
    dist1_06_ptr->setYAxisLabel("Number of Entries");
    dist1_06_ptr->setPlotLabel("sigmaSigma = 0.6");
    dist1_08_ptr->setXAxisLabel("Distribution 1 (random sign)");
    dist1_08_ptr->setYAxisLabel("Number of Entries");
    dist1_08_ptr->setPlotLabel("sigmaSigma = 0.8");

    dist2_02_ptr->setXAxisLabel("Distribution 2 (no sign)");
    dist2_02_ptr->setYAxisLabel("Number of Entries");
    dist2_02_ptr->setPlotLabel("sigmaSigma = 0.2");
    dist2_04_ptr->setXAxisLabel("Distribution 2 (no sign)");
    dist2_04_ptr->setYAxisLabel("Number of Entries");
    dist2_04_ptr->setPlotLabel("sigmaSigma = 0.4");
    dist2_06_ptr->setXAxisLabel("Distribution 2 (no sign)");
    dist2_06_ptr->setYAxisLabel("Number of Entries");
    dist2_06_ptr->setPlotLabel("sigmaSigma = 0.6");
    dist2_08_ptr->setXAxisLabel("Distribution 2 (no sign)");
    dist2_08_ptr->setYAxisLabel("Number of Entries");
    dist2_08_ptr->setPlotLabel("sigmaSigma = 0.8");

    std::shared_ptr<GGraph2D> const devel1_02_ptr(new GGraph2D());
    std::shared_ptr<GGraph2D> const devel1_04_ptr(new GGraph2D());
    std::shared_ptr<GGraph2D> const devel1_06_ptr(new GGraph2D());
    std::shared_ptr<GGraph2D> const devel1_08_ptr(new GGraph2D());

    std::shared_ptr<GGraph2D> const devel2_02_ptr(new GGraph2D());
    std::shared_ptr<GGraph2D> const devel2_04_ptr(new GGraph2D());
    std::shared_ptr<GGraph2D> const devel2_06_ptr(new GGraph2D());
    std::shared_ptr<GGraph2D> const devel2_08_ptr(new GGraph2D());

    devel1_02_ptr->setXAxisLabel("Call");
    devel1_02_ptr->setYAxisLabel("Sigma with Distribution 1 (random sign)");
    devel1_02_ptr->setPlotLabel("sigmaSigma = 0.2");
    devel1_04_ptr->setXAxisLabel("Call");
    devel1_04_ptr->setYAxisLabel("Sigma with Distribution 1 (random sign)");
    devel1_04_ptr->setPlotLabel("sigmaSigma = 0.4");
    devel1_06_ptr->setXAxisLabel("Call");
    devel1_06_ptr->setYAxisLabel("Sigma with Distribution 1 (random sign)");
    devel1_06_ptr->setPlotLabel("sigmaSigma = 0.6");
    devel1_08_ptr->setXAxisLabel("Call");
    devel1_08_ptr->setYAxisLabel("Sigma with Distribution 1 (random sign)");
    devel1_08_ptr->setPlotLabel("sigmaSigma = 0.8");

    devel2_02_ptr->setXAxisLabel("Call");
    devel2_02_ptr->setYAxisLabel("Sigma with Distribution 2 (no sign)");
    devel2_02_ptr->setPlotLabel("sigmaSigma = 0.2");
    devel2_04_ptr->setXAxisLabel("Call");
    devel2_04_ptr->setYAxisLabel("Sigma with Distribution 2 (no sign)");
    devel2_04_ptr->setPlotLabel("sigmaSigma = 0.4");
    devel2_06_ptr->setXAxisLabel("Call");
    devel2_06_ptr->setYAxisLabel("Sigma with Distribution 2 (no sign)");
    devel2_06_ptr->setPlotLabel("sigmaSigma = 0.6");
    devel2_08_ptr->setXAxisLabel("Call");
    devel2_08_ptr->setYAxisLabel("Sigma with Distribution 2 (no sign)");
    devel2_08_ptr->setPlotLabel("sigmaSigma = 0.8");

    // Fill data into plotters
    for(std::size_t p = 0; p < NPOINTS; p++) {
        // Obtain the data points
        fact_1_02 = dist1(gr_ptr, 0.2);
        fact_1_04 = dist1(gr_ptr, 0.4);
        fact_1_06 = dist1(gr_ptr, 0.6);
        fact_1_08 = dist1(gr_ptr, 0.8);

        fact_2_02 = dist2(gr_ptr, 0.2);
        fact_2_04 = dist2(gr_ptr, 0.4);
        fact_2_06 = dist2(gr_ptr, 0.6);
        fact_2_08 = dist2(gr_ptr, 0.8);

        // Fill the points into histograms
        dist1_02_ptr->add(fact_1_02);
        dist1_04_ptr->add(fact_1_04);
        dist1_06_ptr->add(fact_1_06);
        dist1_08_ptr->add(fact_1_08);

        dist2_02_ptr->add(fact_2_02);
        dist2_04_ptr->add(fact_2_04);
        dist2_06_ptr->add(fact_2_06);
        dist2_08_ptr->add(fact_2_08);

        // Update our fake sigmas
        sigma_1_02 *= fact_1_02;
        sigma_1_04 *= fact_1_04;
        sigma_1_06 *= fact_1_06;
        sigma_1_08 *= fact_1_08;

        sigma_2_02 *= fact_2_02;
        sigma_2_04 *= fact_2_04;
        sigma_2_06 *= fact_2_06;
        sigma_2_08 *= fact_2_08;

        enforceRangeConstraint(sigma_1_02, 0., 1., "GSigmaAdaptionTest / 1");
        enforceRangeConstraint(sigma_1_04, 0., 1., "GSigmaAdaptionTest / 2");
        enforceRangeConstraint(sigma_1_06, 0., 1., "GSigmaAdaptionTest / 3");
        enforceRangeConstraint(sigma_1_08, 0., 1., "GSigmaAdaptionTest / 4");

        enforceRangeConstraint(sigma_2_02, 0., 1., "GSigmaAdaptionTest / 5");
        enforceRangeConstraint(sigma_2_04, 0., 1., "GSigmaAdaptionTest / 6");
        enforceRangeConstraint(sigma_2_06, 0., 1., "GSigmaAdaptionTest / 7");
        enforceRangeConstraint(sigma_2_08, 0., 1., "GSigmaAdaptionTest / 8");

        // Record the development of a sigma over time, when no
        // selection pressure exists
        devel1_02_ptr->add(static_cast<double>(p), sigma_1_02);
        devel1_04_ptr->add(static_cast<double>(p), sigma_1_04);
        devel1_06_ptr->add(static_cast<double>(p), sigma_1_06);
        devel1_08_ptr->add(static_cast<double>(p), sigma_1_08);

        devel2_02_ptr->add(static_cast<double>(p), sigma_2_02);
        devel2_04_ptr->add(static_cast<double>(p), sigma_2_04);
        devel2_06_ptr->add(static_cast<double>(p), sigma_2_06);
        devel2_08_ptr->add(static_cast<double>(p), sigma_2_08);
    }

    // Add the plots to the plot-designers
    gpd_dist.registerPlotter(dist1_02_ptr);
    gpd_dist.registerPlotter(dist2_02_ptr);
    gpd_dist.registerPlotter(dist1_04_ptr);
    gpd_dist.registerPlotter(dist2_04_ptr);
    gpd_dist.registerPlotter(dist1_06_ptr);
    gpd_dist.registerPlotter(dist2_06_ptr);
    gpd_dist.registerPlotter(dist1_08_ptr);
    gpd_dist.registerPlotter(dist2_08_ptr);

    gpd_devel.registerPlotter(devel1_02_ptr);
    gpd_devel.registerPlotter(devel2_02_ptr);
    gpd_devel.registerPlotter(devel1_04_ptr);
    gpd_devel.registerPlotter(devel2_04_ptr);
    gpd_devel.registerPlotter(devel1_06_ptr);
    gpd_devel.registerPlotter(devel2_06_ptr);
    gpd_devel.registerPlotter(devel1_08_ptr);
    gpd_devel.registerPlotter(devel2_08_ptr);

    // Write the results out to files
    gpd_dist.writeToFile("multiplierDistributions.C");
    gpd_devel.writeToFile("sigmaDevelopment.C");
}
