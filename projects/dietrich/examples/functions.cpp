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
 * @file functions.cpp
 * @brief Formula-driven function plotters via the lower-level plotter-object API.
 *
 * Unlike the data-oriented plotters fed through GDataLog, GFunctionPlotter1D/2D
 * carry no sampled data -- they emit a ROOT TF1 / TF2 from a formula string. This
 * demo also illustrates a deliberate backend-capability boundary: function plotters
 * are a ROOT-only feature; the gnuplot / matplotlib / Octave backends render sampled
 * DATA and reject a function plotter with a clear exception that points you to ROOT.
 */

#include <iostream>
#include <memory>
#include <numbers>
#include <tuple>

#include "dietrich/GPlotDesigner.hpp"

using namespace Gem::Dietrich;

int main() {
    //! [dietrich-functions]
    const std::tuple<double, double> rx(-std::numbers::pi, std::numbers::pi);
    const std::tuple<double, double> ry(-std::numbers::pi, std::numbers::pi);

    // A 1-d function (drawn as a ROOT TF1) ...
    auto sinc = std::make_shared<GFunctionPlotter1D>("sin(x)/x", rx);
    sinc->setPlotLabel("sinc");
    sinc->setXAxisLabel("x");
    sinc->setYAxisLabel("sin(x)/x");

    // ... and a 2-d surface (a ROOT TF2, drawn with the "surf1" style).
    auto surface = std::make_shared<GFunctionPlotter2D>("cos(x)*sin(y)", rx, ry);
    surface->setPlotLabel("cos(x) sin(y)");
    surface->setXAxisLabel("x");
    surface->setYAxisLabel("y");
    surface->setDrawingArguments("surf1");

    // The lower-level API: build a designer, register plotters, write the ROOT macro.
    GPlotDesigner gpd("Analytic functions", 1, 2);
    gpd.registerPlotter(sinc);
    gpd.registerPlotter(surface);
    gpd.writeToFile("functions.C"); // the default (ROOT) backend
    //! [dietrich-functions]

    std::cout << "functions: wrote functions.C (function plotters are a ROOT-only capability;\n"
              << "           the gnuplot / matplotlib / Octave backends would reject them)\n";
    return 0;
}
