/**
 * @file GPlotDesignerTest.cpp
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

// Standard headers go here
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <tuple>

// Geneva headers go here
#include "dietrich/GPlotDesigner.hpp"

using namespace Gem::Common;
using namespace Gem::Dietrich; // the plotting types live here

namespace {

/**
 * @brief A sampled surface drawn with its colour map projected onto the floor or ceiling of the box,
 * and a short path drawn onto that map as an overlay (a line through markers, at the map's height).
 *
 * @param where Where the colour map is shown
 * @param label The plot label
 * @return The surface plotter, with the path registered as its secondary plotter
 */
std::shared_ptr<GGraph3D> projectedSurface(surfaceProjection where, const std::string &label) {
    auto surface = std::make_shared<GGraph3D>();
    surface->setPlotLabel(label);
    surface->setXAxisLabel("x");
    surface->setYAxisLabel("y");
    surface->setZAxisLabel("z");
    surface->setSurfaceProjection(where);

    constexpr std::size_t nodes = 25;
    double z_lo = std::numeric_limits<double>::max();
    double z_hi = std::numeric_limits<double>::lowest();
    for(std::size_t i = 0; i < nodes; ++i) {
        for(std::size_t j = 0; j < nodes; ++j) {
            double const x = -2. + 4. * static_cast<double>(i) / static_cast<double>(nodes - 1);
            double const y = -2. + 4. * static_cast<double>(j) / static_cast<double>(nodes - 1);
            double const z = std::cos(x) * std::sin(y) + 0.1 * (x * x + y * y);
            (*surface) & std::tuple<double, double, double>(x, y, z);
            z_lo = std::min(z_lo, z);
            z_hi = std::max(z_hi, z);
        }
    }

    // The path lies on the map: its z is the height at which the projection is drawn.
    double const plane = (where == surfaceProjection::FLOOR) ? z_lo : z_hi;
    auto path = std::make_shared<GGraph3D>();
    path->setPlotLabel("a path on the map");
    path->setDrawLines(true);
    for(std::size_t k = 0; k < 20; ++k) {
        double const t = static_cast<double>(k) / 19.;
        (*path) & std::tuple<double, double, double>(1.8 * (1. - t) * std::cos(6. * t), 1.8 * (1. - t) * std::sin(6. * t), plane);
    }
    surface->registerSecondaryPlotter(path);
    return surface;
}

} // anonymous namespace

// NOLINTNEXTLINE(readability-function-size) -- single demo main driving the ROOT/gnuplot/matplotlib backends end to end with the same graph/histogram/function data
int main(int argc, char **argv) {
    std::tuple<double, double> const minMaxX(
        -std::numbers::pi,
        std::numbers::pi
    );
    std::tuple<double, double> const minMaxY(
        -std::numbers::pi,
        std::numbers::pi
    );

    std::shared_ptr<GGraph2D> const gsin_ptr(new GGraph2D());
    gsin_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
    gsin_ptr->setPlotLabel("Sine and cosine functions, plotted through TGraph");
    gsin_ptr->setXAxisLabel("x");
    gsin_ptr->setYAxisLabel("sin(x) vs. cos(x)");

    std::shared_ptr<GGraph2D> const gcos_ptr(new GGraph2D());
    gcos_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
    gcos_ptr->setPlotLabel("A cosine function, plotted through TGraph");
    gcos_ptr->setXAxisLabel("x");
    gcos_ptr->setYAxisLabel("cos(x)");

    std::shared_ptr<GGraph2D> const gcos_ptr_2(new GGraph2D());
    gcos_ptr_2->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
    gsin_ptr->registerSecondaryPlotter(gcos_ptr_2);

    for(std::size_t i = 0; i < 1000; i++) {
        double const x = 2 * std::numbers::pi * static_cast<double>(i) / 1000. -
                   std::numbers::pi;

        (*gsin_ptr) & std::tuple<double, double>(x, sin(x));
        (*gcos_ptr) & std::tuple<double, double>(x, cos(x));
        (*gcos_ptr_2) & std::tuple<double, double>(x, cos(x));
    }

    std::shared_ptr<GFunctionPlotter1D> const gsin_plotter_1D_ptr(
        new GFunctionPlotter1D("sin(x)", minMaxX)
    );
    gsin_plotter_1D_ptr->setPlotLabel("A sine function, plotted through TF1");
    gsin_plotter_1D_ptr->setXAxisLabel("x");
    gsin_plotter_1D_ptr->setYAxisLabel("sin(x)");

    std::shared_ptr<GFunctionPlotter1D> const gcos_plotter_1D_ptr(
        new GFunctionPlotter1D("cos(x)", minMaxX)
    );
    gcos_plotter_1D_ptr->setPlotLabel("A cosine function, plotted through TF1");
    gcos_plotter_1D_ptr->setXAxisLabel("x");
    gcos_plotter_1D_ptr->setYAxisLabel("cos(x)");

    std::shared_ptr<GFunctionPlotter2D> const schwefel_plotter_2D_ptr(
        new GFunctionPlotter2D("-0.5*(x*sin(sqrt(abs(x))) + y*sin(sqrt(abs(y))))", minMaxX, minMaxY)
    );
    schwefel_plotter_2D_ptr->setPlotLabel("The Schwefel function");
    schwefel_plotter_2D_ptr->setXAxisLabel("x");
    schwefel_plotter_2D_ptr->setYAxisLabel("y");
    schwefel_plotter_2D_ptr->setYAxisLabel("Schwefel function");
    schwefel_plotter_2D_ptr->setDrawingArguments("surf1");

    std::shared_ptr<GFunctionPlotter2D> const noisyParabola_plotter_2D_ptr(
        new GFunctionPlotter2D("(cos(x^2+y^2) + 2)*(x^2+y^2)", minMaxX, minMaxY)
    );
    noisyParabola_plotter_2D_ptr->setPlotLabel("The noisy parabola");
    noisyParabola_plotter_2D_ptr->setXAxisLabel("x");
    noisyParabola_plotter_2D_ptr->setYAxisLabel("y");
    noisyParabola_plotter_2D_ptr->setYAxisLabel("Noisy parabola");
    noisyParabola_plotter_2D_ptr->setDrawingArguments("surf1");

    GPlotDesigner gpd("Sine and cosine and 2D-functions", 2, 4);

    gpd.setCanvasDimensions(1200, 1400);
    gpd.registerPlotter(gsin_ptr);
    gpd.registerPlotter(gcos_ptr);
    gpd.registerPlotter(gsin_plotter_1D_ptr);
    gpd.registerPlotter(gcos_plotter_1D_ptr);
    gpd.registerPlotter(schwefel_plotter_2D_ptr);
    gpd.registerPlotter(noisyParabola_plotter_2D_ptr);
    gpd.registerPlotter(projectedSurface(surfaceProjection::FLOOR, "a surface, its map on the floor"));
    gpd.registerPlotter(projectedSurface(surfaceProjection::CEILING, "a surface, its map on the ceiling"));

    gpd.writeToFile("result.C");

    // -------------------------------------------------------------------------
    // A graphs-only designer emitted through the gnuplot backend. The gnuplot
    // backend supports only the graph plotters (GGraph2D/2ED/3D/4D), so this uses
    // a separate designer that registers only graphs (no functions / histograms).
    std::shared_ptr<GGraph2D> const gp_sin_ptr(new GGraph2D());
    gp_sin_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
    gp_sin_ptr->setPlotLabel("sin(x), gnuplot");
    gp_sin_ptr->setXAxisLabel("x");
    gp_sin_ptr->setYAxisLabel("sin(x)");

    // A secondary plotter sharing the first pad (a second inline dataset).
    std::shared_ptr<GGraph2D> const gp_cos_ptr(new GGraph2D());
    gp_cos_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
    gp_cos_ptr->setPlotLabel("cos(x), gnuplot");
    gp_sin_ptr->registerSecondaryPlotter(gp_cos_ptr);

    std::shared_ptr<GGraph3D> const gp_helix_ptr(new GGraph3D());
    gp_helix_ptr->setPlotLabel("a helix, gnuplot");
    gp_helix_ptr->setXAxisLabel("x");
    gp_helix_ptr->setYAxisLabel("y");
    gp_helix_ptr->setZAxisLabel("z");

    for(std::size_t i = 0; i < 200; i++) {
        double const x = 2 * std::numbers::pi * static_cast<double>(i) / 200. - std::numbers::pi;
        (*gp_sin_ptr) & std::tuple<double, double>(x, std::sin(x));
        (*gp_cos_ptr) & std::tuple<double, double>(x, std::cos(x));

        double const t = 4 * std::numbers::pi * static_cast<double>(i) / 200.;
        (*gp_helix_ptr) &
            std::tuple<double, double, double>(std::cos(t), std::sin(t), t);
    }

    GPlotDesigner gpd_gnuplot("Graphs through gnuplot", 2, 2);
    gpd_gnuplot.setPlotBackend(Gem::Dietrich::plotBackend::GNUPLOT);
    gpd_gnuplot.registerPlotter(gp_sin_ptr);
    gpd_gnuplot.registerPlotter(gp_helix_ptr);
    gpd_gnuplot.registerPlotter(projectedSurface(surfaceProjection::FLOOR, "a surface, its map on the floor, gnuplot"));
    gpd_gnuplot.registerPlotter(projectedSurface(surfaceProjection::CEILING, "a surface, its map on the ceiling, gnuplot"));
    gpd_gnuplot.writeToFile("result.gp");

    // -------------------------------------------------------------------------
    // The same graphs emitted through the matplotlib backend (which supports the
    // graph plotters and -- from stage 2 -- histograms). A fresh set of plotters is
    // used because a plotter is registered into exactly one designer; the data is
    // identical to the gnuplot demo above.
    std::shared_ptr<GGraph2D> const mpl_sin_ptr(new GGraph2D());
    mpl_sin_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
    mpl_sin_ptr->setPlotLabel("sin(x), matplotlib");
    mpl_sin_ptr->setXAxisLabel("x");
    mpl_sin_ptr->setYAxisLabel("sin(x)");

    // A secondary plotter sharing the first pad (overlaid on the same axes).
    std::shared_ptr<GGraph2D> const mpl_cos_ptr(new GGraph2D());
    mpl_cos_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
    mpl_cos_ptr->setPlotLabel("cos(x), matplotlib");
    mpl_sin_ptr->registerSecondaryPlotter(mpl_cos_ptr);

    std::shared_ptr<GGraph3D> const mpl_helix_ptr(new GGraph3D());
    mpl_helix_ptr->setPlotLabel("a helix, matplotlib");
    mpl_helix_ptr->setXAxisLabel("x");
    mpl_helix_ptr->setYAxisLabel("y");
    mpl_helix_ptr->setZAxisLabel("z");

    for(std::size_t i = 0; i < 200; i++) {
        double const x = 2 * std::numbers::pi * static_cast<double>(i) / 200. - std::numbers::pi;
        (*mpl_sin_ptr) & std::tuple<double, double>(x, std::sin(x));
        (*mpl_cos_ptr) & std::tuple<double, double>(x, std::cos(x));

        double const t = 4 * std::numbers::pi * static_cast<double>(i) / 200.;
        (*mpl_helix_ptr) &
            std::tuple<double, double, double>(std::cos(t), std::sin(t), t);
    }

    // Histograms are a matplotlib strength (and a gnuplot gap): a 1-d and a 2-d
    // histogram exercise ax.hist / ax.hist2d.
    std::shared_ptr<GHistogram1D> const mpl_hist1d_ptr(new GHistogram1D(20, -4.0, 4.0));
    mpl_hist1d_ptr->setPlotLabel("a 1d histogram, matplotlib");
    mpl_hist1d_ptr->setXAxisLabel("value");
    mpl_hist1d_ptr->setYAxisLabel("count");

    std::shared_ptr<GHistogram2D> const mpl_hist2d_ptr(
        new GHistogram2D(20, 20, -4.0, 4.0, -4.0, 4.0)
    );
    mpl_hist2d_ptr->setPlotLabel("a 2d histogram, matplotlib");
    mpl_hist2d_ptr->setXAxisLabel("x");
    mpl_hist2d_ptr->setYAxisLabel("y");

    // An INTEGER histogram, rendered by the matplotlib backend (it reads the int32
    // samples via the type-tagged dataColumns() and emits an ax.hist call just like the
    // double histogram).
    std::shared_ptr<GHistogram1I> const mpl_hist1i_ptr(new GHistogram1I(11, -5.0, 6.0));
    mpl_hist1i_ptr->setPlotLabel("an integer histogram, matplotlib");
    mpl_hist1i_ptr->setXAxisLabel("value");
    mpl_hist1i_ptr->setYAxisLabel("count");

    for(std::size_t i = 0; i < 2000; i++) {
        double const x = 4. * std::sin(static_cast<double>(i));
        double const y = 4. * std::cos(static_cast<double>(i) * 1.3);
        (*mpl_hist1d_ptr) & x;
        (*mpl_hist2d_ptr) & std::tuple<double, double>(x, y);
        (*mpl_hist1i_ptr) & static_cast<std::int32_t>(std::lround(x));
    }

    GPlotDesigner gpd_mpl("Graphs and histograms through matplotlib", 2, 3);
    gpd_mpl.setPlotBackend(Gem::Dietrich::plotBackend::MATPLOTLIB);
    gpd_mpl.registerPlotter(mpl_sin_ptr);
    gpd_mpl.registerPlotter(mpl_helix_ptr);
    gpd_mpl.registerPlotter(mpl_hist1d_ptr);
    gpd_mpl.registerPlotter(mpl_hist2d_ptr);
    gpd_mpl.registerPlotter(mpl_hist1i_ptr);
    gpd_mpl.registerPlotter(projectedSurface(surfaceProjection::FLOOR, "a surface, its map on the floor, matplotlib"));
    gpd_mpl.writeToFile("result.py");

    // -------------------------------------------------------------------------
    // The same graphs-and-histograms set emitted through the Octave / MATLAB backend
    // (which supports the graph plotters and histograms, using only base Octave-and-MATLAB
    // functions). A fresh set of plotters is used because a plotter is registered into
    // exactly one designer; the data is identical to the matplotlib demo above.
    std::shared_ptr<GGraph2D> const oct_sin_ptr(new GGraph2D());
    oct_sin_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
    oct_sin_ptr->setPlotLabel("sin(x), octave");
    oct_sin_ptr->setXAxisLabel("x");
    oct_sin_ptr->setYAxisLabel("sin(x)");

    // A secondary plotter sharing the first pad (overlaid via `hold on`).
    std::shared_ptr<GGraph2D> const oct_cos_ptr(new GGraph2D());
    oct_cos_ptr->setPlotMode(Gem::Dietrich::graphPlotMode::SCATTER);
    oct_cos_ptr->setPlotLabel("cos(x), octave");
    oct_sin_ptr->registerSecondaryPlotter(oct_cos_ptr);

    std::shared_ptr<GGraph3D> const oct_helix_ptr(new GGraph3D());
    oct_helix_ptr->setPlotLabel("a helix, octave");
    oct_helix_ptr->setXAxisLabel("x");
    oct_helix_ptr->setYAxisLabel("y");
    oct_helix_ptr->setZAxisLabel("z");

    for(std::size_t i = 0; i < 200; i++) {
        double const x = 2 * std::numbers::pi * static_cast<double>(i) / 200. - std::numbers::pi;
        (*oct_sin_ptr) & std::tuple<double, double>(x, std::sin(x));
        (*oct_cos_ptr) & std::tuple<double, double>(x, std::cos(x));

        double const t = 4 * std::numbers::pi * static_cast<double>(i) / 200.;
        (*oct_helix_ptr) &
            std::tuple<double, double, double>(std::cos(t), std::sin(t), t);
    }

    std::shared_ptr<GHistogram1D> const oct_hist1d_ptr(new GHistogram1D(20, -4.0, 4.0));
    oct_hist1d_ptr->setPlotLabel("a 1d histogram, octave");
    oct_hist1d_ptr->setXAxisLabel("value");
    oct_hist1d_ptr->setYAxisLabel("count");

    std::shared_ptr<GHistogram2D> const oct_hist2d_ptr(
        new GHistogram2D(20, 20, -4.0, 4.0, -4.0, 4.0)
    );
    oct_hist2d_ptr->setPlotLabel("a 2d histogram, octave");
    oct_hist2d_ptr->setXAxisLabel("x");
    oct_hist2d_ptr->setYAxisLabel("y");

    std::shared_ptr<GHistogram1I> const oct_hist1i_ptr(new GHistogram1I(11, -5.0, 6.0));
    oct_hist1i_ptr->setPlotLabel("an integer histogram, octave");
    oct_hist1i_ptr->setXAxisLabel("value");
    oct_hist1i_ptr->setYAxisLabel("count");

    for(std::size_t i = 0; i < 2000; i++) {
        double const x = 4. * std::sin(static_cast<double>(i));
        double const y = 4. * std::cos(static_cast<double>(i) * 1.3);
        (*oct_hist1d_ptr) & x;
        (*oct_hist2d_ptr) & std::tuple<double, double>(x, y);
        (*oct_hist1i_ptr) & static_cast<std::int32_t>(std::lround(x));
    }

    GPlotDesigner gpd_oct("Graphs and histograms through Octave", 2, 3);
    gpd_oct.setPlotBackend(Gem::Dietrich::plotBackend::OCTAVE);
    gpd_oct.registerPlotter(oct_sin_ptr);
    gpd_oct.registerPlotter(oct_helix_ptr);
    gpd_oct.registerPlotter(oct_hist1d_ptr);
    gpd_oct.registerPlotter(oct_hist2d_ptr);
    gpd_oct.registerPlotter(oct_hist1i_ptr);
    gpd_oct.registerPlotter(projectedSurface(surfaceProjection::CEILING, "a surface, its map on the ceiling, Octave"));
    gpd_oct.writeToFile("result.m");

    // -------------------------------------------------------------------------
    // The DATA backend (GDataEmitter): export the raw series data (NOT a rendered plot)
    // in BOTH formats -- a human-inspectable CSV and a binary numpy .npz. A fresh set of
    // plotters is used because a plotter is registered into exactly one designer; the
    // values are deterministic so the validity harness can round-trip them exactly.
    //
    // The first series (a GGraph2D "data graph") holds simple, exactly-representable
    // values; the harness asserts that series_0's shape and a couple of values
    // round-trip bit-exactly through numpy.load().
    std::shared_ptr<GGraph2D> const data_g2d_ptr(new GGraph2D());
    data_g2d_ptr->setPlotLabel("data graph");
    data_g2d_ptr->setXAxisLabel("x");
    data_g2d_ptr->setYAxisLabel("y");
    for(std::size_t i = 0; i < 5; i++) {
        const double x = static_cast<double>(i);          // 0,1,2,3,4 -- exact in float64
        const double y = static_cast<double>(i) * 0.5;     // 0,0.5,1,1.5,2 -- exact in float64
        (*data_g2d_ptr) & std::tuple<double, double>(x, y);
    }

    std::shared_ptr<GHistogram1D> const data_hist1d_ptr(new GHistogram1D(10, 0.0, 10.0));
    data_hist1d_ptr->setPlotLabel("data histogram");
    data_hist1d_ptr->setXAxisLabel("value");
    for(std::size_t i = 0; i < 8; i++) {
        (*data_hist1d_ptr) & static_cast<double>(i);
    }

    // CSV mode -> result_data.csv (human-inspectable text).
    {
        GPlotDesigner gpd_csv("Series data (CSV)", 1, 2);
        gpd_csv.setDataFormat(Gem::Dietrich::dataFormat::CSV);
        gpd_csv.registerPlotter(data_g2d_ptr);
        gpd_csv.registerPlotter(data_hist1d_ptr);
        gpd_csv.writeToFile("result_data.csv");
    }

    // NPZ mode -> result_data.npz (binary numpy archive). Fresh plotters, identical data,
    // because each plotter belongs to a single designer.
    std::shared_ptr<GGraph2D> const npz_g2d_ptr(new GGraph2D());
    npz_g2d_ptr->setPlotLabel("data graph");
    npz_g2d_ptr->setXAxisLabel("x");
    npz_g2d_ptr->setYAxisLabel("y");
    for(std::size_t i = 0; i < 5; i++) {
        const double x = static_cast<double>(i);
        const double y = static_cast<double>(i) * 0.5;
        (*npz_g2d_ptr) & std::tuple<double, double>(x, y);
    }

    std::shared_ptr<GHistogram1D> const npz_hist1d_ptr(new GHistogram1D(10, 0.0, 10.0));
    npz_hist1d_ptr->setPlotLabel("data histogram");
    npz_hist1d_ptr->setXAxisLabel("value");
    for(std::size_t i = 0; i < 8; i++) {
        (*npz_hist1d_ptr) & static_cast<double>(i);
    }

    {
        GPlotDesigner gpd_npz("Series data (NPZ)", 1, 2);
        gpd_npz.setDataFormat(Gem::Dietrich::dataFormat::NPZ);
        gpd_npz.registerPlotter(npz_g2d_ptr);
        gpd_npz.registerPlotter(npz_hist1d_ptr);
        gpd_npz.writeToFile("result_data.npz");
    }

    // A COMPREHENSIVE data export (result_data_full.npz) covering every kind the external
    // renderer (scripts/geneva_plot_render.py) understands -- graph_2d (+ a secondary
    // overlay sharing the pad), graph_2d_err, graph_3d, graph_4d, hist_1d and hist_2d --
    // laid out on a 2x3 canvas. The render-validity ctest feeds this to the bundled Python
    // tool, exercising the (data + self-describing manifest) -> figure round-trip end to end.
    {
        auto full_g2d = std::make_shared<GGraph2D>();
        full_g2d->setPlotLabel("g2d primary");
        auto full_g2d_overlay = std::make_shared<GGraph2D>();
        full_g2d_overlay->setPlotLabel("g2d overlay");
        for(std::size_t i = 0; i < 6; i++) {
            const double x = static_cast<double>(i);
            (*full_g2d) & std::tuple<double, double>(x, x * x);
            (*full_g2d_overlay) & std::tuple<double, double>(x, 2.0 * x);
        }
        full_g2d->registerSecondaryPlotter(full_g2d_overlay); // overlay -> same pad

        auto full_g2ed = std::make_shared<GGraph2ED>();
        full_g2ed->setPlotLabel("g2ed");
        for(std::size_t i = 0; i < 6; i++) {
            const double x = static_cast<double>(i);
            (*full_g2ed) & std::tuple<double, double, double, double>(x, 0.1, x * 0.5, 0.2);
        }

        auto full_g3d = std::make_shared<GGraph3D>();
        full_g3d->setPlotLabel("g3d");
        for(std::size_t i = 0; i < 20; i++) {
            const double t = static_cast<double>(i) * 0.5;
            (*full_g3d) & std::tuple<double, double, double>(std::sin(t), std::cos(t), t);
        }

        auto full_g4d = std::make_shared<GGraph4D>();
        full_g4d->setPlotLabel("g4d");
        for(std::size_t i = 0; i < 20; i++) {
            const double t = static_cast<double>(i) * 0.5;
            (*full_g4d) & std::tuple<double, double, double, double>(std::sin(t), std::cos(t), t, t * t);
        }

        auto full_h1d = std::make_shared<GHistogram1D>(15, -4.0, 4.0);
        full_h1d->setPlotLabel("h1d");
        auto full_h2d = std::make_shared<GHistogram2D>(15, 15, -4.0, 4.0, -4.0, 4.0);
        full_h2d->setPlotLabel("h2d");
        for(std::size_t i = 0; i < 64; i++) {
            const double v = std::sin(static_cast<double>(i)) * 3.0;
            const double w = std::cos(static_cast<double>(i)) * 3.0;
            (*full_h1d) & v;
            (*full_h2d) & std::tuple<double, double>(v, w);
        }

        // An INTEGER histogram: its samples export with their TRUE int32 dtype (the .npz
        // member is a real numpy int32 array). The known values 0,0,1,1,1,2,...,9 let the
        // data-validity harness assert the int32 round-trip bit-exactly.
        auto full_h1i = std::make_shared<GHistogram1I>(10, 0, 10);
        full_h1i->setPlotLabel("h1i");
        for(std::int32_t value = 0; value < 10; value++) {
            for(std::int32_t rep = 0; rep <= value; rep++) {
                (*full_h1i) & value; // value appears (value+1) times
            }
        }

        GPlotDesigner gpd_full("Series data (render coverage)", 3, 3);
        gpd_full.setDataFormat(Gem::Dietrich::dataFormat::NPZ);
        gpd_full.registerPlotter(full_g2d);
        gpd_full.registerPlotter(full_g2ed);
        gpd_full.registerPlotter(full_g3d);
        gpd_full.registerPlotter(full_g4d);
        gpd_full.registerPlotter(full_h1d);
        gpd_full.registerPlotter(full_h2d);
        gpd_full.registerPlotter(full_h1i);
        gpd_full.writeToFile("result_data_full.npz");
    }
}
