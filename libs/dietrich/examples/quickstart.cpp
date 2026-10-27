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
 * @file quickstart.cpp
 * @brief Dietrich in five lines: the modern, plotter-object-free GDataLog API.
 *
 * Declare ONE series as a GPlotSpec, push (x, y) rows, and write the SAME log to
 * every backend. This is the "one painter, many idioms" idea in code: a single
 * data source rendered as a ROOT macro, a gnuplot script, a matplotlib script, an
 * Octave / MATLAB script and a raw CSV export.
 */

#include <cmath>
#include <iostream>
#include <numbers>

#include "dietrich/GPlotDesigner.hpp"

//! [dietrich-quickstart#1]
using namespace Gem::Dietrich;
//! [dietrich-quickstart#1]

int main() {
    //! [dietrich-quickstart#2]
    // A 1x1 canvas (one pad) titled for the figure.
    GDataLog log("Dietrich quickstart: y = sin(x)", 1, 1);

    // Declare what the plot IS (a 2-d curve), not how to build a plotter.
    GPlotSpec spec(plotKind::graph_2d);
    spec.plot_mode = graphPlotMode::CURVE;
    spec.name = "sin(x)";
    spec.x_label = "x";
    spec.y_label = "sin(x)";
    spec.columns = {"x", "y"};
    const auto id = log.declareSeries(spec);

    // Push the data, row by row.
    for(int i = 0; i <= 200; ++i) {
        const double x = -std::numbers::pi + 2. * std::numbers::pi * static_cast<double>(i) / 200.;
        log.append(id, x, std::sin(x));
    }

    // One source -> many backends. The same log renders through each emitter.
    log.writeToFile("quickstart.C",   plotBackend::ROOT);       // a ROOT macro
    log.writeToFile("quickstart.gp",  plotBackend::GNUPLOT);    // a gnuplot script
    log.writeToFile("quickstart.py",  plotBackend::MATPLOTLIB); // a matplotlib script
    log.writeToFile("quickstart.m",   plotBackend::OCTAVE);     // an Octave / MATLAB script
    log.writeToFile("quickstart.csv", plotBackend::DATA);       // the raw data (CSV + manifest)
    //! [dietrich-quickstart#2]

    std::cout << "quickstart: wrote quickstart.{C,gp,py,m,csv}\n";
    return 0;
}
