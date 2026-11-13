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
 * @file gallery.cpp
 * @brief A 2x2 canvas exercising the data-driven plot kinds in one figure: a 2-d
 * curve, a 3-d helix, a 1-d histogram and a fixed-range 2-d histogram. The
 * histogram data is drawn from the standard library's RNG. Rendered through the
 * three histogram-capable backends (ROOT, matplotlib, Octave). (Formula-driven
 * function plotters are shown separately in functions.cpp.)
 */

#include <cmath>
#include <iostream>
#include <random>
#include <span>

#include "dietrich/GPlotDesigner.hpp"

using namespace Gem::Dietrich;

// NOLINTNEXTLINE(readability-function-size) -- single demo main assembling the four gallery pads (curve, helix, 1-d and 2-d histograms) and writing all three backend scripts
int main() {
    std::mt19937 rng(20260623);            // a fixed seed -> reproducible figures
    std::normal_distribution<double> nd(0., 1.);

    GDataLog log("Dietrich gallery", 2, 2); // four pads

    // Pad 1: a 2-d curve (a damped sine).
    GPlotSpec curve(plotKind::graph_2d);
    curve.plot_mode = graphPlotMode::CURVE;
    curve.name = "damped sine";
    curve.x_label = "x";
    curve.y_label = "exp(-x/5) sin(x)";
    curve.columns = {"x", "y"};
    const auto cid = log.declareSeries(curve);
    for(int i = 0; i <= 300; ++i) {
        const double x = 0.1 * static_cast<double>(i);
        log.append(cid, x, std::exp(-x / 5.) * std::sin(x));
    }

    // Pad 2: a 3-d helix.
    GPlotSpec helix(plotKind::graph_3d);
    helix.name = "helix";
    helix.x_label = "x";
    helix.y_label = "y";
    helix.z_label = "z";
    helix.columns = {"x", "y", "z"};
    const auto hid = log.declareSeries(helix);
    for(int i = 0; i <= 400; ++i) {
        const double t = 0.05 * static_cast<double>(i);
        log.append(hid, std::cos(t), std::sin(t), t);
    }

    // Pad 3: a 1-d histogram of N(0,1) samples (one value per row).
    GPlotSpec h1(plotKind::hist_1d);
    h1.name = "N(0,1) samples";
    h1.x_label = "value";
    h1.y_label = "count";
    h1.columns = {"value"};
    h1.n_bins_x = 40;
    h1.range_x = std::make_tuple(-4., 4.);
    const auto h1id = log.declareSeries(h1);
    for(int i = 0; i < 20000; ++i) {
        const double v = nd(rng);
        log.append(h1id, std::span<const double>(&v, 1));
    }

    // Pad 4: a fixed-range 2-d histogram of a 2-d Gaussian (drawn as boxes).
    GPlotSpec h2(plotKind::hist_2d);
    h2.name = "2-d Gaussian";
    h2.x_label = "x";
    h2.y_label = "y";
    h2.drawing_args = "BOX";
    h2.columns = {"x", "y"};
    h2.n_bins_x = 30;
    h2.n_bins_y = 30;
    h2.range_x = std::make_tuple(-4., 4.);
    h2.range_y = std::make_tuple(-4., 4.);
    const auto h2id = log.declareSeries(h2);
    for(int i = 0; i < 30000; ++i) {
        log.append(h2id, nd(rng), nd(rng));
    }

    log.writeToFile("gallery.C",  plotBackend::ROOT);
    log.writeToFile("gallery.py", plotBackend::MATPLOTLIB);
    log.writeToFile("gallery.m",  plotBackend::OCTAVE);

    std::cout << "gallery: wrote gallery.{C,py,m}\n";
    return 0;
}
