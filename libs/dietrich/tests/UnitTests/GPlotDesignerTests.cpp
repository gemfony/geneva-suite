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

/********************************************************************************
 * Regression tests for the GPlotDesigner ROOT-macro code generator. These pin the
 * fixes to the emitted macros (a review of the generator, June 2026, and later
 * defects) by asserting on the generated macro text directly -- no ROOT
 * installation required. The complementary
 * "does ROOT actually accept the macro" check is wired into ctest as
 * GPlotDesignerRootValidity (only when root + xvfb-run are available).
 ********************************************************************************/

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <unistd.h> // getpid(): a scratch directory of this process

#include "dietrich/GPlotDesigner.hpp"
#include "dietrich/plotting/detail/GPlotDetail.hpp" // emitRootLegend(), tested directly

#include "common/testing/GStandardTests.hpp"

using namespace Gem::Common;
using namespace Gem::Dietrich; // the plotting types under test live here

namespace {
/**
 * The ROOT macro of one pad that holds @p p (and its overlays).
 * @param p The pad's primary plotter
 * @return The macro
 */
std::string rootMacroOf(const std::shared_ptr<GBasePlotter> &p) {
    GPlotDesigner gpd("one pad", 1, 1);
    gpd.registerPlotter(p);
    return gpd.plot();
}
} // namespace

/******************************************************************************/
// BUG-1: user strings must be escaped before they go into a ROOT C-string literal.
TEST_CASE("GPlotDesigner escapes user strings in emitted ROOT literals", "[plotting]") {
    auto g = std::make_shared<GGraph2D>();
    *g &std::tuple<double, double>(1.0, 2.0);
    g->setXAxisLabel(std::string("a\"b"));        // an embedded double quote
    g->setPlotLabel(std::string("line1\nline2")); // an embedded newline

    const std::string s = rootMacroOf(g);

    // The quote is backslash-escaped (a\"b), never a bare a"b that closes the literal early.
    CHECK(s.contains("a\\\"b"));
    // The newline is emitted as the two characters backslash-n, not a raw line break.
    CHECK(s.contains("line1\\nline2"));
    CHECK(!s.contains("line1\nline2"));
}

/******************************************************************************/
// BUG-3: coordinates are emitted at full (round-trippable) precision, not the
// default 6 significant digits, and locale-independently.
TEST_CASE("GPlotDesigner emits full-precision coordinates", "[plotting]") {
    auto g = std::make_shared<GGraph2D>();
    *g &std::tuple<double, double>(1.0 / 3.0, 2.0 / 3.0);

    const std::string s = rootMacroOf(g);

    // 6-significant-digit default would render 0.333333; full precision keeps many more digits.
    CHECK(s.contains("3333333333"));
    // Locale-independent: the decimal separator is a dot, never a comma.
    CHECK(!s.contains("0,33333"));
}

/******************************************************************************/
// BUG-2: the GGraph3D poly-line must receive every data point, including the first
// (the pre-fix loop started at begin()+1, dropping point 0 and leaving an origin vertex).
TEST_CASE("GGraph3D poly-line emits every point including the first", "[plotting]") {
    auto g = std::make_shared<GGraph3D>();
    g->setDrawLines(true);
    *g &std::tuple<double, double, double>(1., 1., 1.);
    *g &std::tuple<double, double, double>(2., 2., 2.);
    *g &std::tuple<double, double, double>(3., 3., 3.);

    const std::string s = rootMacroOf(g);

    const std::string::size_type poly = s.find("TPolyLine3D");
    REQUIRE(poly != std::string::npos);

    // Count the poly-line's SetPoint() calls (those after the TPolyLine3D declaration).
    std::size_t n = 0;
    for(std::string::size_type pos = s.find("->SetPoint(", poly); pos != std::string::npos;
        pos = s.find("->SetPoint(", pos + 1)) {
        ++n;
    }
    CHECK(n == 3); // all three points (pre-fix: only two)
    // Index 0 is the FIRST data point (1,1,1), not the second.
    CHECK(s.find("->SetPoint(0, 1, 1, 1)", poly) != std::string::npos);
}

/******************************************************************************/
// A TGraph2D owns no axes: GetXaxis() reaches into an internal histogram that ROOT REBUILDS when
// the graph is painted with a surface / colour-map option, which discards any title set
// beforehand. The axis titles must therefore be emitted after the draw command AND after the pad
// update that forces the painting (pre-fix they came before the draw, and every SURF*/COLZ plot --
// e.g. a scanned fitness landscape -- came out with unlabelled axes).
TEST_CASE("GGraph3D sets its axis titles after the draw command", "[plotting]") {
    auto g = std::make_shared<GGraph3D>();
    g->setXAxisLabel(std::string("x_axis"));
    g->setYAxisLabel(std::string("y_axis"));
    g->setZAxisLabel(std::string("z_axis"));
    g->setDrawingArguments(std::string("SURF2Z"));
    *g &std::tuple<double, double, double>(1., 1., 1.);

    const std::string s = rootMacroOf(g);

    const std::string::size_type draw = s.find("->Draw(\"SURF2Z\")");
    REQUIRE(draw != std::string::npos);

    // The pad update between them is what forces the painting the titles must survive.
    const std::string::size_type update = s.find("gPad->Update();");
    REQUIRE(update != std::string::npos);
    CHECK(update > draw);

    for(const auto *axis : {"x_axis", "y_axis", "z_axis"}) {
        const std::string::size_type title = s.find(axis);
        REQUIRE(title != std::string::npos);
        CHECK(title > update); // pre-fix: the title was set before the draw and was discarded
    }
}

/******************************************************************************/
// BUG-5/6: function plotters emit a QUOTED draw option, and the 2D plotter honours
// its configured drawing arguments (pre-fix it emitted an unquoted, and for 2D empty, option).
TEST_CASE("function plotters emit a quoted and honoured draw option", "[plotting]") {
    const std::tuple<double, double> rx(-1., 1.);
    const std::tuple<double, double> ry(-1., 1.);

    auto f2 = std::make_shared<GFunctionPlotter2D>("x*y", rx, ry);
    f2->setDrawingArguments("surf1");
    CHECK(rootMacroOf(f2).contains("->Draw(\"surf1\")"));

    auto f1 = std::make_shared<GFunctionPlotter1D>("sin(x)", rx);
    f1->setDrawingArguments("L");
    CHECK(rootMacroOf(f1).contains("->Draw(\"L\")"));
}

/******************************************************************************/
// BUG-4: an auto-ranged histogram with no data must throw (not dereference end()).
TEST_CASE("auto-ranged histogram with no data throws instead of UB", "[plotting]") {
    // 10 bins, no data added -> default min==max -> auto-range path
    CHECK_THROWS(rootMacroOf(std::make_shared<GHistogram1D>(10)));
}

/******************************************************************************/
// The generic project<I>() implementation (which replaced nine copy-pasted
// per-arity/per-axis specializations) must produce correctly labelled, fully
// filled projections for every axis, on both the explicit-range and the
// auto-range (boundaries derived via getMinMax) path.
TEST_CASE("project<I> yields labelled 1-d projections for every axis", "[plotting]") {
    GGraph4D g;
    g.setPlotLabel("traj");
    g.setXAxisLabel("a");
    g.setYAxisLabel("b");
    g.setZAxisLabel("c");
    g.add(1., 10., 100., 1000.);
    g.add(2., 20., 200., 2000.);
    g.add(3., 30., 300., 3000.);

    auto px = g.projectX(10, {0., 4.});
    auto py = g.projectY(10, {}); // default range -> auto-ranged from the data
    auto pz = g.projectZ(10, {});
    auto pw = g.projectW(10, {});

    CHECK(px->currentSize() == 3);
    CHECK(pw->currentSize() == 3);
    CHECK(px->xAxisLabel() == "a");
    CHECK(py->xAxisLabel() == "b");
    CHECK(pz->xAxisLabel() == "c");
    CHECK(pw->xAxisLabel() == "w"); // no fourth GBasePlotter axis label
    CHECK(px->yAxisLabel() == "Number of entries");
    CHECK(px->plotLabel() == "traj / x-projection");
    CHECK(pw->plotLabel() == "traj / w-projection");

    // The two-axis collectors (histogram header) share the same implementation
    GGraph2D g2;
    g2.setPlotLabel("xy");
    g2.setYAxisLabel("y");
    g2.add(1., 5.);
    g2.add(2., 6.);
    auto p2y = g2.projectY(4, {});
    CHECK(p2y->currentSize() == 2);
    CHECK(p2y->xAxisLabel() == "y");
    CHECK(p2y->plotLabel() == "xy / y-projection");
}

/******************************************************************************/
// The gnuplot backend emits a valid gnuplot script for graph plotters.
TEST_CASE("gnuplot backend emits a multiplot script for a GGraph2D", "[plotting]") {
    auto g = std::make_shared<GGraph2D>();
    (*g) & std::tuple<double, double>(1.0, 2.0);
    (*g) & std::tuple<double, double>(3.0, 4.0);
    g->setPlotLabel(std::string("a\"b")); // an embedded double quote (must be escaped)
    g->setXAxisLabel(std::string("the x axis"));

    GPlotDesigner gpd("gnuplot graphs", 1, 1);
    gpd.setPlotBackend(plotBackend::GNUPLOT);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();

    // The multiplot grid is present; the data is a named datablock (NOT an inline '-' inside multiplot,
    // which gnuplot cannot read) referenced by a 2-d plot command.
    CHECK(s.contains("set multiplot"));
    CHECK(s.contains("$D0 << EOD"));
    CHECK(s.contains("plot $D0 "));
    CHECK(!s.contains("plot '-'"));
    CHECK(s.contains("unset multiplot"));
    // The data rows are present (in the datablock).
    CHECK(s.contains("1 2"));
    CHECK(s.contains("3 4"));
    // Labels are gnuplot-escaped (a\"b), never a bare a"b that closes the literal early.
    CHECK(s.contains("a\\\"b"));
    CHECK(s.contains("the x axis"));
}

/******************************************************************************/
// A copy, and an assigned designer, emit through the backend of their source. (A defect: the copy
// constructor left the backend out, so a copy of a gnuplot designer emitted ROOT, and an assigned
// designer did the same once the assignment went through the copy constructor.)
TEST_CASE("a copied or assigned designer keeps its source's backend", "[plotting][copy]") {
    auto g = std::make_shared<GGraph2D>();
    (*g) & std::tuple<double, double>(1.0, 2.0);

    GPlotDesigner gpd("gnuplot copy", 1, 1);
    gpd.setPlotBackend(plotBackend::GNUPLOT);
    gpd.registerPlotter(g);
    const std::string original = gpd.plot();
    REQUIRE(original.contains("set multiplot"));

    const GPlotDesigner copied(gpd);
    CHECK(copied.plot() == original);

    GPlotDesigner assigned("assigned", 1, 1);
    assigned = gpd;
    CHECK(assigned.plot() == original);
}

/******************************************************************************/
// The gnuplot backend uses splot for a GGraph3D.
TEST_CASE("gnuplot backend uses splot for a GGraph3D", "[plotting]") {
    auto g = std::make_shared<GGraph3D>();
    (*g) & std::tuple<double, double, double>(1.0, 2.0, 3.0);

    GPlotDesigner gpd("gnuplot 3d", 1, 1);
    gpd.setPlotBackend(plotBackend::GNUPLOT);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();
    CHECK(s.contains("$D0 << EOD"));
    CHECK(s.contains("splot $D0 "));
    CHECK(s.contains("1 2 3"));
}

/******************************************************************************/
// The gnuplot backend rejects non-graph plotters (histograms, functions).
TEST_CASE("gnuplot backend throws for non-graph plotters", "[plotting]") {
    {
        auto h = std::make_shared<GHistogram1D>(10, 0.0, 1.0);
        (*h) & 0.5;
        GPlotDesigner gpd("gnuplot hist", 1, 1);
        gpd.setPlotBackend(plotBackend::GNUPLOT);
        gpd.registerPlotter(h);
        CHECK_THROWS(gpd.plot());
    }
    {
        const std::tuple<double, double> rx(-1., 1.);
        auto f = std::make_shared<GFunctionPlotter1D>("sin(x)", rx);
        GPlotDesigner gpd("gnuplot func", 1, 1);
        gpd.setPlotBackend(plotBackend::GNUPLOT);
        gpd.registerPlotter(f);
        CHECK_THROWS(gpd.plot());
    }
}

/******************************************************************************/
// The matplotlib backend emits a headless, Agg-selected Python script for a GGraph2D,
// with escaped labels and an axes plot call.
TEST_CASE("matplotlib backend emits a headless plot script for a GGraph2D", "[plotting]") {
    auto g = std::make_shared<GGraph2D>();
    (*g) & std::tuple<double, double>(1.0, 2.0);
    (*g) & std::tuple<double, double>(3.0, 4.0);
    g->setPlotLabel(std::string("a\"b")); // an embedded double quote (must be escaped)
    g->setXAxisLabel(std::string("the x axis"));

    GPlotDesigner gpd("matplotlib graphs", 1, 1);
    gpd.setPlotBackend(plotBackend::MATPLOTLIB);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();

    // The script selects the headless Agg backend BEFORE importing pyplot, builds a figure with a
    // subplot, and plots into the axes -- but never calls savefig (terminal-agnostic).
    CHECK(s.contains("matplotlib.use(\"Agg\")"));
    CHECK(s.contains("add_subplot("));
    CHECK(s.contains(".plot("));
    CHECK(!s.contains("savefig"));
    // The data values are present in the emitted list literals.
    CHECK(s.contains("1"));
    // Labels are python-escaped (a\"b), never a bare a"b that closes the literal early.
    CHECK(s.contains("a\\\"b"));
    CHECK(s.contains("the x axis"));
}

/******************************************************************************/
// The matplotlib backend uses a 3-d projection for a GGraph3D.
TEST_CASE("matplotlib backend uses a 3d projection for a GGraph3D", "[plotting]") {
    auto g = std::make_shared<GGraph3D>();
    (*g) & std::tuple<double, double, double>(1.0, 2.0, 3.0);

    GPlotDesigner gpd("matplotlib 3d", 1, 1);
    gpd.setPlotBackend(plotBackend::MATPLOTLIB);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();
    CHECK(s.contains("projection=\"3d\""));
    CHECK(s.contains("set_zlabel("));
}

/******************************************************************************/
// The matplotlib backend emits ax.hist / ax.hist2d for the histogram plotters.
TEST_CASE("matplotlib backend emits hist calls for histograms", "[plotting]") {
    {
        auto h = std::make_shared<GHistogram1D>(7, 0.0, 1.0);
        (*h) & 0.25;
        (*h) & 0.75;
        GPlotDesigner gpd("matplotlib hist1d", 1, 1);
        gpd.setPlotBackend(plotBackend::MATPLOTLIB);
        gpd.registerPlotter(h);

        const std::string s = gpd.plot();
        // A 1-d histogram is a flat (non-3d) Axes with an ax.hist(...) call honouring the bin count.
        CHECK(s.contains(".hist("));
        CHECK(s.contains("bins=7"));
        CHECK(!s.contains("projection=\"3d\""));
    }
    {
        auto h = std::make_shared<GHistogram2D>(5, 3, 0.0, 1.0, 0.0, 1.0);
        (*h) & std::tuple<double, double>(0.25, 0.5);
        GPlotDesigner gpd("matplotlib hist2d", 1, 1);
        gpd.setPlotBackend(plotBackend::MATPLOTLIB);
        gpd.registerPlotter(h);

        const std::string s = gpd.plot();
        // A 2-d histogram uses ax.hist2d(...) with the per-axis bin counts and a colourbar.
        CHECK(s.contains(".hist2d("));
        CHECK(s.contains("bins=[5, 3]"));
        CHECK(s.contains("colorbar("));
    }
}

/******************************************************************************/
// The matplotlib backend still rejects function plotters (deferred to a later pass).
TEST_CASE("matplotlib backend throws for function plotters", "[plotting]") {
    const std::tuple<double, double> rx(-1., 1.);
    auto f = std::make_shared<GFunctionPlotter1D>("sin(x)", rx);
    GPlotDesigner gpd("matplotlib func", 1, 1);
    gpd.setPlotBackend(plotBackend::MATPLOTLIB);
    gpd.registerPlotter(f);
    CHECK_THROWS(gpd.plot());
}

/******************************************************************************/
// The Octave backend emits a base-function .m script for a GGraph2D, with a figure, a
// subplot, a plot() call, a doubled-apostrophe label and no trailing print (terminal-agnostic).
TEST_CASE("octave backend emits a .m script for a GGraph2D", "[plotting]") {
    auto g = std::make_shared<GGraph2D>();
    (*g) & std::tuple<double, double>(1.0, 2.0);
    (*g) & std::tuple<double, double>(3.0, 4.0);
    g->setPlotLabel(std::string("a'b")); // an embedded apostrophe (must be doubled)
    g->setXAxisLabel(std::string("the x axis"));

    GPlotDesigner gpd("octave graphs", 1, 1);
    gpd.setPlotBackend(plotBackend::OCTAVE);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();

    // A figure with a subplot and a plot() call, but never a print/saveas (terminal-agnostic).
    CHECK(s.contains("figure();"));
    CHECK(s.contains("subplot("));
    CHECK(s.contains("plot("));
    CHECK(!s.contains("print("));
    CHECK(!s.contains("saveas("));
    // The x values land in an inline row-vector literal.
    CHECK(s.contains("[1, 3]"));
    // Labels are octave-escaped (a''b), never a bare a'b that would close the literal early.
    CHECK(s.contains("a''b"));
    CHECK(s.contains("the x axis"));
    // The emitter advertises the .m extension.
    CHECK(OctaveEmitter{}.fileExtension() == std::string(".m"));
}

/******************************************************************************/
// The Octave backend uses plot3 + zlabel for a GGraph3D.
TEST_CASE("octave backend uses plot3 for a GGraph3D", "[plotting]") {
    auto g = std::make_shared<GGraph3D>();
    (*g) & std::tuple<double, double, double>(1.0, 2.0, 3.0);

    GPlotDesigner gpd("octave 3d", 1, 1);
    gpd.setPlotBackend(plotBackend::OCTAVE);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();
    CHECK(s.contains("plot3("));
    CHECK(s.contains("zlabel("));
}

/******************************************************************************/
// The Octave backend emits hist() for a 1-d histogram and a base accumarray-based 2-d
// histogram (no hist3 toolbox dependency) for a 2-d histogram.
TEST_CASE("octave backend emits histogram calls for histograms", "[plotting]") {
    {
        auto h = std::make_shared<GHistogram1D>(7, 0.0, 1.0);
        (*h) & 0.25;
        (*h) & 0.75;
        GPlotDesigner gpd("octave hist1d", 1, 1);
        gpd.setPlotBackend(plotBackend::OCTAVE);
        gpd.registerPlotter(h);

        const std::string s = gpd.plot();
        // A 1-d histogram uses hist(samples, nbins) honouring the bin count.
        CHECK(s.contains("hist("));
        CHECK(s.contains(", 7);"));
    }
    {
        auto h = std::make_shared<GHistogram2D>(5, 3, 0.0, 1.0, 0.0, 1.0);
        (*h) & std::tuple<double, double>(0.25, 0.5);
        GPlotDesigner gpd("octave hist2d", 1, 1);
        gpd.setPlotBackend(plotBackend::OCTAVE);
        gpd.registerPlotter(h);

        const std::string s = gpd.plot();
        // A 2-d histogram is binned with histc + accumarray (base only, no hist3) and drawn
        // with imagesc + a colourbar; the per-axis bin counts come from the spec.
        CHECK(s.contains("accumarray("));
        CHECK(s.contains("imagesc("));
        CHECK(s.contains("colorbar;"));
        CHECK(s.contains("_nbx = 5; _nby = 3;"));
    }
}

/******************************************************************************/
// The Octave backend rejects function plotters (deferred to a later pass, like matplotlib).
TEST_CASE("octave backend throws for function plotters", "[plotting]") {
    const std::tuple<double, double> rx(-1., 1.);
    auto f = std::make_shared<GFunctionPlotter1D>("sin(x)", rx);
    GPlotDesigner gpd("octave func", 1, 1);
    gpd.setPlotBackend(plotBackend::OCTAVE);
    gpd.registerPlotter(f);
    CHECK_THROWS(gpd.plot());
}

/******************************************************************************/
// The DATA backend in CSV mode exports the raw series data: a `# series` header comment,
// a column-name row and the data rows for a GGraph2D (NOT a rendered plot).
TEST_CASE("data backend (CSV) emits the raw series data for a GGraph2D", "[plotting]") {
    auto g = std::make_shared<GGraph2D>();
    (*g) & std::tuple<double, double>(1.0, 2.0);
    (*g) & std::tuple<double, double>(3.0, 4.0);
    g->setPlotLabel(std::string("my series"));

    GPlotDesigner gpd("data csv", 1, 1);
    gpd.setDataFormat(dataFormat::CSV);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();

    // A leading canvas comment records the title and the pad grid for an external renderer.
    CHECK(s.contains("# canvas: \"data csv\" c_x_div=1 c_y_div=1"));
    // The section header comment names the series, its class name, canonical plot kind,
    // (spec) role, columns and its pad / overlay placement.
    CHECK(
        s.find(
            "# series 0: \"my series\" kind=GGraph2D plotkind=graph_2d role=xy "
            "columns=x,y pad=0 secondary=0"
        ) != std::string::npos
    );
    // The column-name header row.
    CHECK(s.contains("x,y"));
    // The data rows, full precision, dot decimal separator.
    CHECK(s.contains("1,2"));
    CHECK(s.contains("3,4"));
    // No rendering / script content leaks into the data export.
    CHECK(!s.contains("import"));
    CHECK(!s.contains("TCanvas"));
}

/******************************************************************************/
// GPlotSpec: a GGraph2D reports kind==graph_2d with columns {"x","y"}, and its
// toJson() carries the kind string and the (escaped) label.
TEST_CASE("GGraph2D reports a graph_2d GPlotSpec", "[plotting]") {
    GGraph2D g;
    g.setPlotLabel(std::string("a\"b")); // an embedded double quote (must be escaped in JSON)
    g.setXAxisLabel(std::string("the x axis"));

    const GPlotSpec spec = g.plotSpec();
    CHECK(spec.kind == plotKind::graph_2d);
    CHECK(spec.role == std::string("xy"));
    CHECK(spec.columns == std::vector<std::string>({"x", "y"}));
    CHECK(spec.name == std::string("a\"b"));
    CHECK(spec.x_label == std::string("the x axis"));
    CHECK_FALSE(spec.n_bins_x.has_value());

    const std::string j = spec.toJson();
    // The kind string is present.
    CHECK(j.contains("graph_2d"));
    // The embedded quote is JSON-escaped (a\"b), never a bare a"b closing the string early.
    CHECK(j.contains("a\\\"b"));
    // The column names appear in the JSON.
    CHECK(j.contains("\"x\""));
    CHECK(j.contains("\"y\""));
}

/******************************************************************************/
// GPlotSpec: a GHistogram1D reports kind==hist_1d and carries its bin count in n_bins_x.
TEST_CASE("GHistogram1D reports a hist_1d GPlotSpec with n_bins_x", "[plotting]") {
    GHistogram1D const h(7, 0.0, 1.0);

    const GPlotSpec spec = h.plotSpec();
    CHECK(spec.kind == plotKind::hist_1d);
    CHECK(spec.role == std::string("distribution"));
    REQUIRE(spec.n_bins_x.has_value());
    CHECK(*spec.n_bins_x == 7);

    const std::string j = spec.toJson();
    CHECK(j.contains("hist_1d"));
    CHECK(j.contains("\"n_bins_x\": 7"));
}

/******************************************************************************/
// The DATA backend in NPZ mode produces a non-empty binary numpy .npz archive: the bytes
// begin with the ZIP local-file-header signature "PK\x03\x04".
TEST_CASE("data backend (NPZ) emits a non-empty ZIP/.npz archive", "[plotting]") {
    auto g = std::make_shared<GGraph2D>();
    (*g) & std::tuple<double, double>(1.0, 2.0);
    (*g) & std::tuple<double, double>(3.0, 4.0);

    GPlotDesigner gpd("data npz", 1, 1);
    gpd.setDataFormat(dataFormat::NPZ);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();

    REQUIRE(s.size() > 4);
    // The .npz is an uncompressed ZIP: it begins with the local file header signature.
    CHECK(s.compare(0, 4, std::string("PK\x03\x04", 4)) == 0);
    // It is a real archive: an end-of-central-directory record ("PK\x05\x06") is present.
    CHECK(s.contains(std::string("PK\x05\x06", 4)));
    // The .npy member magic and the manifest member name are embedded.
    CHECK(s.contains(std::string("\x93NUMPY", 6)));
    CHECK(s.contains("manifest.json"));
}

/******************************************************************************/
// dataColumns(): a plotter reports type-tagged (float64 / int32) views of its columns in
// storage order; a dataless plotter reports an empty list.
TEST_CASE("dataColumns reports columns generically in storage order", "[plotting]") {
    // GGraph2ED stores four float64 columns (x, ex, y, ey) -- the tricky reorder case.
    GGraph2ED g;
    g &std::tuple<double, double, double, double>(1.0, 0.1, 2.0, 0.2);
    g &std::tuple<double, double, double, double>(3.0, 0.3, 4.0, 0.4);

    const auto cols = g.dataColumns();
    REQUIRE(cols.size() == 4); // matches plotSpec().columns {"x","ex","y","ey"}
    CHECK(g.plotSpec().columns.size() == cols.size());
    // Each column is the float64 alternative of the type-tagged variant.
    const auto *x = std::get<const std::vector<double> *>(cols[0]);
    REQUIRE(x->size() == 2);
    CHECK((*x)[0] == 1.0);                                              // x
    CHECK((*std::get<const std::vector<double> *>(cols[1]))[0] == 0.1); // ex
    CHECK((*std::get<const std::vector<double> *>(cols[2]))[0] == 2.0); // y
    CHECK((*std::get<const std::vector<double> *>(cols[3]))[1] == 0.4); // ey

    // The integer histogram now exports its samples as a TRUE int32 column.
    GHistogram1I hi(5, 0.0, 10.0);
    hi &std::int32_t(2);
    hi &std::int32_t(7);
    const auto icols = hi.dataColumns();
    REQUIRE(icols.size() == 1);
    const auto *iv = std::get<const std::vector<std::int32_t> *>(icols[0]); // int32 alternative
    REQUIRE(iv->size() == 2);
    CHECK((*iv)[0] == 2);
    CHECK((*iv)[1] == 7);

    // A function plotter holds no sample columns at all.
    GFunctionPlotter1D const fp(std::string("x^2"), std::tuple<double, double>(-1.0, 1.0));
    CHECK(fp.dataColumns().empty());
}

/******************************************************************************/
// makePlotter(): the inverse of plotSpec(). A plotter -> spec -> makePlotter round-trip
// reproduces the same plotSpec() (kind, role, labels, columns, bins).
TEST_CASE("makePlotter reconstructs a plotter from its GPlotSpec", "[plotting]") {
    SECTION("graph round-trips kind/labels/columns") {
        GGraph2ED orig;
        orig.setPlotLabel(std::string("err series"));
        orig.setXAxisLabel(std::string("xx"));
        orig.setYAxisLabel(std::string("yy"));
        orig.setDrawingArguments(std::string("AP"));

        const GPlotSpec spec = orig.plotSpec();
        const auto rebuilt = makePlotter(spec);
        REQUIRE(rebuilt != nullptr);

        const GPlotSpec rspec = rebuilt->plotSpec();
        CHECK(rspec.kind == plotKind::graph_2d_err);
        CHECK(rspec.role == spec.role);
        CHECK(rspec.name == std::string("err series"));
        CHECK(rspec.x_label == std::string("xx"));
        CHECK(rspec.y_label == std::string("yy"));
        CHECK(rspec.drawing_args == std::string("AP"));
        CHECK(rspec.columns == spec.columns);
        // The rebuilt plotter is empty (no data carried by the spec).
        CHECK(std::get<const std::vector<double> *>(rebuilt->dataColumns()[0])->empty());
    }

    SECTION("auto-ranged histogram round-trips its bin counts (no range)") {
        GHistogram2D const orig(6, 8);
        const GPlotSpec spec = orig.plotSpec();
        CHECK_FALSE(spec.range_x.has_value()); // auto-ranged -> no fixed range carried
        CHECK_FALSE(spec.range_y.has_value());
        const auto rebuilt = makePlotter(spec);

        const GPlotSpec rspec = rebuilt->plotSpec();
        CHECK(rspec.kind == plotKind::hist_2d);
        REQUIRE(rspec.n_bins_x.has_value());
        REQUIRE(rspec.n_bins_y.has_value());
        CHECK(*rspec.n_bins_x == 6);
        CHECK(*rspec.n_bins_y == 8);
    }

    SECTION("fixed-range 1-d histogram round-trips its bins AND its range") {
        GHistogram1D const orig(7, -2.0, 3.0);
        const GPlotSpec spec = orig.plotSpec();
        REQUIRE(spec.range_x.has_value());
        CHECK(std::get<0>(*spec.range_x) == -2.0);
        CHECK(std::get<1>(*spec.range_x) == 3.0);

        const auto rebuilt = makePlotter(spec);
        const auto *h = dynamic_cast<const GHistogram1D *>(rebuilt.get());
        REQUIRE(h != nullptr);
        CHECK(h->getNBinsX() == 7);
        CHECK(h->getMinX() == -2.0);
        CHECK(h->getMaxX() == 3.0);
    }

    SECTION("fixed-range 2-d histogram round-trips both axis ranges") {
        GHistogram2D const orig(5, 9, -4.0, 4.0, 0.0, 10.0);
        const GPlotSpec spec = orig.plotSpec();
        REQUIRE(spec.range_x.has_value());
        REQUIRE(spec.range_y.has_value());

        const auto rebuilt = makePlotter(spec);
        const auto *h = dynamic_cast<const GHistogram2D *>(rebuilt.get());
        REQUIRE(h != nullptr);
        CHECK(h->getNBinsX() == 5);
        CHECK(h->getNBinsY() == 9);
        CHECK(h->getMinX() == -4.0);
        CHECK(h->getMaxX() == 4.0);
        CHECK(h->getMinY() == 0.0);
        CHECK(h->getMaxY() == 10.0);
    }

    SECTION("a fixed-range integer histogram now round-trips through its spec") {
        GHistogram1I const orig(5, 0.0, 10.0);
        const GPlotSpec spec = orig.plotSpec();
        REQUIRE(spec.range_x.has_value());

        const auto rebuilt = makePlotter(spec);
        const auto *h = dynamic_cast<const GHistogram1I *>(rebuilt.get());
        REQUIRE(h != nullptr);
        CHECK(h->getNBinsX() == 5);
        CHECK(h->getMinX() == 0.0);
        CHECK(h->getMaxX() == 10.0);
    }

    SECTION("a function without formula and a range-less integer histogram cannot be rebuilt") {
        GPlotSpec const fspec(plotKind::function_1d);
        CHECK_THROWS(makePlotter(fspec));
        // hist_1i with no range still cannot be reconstructed (no auto-range form).
        GPlotSpec const ispec(plotKind::hist_1i);
        CHECK_THROWS(makePlotter(ispec));
    }
}

/******************************************************************************/
// A function and a 4-d graph report in their GPlotSpec what ROOT draws them from -- the formula,
// its range and sampling; the markers' sizes, order and number -- and makePlotter() rebuilds them
// from it.
TEST_CASE("functions and 4-d graphs report their drawing in the GPlotSpec", "[plotting]") {
    SECTION("a 1-d function") {
        GFunctionPlotter1D f("sin(x)", std::make_tuple(-2., 3.));
        f.setNSamplesX(42);
        const GPlotSpec spec = f.plotSpec();
        CHECK(spec.function == "sin(x)");
        REQUIRE(spec.range_x.has_value());
        CHECK(*spec.range_x == std::make_tuple(-2., 3.));
        CHECK(spec.n_bins_x == 42);
        CHECK(spec.toJson().contains("\"function\": \"sin(x)\""));
        const GPlotSpec rebuilt = makePlotter(spec)->plotSpec();
        CHECK(rebuilt.toJson() == spec.toJson());
    }
    SECTION("a 2-d function") {
        GFunctionPlotter2D f("x*y", std::make_tuple(-1., 1.), std::make_tuple(0., 2.));
        f.setNSamplesX(20);
        f.setNSamplesY(30);
        const GPlotSpec spec = f.plotSpec();
        CHECK(spec.function == "x*y");
        REQUIRE(spec.range_y.has_value());
        CHECK(*spec.range_y == std::make_tuple(0., 2.));
        CHECK(spec.n_bins_x == 20);
        CHECK(spec.n_bins_y == 30);
        const GPlotSpec rebuilt = makePlotter(spec)->plotSpec();
        CHECK(rebuilt.toJson() == spec.toJson());
    }
    SECTION("a 4-d graph: its markers where they differ from the defaults") {
        GGraph4D plain;
        const GPlotSpec plain_spec = plain.plotSpec();
        CHECK_FALSE(plain_spec.marker_size.has_value());
        CHECK_FALSE(plain_spec.small_w_large_marker.has_value());
        CHECK_FALSE(plain_spec.n_best.has_value());
        CHECK_FALSE(plain_spec.toJson().contains("marker"));

        GGraph4D g;
        g.setMinMarkerSize(0.25);
        g.setMaxMarkerSize(2.5);
        g.setSmallWLargeMarker(false);
        g.setNBest(3);
        const GPlotSpec spec = g.plotSpec();
        REQUIRE(spec.marker_size.has_value());
        CHECK(*spec.marker_size == std::make_tuple(0.25, 2.5));
        CHECK(spec.small_w_large_marker == false);
        CHECK(spec.n_best == 3);
        CHECK(spec.toJson().contains("\"n_best\": 3"));
        CHECK(spec.toJson().contains("\"small_w_large_marker\": false"));
        const auto rebuilt = makePlotter(spec);
        const auto *g4 = dynamic_cast<const GGraph4D *>(rebuilt.get());
        REQUIRE(g4 != nullptr);
        CHECK(g4->getMinMarkerSize() == 0.25);
        CHECK(g4->getMaxMarkerSize() == 2.5);
        CHECK_FALSE(g4->getSmallWLargeMarker());
        CHECK(g4->getNBest() == 3);
    }
}

/******************************************************************************/
namespace {
/**
 * A plotter built from a plot spec (makePlotter()), with rows of data.
 * @param spec The spec
 * @param rows Its data, a row per point
 * @return The plotter
 */
std::shared_ptr<GBasePlotter>
fromSpec(const GPlotSpec &spec, const std::vector<std::vector<double>> &rows) {
    std::shared_ptr<GBasePlotter> p = makePlotter(spec);
    for(const auto &row : rows) {
        p->appendRow(row);
    }
    return p;
}

/**
 * @param text A text
 * @param what A piece of it
 * @return How often the piece occurs in the text
 */
std::size_t occurrences(const std::string &text, const std::string &what) {
    std::size_t n = 0;
    for(auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}
} // namespace

// Every plot kind is drawn by the ROOT emitter from what its plot spec and its data say: a plotter
// built from a spec (makePlotter()) gets the objects of its kind, with the spec's labels, bins,
// ranges, formula and markers.
TEST_CASE("ROOT draws every plot kind from its GPlotSpec", "[plotting]") {
    const std::vector<std::vector<double>> xy{{0., 1.}, {1., 3.}, {2., 2.}};
    SECTION("2-d graph") {
        GPlotSpec spec(plotKind::graph_2d);
        spec.name = "a graph";
        spec.x_label = "the x";
        spec.plot_mode = graphPlotMode::CURVE;
        spec.arrows = true;
        const std::string m = rootMacroOf(fromSpec(spec, xy));
        CHECK(m.contains("TGraph *graph_0 = new TGraph(3, x_array__0, y_array__0);"));
        CHECK(m.contains("y_array__0[1] = 3;"));
        CHECK(m.contains("graph_0->GetXaxis()->SetTitle(\"the x\");"));
        CHECK(m.contains("graph_0->SetTitle(\"a graph\");"));
        CHECK(occurrences(m, "new TArrow(") == 2); // between neighbouring points
    }
    SECTION("2-d graph with errors") {
        GPlotSpec spec(plotKind::graph_2d_err);
        spec.plot_mode = graphPlotMode::SCATTER;
        const std::string m = rootMacroOf(fromSpec(spec, {{0., 0.1, 1., 0.2}, {1., 0.1, 2., 0.3}}));
        CHECK(m.contains("new TGraphErrors(2, x_array__0, y_array__0, ex_array__0 ,ey_array__0);"));
        CHECK(m.contains("ey_array__0[1] = 0.29999999999999999;"));
        CHECK(m.contains("graph__0->Draw(\"AP\");"));
    }
    SECTION("3-d graph") {
        GPlotSpec spec(plotKind::graph_3d);
        spec.plot_mode = graphPlotMode::CURVE;
        spec.floor_shadow = true;
        const std::string m =
            rootMacroOf(fromSpec(spec, {{0., 0., 1.}, {1., 0., 2.}, {0., 1., 3.}}));
        CHECK(m.contains("TGraph2D *graph__0 = new TGraph2D(3,"));
        CHECK(m.contains("TPolyLine3D *lines_graph__0"));
        CHECK(m.contains("shadow_graph__0"));
    }
    SECTION("4-d graph: its markers from the spec") {
        GPlotSpec spec(plotKind::graph_4d);
        spec.n_best = 2;
        spec.marker_size = std::make_tuple(0.5, 1.5);
        const std::string m = rootMacroOf(
            fromSpec(spec, {{0., 0., 0., 3.}, {1., 0., 1., 1.}, {0., 1., 2., 2.}, {1., 1., 3., 4.}})
        );
        CHECK(m.contains("TH3F *fr__0"));
        CHECK(occurrences(m, "new TPolyMarker3D(1);") == 2); // the two best only
        CHECK(m.contains("pm3d__0_0->SetMarkerSize(3);"));   // the best: twice the largest
        CHECK(m.contains("pm3d__0_0->SetPoint(0, 1, 0, 1); // w = 1"));
    }
    SECTION("1-d histograms, fixed and automatic range") {
        GPlotSpec spec(plotKind::hist_1d);
        spec.n_bins_x = 4;
        spec.range_x = std::make_tuple(-1., 1.);
        CHECK(rootMacroOf(fromSpec(spec, {{0.5}}))
                  .contains("TH1D *histD_0 = new TH1D(\"histD_0\", \"histD_0\",4, -1, 1);"));
        spec.range_x.reset();
        const std::string m = rootMacroOf(fromSpec(spec, {{1.}, {3.}}));
        CHECK(m.contains("new TH1D(\"histD_0\", \"histD_0\",4, 1, 3.5);")); // a bin above
        CHECK(m.contains("histD_0->Fill(3.0000000000000000);"));
        spec.n_bins_x = 4;
        CHECK_THROWS(rootMacroOf(fromSpec(spec, {}))); // no data to take the range from
    }
    SECTION("1-d integer histogram") {
        GPlotSpec spec(plotKind::hist_1i);
        spec.n_bins_x = 5;
        spec.range_x = std::make_tuple(0., 5.);
        const std::string m = rootMacroOf(fromSpec(spec, {{1.}, {4.}}));
        CHECK(m.contains("TH1I *histI_0 = new TH1I(\"histI_0\", \"histI_0\",5, 0, 5);"));
        CHECK(m.contains("histI_0->Fill(4);"));
    }
    SECTION("2-d histogram") {
        GPlotSpec spec(plotKind::hist_2d);
        spec.n_bins_x = 3;
        spec.n_bins_y = 6;
        spec.range_x = std::make_tuple(0., 3.);
        spec.range_y = std::make_tuple(0., 6.);
        spec.drawing_args = "LEGO";
        const std::string m = rootMacroOf(fromSpec(spec, xy));
        CHECK(m.contains("new TH2D(\"hist2D_0\", \"hist2D_0\",3, 0, 3,6, 0, 6);"));
        CHECK(m.contains("hist2D_0->Draw(\"LEGO\");"));
    }
    SECTION("functions") {
        GPlotSpec spec(plotKind::function_1d);
        spec.function = "x*x";
        spec.range_x = std::make_tuple(0., 2.);
        spec.n_bins_x = 17;
        const std::string m = rootMacroOf(makePlotter(spec));
        CHECK(m.contains("TF1 *func1D_0 = new TF1(\"func1D_0\", \"x*x\",0, 2);"));
        CHECK(m.contains("func1D_0->SetNpx(17);"));
        GPlotSpec spec2(plotKind::function_2d);
        spec2.function = "x+y";
        spec2.range_x = std::make_tuple(0., 1.);
        spec2.range_y = std::make_tuple(-1., 1.);
        spec2.n_bins_y = 9;
        const std::string m2 = rootMacroOf(makePlotter(spec2));
        CHECK(m2.contains("TF2 *func2D_0 = new TF2(\"func2D_0\", \"x+y\",0, 1, -1, 1);"));
        CHECK(m2.contains("func2D_0->SetNpy(9);"));
        spec2.range_y = std::make_tuple(1., -1.);
        CHECK_THROWS(rootMacroOf(makePlotter(spec2))); // an empty range
    }
}

/******************************************************************************/
// One plot spec, two backends: ROOT and gnuplot draw the same series -- the same values in the same
// order -- with the same axis labels and the same legend.
TEST_CASE("ROOT and gnuplot draw the same content from one GPlotSpec", "[plotting]") {
    GPlotSpec spec(plotKind::graph_2d);
    spec.name = "series one";
    spec.x_label = "the x";
    spec.y_label = "the y";
    spec.plot_mode = graphPlotMode::CURVE;
    GPlotSpec spec2 = spec;
    spec2.name = "series two";
    std::vector<std::vector<double>> rows;
    std::vector<std::vector<double>> rows2;
    for(int i = 0; i < 6; ++i) {
        rows.push_back({0.5 * i, (i * i) + (1. / 3.)});
        rows2.push_back({0.5 * i, 2. - (0.1 * i)});
    }
    const auto primary = fromSpec(spec, rows);
    primary->registerSecondaryPlotter(fromSpec(spec2, rows2));

    GPlotDesigner root_gpd("one spec", 1, 1);
    root_gpd.registerPlotter(primary);
    const std::string root = root_gpd.plot();
    GPlotDesigner gp_gpd("one spec", 1, 1);
    gp_gpd.setPlotBackend(plotBackend::GNUPLOT);
    gp_gpd.registerPlotter(primary);
    const std::string gp = gp_gpd.plot();

    // The series as each backend states them: ROOT's arrays, gnuplot's datablocks.
    const auto root_series = [&root](const std::string &suffix, std::size_t n) {
        std::vector<std::vector<double>> values;
        for(std::size_t i = 0; i < n; ++i) {
            std::vector<double> row;
            for(const std::string axis : {"x", "y"}) {
                const std::string key =
                    axis + "_array_" + suffix + "[" + std::to_string(i) + "] = ";
                const auto at = root.find(key);
                REQUIRE(at != std::string::npos);
                row.push_back(std::stod(root.substr(at + key.size())));
            }
            values.push_back(row);
        }
        return values;
    };
    const auto gnuplot_series = [&gp](const std::string &block) {
        std::vector<std::vector<double>> values;
        const auto at = gp.find(block + " << EOD\n");
        REQUIRE(at != std::string::npos);
        std::istringstream lines(gp.substr(at + block.size() + 8));
        for(std::string line; std::getline(lines, line) and line != "EOD";) {
            std::istringstream fields(line);
            double x = 0.;
            double y = 0.;
            fields >> x >> y;
            values.push_back({x, y});
        }
        return values;
    };
    CHECK(root_series("_0", rows.size()) == rows);
    CHECK(gnuplot_series("$D0") == rows);
    CHECK(root_series("_0_0", rows2.size()) == rows2);
    CHECK(gnuplot_series("$D1") == rows2);

    // The labels and the legend.
    CHECK(root.contains("->GetXaxis()->SetTitle(\"the x\");"));
    CHECK(gp.contains("set xlabel \"the x\""));
    CHECK(root.contains("->GetYaxis()->SetTitle(\"the y\");"));
    CHECK(gp.contains("set ylabel \"the y\""));
    for(const std::string label : {"series one", "series two"}) {
        CHECK(root.contains(", \"" + label + "\", \"lp\");")); // legend->AddEntry(...)
        CHECK(gp.contains("title \"" + label + "\""));
    }
}

/******************************************************************************/
// GPlotSpec now carries the GGraph2D/2ED plot mode (scatter vs curve), and makePlotter
// applies it -- so the choice round-trips through the spec.
TEST_CASE("GPlotSpec round-trips the graph plot mode", "[plotting]") {
    GGraph2D g;
    g.setPlotMode(graphPlotMode::SCATTER);
    REQUIRE(g.plotSpec().plot_mode.has_value());
    CHECK(*g.plotSpec().plot_mode == graphPlotMode::SCATTER);

    const auto rebuilt = makePlotter(g.plotSpec());
    const auto *g2 = dynamic_cast<const GGraph2D *>(rebuilt.get());
    REQUIRE(g2 != nullptr);
    CHECK(g2->getPlotMode() == graphPlotMode::SCATTER);
}

/******************************************************************************/
// GDataLog (the modern, plotter-object-free API) must produce the SAME output as the
// legacy plotter-object path: declare specs + push data == build plotters + add data.
// This is the PoC's core equivalence claim -- it lets production code (monitors) drop the
// fused plotter objects without changing a single byte of emitted output.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent byte-identity invariant (legacy plotter-object output == GDataLog output) exercised via SECTIONs across every plot kind; each section is a self-contained legacy-vs-modern comparison, already about as separated as Catch2 sections allow
TEST_CASE("GDataLog reproduces the legacy plotter-object output byte-for-byte", "[plotting]") {
    SECTION("a 2-d graph with a scatter overlay (plot mode + overlay + labels + data)") {
        // --- Legacy: build plotter objects, set modes/labels, add data, register. ---
        auto prim = std::make_shared<GGraph2D>();
        prim->setPlotLabel("global best");
        prim->setXAxisLabel("Iteration");
        prim->setYAxisLabel("Best Fitness");
        prim->setPlotMode(graphPlotMode::CURVE);
        prim->add(0., 5.);
        prim->add(1., 3.);
        prim->add(2., 2.);

        auto over = std::make_shared<GGraph2D>();
        over->setPlotLabel("iteration best");
        over->setXAxisLabel("Iteration");
        over->setYAxisLabel("Best Fitness");
        over->setPlotMode(graphPlotMode::SCATTER);
        over->add(0., 6.);
        over->add(1., 4.);
        over->add(2., 3.);
        prim->registerSecondaryPlotter(over);

        GPlotDesigner gpd_legacy("Progress", 1, 1);
        gpd_legacy.registerPlotter(prim);
        const std::string legacy = gpd_legacy.plot();

        // --- Modern: declare specs (from the same plotters' plotSpec()) + push the data. ---
        GDataLog log("Progress", 1, 1);
        const auto pid = log.declareSeries(prim->plotSpec());
        log.append(pid, 0., 5.);
        log.append(pid, 1., 3.);
        log.append(pid, 2., 2.);
        const auto oid = log.overlaySeries(pid, over->plotSpec());
        log.append(oid, 0., 6.);
        log.append(oid, 1., 4.);
        log.append(oid, 2., 3.);

        CHECK(log.nSeries() == 2);
        CHECK(log.toDesigner().plot() == legacy);
    }

    SECTION("a 1-d (auto-ranged) histogram (bins round-trip through the spec)") {
        // Auto-ranged: makePlotter rebuilds via the single-bin-count ctor, so both sides
        // derive the same range from the data. (The fixed-range case is covered in the
        // dedicated sections below.)
        auto h = std::make_shared<GHistogram1D>(12);
        h->setPlotLabel("a distribution");
        h->setXAxisLabel("value");
        // Compute the sample data ONCE and feed both paths from it, so both receive bit-identical
        // doubles. Recomputing 0.1*i-1.0 separately per path is NOT reproducible across compilers:
        // clang FMA-contracts the `const double v = ...` statement but not the `& (...)` argument
        // expression, so the two paths would otherwise Fill() values differing in the last ULP and
        // the byte-identity check would (correctly) fail on that incidental difference, not on any
        // real divergence between the plotting paths.
        std::vector<double> data;
        data.reserve(20);
        for(int i = 0; i < 20; ++i) {
            data.push_back(0.1 * static_cast<double>(i) - 1.0);
        }
        for(const double v : data) {
            (*h) & v;
        }
        GPlotDesigner gpd_legacy("Dist", 1, 1);
        gpd_legacy.registerPlotter(h);
        const std::string legacy = gpd_legacy.plot();

        GDataLog log("Dist", 1, 1);
        const auto id = log.declareSeries(h->plotSpec());
        for(const double v : data) {
            log.append(id, std::span<const double>(&v, 1));
        }
        CHECK(log.toDesigner().plot() == legacy);
    }

    SECTION("a FIXED-range 1-d histogram (range round-trips through the spec)") {
        // The Phase-B follow-up: a fixed-range histogram (as the monitors use) now round-trips
        // its range through GPlotSpec, so GDataLog emits byte-identically to the plotter-object path.
        auto h = std::make_shared<GHistogram1D>(15, -1.0, 2.0);
        h->setPlotLabel("a fixed distribution");
        h->setXAxisLabel("value");
        // Compute the data once and feed both paths from it (see the auto-ranged section above for
        // why per-path recomputation is not bit-reproducible across compilers).
        std::vector<double> data;
        data.reserve(20);
        for(int i = 0; i < 20; ++i) {
            data.push_back(0.1 * static_cast<double>(i) - 1.0);
        }
        for(const double v : data) {
            (*h) & v;
        }
        GPlotDesigner gpd_legacy("Dist", 1, 1);
        gpd_legacy.registerPlotter(h);
        const std::string legacy = gpd_legacy.plot();

        GDataLog log("Dist", 1, 1);
        const auto id = log.declareSeries(h->plotSpec());
        for(const double v : data) {
            log.append(id, std::span<const double>(&v, 1));
        }
        CHECK(log.toDesigner().plot() == legacy);
    }

    SECTION("a FIXED-range 2-d histogram (both axis ranges round-trip)") {
        auto h = std::make_shared<GHistogram2D>(10, 10, -4.0, 4.0, -4.0, 4.0);
        h->setPlotLabel("a 2-d fixed distribution");
        h->setXAxisLabel("x");
        h->setYAxisLabel("y");
        // Compute the (x, y) samples once and feed both paths from them (see the 1-d auto-ranged
        // section for why per-path recomputation is not bit-reproducible across compilers).
        std::vector<std::pair<double, double>> data;
        data.reserve(12);
        for(int i = 0; i < 12; ++i) {
            data.emplace_back(
                0.5 * static_cast<double>(i) - 3.0,
                3.0 - 0.4 * static_cast<double>(i)
            );
        }
        for(const auto &[x, y] : data) {
            h->add(x, y);
        }
        GPlotDesigner gpd_legacy("Dist2D", 1, 1);
        gpd_legacy.registerPlotter(h);
        const std::string legacy = gpd_legacy.plot();

        GDataLog log("Dist2D", 1, 1);
        const auto id = log.declareSeries(h->plotSpec());
        for(const auto &[x, y] : data) {
            log.append(id, x, y);
        }
        CHECK(log.toDesigner().plot() == legacy);
    }

    SECTION("sortX + setAddPrintCommand + canvas dimensions round-trip") {
        // Unsorted x data exercises sortByFirstColumn; print command + canvas dims
        // exercise the forwarded designer settings. (These are exactly the GProgressPlotterT
        // INFOEND touchpoints.)
        auto g = std::make_shared<GGraph2D>();
        g->setPlotLabel("progress");
        g->setXAxisLabel("Iteration");
        g->setYAxisLabel("Fitness");
        g->setPlotMode(graphPlotMode::CURVE);
        g->add(2., 1.);
        g->add(0., 5.);
        g->add(1., 3.); // deliberately out of x order
        g->sortX();
        GPlotDesigner gpd_legacy("Progress information", 1, 1);
        gpd_legacy.setCanvasDimensions(1024, 768);
        gpd_legacy.setAddPrintCommand(true);
        gpd_legacy.registerPlotter(g);
        const std::string legacy = gpd_legacy.plot();

        // Build a fresh unsorted graph so its spec carries no accidental sort.
        auto gs = std::make_shared<GGraph2D>();
        gs->setPlotLabel("progress");
        gs->setXAxisLabel("Iteration");
        gs->setYAxisLabel("Fitness");
        gs->setPlotMode(graphPlotMode::CURVE);

        GDataLog log("Progress information", 1, 1);
        log.setCanvasDimensions(1024, 768);
        log.setAddPrintCommand(true);
        const auto id = log.declareSeries(gs->plotSpec());
        log.append(id, 2., 1.);
        log.append(id, 0., 5.);
        log.append(id, 1., 3.); // same out-of-order data
        log.sortByFirstColumn(id);

        CHECK(log.toDesigner().plot() == legacy);
    }
}

/******************************************************************************/
// Standard GCommonInterfaceT-contract tests: every plotter / designer / decorator is run through the
// construction / clone / load / (de-)serialization round-trips of the installed harness
// (common/testing/GStandardTests.hpp). TFactory_GUnitTests<T>() is specialized for the types without a
// public default constructor.

namespace Gem::Common::Testing {

template <>
std::shared_ptr<GHistogram1D> TFactory_GUnitTests<GHistogram1D>() {
    return std::make_shared<GHistogram1D>(std::size_t(20));
}
template <>
std::shared_ptr<GHistogram1I> TFactory_GUnitTests<GHistogram1I>() {
    return std::make_shared<GHistogram1I>(std::size_t(20), 0.0, 10.0);
}
template <>
std::shared_ptr<GHistogram2D> TFactory_GUnitTests<GHistogram2D>() {
    return std::make_shared<GHistogram2D>(std::size_t(20), std::size_t(20));
}
template <>
std::shared_ptr<GFunctionPlotter1D> TFactory_GUnitTests<GFunctionPlotter1D>() {
    return std::make_shared<GFunctionPlotter1D>(
        std::string("x"),
        std::tuple<double, double>(-1.0, 1.0)
    );
}
template <>
std::shared_ptr<GFunctionPlotter2D> TFactory_GUnitTests<GFunctionPlotter2D>() {
    return std::make_shared<GFunctionPlotter2D>(
        std::string("x*y"),
        std::tuple<double, double>(-1.0, 1.0),
        std::tuple<double, double>(-1.0, 1.0)
    );
}
template <>
std::shared_ptr<GPlotDesigner> TFactory_GUnitTests<GPlotDesigner>() {
    return std::make_shared<GPlotDesigner>(
        std::string("test canvas"),
        std::size_t(1),
        std::size_t(1)
    );
}
template <>
std::shared_ptr<GMarker<double>> TFactory_GUnitTests<GMarker<double>>() {
    return std::make_shared<GMarker<double>>(
        std::tuple<double, double>(0.0, 0.0),
        gMarker::closedCircle,
        gColor::black,
        0.05
    );
}

} // namespace Gem::Common::Testing

/******************************************************************************/

TEMPLATE_TEST_CASE(
    "Dietrich plotting types satisfy the standard GCommonInterfaceT contract (no failure expected)",
    "[plotting][standard]",
    GGraph2D,
    GGraph2ED,
    GGraph3D,
    GGraph4D,
    GHistogram1D,
    GHistogram1I,
    GHistogram2D,
    GFunctionPlotter1D,
    GFunctionPlotter2D,
    GPlotDesigner,
    GMarker<double>,
    GDecoratorContainer_2D<double>
) {
    STATIC_REQUIRE(Gem::Common::Testing::ModifiableForTest<TestType>);
    Gem::Common::Testing::StandardTests_no_failure_expected<TestType>();
}

/******************************************************************************/

TEMPLATE_TEST_CASE(
    "Dietrich plotting types satisfy the standard GCommonInterfaceT contract (failures expected)",
    "[plotting][standard]",
    GGraph2D,
    GGraph2ED,
    GGraph3D,
    GGraph4D,
    GHistogram1D,
    GHistogram1I,
    GHistogram2D,
    GFunctionPlotter1D,
    GFunctionPlotter2D,
    GPlotDesigner,
    GMarker<double>,
    GDecoratorContainer_2D<double>
) {
    Gem::Common::Testing::StandardTests_failures_expected<TestType>();
}

/******************************************************************************/
// Regression (2026-07-18): GMarker's bounded decoratorData() had its clipping INVERTED -- it
// emitted plotting code only for markers OUTSIDE the axis ranges and swallowed in-range markers.
// The contract is the opposite: an in-range marker is drawn, an out-of-range one is clipped.

TEST_CASE(
    "GMarker clips to the plot boundaries (in-range drawn and out-of-range empty)",
    "[plotting][decorators]"
) {
    const GMarker<double>
        marker(std::tuple<double, double>(0.5, 0.5), gMarker::closedCircle, gColor::black, 0.05);

    const std::tuple<double, double> x_range(0.0, 1.0);
    const std::tuple<double, double> y_range(0.0, 1.0);

    // In range -> the marker's plotting code is emitted
    CHECK_FALSE(marker.decoratorData(x_range, y_range, "  ", 0).empty());

    // Outside either axis range -> clipped away
    const std::tuple<double, double> far_x(2.0, 3.0);
    const std::tuple<double, double> far_y(-5.0, -4.0);
    CHECK(marker.decoratorData(far_x, y_range, "  ", 0).empty());
    CHECK(marker.decoratorData(x_range, far_y, "  ", 0).empty());
}

/******************************************************************************/
// Regression (2026-07-18): the integer histogram was constructible through the generic plot
// machinery (makePlotter / GPlotSpec "hist_1i") but NOT fillable through the generic
// appendRow(span<const double>) path GDataLog::toDesigner() uses -- the all-double-only fast
// path rejected any collector with an int32 axis, so a hist_1i data log threw at realize time.
// appendRow now checked-narrows into int32 axes (range-checked, fraction truncated, matching
// the tuple insertion operators).

TEST_CASE("GHistogram1I is fillable through the generic appendRow path", "[plotting][collectors]") {
    GHistogram1I hist(10, 0., 10.);

    // A representable value lands in the (int32) column
    const std::array<double, 1> ok_row{3.0};
    CHECK_NOTHROW(hist.appendRow(std::span<const double>(ok_row)));

    // A fractional value is truncated (tuple-insertion semantics), not rejected
    const std::array<double, 1> frac_row{4.7};
    CHECK_NOTHROW(hist.appendRow(std::span<const double>(frac_row)));

    // An out-of-int32-range value is rejected with a clear error
    const std::array<double, 1> big_row{3.0e10};
    CHECK_THROWS(hist.appendRow(std::span<const double>(big_row)));

    // Both accepted rows are visible through the generic column view
    const auto cols = hist.dataColumns();
    REQUIRE(cols.size() == 1);
}

/******************************************************************************/
// Surface projections: a 3-d graph drawn as a surface with its colour map on the floor or ceiling.
namespace {
/** @brief A small sampled surface (3 x 3 grid), optionally projected, with z from 0 to 4 */
std::shared_ptr<GGraph3D> smallSurface(surfaceProjection where) {
    auto g = std::make_shared<GGraph3D>();
    for(int i = -1; i <= 1; ++i) {
        for(int j = -1; j <= 1; ++j) {
            (*g) & std::tuple<double, double, double>(
                       double(i),
                       double(j),
                       double(i * i + j * j) * 2.
                   );
        }
    }
    g->setSurfaceProjection(where);
    return g;
}

/** @brief Renders one plotter on a 1x1 canvas through the given backend */
std::string render(const std::shared_ptr<GBasePlotter> &p, plotBackend backend) {
    GPlotDesigner gpd("projection", 1, 1);
    gpd.setPlotBackend(backend);
    gpd.registerPlotter(p);
    return gpd.plot();
}
} // namespace

TEST_CASE("A surface projection is reported in the plot spec and its JSON", "[plotting]") {
    auto g = smallSurface(surfaceProjection::FLOOR);
    const GPlotSpec spec = g->plotSpec();
    REQUIRE(spec.projection.has_value());
    CHECK(*spec.projection == surfaceProjection::FLOOR);
    CHECK(spec.toJson().contains("\"projection\": \"floor\""));

    // An unprojected 3-d graph reports no projection at all.
    CHECK(not smallSurface(surfaceProjection::NONE)->plotSpec().projection.has_value());
}

TEST_CASE(
    "makePlotter applies a projection and a 3-d curve and refuses a projection elsewhere",
    "[plotting]"
) {
    GPlotSpec spec(plotKind::graph_3d);
    spec.projection = surfaceProjection::CEILING;
    spec.plot_mode = graphPlotMode::CURVE;
    auto p = makePlotter(spec);
    auto *g3d = dynamic_cast<GGraph3D *>(p.get());
    REQUIRE(g3d != nullptr);
    CHECK(g3d->getSurfaceProjection() == surfaceProjection::CEILING);
    CHECK(g3d->getDrawLines());

    GPlotSpec flat(plotKind::graph_2d);
    flat.projection = surfaceProjection::FLOOR;
    CHECK_THROWS(makePlotter(flat));
}

TEST_CASE(
    "ROOT draws a ceiling projection with SURF3 and a floor projection by hand",
    "[plotting]"
) {
    const std::string ceiling = render(smallSurface(surfaceProjection::CEILING), plotBackend::ROOT);
    CHECK(ceiling.contains("Draw(\"SURF3Z\")"));
    CHECK(ceiling.contains("SetMinimum(0)"));
    CHECK(ceiling.contains("SetMaximum(4)"));

    const std::string floor = render(smallSurface(surfaceProjection::FLOOR), plotBackend::ROOT);
    CHECK(floor.contains("Draw(\"SURF1Z\")"));
    CHECK(floor.contains("WCtoNDC("));
    CHECK(floor.contains("Draw(\"SURF1 same\")"));
}

TEST_CASE("A projection refuses custom drawing arguments and being an overlay", "[plotting]") {
    auto g = smallSurface(surfaceProjection::FLOOR);
    g->setDrawingArguments("SURF2Z");
    CHECK_THROWS(render(g, plotBackend::ROOT));

    auto primary = smallSurface(surfaceProjection::NONE);
    primary->registerSecondaryPlotter(smallSurface(surfaceProjection::FLOOR));
    CHECK_THROWS(render(primary, plotBackend::ROOT));
    CHECK_THROWS(render(primary, plotBackend::GNUPLOT));
    CHECK_THROWS(render(primary, plotBackend::MATPLOTLIB));
    CHECK_THROWS(render(primary, plotBackend::OCTAVE));
}

TEST_CASE("The script backends draw a projected surface with their own means", "[plotting]") {
    const std::string gp_floor =
        render(smallSurface(surfaceProjection::FLOOR), plotBackend::GNUPLOT);
    CHECK(gp_floor.contains("set dgrid3d"));
    CHECK(gp_floor.contains("set pm3d at bs"));
    CHECK(gp_floor.contains("with pm3d"));
    CHECK(gp_floor.contains("unset pm3d")); // the next pad starts from the defaults again
    CHECK(render(smallSurface(surfaceProjection::CEILING), plotBackend::GNUPLOT)
              .contains("set pm3d at ts"));

    const std::string mpl = render(smallSurface(surfaceProjection::FLOOR), plotBackend::MATPLOTLIB);
    CHECK(mpl.contains("plot_trisurf("));
    CHECK(mpl.contains("tricontourf("));
    CHECK(mpl.contains("offset=0"));
    CHECK(mpl.contains("set_zlim(0, 4)"));

    const std::string oct = render(smallSurface(surfaceProjection::CEILING), plotBackend::OCTAVE);
    CHECK(oct.contains("delaunay("));
    CHECK(oct.contains("4 * ones(size(z_))"));
    CHECK(oct.contains("zlim([0 4])"));
}

TEST_CASE(
    "A projected map can be drawn without its surface under points casting floor shadows",
    "[plotting]"
) {
    // The map alone: the surface is not drawn, and with it goes the colour scale.
    auto map = smallSurface(surfaceProjection::FLOOR);
    map->setDrawSurface(false);
    CHECK(not map->getDrawSurface());
    CHECK(map->plotSpec().toJson().contains("\"surface\": false"));

    // Points above it, casting shadows on the floor.
    auto points = std::make_shared<GGraph3D>();
    (*points) & std::tuple<double, double, double>(0.5, 0.5, 3.0);
    (*points) & std::tuple<double, double, double>(-0.5, 0.0, 1.0);
    points->setFloorShadow(true);
    CHECK(points->getFloorShadow());
    CHECK(points->plotSpec().toJson().contains("\"floor_shadow\": true"));
    map->registerSecondaryPlotter(points);

    const std::string root = render(map, plotBackend::ROOT);
    CHECK(root.contains("WCtoNDC(")); // the map, cell by cell
    CHECK_FALSE(root.contains("SURF1"));
    CHECK(root.contains("TPolyMarker3D")); // the shadows
    CHECK(root.contains("GetRmin()[2]"));  // at the floor of the box

    const std::string gp = render(map, plotBackend::GNUPLOT);
    CHECK(gp.contains("set pm3d at b\n"));
    CHECK(gp.contains("using 1:2:(0) with points"));

    const std::string mpl = render(map, plotBackend::MATPLOTLIB);
    CHECK_FALSE(mpl.contains("plot_trisurf("));
    CHECK_FALSE(mpl.contains("colorbar("));
    CHECK(mpl.contains("get_zlim()[0]"));

    const std::string oct = render(map, plotBackend::OCTAVE);
    CHECK_FALSE(oct.contains("colorbar;"));
    CHECK(oct.contains("zl_(1) * ones(size(xs_))"));

    // makePlotter honours both fields, and refuses them where there is no 3-d box.
    GPlotSpec spec(plotKind::graph_3d);
    spec.projection = surfaceProjection::FLOOR;
    spec.surface = false;
    spec.floor_shadow = true;
    const auto plotter = makePlotter(spec);
    auto *built = dynamic_cast<GGraph3D *>(plotter.get());
    REQUIRE(built != nullptr);
    CHECK(not built->getDrawSurface());
    CHECK(built->getFloorShadow());
    GPlotSpec flat(plotKind::graph_2d);
    flat.floor_shadow = true;
    CHECK_THROWS(makePlotter(flat));
}

/******************************************************************************/
// Octave keeps the flat 2-d view under `hold on`; a 3-d graph must ask for the 3-d view, or a helix
// is drawn as a circle.
TEST_CASE("Octave draws a 3-d graph in a 3-d view", "[plotting]") {
    auto g = std::make_shared<GGraph3D>();
    (*g) & std::tuple<double, double, double>(1.0, 2.0, 3.0);
    (*g) & std::tuple<double, double, double>(2.0, 3.0, 4.0);

    GPlotDesigner gpd("octave 3d", 1, 1);
    gpd.setPlotBackend(plotBackend::OCTAVE);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();
    CHECK(s.contains("plot3("));
    CHECK(s.contains("view(3);"));
}

/******************************************************************************/
// Logarithmic axes: a plotter can ask for a logarithmic x- or y-axis, and every backend writes
// what its tool needs for that. A plot that does not ask for one gets the script it always got.
namespace {

/** @brief A graph over three decades, with the axis scales as asked for. */
std::shared_ptr<GGraph2D> decadesGraph(bool log_x, bool log_y) {
    auto g = std::make_shared<GGraph2D>();
    (*g) & std::tuple<double, double>(0.01, 1.0);
    (*g) & std::tuple<double, double>(0.1, 10.0);
    (*g) & std::tuple<double, double>(1.0, 100.0);
    g->setXAxisLogarithmic(log_x);
    g->setYAxisLogarithmic(log_y);
    return g;
}

/** @brief Whether everything of a ROOT script is drawn before its (one) legend is built: after
 *  `new TLegend(` the only drawing is the legend's own. */
bool drawnBeforeLegend(const std::string &s) {
    const auto at = s.find("new TLegend(");
    if(at == std::string::npos) {
        return false;
    }
    const std::string tail = s.substr(at);
    std::size_t draws = 0;
    for(auto p = tail.find("Draw("); p != std::string::npos; p = tail.find("Draw(", p + 1)) {
        ++draws;
    }
    std::size_t legend_draws = 0;
    for(auto p = tail.find("legend->Draw("); p != std::string::npos;
        p = tail.find("legend->Draw(", p + 1)) {
        ++legend_draws;
    }
    return draws == legend_draws and legend_draws == 1;
}

/** @brief The script of @p backend for the given plotters, one pad each. */
std::string scriptFor(plotBackend backend, const std::vector<std::shared_ptr<GGraph2D>> &graphs) {
    GPlotDesigner gpd("axes", 1, graphs.size());
    gpd.setPlotBackend(backend);
    for(const auto &g : graphs) {
        gpd.registerPlotter(g);
    }
    return gpd.plot();
}

} /* anonymous namespace */

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per backend; the count comes from the assertion macros
TEST_CASE("A plotter's logarithmic axes reach every backend", "[plotting][logaxis]") {
    const auto log_x = decadesGraph(true, false);
    const auto log_y = decadesGraph(false, true);
    const auto linear = decadesGraph(false, false);

    SECTION("ROOT sets the scale on the pad before it draws") {
        const std::string s = scriptFor(plotBackend::ROOT, {log_x, log_y});
        const auto first_pad = s.find("graphPad->cd(1);");
        const auto second_pad = s.find("graphPad->cd(2);");
        REQUIRE(first_pad != std::string::npos);
        REQUIRE(second_pad != std::string::npos);
        const std::string pad_one = s.substr(first_pad, second_pad - first_pad);
        const std::string pad_two = s.substr(second_pad);
        CHECK(pad_one.contains("gPad->SetLogx(1);"));
        CHECK_FALSE(pad_one.contains("SetLogy"));
        CHECK(pad_one.find("gPad->SetLogx(1);") < pad_one.find("Draw("));
        CHECK(pad_two.contains("gPad->SetLogy(1);"));
        CHECK_FALSE(pad_two.contains("SetLogx"));
        // An axis between two powers of ten would have no labels: both pads ask for more of
        // them, after the graph is drawn (the range is not known before).
        for(const std::string &pad : {pad_one, pad_two}) {
            REQUIRE(pad.contains("SetMoreLogLabels"));
            CHECK(pad.contains("SetNoExponent"));
            CHECK(pad.find("Draw(") < pad.find("SetMoreLogLabels"));
        }
    }

    SECTION("gnuplot switches the scale on, and off again for the next pad") {
        const std::string s = scriptFor(plotBackend::GNUPLOT, {log_x, linear, log_y});
        const auto on = s.find("set logscale x");
        const auto off = s.find("unset logscale x");
        const auto on_y = s.find("set logscale y");
        REQUIRE(on != std::string::npos);
        REQUIRE(off != std::string::npos);
        REQUIRE(on_y != std::string::npos);
        CHECK(on < off);
        CHECK(off < on_y);
    }

    SECTION("matplotlib and Octave set the scale of the axes object") {
        const std::string py = scriptFor(plotBackend::MATPLOTLIB, {log_x, log_y});
        CHECK(py.contains("ax.set_xscale(\"log\")"));
        CHECK(py.contains("ax.set_yscale(\"log\")"));
        const std::string m = scriptFor(plotBackend::OCTAVE, {log_x, log_y});
        CHECK(m.contains("set(gca, 'XScale', 'log');"));
        CHECK(m.contains("set(gca, 'YScale', 'log');"));
    }

    SECTION("a plot with linear axes gets no scale command in any backend") {
        for(const auto backend :
            {plotBackend::ROOT,
             plotBackend::GNUPLOT,
             plotBackend::MATPLOTLIB,
             plotBackend::OCTAVE}) {
            const std::string s = scriptFor(backend, {linear});
            CHECK_FALSE(s.contains("SetLog"));
            CHECK_FALSE(s.contains("MoreLogLabels"));
            CHECK_FALSE(s.contains("logscale"));
            CHECK_FALSE(s.contains("scale(\"log\")"));
            CHECK_FALSE(s.contains("Scale', 'log'"));
        }
    }
}

TEST_CASE(
    "The logarithmic axes are part of a plotter's state and of its spec",
    "[plotting][logaxis]"
) {
    const auto g = decadesGraph(true, false);
    CHECK(g->xAxisLogarithmic());
    CHECK_FALSE(g->yAxisLogarithmic());

    // A copy has them; a plotter that differs in them is a different plotter.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) -- the copy is what is tested
    const GGraph2D copy(*g);
    CHECK(copy.xAxisLogarithmic());
    CHECK(copy == *g);
    GGraph2D changed(*g);
    changed.setYAxisLogarithmic(true);
    CHECK(changed != *g);

    // The spec reports them, writes them into its JSON only when set, and a plotter built from
    // the spec has them again.
    const GPlotSpec spec = g->plotSpec();
    CHECK(spec.x_log);
    CHECK_FALSE(spec.y_log);
    CHECK(spec.toJson().contains("\"x_log\": true"));
    CHECK_FALSE(spec.toJson().contains("y_log"));
    CHECK_FALSE(decadesGraph(false, false)->plotSpec().toJson().contains("_log"));
    const auto rebuilt = makePlotter(spec);
    REQUIRE(rebuilt);
    CHECK(rebuilt->xAxisLogarithmic());
    CHECK_FALSE(rebuilt->yAxisLogarithmic());
}

/******************************************************************************/
// Several graphs in one pad must be told apart and must all be visible. gnuplot does that by
// itself; ROOT, matplotlib and Octave have to be told.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per backend; the count comes from the assertion macros
TEST_CASE(
    "Overlaid graphs get colours and a legend and room in every backend",
    "[plotting][overlay]"
) {
    const auto overlaid = [] {
        auto first = decadesGraph(false, false);
        first->setPlotLabel("first");
        auto second = decadesGraph(false, false);
        second->setPlotLabel("second");
        auto third = decadesGraph(false, false);
        third->setPlotLabel("third");
        first->registerSecondaryPlotter(second);
        first->registerSecondaryPlotter(third);
        return first;
    };

    SECTION("ROOT: a colour per graph, one frame for all, a legend") {
        const std::string s = scriptFor(plotBackend::ROOT, {overlaid()});
        CHECK(s.contains("->SetLineColor(4);"));
        CHECK(s.contains("->SetLineColor(2);"));
        CHECK(s.contains("->SetLineColor(8);"));
        CHECK(s.contains("->SetMarkerColor(8);"));
        CHECK(s.contains("new TLegend(0.3, 0.21);")); // ROOT's own placement
        CHECK(s.contains("first->SetMinimum("));
        CHECK(s.contains("first->GetXaxis()->SetLimits("));
        // The frame takes in the error bars too, or those of the extreme points are cut off.
        CHECK(s.contains("g->GetErrorYlow(i)"));
        CHECK(s.contains("g->GetErrorYhigh(i)"));
        CHECK(s.contains("g->GetErrorXlow(i)"));
        CHECK(s.contains("g->GetErrorXhigh(i)"));
        // The frame and the legend come after the last graph is drawn.
        CHECK(drawnBeforeLegend(s));
    }

    SECTION("matplotlib and Octave show the legend their plot calls prepared") {
        CHECK(scriptFor(plotBackend::MATPLOTLIB, {overlaid()}).contains("ax.legend(handles=["));
        CHECK(scriptFor(plotBackend::OCTAVE, {overlaid()})
                  .contains("legend(s_, {'first', 'second', 'third'});"));
    }

    SECTION("a pad with a single graph gets none of this") {
        const auto single = decadesGraph(false, false);
        const std::string root = scriptFor(plotBackend::ROOT, {single});
        CHECK_FALSE(root.contains("SetLineColor"));
        CHECK_FALSE(root.contains("BuildLegend"));
        CHECK_FALSE(root.contains("SetMinimum"));
        CHECK_FALSE(scriptFor(plotBackend::MATPLOTLIB, {single}).contains("ax.legend("));
        CHECK_FALSE(scriptFor(plotBackend::OCTAVE, {single}).contains("legend"));
    }
}

/******************************************************************************/
// A pad's title apart from the label of its first series, a legend in a chosen corner, the width
// of a line and a figure without the canvas title: each reaches every backend, and a plot that
// does not use them is emitted as before.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per backend; the count comes from the assertion macros
TEST_CASE(
    "A pad title and a placed legend and a line width and an untitled figure reach every backend",
    "[plotting][overlay][padtitle]"
) {
    const auto pad = [](bool set) {
        auto first = decadesGraph(false, false);
        first->setPlotLabel("websocket");
        auto second = decadesGraph(false, false);
        second->setPlotLabel("MPI");
        first->registerSecondaryPlotter(second);
        if(set) {
            first->setPadTitle("Up to 1 s");
            first->setLegendPosition(legendPosition::BOTTOM_RIGHT);
            first->setLineWidth(2);
            second->setLineWidth(2);
        }
        return first;
    };
    const auto script = [&pad](plotBackend backend, bool set) {
        GPlotDesigner gpd("The figure", 1, 1);
        gpd.setPlotBackend(backend);
        gpd.setShowCanvasLabel(not set);
        gpd.registerPlotter(pad(set));
        return gpd.plot();
    };

    SECTION("ROOT: the legend takes the labels, then the pad takes its title") {
        const std::string s = script(plotBackend::ROOT, true);
        CHECK(s.contains("SetTitle(\"websocket\");"));
        CHECK(s.contains("SetTitle(\"MPI\");"));
        const auto legend = s.find("new TLegend(0.56, 0.13, 0.88, 0.29);");
        CHECK(s.contains("legend->SetTextSize(0.045);"));
        const auto title = s.find("n->SetTitle(\"Up to 1 s\");");
        REQUIRE(legend != std::string::npos);
        REQUIRE(title != std::string::npos);
        CHECK(legend < title);
        CHECK(s.contains("->SetLineWidth(2);"));
        CHECK_FALSE(s.contains("TPaveLabel"));
        CHECK(s.contains(R"(new TPad("Graphs", "Graphs", 0.01, 0.01, 0.99, 0.99);)"));
    }

    SECTION("gnuplot, matplotlib and Octave") {
        const std::string gp = script(plotBackend::GNUPLOT, true);
        CHECK(gp.contains("set title \"Up to 1 s\""));
        CHECK(gp.contains("title \"websocket\""));
        CHECK(gp.contains("set key bottom right"));
        CHECK(gp.contains("set key default"));
        CHECK(gp.contains("linewidth 2"));
        CHECK_FALSE(gp.contains("The figure"));
        const std::string py = script(plotBackend::MATPLOTLIB, true);
        CHECK(py.contains("ax.set_title(\"Up to 1 s\")"));
        CHECK(py.contains("label=\"websocket\""));
        CHECK(py.contains("], loc=\"lower right\")"));
        CHECK(py.contains("linewidth=2"));
        CHECK_FALSE(py.contains("suptitle"));
        const std::string m = script(plotBackend::OCTAVE, true);
        CHECK(m.contains("title('Up to 1 s');"));
        CHECK(m.contains("'-o', 'LineWidth', 2"));
        CHECK(m.contains("legend(s_, {'websocket', 'MPI'});"));
        CHECK(m.contains("legend('location', 'southeast');"));
    }

    SECTION("without the settings every backend emits what it did before") {
        const std::string root = script(plotBackend::ROOT, false);
        CHECK(root.contains("new TLegend(0.3, 0.21);"));
        CHECK(root.contains("TPaveLabel"));
        CHECK_FALSE(root.contains("SetLineWidth"));
        CHECK_FALSE(root.contains("the title of the pad"));
        CHECK(script(plotBackend::GNUPLOT, false).contains("title \"The figure\""));
        CHECK_FALSE(script(plotBackend::GNUPLOT, false).contains("set key"));
        CHECK(
            script(plotBackend::MATPLOTLIB, false).contains("labels=[\"websocket\", \"MPI\"])\n")
        );
        CHECK(script(plotBackend::MATPLOTLIB, false).contains("fig.suptitle(\"The figure\")"));
        CHECK_FALSE(script(plotBackend::OCTAVE, false).contains("'location'"));
        CHECK_FALSE(GPlotSpec(plotKind::graph_2d).toJson().contains("pad_title"));
    }

    SECTION("the middle of the right side") {
        const auto [x1, y1, x2, y2] = legendBox(legendPosition::CENTER_RIGHT, 3);
        CHECK(std::abs(x2 - 0.88) < 1e-12);
        CHECK(std::abs(((y1 + y2) / 2.) - 0.5) < 1e-12);
        auto first = pad(true);
        first->setLegendPosition(legendPosition::CENTER_RIGHT);
        GPlotDesigner gpd("The figure", 1, 1);
        gpd.registerPlotter(first);
        gpd.setPlotBackend(plotBackend::GNUPLOT);
        CHECK(gpd.plot().contains("set key right center"));
        gpd.setPlotBackend(plotBackend::MATPLOTLIB);
        CHECK(gpd.plot().contains("], loc=\"center right\")"));
        gpd.setPlotBackend(plotBackend::OCTAVE);
        CHECK(gpd.plot().contains("legend('location', 'east');"));
    }

    SECTION("the settings travel through a GPlotSpec and a GDataLog") {
        const auto first = pad(true);
        const GPlotSpec spec = first->plotSpec();
        CHECK(spec.pad_title == "Up to 1 s");
        CHECK(spec.legend == legendPosition::BOTTOM_RIGHT);
        CHECK(spec.line_width == 2u);
        CHECK(spec.toJson().contains(
            "\"pad_title\": \"Up to 1 s\", \"legend\": \"bottom_right\", \"line_width\": 2"
        ));
        const auto rebuilt = makePlotter(spec);
        CHECK(rebuilt->padTitle() == "Up to 1 s");
        CHECK(rebuilt->getLegendPosition() == legendPosition::BOTTOM_RIGHT);
        CHECK(rebuilt->lineWidth() == 2u);
        CHECK(first->clone<GGraph2D>()->padTitle() == "Up to 1 s");

        GDataLog log("The figure", 1, 1);
        GPlotSpec s(plotKind::graph_2d);
        s.name = "websocket";
        s.pad_title = "All";
        s.columns = {"x", "y"};
        const auto id = log.declareSeries(s);
        log.append(id, 1., 2.);
        log.append(id, 2., 3.);
        log.setShowCanvasLabel(false);
        auto gpd = log.toDesigner();
        CHECK_FALSE(gpd.getShowCanvasLabel());
        gpd.setPlotBackend(plotBackend::ROOT);
        CHECK(gpd.plot().contains("n->SetTitle(\"All\");"));
    }
}

/******************************************************************************/
// A legend names series. It lists the labelled series of a pad, and a pad none of whose series
// carries a label gets no legend at all -- in 3-d as in 2-d. In ROOT the legend is built from the
// series alone: TPad::BuildLegend() made an entry of every object drawn into the pad, so a colour
// map drawn cell by cell under points filled the legend with thousands of cells and left an
// unreadable box over the pad.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per case; the count comes from the assertion macros
TEST_CASE(
    "A legend lists the labelled series of a pad and a pad without any gets none",
    "[plotting][overlay][legend]"
) {
    // A colour map on the floor of the box, without its surface, under points casting shadows: what
    // a progress plotter draws for two parameters.
    const auto map_under_points = [](const std::string &map_label,
                                     const std::string &points_label) {
        auto map = smallSurface(surfaceProjection::FLOOR);
        map->setDrawSurface(false);
        map->setPlotLabel(map_label);
        auto points = std::make_shared<GGraph3D>();
        (*points) & std::tuple<double, double, double>(0.5, 0.5, 3.0);
        (*points) & std::tuple<double, double, double>(-0.5, 0.0, 1.0);
        points->setFloorShadow(true);
        points->setPlotLabel(points_label);
        map->registerSecondaryPlotter(points);
        return map;
    };

    SECTION("a 3-d pad with an overlay and no labels has no legend in any backend") {
        const auto map = map_under_points("", "");
        CHECK(map->legendEntries().size() == 0);
        CHECK_FALSE(render(map, plotBackend::ROOT).contains("Legend"));
        CHECK_FALSE(render(map, plotBackend::MATPLOTLIB).contains("ax.legend("));
        CHECK_FALSE(render(map, plotBackend::OCTAVE).contains("legend"));
    }

    SECTION("a 2-d pad with an overlay and no labels has none either") {
        auto first = decadesGraph(false, false);
        first->registerSecondaryPlotter(decadesGraph(false, false));
        CHECK(first->legendEntries().size() == 0);
        CHECK_FALSE(render(first, plotBackend::ROOT).contains("Legend"));
        CHECK_FALSE(render(first, plotBackend::MATPLOTLIB).contains("ax.legend("));
        CHECK_FALSE(render(first, plotBackend::OCTAVE).contains("legend"));
    }

    SECTION("a labelled 3-d pad gets its legend where it was asked for, built from its series") {
        const auto map = map_under_points("the landscape", "evaluated candidates");
        map->setLegendPosition(legendPosition::BOTTOM_RIGHT);
        CHECK(map->legendEntries().size() == 2);
        const std::string s = render(map, plotBackend::ROOT);
        CHECK(s.contains("new TLegend(0.56, 0.13, 0.88, 0.29);"));
        CHECK(s.contains("->SetTextSize(0.045);"));
        // Only the series enter, each once, named in the script: not the map's cells, the shadows
        // or the frame, which TPad::BuildLegend() would have listed.
        CHECK(s.contains("legend->AddEntry(graph__0_0, \"evaluated candidates\", \"p\");"));
        CHECK(s.contains("legend->AddEntry(sample, \"the landscape\", \"f\");"));
        CHECK(s.contains("legend entries: 2"));
        CHECK_FALSE(s.contains("BuildLegend"));
        // The legend comes after everything of the pad is drawn.
        CHECK(drawnBeforeLegend(s));
        CHECK(render(map, plotBackend::MATPLOTLIB).contains("], loc=\"lower right\")"));
        CHECK(render(map, plotBackend::OCTAVE)
                  .contains("legend(s_, {'the landscape', 'evaluated candidates'});"));
    }

    SECTION("a placed legend is sized for the labelled series only") {
        const auto map = map_under_points("", "evaluated candidates");
        map->setLegendPosition(legendPosition::BOTTOM_RIGHT);
        CHECK(map->legendEntries().size() == 1);
        CHECK(render(map, plotBackend::ROOT).contains("new TLegend(0.56, 0.13, 0.88, 0.225);"));
    }

    SECTION("a placed legend draws a label too long for its box smaller, ROOT's own one need not") {
        const auto map = map_under_points("the landscape", "evaluated candidates");
        map->setLegendPosition(legendPosition::TOP_LEFT);
        const std::string placed = render(map, plotBackend::ROOT);
        CHECK(placed.contains("widest_at(size)"));
        CHECK(placed.contains("size = TMath::Max(floor_size, size * room / widest);"));
        CHECK(placed.contains("legend->SetTextSize(size);"));
        // ROOT's own placement on a 2-d pad sizes the text to its box itself. (The legend's own
        // measuring code, not GetXsize(): the canvas label measures itself too.)
        auto first = decadesGraph(false, false);
        first->setPlotLabel("first");
        auto second = decadesGraph(false, false);
        second->setPlotLabel("second");
        first->registerSecondaryPlotter(second);
        CHECK_FALSE(render(first, plotBackend::ROOT).contains("widest_at"));
    }

    SECTION("a single series gets no legend, labelled or not") {
        auto single = smallSurface(surfaceProjection::NONE);
        single->setPlotLabel("a surface");
        CHECK(single->legendEntries().size() == 0);
        CHECK_FALSE(render(single, plotBackend::ROOT).contains("Legend"));
    }
}

/******************************************************************************/
// The legend of a pad is one description -- which series, in which order, with which text and
// which sample -- that every backend draws. A sample shows how its series is drawn: a line for a
// line, markers for points, both for a curve through points, the bars of a histogram, the middle
// colour of a colour map.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per case; the count comes from the assertion macros
TEST_CASE(
    "A legend entry's sample shows how its series is drawn in every backend",
    "[plotting][overlay][legend]"
) {
    SECTION("the sample follows the draw mode of every kind of series") {
        GPlotSpec g2(plotKind::graph_2d);
        g2.plot_mode = graphPlotMode::SCATTER;
        CHECK(legendSampleOf(drawStyleOf(g2)) == legendSample::MARKER);
        g2.plot_mode = graphPlotMode::CURVE;
        CHECK(legendSampleOf(drawStyleOf(g2)) == legendSample::LINE_AND_MARKER);
        g2.drawing_args = "L,same";
        CHECK(legendSampleOf(drawStyleOf(g2)) == legendSample::LINE);
        g2.drawing_args = "A*";
        CHECK(legendSampleOf(drawStyleOf(g2)) == legendSample::MARKER);
        GPlotSpec g3(plotKind::graph_3d);
        CHECK(legendSampleOf(drawStyleOf(g3)) == legendSample::MARKER);
        g3.plot_mode = graphPlotMode::CURVE;
        CHECK(legendSampleOf(drawStyleOf(g3)) == legendSample::LINE_AND_MARKER);
        g3.projection = surfaceProjection::FLOOR;
        CHECK(legendSampleOf(drawStyleOf(g3)) == legendSample::COLOUR_MAP);
        GPlotSpec surface(plotKind::graph_3d);
        surface.drawing_args = "SURF2Z";
        CHECK(legendSampleOf(drawStyleOf(surface)) == legendSample::COLOUR_MAP);
        CHECK(legendSampleOf(drawStyleOf(GPlotSpec(plotKind::graph_4d))) == legendSample::MARKER);
        CHECK(legendSampleOf(drawStyleOf(GPlotSpec(plotKind::hist_1d))) == legendSample::FILL);
        GPlotSpec h2(plotKind::hist_2d);
        CHECK(legendSampleOf(drawStyleOf(h2)) == legendSample::COLOUR_MAP); // no option: ROOT's COL
        h2.drawing_args = "COLZ";
        CHECK(legendSampleOf(drawStyleOf(h2)) == legendSample::COLOUR_MAP);
        h2.drawing_args = "BOX";
        CHECK(legendSampleOf(drawStyleOf(h2)) == legendSample::FILL);
        CHECK(legendSampleOf(drawStyleOf(GPlotSpec(plotKind::function_1d))) == legendSample::LINE);
    }

    SECTION("ROOT draws each sample from its series' object as the description says") {
        auto scatter = decadesGraph(false, false);
        scatter->setPlotMode(graphPlotMode::SCATTER);
        scatter->setPlotLabel("points");
        auto curve = decadesGraph(false, false);
        curve->setPlotMode(graphPlotMode::CURVE);
        curve->setPlotLabel("a curve");
        scatter->registerSecondaryPlotter(curve);
        const std::string s = render(scatter, plotBackend::ROOT);
        CHECK(s.contains("legend->AddEntry(graph_0, \"points\", \"p\");"));
        CHECK(s.contains("legend->AddEntry(graph_0_0, \"a curve\", \"lp\");"));
        CHECK_FALSE(s.contains("InheritsFrom"));
    }

    SECTION("a labelled 4-d graph laid over another is listed, in ROOT and in Octave") {
        const auto cloud = [](const std::string &label) {
            auto g = std::make_shared<GGraph4D>();
            (*g) & std::tuple<double, double, double, double>(0., 0., 0., 1.);
            (*g) & std::tuple<double, double, double, double>(1., 1., 1., 2.);
            (*g) & std::tuple<double, double, double, double>(2., 0., 1., 3.);
            g->setPlotLabel(label);
            return g;
        };
        auto first = cloud("first");
        first->registerSecondaryPlotter(cloud("second"));
        const std::string root = render(first, plotBackend::ROOT);
        CHECK(root.contains("legend->AddEntry(pm3d__0_1, \"first\", \"p\");"));
        CHECK(root.contains("legend->AddEntry(pm3d__0_0_1, \"second\", \"p\");"));
        CHECK_FALSE(
            root.contains("fr__0_0")
        ); // an overlay draws no frame of its own: the pad keeps its box
        const std::string oct = render(first, plotBackend::OCTAVE);
        CHECK(oct.contains("legend(s_, {'first', 'second'});"));
        CHECK_FALSE(oct.contains("legend('show')"));
    }

    SECTION("a labelled map without its surface is listed in matplotlib and Octave too") {
        auto map = smallSurface(surfaceProjection::FLOOR);
        map->setDrawSurface(false);
        map->setPlotLabel("the landscape");
        auto points = std::make_shared<GGraph3D>();
        (*points) & std::tuple<double, double, double>(0.5, 0.5, 3.0);
        points->setPlotLabel(" "); // white space names nothing
        map->registerSecondaryPlotter(points);
        REQUIRE(map->legendEntries().size() == 1);
        CHECK(map->legendEntries()[0].sample == legendSample::COLOUR_MAP);
        const std::string mpl = render(map, plotBackend::MATPLOTLIB);
        CHECK(mpl.contains(
            "ax.legend(handles=[_legend_sample(_colours[0], \"colour_map\", 0)], labels=[\"the "
            "landscape\"], loc=\"upper left\")"
        ));
        CHECK_FALSE(mpl.contains("ax.legend()"));
        const std::string oct = render(map, plotBackend::OCTAVE);
        // A filled sample is a square marker: a patch without data never reaches the gnuplot
        // toolkit's key.
        CHECK(oct.contains(
            "s_(end + 1) = plot3(NaN, NaN, NaN, 'LineStyle', 'none', 'Marker', 's', 'MarkerSize', "
            "10, 'MarkerFaceColor', cs_{1}"
        ));
        CHECK_FALSE(oct.contains("patch("));
        CHECK(oct.contains("legend(s_, {'the landscape'});"));
        const std::string root = render(map, plotBackend::ROOT);
        CHECK(root.contains("legend->AddEntry(sample, \"the landscape\", \"f\");"));
        CHECK(root.contains("legend entries: 1"));
    }

    SECTION("matplotlib and Octave draw a scatter plot as markers alone, as its sample shows") {
        auto scatter = decadesGraph(false, false);
        scatter->setPlotMode(graphPlotMode::SCATTER);
        scatter->setPlotLabel("points");
        auto curve = decadesGraph(false, false);
        curve->setPlotLabel("a curve");
        scatter->registerSecondaryPlotter(curve);
        const std::string mpl = render(scatter, plotBackend::MATPLOTLIB);
        CHECK(mpl.contains("marker=\"o\", linestyle=\"none\", label=\"points\""));
        CHECK(mpl.contains(
            "_legend_sample(_colours[0], \"marker\", 0), _legend_sample(_colours[1], "
            "\"line_and_marker\", 0)"
        ));
        const std::string oct = render(scatter, plotBackend::OCTAVE);
        CHECK(oct.contains("], 'o');"));
        CHECK(oct.contains("], '-o');"));
        CHECK_FALSE(oct.contains("DisplayName")); // a named object would enter the key twice
    }
}

/******************************************************************************/
// gnuplot and the data export (for an external renderer) follow the same legend description as
// ROOT, matplotlib and Octave: no key for a single series or none labelled, the listed series
// alone, and how each is drawn.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per backend; the count comes from the assertion macros
TEST_CASE(
    "gnuplot and the data export follow the legend description",
    "[plotting][overlay][legend]"
) {
    const auto pad = [] {
        auto first = decadesGraph(false, false);
        first->setPlotMode(graphPlotMode::SCATTER);
        first->setPlotLabel("first");
        auto second = decadesGraph(false, false); // no label
        first->registerSecondaryPlotter(second);
        return first;
    };

    SECTION("gnuplot names the listed series alone and shows no key where there is no legend") {
        const std::string gp = render(pad(), plotBackend::GNUPLOT);
        CHECK(gp.contains("with points pointtype 7 title \"first\""));
        CHECK(gp.contains("with linespoints notitle"));
        CHECK_FALSE(gp.contains("unset key"));
        auto single = decadesGraph(false, false);
        single->setPlotLabel("alone");
        const std::string one = render(single, plotBackend::GNUPLOT);
        CHECK(one.contains("unset key"));
        CHECK(one.contains("set key default"));
    }

    SECTION("the data export says which series the legend lists, with which sample") {
        GPlotDesigner gpd("data csv", 1, 1);
        gpd.setDataFormat(dataFormat::CSV);
        gpd.registerPlotter(pad());
        const std::string csv = gpd.plot();
        CHECK(csv.contains("secondary=0 draw_style=markers legend_sample=marker\n"));
        CHECK(csv.contains("secondary=1 draw_style=line+markers\n"));
    }
}

/******************************************************************************/
// Where a legend lies and how small its text may get. On a 3-d pad ROOT's own placement sees only
// the pad's 2-d grid and put the legend over the front-left axis; AUTO there is the band at the top
// left, in every backend the upper left. A label too long for its box is drawn smaller down to a
// readable size, and then the box grows away from its side of the pad.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per case; the count comes from the assertion macros
TEST_CASE(
    "A 3-d pad's legend lies at the upper left and a long label widens its box",
    "[plotting][overlay][legend]"
) {
    const auto map_under_points = [](const std::string &points_label) {
        auto map = smallSurface(surfaceProjection::FLOOR);
        map->setDrawSurface(false);
        map->setPlotLabel("the landscape");
        auto points = std::make_shared<GGraph3D>();
        (*points) & std::tuple<double, double, double>(0.5, 0.5, 3.0);
        points->setPlotLabel(points_label);
        map->registerSecondaryPlotter(points);
        return map;
    };

    SECTION("AUTO on a 3-d pad is the band at the top left, on a 2-d pad ROOT's own placement") {
        const auto map = map_under_points("evaluated candidates");
        const auto [x1, y1, x2, y2] = legendBox3D(2);
        CHECK(std::abs(y2 - 0.92) < 1e-12);
        CHECK(std::abs(y1 - 0.82) < 1e-12);
        CHECK(std::abs(x1 - 0.01) < 1e-12);
        const std::string root = render(map, plotBackend::ROOT);
        CHECK(root.contains(
            "TLegend *legend = new TLegend(0.01, 0.82, 0.35, 0.92); // a 3-d pad: the band at the "
            "top left"
        ));
        CHECK(root.contains("legend->SetTextSize(0.035);"));
        CHECK_FALSE(root.contains("new TLegend(0.3, 0.21)"));
        CHECK(render(map, plotBackend::MATPLOTLIB).contains("], loc=\"upper left\")"));
        CHECK(render(map, plotBackend::OCTAVE).contains("legend('location', 'northwest');"));
        auto first = decadesGraph(false, false);
        first->setPlotLabel("first");
        auto second = decadesGraph(false, false);
        second->setPlotLabel("second");
        first->registerSecondaryPlotter(second);
        CHECK(render(first, plotBackend::ROOT)
                  .contains("TLegend *legend = new TLegend(0.3, 0.21); // placed by ROOT"));
        CHECK_FALSE(render(first, plotBackend::MATPLOTLIB).contains("loc="));
    }

    SECTION("gnuplot puts the key of a 3-d pad at the top left") {
        auto points = std::make_shared<GGraph3D>();
        (*points) & std::tuple<double, double, double>(0.5, 0.5, 3.0);
        points->setPlotLabel("points");
        auto more = std::make_shared<GGraph3D>();
        (*more) & std::tuple<double, double, double>(0.2, 0.1, 1.0);
        more->setPlotLabel("more points");
        points->registerSecondaryPlotter(more);
        CHECK(render(points, plotBackend::GNUPLOT).contains("set key top left"));
    }

    SECTION("a label too long for its box: no smaller than the floor, and the box grows") {
        const std::string long_label(120, 'x');
        auto map = map_under_points(long_label);
        map->setLegendPosition(legendPosition::TOP_LEFT);
        const std::string left = render(map, plotBackend::ROOT);
        CHECK(left.contains("floor_size = 0.035;"));
        CHECK(left.contains(
            "legend->SetX2(TMath::Min(TMath::Max(x2, 1. - gPad->GetRightMargin() - 0.02), x1 + "
            "width));"
        ));
        CHECK(left.contains("e->SetLabel((text + \"...\").Data());"));
        map->setLegendPosition(legendPosition::BOTTOM_RIGHT);
        const std::string right = render(map, plotBackend::ROOT);
        CHECK(right.contains(
            "legend->SetX1(TMath::Max(TMath::Min(x1, gPad->GetLeftMargin() + 0.01), x2 - width));"
        ));
        CHECK(LEGENDMINTEXTSIZE < LEGENDTEXTSIZE);
    }
}

/******************************************************************************/
// One resolved description per series, GBasePlotter::drawStyle(): every backend takes both its
// drawing of a series and the series' legend sample from it, the data export carries it for the
// external renderer, and ROOT's draw option of a 2-d graph is made from it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per case; the count comes from the assertion macros
TEST_CASE(
    "Every backend draws a series and its legend sample from one draw style",
    "[plotting][overlay][legend][style]"
) {
    const auto errors = [](const std::string &label) {
        auto g = std::make_shared<GGraph2ED>();
        (*g) & std::tuple<double, double, double, double>(1., 0.1, 2., 0.2);
        (*g) & std::tuple<double, double, double, double>(2., 0.1, 3., 0.2);
        g->setPlotLabel(label);
        return g;
    };

    SECTION("an error-bar curve is drawn with its line and listed with it, in every backend") {
        auto first = errors("first");
        first->registerSecondaryPlotter(errors("second"));
        CHECK(first->drawStyle().line);
        CHECK(first->drawStyle().error_bars);
        const std::string mpl = render(first, plotBackend::MATPLOTLIB);
        CHECK(mpl.contains("fmt=\"-o\""));
        CHECK(mpl.contains("_legend_sample(_colours[0], \"line_and_marker\", 0)"));
        const std::string oct = render(first, plotBackend::OCTAVE);
        CHECK(oct.contains("], '-o');"));
        CHECK(render(first, plotBackend::GNUPLOT).contains("with xyerrorlines title \"first\""));
        CHECK(render(first, plotBackend::ROOT).contains("->Draw(\"APL\");"));
        GPlotDesigner gpd("data", 1, 1);
        gpd.setDataFormat(dataFormat::CSV);
        gpd.registerPlotter(first);
        CHECK(
            gpd.plot().contains("draw_style=line+markers+error_bars legend_sample=line_and_marker")
        );
    }

    SECTION("a line sample takes the series' line width in matplotlib and Octave") {
        auto first = decadesGraph(false, false);
        first->setPlotLabel("first");
        first->setLineWidth(2);
        auto second = decadesGraph(false, false);
        second->setPlotLabel("second");
        second->setLineWidth(2);
        first->registerSecondaryPlotter(second);
        CHECK(render(first, plotBackend::MATPLOTLIB)
                  .contains("_legend_sample(_colours[0], \"line_and_marker\", 2)"));
        CHECK(render(first, plotBackend::OCTAVE).contains("'Color', cs_{1}, 'LineWidth', 2);"));
    }

    SECTION("a 2-d histogram's own option and a graph's arrows reach the draw style") {
        GHistogram2D boxes(10, 10);
        CHECK(boxes.drawStyle().colour_map); // no option: ROOT draws COL
        boxes.set2DOpt(tddropt::BOX);
        CHECK(boxes.drawStyle().fill); // boxes in the histogram's colours
        CHECK_FALSE(isThreeDimensional(boxes.plotSpec()));
        GHistogram2D surface(10, 10);
        surface.set2DOpt(tddropt::SURFONE);
        CHECK(surface.drawStyle().colour_map);
        CHECK(isThreeDimensional(surface.plotSpec()));
        auto arrows = decadesGraph(false, false);
        arrows->setDrawArrows(true);
        CHECK(arrows->plotSpec().toJson().contains("\"arrows\": true"));
        CHECK_FALSE(arrows->drawStyle().line); // the arrows join the points, ROOT draws them alone
        CHECK(legendSampleOf(arrows->drawStyle()) == legendSample::MARKER);
        CHECK(makePlotter(arrows->plotSpec())->drawStyle().arrows);
    }

    SECTION("a 3-d curve's sample is an object of its own; the drawn graph keeps its line") {
        auto map = smallSurface(surfaceProjection::NONE);
        map->setPlotLabel("points");
        auto curve = std::make_shared<GGraph3D>();
        (*curve) & std::tuple<double, double, double>(0., 0., 1.);
        (*curve) & std::tuple<double, double, double>(1., 1., 2.);
        curve->setDrawLines(true);
        curve->setPlotLabel("a curve");
        map->registerSecondaryPlotter(curve);
        const std::string root = render(map, plotBackend::ROOT);
        // Only the poly-line has width 3, not the drawn graph.
        CHECK(root.contains("lines_graph__0_0->SetLineWidth(3);"));
        CHECK_FALSE(root.contains("\ngraph__0_0->SetLineWidth(3);"));
        CHECK_FALSE(root.contains(" graph__0_0->SetLineWidth(3);"));
        CHECK(root.contains("TGraph *legend_graph__0_0 = new TGraph();"));
        CHECK(root.contains("legend->AddEntry(legend_graph__0_0, \"a curve\", \"lp\");"));
    }

    SECTION("a series that names no ROOT object gets a sample object in its colour") {
        std::ostringstream out;
        emitRootLegend(
            out,
            "",
            legendPosition::TOP_LEFT,
            {{"", "mine", legendSample::LINE, 1, 3}},
            false
        );
        const std::string s = out.str();
        CHECK(s.contains("sample->SetLineColor(2);"));
        CHECK(s.contains("sample->SetLineWidth(3);"));
        CHECK(s.contains("legend->AddEntry(sample, \"mine\", \"l\");"));
    }
}

/******************************************************************************/
// The band of a 3-d pad has a bottom: five entries reach it, six or seven share it at a second,
// lower text size, and more are summed up in a last row.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per case; the count comes from the assertion macros
TEST_CASE(
    "The band of a 3-d pad's legend keeps its bottom for many series",
    "[plotting][overlay][legend]"
) {
    CHECK(std::abs(std::get<1>(legendBox3D(5)) - 0.70) < 1e-12);
    CHECK(std::abs(std::get<1>(legendBox3D(9)) - 0.70) < 1e-12);
    CHECK(legendTextSize3D(5) == LEGENDMINTEXTSIZE);
    CHECK(legendTextSize3D(6) == LEGENDCROWDEDTEXTSIZE);

    auto primary = smallSurface(surfaceProjection::NONE);
    primary->setPlotLabel("series 0");
    for(int i = 1; i < 9; ++i) {
        auto more = std::make_shared<GGraph3D>();
        (*more) & std::tuple<double, double, double>(0.1 * i, 0.2, 1.);
        more->setPlotLabel("series " + std::to_string(i));
        primary->registerSecondaryPlotter(more);
    }
    REQUIRE(primary->legendEntries().size() == 9);
    const std::string root = render(primary, plotBackend::ROOT);
    CHECK(
        root.contains("new TLegend(0.01, 0.7, 0.35, 0.92); // a 3-d pad: the band at the top left")
    );
    CHECK(root.contains("legend->SetTextSize(0.025);"));
    CHECK(root.contains("\"series 5\""));
    CHECK_FALSE(root.contains("\"series 6\", "));
    CHECK(root.contains("legend->AddEntry((TObject *)nullptr, \"and 3 more series\", \"\");"));
    CHECK(root.contains("legend entries: 7"));
}

/******************************************************************************/
// ROOT's own placement of a 2-d legend does not avoid data; FREEST puts it in the corner that
// covers the fewest of the pad's points and line samples, in every backend.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per case; the count comes from the assertion macros
TEST_CASE(
    "FREEST puts a 2-d legend in the corner its data leave most free",
    "[plotting][overlay][legend]"
) {
    SECTION("the freest corner of points and a curve") {
        // A cloud along the top and a curve through the lower right: the lower right, the first
        // preference, is covered, the middle of the right side, the second, is free.
        std::vector<std::pair<double, double>> cloud;
        for(int i = 0; i <= 40; ++i) {
            cloud.emplace_back(i, 8. + 0.05 * (i % 7));
        }
        const std::vector<std::vector<std::pair<double, double>>> curve{{{20., 0.}, {40., 0.5}}};
        CHECK(freestLegendCorner(curve, {cloud}, 2) == legendPosition::CENTER_RIGHT);
        // A point in the lower left instead of the curve: the lower right is free.
        auto with_point = cloud;
        with_point.emplace_back(0., 0.);
        CHECK(freestLegendCorner({}, {with_point}, 2) == legendPosition::BOTTOM_RIGHT);
        // Covered everywhere: the corner that covers the fewest.
        std::vector<std::pair<double, double>> everywhere;
        for(int i = 0; i <= 40; ++i) {
            for(int j = 0; j <= 40; ++j) {
                if(not(i > 30 and j > 30)) { // the top right is left a little freer
                    everywhere.emplace_back(i, j);
                }
            }
        }
        CHECK(freestLegendCorner({}, {everywhere}, 2) == legendPosition::TOP_RIGHT);
        // A box that grows for a long label is judged at its grown width: points at the left of the
        // middle of the right side leave it free for a narrow box only.
        std::vector<std::pair<double, double>> left_of_middle{{0., 0.}, {40., 10.}};
        for(int i = 10; i <= 20; ++i) {
            left_of_middle.emplace_back(i, 5.);
        }
        const std::vector<std::vector<std::pair<double, double>>> bottom_curve{
            {{0., 0.}, {40., 0.}}
        };
        CHECK(
            freestLegendCorner(bottom_curve, {left_of_middle}, 2) == legendPosition::CENTER_RIGHT
        );
        CHECK(
            freestLegendCorner(bottom_curve, {left_of_middle}, 2, false, false, 0.75) !=
            legendPosition::CENTER_RIGHT
        );
        // A vertex of a curve weighs as much as a marker: a box over a piece of line with two
        // vertices is freer than one over ten markers.
        std::vector<std::pair<double, double>> right_cloud{{0., 0.}, {40., 10.}};
        for(int i = 0; i < 10; ++i) {
            right_cloud.emplace_back(35. + 0.3 * i, 1.); // the lower right
            right_cloud.emplace_back(35. + 0.3 * i, 5.); // the middle of the right side
            right_cloud.emplace_back(2. + 0.3 * i, 9.);  // the top left
            right_cloud.emplace_back(35. + 0.3 * i, 9.); // the top right
        }
        const std::vector<std::vector<std::pair<double, double>>> low_left{{{0., 0.5}, {8., 0.5}}};
        CHECK(freestLegendCorner(low_left, {right_cloud}, 2) == legendPosition::BOTTOM_LEFT);
        // A segment that crosses a box without a vertex inside counts like a marker: a box it
        // crosses is no freer than one over a single marker (the first of them in the order of
        // preference is taken), however little of the segment lies inside.
        const std::vector<std::vector<std::pair<double, double>>> crossing{
            {{30., 3.6}, {30., 6.4}}
        };
        const std::vector<std::vector<std::pair<double, double>>> markers{
            {{0., 0.},
             {40., 10.},
             {38., 1.}, // the range; one marker in the lower right
             {2., 9.},
             {3., 9.},
             {4., 9.},
             {2., 1.},
             {3., 1.}, // three in the top left and lower left,
             {4., 1.},
             {36., 9.},
             {37., 9.},
             {38., 9.}}
        }; // and in the top right
        CHECK(freestLegendCorner(crossing, markers, 2) == legendPosition::BOTTOM_RIGHT);
    }

    SECTION("every backend places a FREEST legend in that corner; a 3-d pad keeps its band") {
        auto points = std::make_shared<GGraph2D>();
        points->setPlotMode(graphPlotMode::SCATTER);
        points->setPlotLabel("points");
        auto line = std::make_shared<GGraph2D>();
        line->setPlotLabel("a line");
        for(int i = 0; i <= 40; ++i) {
            (*points) & std::tuple<double, double>(i, 8. + 0.05 * (i % 7));
        }
        (*line) & std::tuple<double, double>(20., 0.);
        (*line) & std::tuple<double, double>(40., 0.5);
        points->registerSecondaryPlotter(line);
        points->setLegendPosition(legendPosition::FREEST);
        CHECK(points->getLegendPosition() == legendPosition::FREEST);
        CHECK(points->legendPlace() == legendPosition::CENTER_RIGHT);
        CHECK(render(points, plotBackend::ROOT).contains("new TLegend(0.56, 0.42, 0.88, 0.58);"));
        CHECK(render(points, plotBackend::MATPLOTLIB).contains("], loc=\"center right\")"));
        CHECK(render(points, plotBackend::OCTAVE).contains("legend('location', 'east');"));
        CHECK(render(points, plotBackend::GNUPLOT).contains("set key right center"));
        CHECK(points->plotSpec().toJson().contains("\"legend\": \"freest\""));

        auto map = smallSurface(surfaceProjection::NONE);
        map->setPlotLabel("a surface");
        auto more = smallSurface(surfaceProjection::NONE);
        more->setPlotLabel("another");
        map->registerSecondaryPlotter(more);
        map->setLegendPosition(legendPosition::FREEST);
        CHECK(map->legendPlace() == legendPosition::AUTO);
        CHECK(render(map, plotBackend::ROOT).contains("// a 3-d pad: the band at the top left"));
    }
}

/******************************************************************************/
// 4-d graphs laid over each other share the primary's frame, which holds all their points; every
// marker paints its one point only.
TEST_CASE(
    "Overlaid 4-d graphs share one frame that holds all their points",
    "[plotting][overlay]"
) {
    auto first = std::make_shared<GGraph4D>();
    (*first) & std::tuple<double, double, double, double>(0., 0., 0., 1.);
    (*first) & std::tuple<double, double, double, double>(2., 1., 1., 2.);
    auto second = std::make_shared<GGraph4D>();
    (*second) & std::tuple<double, double, double, double>(3., -1., 4., 1.);
    (*second) & std::tuple<double, double, double, double>(1., 0.5, 0.5, 3.);
    first->registerSecondaryPlotter(second);
    const std::string root = render(first, plotBackend::ROOT);
    CHECK(root.contains("new TH3F(\"fr__0\",\"fr__0\",10, 0, 3, 10, -1, 1, 10, 0, 4);"));
    CHECK_FALSE(root.contains("fr__0_0"));
    CHECK(root.contains("pm3d__0_1->SetPoint(0, "));
    CHECK(root.contains("pm3d__0_0_1->SetPoint(0, "));
    CHECK_FALSE(root.contains("->SetPoint(1, "));
}

/******************************************************************************/
// A figure whose data leave no corner free makes room for its legend: the frame leaves the legend a
// band of its own below (or above) the data.
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one section per case; the count comes from the assertion macros
TEST_CASE("A pad can leave its legend a band of its own", "[plotting][overlay][legend]") {
    auto first = decadesGraph(false, false);
    first->setPlotLabel("first");
    auto second = decadesGraph(false, false);
    second->setPlotLabel("second");
    first->registerSecondaryPlotter(second);
    first->setLegendPosition(legendPosition::BOTTOM_RIGHT);
    CHECK_FALSE(render(first, plotBackend::ROOT).contains("the legend's band"));
    first->setLegendRoom(true);
    const std::string below = render(first, plotBackend::ROOT);
    CHECK(below.contains("the legend's band: no data under the legend, by construction"));
    CHECK(below.contains("const double edge = 0.31; // of the pad"));
    CHECK(below.contains("if(new_lo < lo) first->SetMinimum(g(new_lo));"));
    CHECK(below.contains("// legend room: 1"));
    first->setLegendPosition(legendPosition::TOP_LEFT);
    const std::string above = render(first, plotBackend::ROOT);
    CHECK(above.contains("const double edge = 0.7; // of the pad"));
    CHECK(above.contains("if(new_hi > hi) first->SetMaximum(g(new_hi));"));
    // No band where the legend lies in no corner.
    first->setLegendPosition(legendPosition::CENTER_RIGHT);
    CHECK_FALSE(render(first, plotBackend::ROOT).contains("the legend's band"));
    // It travels through the spec.
    first->setLegendPosition(legendPosition::BOTTOM_RIGHT);
    CHECK(first->plotSpec().toJson().contains("\"legend_room\": true"));
    CHECK(makePlotter(first->plotSpec())->legendRoom());
}

/******************************************************************************/
// A 2-d histogram rebuilt from its plot spec keeps its set2DOpt() choice as a choice: as an overlay
// it is still drawn into the pad of its primary.
TEST_CASE(
    "A 2-d histogram rebuilt from its spec keeps its option and its place as an overlay",
    "[plotting][overlay]"
) {
    auto primary = std::make_shared<GHistogram2D>(10, 10);
    primary->set2DOpt(tddropt::BOX);
    (*primary) & std::tuple<double, double>(0., 0.);
    (*primary) & std::tuple<double, double>(1., 1.);
    auto original = std::make_shared<GHistogram2D>(10, 10);
    original->set2DOpt(tddropt::BOX);
    auto rebuilt = makePlotter(original->plotSpec());
    auto *h2 = dynamic_cast<GHistogram2D *>(rebuilt.get());
    REQUIRE(h2 != nullptr);
    (*h2) & std::tuple<double, double>(0.5, 0.5);
    (*h2) & std::tuple<double, double>(0.2, 0.8);
    CHECK(h2->get2DOpt() == tddropt::BOX);
    CHECK(h2->plotSpec().drawing_args == "BOX");
    primary->registerSecondaryPlotter(std::shared_ptr<GBasePlotter>(std::move(rebuilt)));
    const std::string root = render(primary, plotBackend::ROOT);
    CHECK(root.contains("->Draw(\"BOX,same\");"));
}

/******************************************************************************/
// The canvas label of a ROOT figure has one text size, whatever its letters: ROOT's own size follows
// the height of the label's letters, so a label without descenders came out smaller than one with
// them. The size is set, and only a label too wide for its box is drawn smaller.
TEST_CASE(
    "The canvas label of a ROOT figure has one text size whatever its letters",
    "[plotting]"
) {
    auto g = std::make_shared<GGraph2D>();
    (*g) & std::tuple<double, double>(1.0, 2.0);
    GPlotDesigner gpd("nm on the Zakharov function", 1, 1);
    gpd.registerPlotter(g);

    const std::string s = gpd.plot();
    std::ostringstream size;
    size.imbue(std::locale::classic());
    size << "double size = " << CANVASLABELTEXTSIZE << ";";
    const auto label = s.find("new TPaveLabel(");
    const auto set = s.find("canvasTitle->SetTextSize(size);");
    const auto draw = s.find("canvasTitle->Draw();");
    REQUIRE(label != std::string::npos);
    CHECK(s.contains(size.str()));
    REQUIRE(set != std::string::npos);
    REQUIRE(draw != std::string::npos);
    CHECK(label < set);
    CHECK(set < draw);
    // Without a canvas label there is nothing to size.
    gpd.setShowCanvasLabel(false);
    CHECK_FALSE(gpd.plot().contains("canvasTitle"));
}

/******************************************************************************/
// writeToFile() did not look at its stream: a file it could not open (a directory that does not
// exist, an empty name) was left unwritten without a word, and the caller believed the plot saved.
TEST_CASE("writeToFile refuses a file it cannot write", "[plotting]") {
    auto g = std::make_shared<GGraph2D>();
    (*g) & std::tuple<double, double>(1.0, 2.0);
    GPlotDesigner gpd("a figure", 1, 1);
    gpd.registerPlotter(g);

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("dietrich-write-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    CHECK_THROWS_AS(gpd.writeToFile(dir / "no-such-directory" / "figure.C"), gem_exception);
    CHECK_THROWS_AS(gpd.writeToFile(std::filesystem::path{}), gem_exception);

    // A file that can be written is written in full.
    const std::filesystem::path file = dir / "figure.C";
    gpd.writeToFile(file);
    REQUIRE(std::filesystem::exists(file));
    CHECK(std::filesystem::file_size(file) == gpd.plot(file).size());
    std::filesystem::remove_all(dir);
}

/******************************************************************************/
// ROOT puts an axis title at the far end of its axis. In a 3-d graph the x title then ran into the
// colour palette of a projected surface and the y title into the tick labels of its own axis. The
// titles of the two floor axes are centred and set further out.
TEST_CASE("The floor-axis titles of a 3-d graph are centred and clear of the ticks", "[plotting]") {
    for(const auto projection : {surfaceProjection::NONE, surfaceProjection::FLOOR}) {
        auto g = std::make_shared<GGraph3D>();
        for(int i = 0; i < 3; ++i) {
            for(int j = 0; j < 3; ++j) {
                (*g) & std::tuple<double, double, double>(i, j, i * j);
            }
        }
        g->setSurfaceProjection(projection);
        g->setXAxisLabel("input 1");
        g->setYAxisLabel("input 2");
        const std::string s = rootMacroOf(g);
        CHECK(s.contains("->GetXaxis()->CenterTitle();"));
        CHECK(s.contains("->GetYaxis()->CenterTitle();"));
        CHECK(s.contains("->GetXaxis()->SetTitleOffset(2);"));
        CHECK(s.contains("->GetYaxis()->SetTitleOffset(2.5);"));
    }
}

/******************************************************************************/
// ROOT's own default marker is a dot of one pixel: the points of a scatter plot came out faint and
// the legend sample, which copies the marker, invisible. A scatter plot -- also one with error bars
// -- now draws small filled circles; a curve keeps its line without them, and a draw option of the
// plotter's own that names ROOT's star marker keeps its stars.
TEST_CASE("A 2-d scatter plot draws visible markers in ROOT", "[plotting]") {
    const std::string marker = "->SetMarkerStyle(20);";
    const std::string size = "->SetMarkerSize(0.5);";

    auto scatter = std::make_shared<GGraph2D>();
    scatter->setPlotMode(graphPlotMode::SCATTER);
    (*scatter) & std::tuple<double, double>(1.0, 2.0);
    (*scatter) & std::tuple<double, double>(2.0, 3.0);
    const std::string s = rootMacroOf(scatter);
    CHECK(s.contains(marker));
    CHECK(s.contains(size));

    auto with_errors = std::make_shared<GGraph2ED>();
    with_errors->setPlotMode(graphPlotMode::SCATTER);
    (*with_errors) & std::tuple<double, double, double, double>(1.0, 0.1, 2.0, 0.2);
    (*with_errors) & std::tuple<double, double, double, double>(3.0, 0.3, 4.0, 0.4);
    const std::string e = rootMacroOf(with_errors);
    CHECK(e.contains(marker));
    CHECK(e.contains(size));

    auto curve = std::make_shared<GGraph2D>();
    curve->setPlotMode(graphPlotMode::CURVE);
    (*curve) & std::tuple<double, double>(1.0, 2.0);
    (*curve) & std::tuple<double, double>(2.0, 3.0);
    CHECK_FALSE(rootMacroOf(curve).contains(size));

    // A draw option of the plotter's own that names ROOT's star marker keeps its stars as they were
    auto stars = std::make_shared<GGraph2D>();
    stars->setPlotMode(graphPlotMode::SCATTER);
    stars->setDrawingArguments("A*");
    (*stars) & std::tuple<double, double>(1.0, 2.0);
    (*stars) & std::tuple<double, double>(2.0, 3.0);
    CHECK_FALSE(rootMacroOf(stars).contains(size));
}
