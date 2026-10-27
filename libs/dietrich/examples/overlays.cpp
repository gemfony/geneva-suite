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
 * @file overlays.cpp
 * @brief Two plotters sharing one pad, and the two plot modes. A primary series of
 * noisy "measured" points (drawn as a SCATTER) is overlaid with a secondary series
 * holding the smooth "fit" (drawn as a CURVE) via overlaySeries(). Rendered through
 * the graph-capable backends (ROOT and gnuplot).
 */

#include <cmath>
#include <iostream>
#include <random>

#include "dietrich/GPlotDesigner.hpp"

using namespace Gem::Dietrich;

int main() {
    std::mt19937 rng(20260623);
    std::normal_distribution<double> noise(0., 0.3);

    //! [dietrich-overlays]
    GDataLog log("Measured vs. fitted", 1, 1);

    // Primary series: the noisy measurements, shown as individual markers.
    GPlotSpec measured(plotKind::graph_2d);
    measured.plot_mode = graphPlotMode::SCATTER;
    measured.name = "measured";
    measured.x_label = "x";
    measured.y_label = "y";
    measured.columns = {"x", "y"};
    const auto mid = log.declareSeries(measured);

    // Secondary series: the fit, OVERLAID on the primary's pad as a smooth curve.
    GPlotSpec fit(plotKind::graph_2d);
    fit.plot_mode = graphPlotMode::CURVE;
    fit.name = "fit: 2 sin(x)";
    fit.x_label = "x";
    fit.y_label = "y";
    fit.columns = {"x", "y"};
    const auto fid = log.overlaySeries(mid, fit);

    // A few scattered measurements ...
    for(int i = 0; i <= 60; ++i) {
        const double x = 0.1 * static_cast<double>(i);
        log.append(mid, x, 2. * std::sin(x) + noise(rng));
    }
    // ... and the densely-sampled underlying curve.
    for(int i = 0; i <= 300; ++i) {
        const double x = 0.02 * static_cast<double>(i);
        log.append(fid, x, 2. * std::sin(x));
    }

    log.writeToFile("overlays.C",  plotBackend::ROOT);
    log.writeToFile("overlays.gp", plotBackend::GNUPLOT);
    //! [dietrich-overlays]

    std::cout << "overlays: wrote overlays.{C,gp}\n";
    return 0;
}
