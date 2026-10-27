/**
 * @file DependentDistributionTests.cpp
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

/**
 * This directory contains a simple program that documents
 * dependent random distributions. E.g. it answers the queststion:
 * How does a distribution for a variable x_n look like, when it
 * is calculated from a formula "Const - Sum(x_1 ... x_(n-1)) .
 * It currently answers this questions for 1-4 "free" variables.
 */

// Standard header files
#include <math.h>

#include <fstream>
#include <iostream>
#include <random>
#include <vector>

// Boost header files

// Geneva header files
#include "dietrich/GPlotDesigner.hpp"
#include "hap/GRandomT.hpp"

using namespace Gem::Hap;
using namespace Gem::Common;
using namespace Gem::Dietrich; // plotting types live here now
using namespace boost;

const std::size_t NENTRIES = 100000;
const std::size_t NBINS = 100;

std::uniform_real_distribution<double> uniform_real_distribution;

// NOLINTNEXTLINE(readability-function-size) -- main() of a doc-program: builds the fixed set of dependent-distribution histograms, fills them in one sampling loop, and hands them to GPlotDesigner
int main(int argc, char **argv) {
    double x1 = NAN;
    double x2 = NAN;
    double x3 = NAN;
    double x4 = NAN;
    double sum1 = NAN;
    double sum2 = NAN;
    double sum3 = NAN;
    double sum4 = NAN;

    std::shared_ptr<GRandomT<randomSource::QUEUE>> const gr_ptr(
        new GRandomT<randomSource::QUEUE>()
    );

    std::shared_ptr<GHistogram1D> const x1_ptr(new GHistogram1D(NBINS, 0., 1.));
    std::shared_ptr<GHistogram1D> const x2_ptr(new GHistogram1D(NBINS, 0., 1.));
    std::shared_ptr<GHistogram1D> const x3_ptr(new GHistogram1D(NBINS, 0., 1.));
    std::shared_ptr<GHistogram1D> const x4_ptr(new GHistogram1D(NBINS, 0., 1.));

    std::shared_ptr<GHistogram1D> const sum1_extract_ptr(new GHistogram1D(NBINS, 0., 1.)); // 1 - x1
    std::shared_ptr<GHistogram1D> const sum2_extract_ptr(new GHistogram1D(NBINS, 0., 1.)); // 1 - (x1+x2)
    std::shared_ptr<GHistogram1D> const sum3_extract_ptr(
        new GHistogram1D(NBINS, 0., 1.)
    ); // 1 - (x1+x2+x3)
    std::shared_ptr<GHistogram1D> const sum4_extract_ptr(
        new GHistogram1D(NBINS, 0., 1.)
    ); // 1 - (x1+x2+x3+x4)

    std::shared_ptr<GHistogram1D> const sum1_all_ptr(new GHistogram1D(NBINS, 0., 1.));  // 1 - x1
    std::shared_ptr<GHistogram1D> const sum2_all_ptr(new GHistogram1D(NBINS, -1., 1.)); // 1 - (x1+x2)
    std::shared_ptr<GHistogram1D> const sum3_all_ptr(new GHistogram1D(NBINS, -2., 1.)); // 1 - (x1+x2+x3)
    std::shared_ptr<GHistogram1D> const sum4_all_ptr(
        new GHistogram1D(NBINS, -3., 1.)
    ); // 1 - (x1+x2+x3+x4)

    x1_ptr->setXAxisLabel("x1");
    x1_ptr->setYAxisLabel("Number of Entries");
    x1_ptr->setPlotLabel("x_{1}");
    x2_ptr->setXAxisLabel("x2");
    x2_ptr->setYAxisLabel("Number of Entries");
    x2_ptr->setPlotLabel("x_{2}");
    x3_ptr->setXAxisLabel("x3");
    x3_ptr->setYAxisLabel("Number of Entries");
    x3_ptr->setPlotLabel("x_{3}");
    x4_ptr->setXAxisLabel("x4");
    x4_ptr->setYAxisLabel("Number of Entries");
    x4_ptr->setPlotLabel("x_{4}");

    sum1_extract_ptr->setXAxisLabel("1-x1");
    sum1_extract_ptr->setYAxisLabel("Number of Entries");
    sum1_extract_ptr->setPlotLabel("1.-x_{1}, extract");
    sum2_extract_ptr->setXAxisLabel("1-(x1+x2)");
    sum2_extract_ptr->setYAxisLabel("Number of Entries");
    sum2_extract_ptr->setPlotLabel("1.-(x_{1}+x_{2}), extract");
    sum3_extract_ptr->setXAxisLabel("1-(x1+x2+x3)");
    sum3_extract_ptr->setYAxisLabel("Number of Entries");
    sum3_extract_ptr->setPlotLabel("1.-(x_{1}+x_{2}+x_{3}), extract");
    sum4_extract_ptr->setXAxisLabel("1-(x1+x2+x3+x4)");
    sum4_extract_ptr->setYAxisLabel("Number of Entries");
    sum4_extract_ptr->setPlotLabel("1.-(x_{1}+x_{2}+x_{3}+x_{4}), extract");

    sum1_all_ptr->setXAxisLabel("1-x1");
    sum1_all_ptr->setYAxisLabel("Number of Entries");
    sum1_all_ptr->setPlotLabel("1.-x_{1}, all");
    sum2_all_ptr->setXAxisLabel("1-(x1+x2)");
    sum2_all_ptr->setYAxisLabel("Number of Entries");
    sum2_all_ptr->setPlotLabel("1.-(x_{1}+x_{2}), all");
    sum3_all_ptr->setXAxisLabel("1-(x1+x2+x3)");
    sum3_all_ptr->setYAxisLabel("Number of Entries");
    sum3_all_ptr->setPlotLabel("1.-(x_{1}+x_{2}+x_{3}), all");
    sum4_all_ptr->setXAxisLabel("1-(x1+x2+x3+x4)");
    sum4_all_ptr->setYAxisLabel("Number of Entries");
    sum4_all_ptr->setPlotLabel("1.-(x_{1}+x_{2}+x_{3}+x_{4}), all");

    for(std::size_t i = 0; i < NENTRIES; i++) {
        x1 = uniform_real_distribution(*gr_ptr);
        x2 = uniform_real_distribution(*gr_ptr);
        x3 = uniform_real_distribution(*gr_ptr);
        x4 = uniform_real_distribution(*gr_ptr);

        sum1 = x1;
        sum2 = x1 + x2;
        sum3 = x1 + x2 + x3;
        sum4 = x1 + x2 + x3 + x4;

        x1_ptr->add(x1);
        x2_ptr->add(x2);
        x3_ptr->add(x3);
        x4_ptr->add(x4);

        sum1_extract_ptr->add(1. - sum1);
        sum2_extract_ptr->add(1. - sum2);
        sum3_extract_ptr->add(1. - sum3);
        sum4_extract_ptr->add(1. - sum4);

        sum1_all_ptr->add(1. - sum1);
        sum2_all_ptr->add(1. - sum2);
        sum3_all_ptr->add(1. - sum3);
        sum4_all_ptr->add(1. - sum4);
    }

    GPlotDesigner gpd("Dependent random number distributions", 4, 3);

    gpd.setCanvasDimensions(1600, 1200);

    gpd.registerPlotter(x1_ptr);
    gpd.registerPlotter(x2_ptr);
    gpd.registerPlotter(x3_ptr);
    gpd.registerPlotter(x4_ptr);

    gpd.registerPlotter(sum1_extract_ptr);
    gpd.registerPlotter(sum2_extract_ptr);
    gpd.registerPlotter(sum3_extract_ptr);
    gpd.registerPlotter(sum4_extract_ptr);

    gpd.registerPlotter(sum1_all_ptr);
    gpd.registerPlotter(sum2_all_ptr);
    gpd.registerPlotter(sum3_all_ptr);
    gpd.registerPlotter(sum4_all_ptr);

    gpd.writeToFile("rootPlotDependentDistributions.C");
}
