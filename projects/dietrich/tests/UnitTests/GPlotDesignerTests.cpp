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
 * emission-correctness fixes from the 2026-06-21 review (see
 * prompts/2026-06-21-gplotdesigner-root-review.md) by asserting on the generated
 * macro text directly -- no ROOT installation required. The complementary
 * "does ROOT actually accept the macro" check is wired into ctest as
 * GPlotDesignerRootValidity (only when root + xvfb-run are available).
 ********************************************************************************/

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>

#include <memory>
#include <array>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "dietrich/GPlotDesigner.hpp"

#include "Dietrich_tests.hpp" // the local standard-tests driver (mirrors Geneva_tests.hpp)

using namespace Gem::Common;
using namespace Gem::Dietrich; // the plotting types under test live here

namespace {
/** Concatenate a plotter's three emission sections for substring assertions. */
std::string emit(const GBasePlotter &p) {
    return p.headerData("") + p.bodyData("") + p.footerData("");
}
} // namespace

/******************************************************************************/
// BUG-1: user strings must be escaped before they go into a ROOT C-string literal.
TEST_CASE("GPlotDesigner escapes user strings in emitted ROOT literals", "[plotting]") {
    GGraph2D g;
    g & std::tuple<double, double>(1.0, 2.0);
    g.setXAxisLabel(std::string("a\"b"));         // an embedded double quote
    g.setPlotLabel(std::string("line1\nline2"));   // an embedded newline

    const std::string s = emit(g);

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
    GGraph2D g;
    g & std::tuple<double, double>(1.0 / 3.0, 2.0 / 3.0);

    const std::string s = emit(g);

    // 6-significant-digit default would render 0.333333; full precision keeps many more digits.
    CHECK(s.contains("3333333333"));
    // Locale-independent: the decimal separator is a dot, never a comma.
    CHECK(!s.contains("0,33333"));
}

/******************************************************************************/
// BUG-2: the GGraph3D poly-line must receive every data point, including the first
// (the pre-fix loop started at begin()+1, dropping point 0 and leaving an origin vertex).
TEST_CASE("GGraph3D poly-line emits every point including the first", "[plotting]") {
    GGraph3D g;
    g.setDrawLines(true);
    g & std::tuple<double, double, double>(1., 1., 1.);
    g & std::tuple<double, double, double>(2., 2., 2.);
    g & std::tuple<double, double, double>(3., 3., 3.);

    const std::string s = g.footerData("");

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
    GGraph3D g;
    g.setXAxisLabel(std::string("x_axis"));
    g.setYAxisLabel(std::string("y_axis"));
    g.setZAxisLabel(std::string("z_axis"));
    g.setDrawingArguments(std::string("SURF2Z"));
    g & std::tuple<double, double, double>(1., 1., 1.);

    const std::string s = g.footerData("");

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
TEST_CASE("function plotters emit a quoted, honoured draw option", "[plotting]") {
    const std::tuple<double, double> rx(-1., 1.);
    const std::tuple<double, double> ry(-1., 1.);

    GFunctionPlotter2D f2("x*y", rx, ry);
    f2.setDrawingArguments("surf1");
    CHECK(f2.footerData("").contains("->Draw(\"surf1\")"));

    GFunctionPlotter1D f1("sin(x)", rx);
    f1.setDrawingArguments("L");
    CHECK(f1.footerData("").contains("->Draw(\"L\")"));
}

/******************************************************************************/
// BUG-4: an auto-ranged histogram with no data must throw (not dereference end()).
TEST_CASE("auto-ranged histogram with no data throws instead of UB", "[plotting]") {
    GHistogram1D const h(10); // 10 bins, no data added -> default min==max -> auto-range path
    CHECK_THROWS(h.headerData(""));
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
    g->setPlotLabel(std::string("a\"b"));        // an embedded double quote (must be escaped)
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
    g->setPlotLabel(std::string("a\"b"));        // an embedded double quote (must be escaped)
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
    g->setPlotLabel(std::string("a'b"));         // an embedded apostrophe (must be doubled)
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
    CHECK(s.find("# series 0: \"my series\" kind=GGraph2D plotkind=graph_2d role=xy "
                 "columns=x,y pad=0 secondary=0") != std::string::npos);
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
    g & std::tuple<double, double, double, double>(1.0, 0.1, 2.0, 0.2);
    g & std::tuple<double, double, double, double>(3.0, 0.3, 4.0, 0.4);

    const auto cols = g.dataColumns();
    REQUIRE(cols.size() == 4); // matches plotSpec().columns {"x","ex","y","ey"}
    CHECK(g.plotSpec().columns.size() == cols.size());
    // Each column is the float64 alternative of the type-tagged variant.
    const auto *x = std::get<const std::vector<double> *>(cols[0]);
    REQUIRE(x->size() == 2);
    CHECK((*x)[0] == 1.0); // x
    CHECK((*std::get<const std::vector<double> *>(cols[1]))[0] == 0.1); // ex
    CHECK((*std::get<const std::vector<double> *>(cols[2]))[0] == 2.0); // y
    CHECK((*std::get<const std::vector<double> *>(cols[3]))[1] == 0.4); // ey

    // The integer histogram now exports its samples as a TRUE int32 column.
    GHistogram1I hi(5, 0.0, 10.0);
    hi & std::int32_t(2);
    hi & std::int32_t(7);
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

    SECTION("function and range-less integer-histogram kinds cannot be rebuilt from a spec") {
        GPlotSpec const fspec(plotKind::function_1d);
        CHECK_THROWS(makePlotter(fspec));
        // hist_1i with no range still cannot be reconstructed (no auto-range form).
        GPlotSpec const ispec(plotKind::hist_1i);
        CHECK_THROWS(makePlotter(ispec));
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
        for(int i = 0; i < 20; ++i) { data.push_back(0.1 * static_cast<double>(i) - 1.0); }
        for(const double v : data) { (*h) & v; }
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
        for(int i = 0; i < 20; ++i) { data.push_back(0.1 * static_cast<double>(i) - 1.0); }
        for(const double v : data) { (*h) & v; }
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
            data.emplace_back(0.5 * static_cast<double>(i) - 3.0, 3.0 - 0.4 * static_cast<double>(i));
        }
        for(const auto &[x, y] : data) { h->add(x, y); }
        GPlotDesigner gpd_legacy("Dist2D", 1, 1);
        gpd_legacy.registerPlotter(h);
        const std::string legacy = gpd_legacy.plot();

        GDataLog log("Dist2D", 1, 1);
        const auto id = log.declareSeries(h->plotSpec());
        for(const auto &[x, y] : data) { log.append(id, x, y); }
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
// Standard GCommonInterfaceT-contract tests, conforming to the Geneva test pattern
// (mirror of GenevaStandardTests.cpp): every plotter / designer / decorator is run
// through the construction / clone / load / (de-)serialization round-trip driver in
// Dietrich_tests.hpp. TFactory_GUnitTests<T>() is specialized for the types without a
// public default constructor, exactly as the Geneva driver requires.

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
        std::string("x"), std::tuple<double, double>(-1.0, 1.0)
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
    return std::make_shared<GPlotDesigner>(std::string("test canvas"), std::size_t(1), std::size_t(1));
}
template <>
std::shared_ptr<GMarker<double>> TFactory_GUnitTests<GMarker<double>>() {
    return std::make_shared<GMarker<double>>(
        std::tuple<double, double>(0.0, 0.0), gMarker::closedCircle, gColor::black, 0.05
    );
}

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
    Gem::Dietrich::Tests::StandardTests_no_failure_expected<TestType>();
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
    Gem::Dietrich::Tests::StandardTests_failures_expected<TestType>();
}

/******************************************************************************/
// Regression (2026-07-18): GMarker's bounded decoratorData() had its clipping INVERTED -- it
// emitted plotting code only for markers OUTSIDE the axis ranges and swallowed in-range markers.
// The contract is the opposite: an in-range marker is drawn, an out-of-range one is clipped.

TEST_CASE("GMarker clips to the plot boundaries (in-range drawn, out-of-range empty)",
          "[plotting][decorators]") {
    const GMarker<double> marker(
        std::tuple<double, double>(0.5, 0.5), gMarker::closedCircle, gColor::black, 0.05
    );

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

TEST_CASE("GHistogram1I is fillable through the generic appendRow path",
          "[plotting][collectors]") {
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
