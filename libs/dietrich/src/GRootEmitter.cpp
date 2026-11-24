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

#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "dietrich/GPlotDesigner.hpp"
#include "dietrich/plotting/detail/GPlotDetail.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <istream>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace Gem::Dietrich {

// The plotting library builds on common's facilities (logging, serialization helpers,
// exception types, make_member, EmitStream, ...); make them visible here without
// per-name qualification. This affects lookup only within Gem::Dietrich.
using namespace Gem::Common;
// Dietrich declares its own to_string(plotKind), which would otherwise shadow common's
// numeric to_string(...) for unqualified calls; merge common's overloads back in.
using Gem::Common::to_string;

namespace {

/** @brief The offset of the x-axis title of a 3-d graph from its axis (ROOT's unit: the title size) */
constexpr double GRAPH3DXTITLEOFFSET = 2.0;
/** @brief The offset of the y-axis title of a 3-d graph; its tick labels stand further out */
constexpr double GRAPH3DYTITLEOFFSET = 2.5;

/******************************************************************************/
/**
 * One series of a pad as the ROOT macro draws it: what its plotter reports -- its plot spec, its
 * columns, its draw style and its data-structure marker -- and where it stands in the pad. The
 * macro is written from this alone, as the other backends write their scripts.
 */
struct rootSeries {
    const GBasePlotter *plotter = nullptr; ///< the series' plotter (its draw style and marker)
    GPlotSpec spec;                        ///< what the plotter reports (GBasePlotter::plotSpec())
    std::vector<GPlotColumn> columns;      ///< its data (GBasePlotter::dataColumns())
    bool secondary = false;                ///< an overlay of the pad's primary plotter
    std::size_t parent_id = 0;             ///< the id of the pad's primary plotter (an overlay's)
    std::size_t own_id = 0;  ///< the primary's id, or the overlay's place among the overlays
    bool shares_pad = false; ///< the pad holds several series
};

/**
 * @param s A series
 * @return The suffix that makes the names of its objects in the macro unique: "_<id>" for a primary
 *         plotter, "_<parent id>_<place>" for an overlay
 */
std::string suffixOf(const rootSeries &s) {
    if(not s.secondary) {
        return "_" + to_string(s.own_id);
    }
    return "_" + to_string(s.parent_id) + "_" + to_string(s.own_id);
}

/**
 * @param s A series
 * @return The number of its data points (0 for a function)
 */
std::size_t pointsOf(const rootSeries &s) {
    return s.columns.empty() ? 0 : columnSize(s.columns[0]);
}

/**
 * @param s A series
 * @param i The index of one of its columns
 * @return The column, which must hold doubles
 */
const std::vector<double> &doublesOf(const rootSeries &s, std::size_t i) {
    if(i >= s.columns.size() or
       not std::holds_alternative<const std::vector<double> *>(s.columns[i])) {
        throw gem_exception(
            g_error_streamer() << "GRootEmitter: column " << i << " of "
                               << s.plotter->getPlotterName() << " (kind \""
                               << to_string(s.spec.kind) << "\") is missing or holds no doubles"
                               << '\n'
        );
    }
    return *std::get<const std::vector<double> *>(s.columns[i]);
}

/**
 * Writes the data-structure marker of a series as a comment line of its own, if it has one.
 *
 * @param out The macro text
 * @param s The series
 */
void markerLine(std::ostream &out, const rootSeries &s) {
    if(not s.plotter->dsMarker().empty()) {
        out << "// " + rootEscape(s.plotter->dsMarker()) << '\n';
    }
}

/**
 * @param s A 3-d graph
 * @return true if a line joins its points
 */
bool curveOf(const rootSeries &s) {
    return s.spec.plot_mode == graphPlotMode::CURVE and pointsOf(s) >= 2;
}

/**
 * The number of columns ROOT reads for a plot kind; a plotter that reports fewer cannot be drawn.
 *
 * @param kind A plot kind
 * @return The number of data columns its series needs
 */
std::size_t columnsNeeded(plotKind kind) {
    switch(kind) {
    case plotKind::graph_2d:
    case plotKind::hist_2d:
        return 2;
    case plotKind::graph_2d_err:
    case plotKind::graph_4d:
        return 4;
    case plotKind::graph_3d:
        return 3;
    case plotKind::hist_1d:
    case plotKind::hist_1i:
        return 1;
    case plotKind::function_1d:
    case plotKind::function_2d:
        return 0;
    }
    return 0;
}

/**
 * The automatic range of a histogram axis: from the smallest value to one bin width above the
 * largest, since ROOT's upper edge is exclusive -- a range that ended at the largest value would put
 * every entry equal to it into the overflow bin, which is not drawn. One value alone gets the range
 * [value - 0.5, value + 0.5].
 *
 * @param column The values of the axis (not empty)
 * @param n_bins The number of bins of the axis
 * @return The lower and the upper edge
 */
std::tuple<double, double> rangeHolding(const GPlotColumn &column, std::size_t n_bins) {
    const auto [lo, hi] = columnRange(column);
    if(hi <= lo) {
        return {lo - 0.5, hi + 0.5};
    }
    return {lo, hi + ((hi - lo) / static_cast<double>(std::max<std::size_t>(n_bins, 1)))};
}

/**
 * Refuses a histogram without data whose range is to come from its data.
 *
 * @param s A histogram
 */
void requireData(const rootSeries &s) {
    if(pointsOf(s) == 0) {
        throw gem_exception(
            g_error_streamer() << "Cannot determine the data range of the empty histogram \""
                               << s.spec.name << "\"; give it a range or data" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * The name of the object in the macro whose look the legend sample of a series copies: the graph,
 * histogram or function its footer draws (a 4-d graph: one of its markers; a 3-d curve: a sample
 * object of its own, never drawn).
 *
 * @param s The series
 * @return The name of the object
 */
std::string legendObjectOf(const rootSeries &s) {
    const std::string suffix = suffixOf(s);
    switch(s.spec.kind) {
    case plotKind::graph_2d:
        return "graph" + suffix;
    case plotKind::graph_2d_err:
        return "graph_" + suffix;
    case plotKind::graph_3d:
        // A curve through the points has a sample object of its own (see graph3DFooter()).
        return (curveOf(s) ? "legend_graph_" : "graph_") + suffix;
    case plotKind::graph_4d:
        // A 4-d graph draws one marker per point, the best one larger and blue; an ordinary point
        // is the sample, unless only the best one is drawn.
        return "pm3d_" + suffix + (s.spec.n_best.value_or(0) == 1 ? "_0" : "_1");
    case plotKind::hist_1d:
        return "histD" + suffix;
    case plotKind::hist_1i:
        return "histI" + suffix;
    case plotKind::hist_2d:
        return "hist2D" + suffix;
    case plotKind::function_1d:
        return "func1D" + suffix;
    case plotKind::function_2d:
        return "func2D" + suffix;
    }
    return {};
}

/**
 * ROOT's draw option of a series: the plotter's own drawing arguments, or one made from its draw
 * style or its kind, with "same" for an overlay where ROOT needs it.
 *
 * @param s The series
 * @return The draw option
 */
std::string drawOptionOf(const rootSeries &s) {
    const std::string &own = s.spec.drawing_args;
    switch(s.spec.kind) {
    case plotKind::graph_2d:
    case plotKind::graph_2d_err: {
        if(not own.empty()) {
            return own;
        }
        // Markers, and a line through them for a curve, as the series' draw style says.
        const GDrawStyle style = s.plotter->drawStyle();
        const std::string d_a = std::string(style.markers ? "P" : "") + (style.line ? "L" : "");
        return s.secondary ? d_a + ",same" : "A" + d_a;
    }
    case plotKind::graph_3d:
        if(not own.empty()) {
            return own;
        }
        return s.secondary ? "P,same" : "P";
    case plotKind::graph_4d:
        return {}; // drawn marker by marker
    case plotKind::hist_1d:
    case plotKind::hist_1i:
        if(not own.empty()) {
            return own;
        }
        return s.secondary ? "same" : "";
    case plotKind::hist_2d:
        // The spec reports a set2DOpt() choice as the drawing arguments: such a choice is laid over
        // the pad's primary with "same"; arguments of the plotter's own are taken as they are.
        if(own.empty() or GHistogram2D::drawingOptionNamed(own).has_value()) {
            return s.secondary ? own + ",same" : own;
        }
        return own;
    case plotKind::function_1d:
    case plotKind::function_2d:
        if(not s.secondary) {
            return own;
        }
        return own.empty() ? "same" : own + ",same";
    }
    return own;
}

/******************************************************************************/
// The graphs: their data are declared and filled as arrays, one per column.

/**
 * @param s A graph
 * @return The names of the arrays of its columns, in the order of the columns
 */
std::vector<std::string> arraysOf(const rootSeries &s) {
    std::vector<std::string> names;
    switch(s.spec.kind) {
    case plotKind::graph_2d:
        names = {"x", "y"};
        break;
    case plotKind::graph_2d_err:
        names = {"x", "ex", "y", "ey"};
        break;
    case plotKind::graph_3d:
        names = {"x", "y", "z"};
        break;
    default:
        break;
    }
    for(auto &name : names) {
        name += "_array_" + suffixOf(s);
    }
    return names;
}

/**
 * @param s A graph
 * @param indent The indentation of every line
 * @return The declarations of its arrays, the first carrying the data-structure marker
 */
std::string graphHeader(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const std::string n = to_string(pointsOf(s));
    for(const auto &[c, name] : arraysOf(s) | std::views::enumerate) {
        out << indent << "double " << name << "[" << n << "];"
            << (c == 0 ? dsMarkerComment(s.plotter->dsMarker()) : std::string{}) << '\n';
    }
    out << '\n';
    return out.str();
}

/**
 * @param s A graph
 * @param indent The indentation of every line
 * @return The statements that fill its arrays: a point per line (a graph with errors: a value per
 *         line)
 */
std::string graphBody(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    markerLine(out, s);
    const auto names = arraysOf(s);
    const bool point_per_line = s.spec.kind != plotKind::graph_2d_err;
    for(std::size_t i = 0; i < pointsOf(s); ++i) {
        for(const auto &[c, name] : names | std::views::enumerate) {
            const auto column = static_cast<std::size_t>(c);
            out << ((point_per_line and column > 0) ? std::string("\t") : indent) << name << "["
                << i << "] = " << doublesOf(s, column)[i] << ";" << (point_per_line ? "" : "\n");
        }
        if(point_per_line) {
            out << '\n';
        }
    }
    out << '\n';
    return out.str();
}

/**
 * The axis titles, the plot title, the colour of the series in a shared pad and its line width.
 *
 * @param out The macro text
 * @param s A 2-d graph
 * @param graph The name of its graph
 * @param indent The indentation of every line
 */
void graph2DLook(
    std::ostream &out,
    const rootSeries &s,
    const std::string &graph,
    const std::string &indent
) {
    out << indent << graph << "->GetXaxis()->SetTitle(\"" << rootEscape(s.spec.x_label) << "\");"
        << '\n'
        << indent << graph << "->GetYaxis()->SetTitle(\"" << rootEscape(s.spec.y_label) << "\");"
        << '\n';
    emitRootTitle(out, indent, graph, s.spec.name);
    // In a pad that holds several graphs each one gets a colour of its own.
    if(s.shares_pad) {
        emitRootSeriesColour(out, indent, graph, s.secondary ? s.own_id + 1 : 0);
    }
    if(s.spec.line_width.has_value()) {
        out << indent << graph << "->SetLineWidth(" << *s.spec.line_width << ");" << '\n';
    }
}

/**
 * @param s A 2-d graph
 * @param indent The indentation of every line
 * @return The statements that draw it, with arrows between its points if it has them
 */
std::string graph2DFooter(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const auto arrays = arraysOf(s);
    const std::string graph = legendObjectOf(s);
    const std::size_t n = pointsOf(s);
    markerLine(out, s);
    out << indent << "TGraph *" << graph << " = new TGraph(" << n << ", " << arrays[0] << ", "
        << arrays[1] << ");" << '\n';
    graph2DLook(out, s, graph, indent);
    out << indent << graph << "->Draw(\"" << drawOptionOf(s) << "\");" << '\n' << '\n';
    if(s.spec.arrows.value_or(false) and n >= 2) {
        const auto &x = doublesOf(s, 0);
        const auto &y = doublesOf(s, 1);
        // One arrow per pair of neighbouring points; the pair's index names the arrow.
        for(std::size_t i = 0; i + 1 < n; ++i) {
            const std::string arrow = "ta_" + graph + "_" + to_string(i);
            out << indent << "TArrow * " << arrow << " = new TArrow(" << x[i] << ", " << y[i] << ","
                << x[i + 1] << ", " << y[i + 1] << ", " << 0.05 << ", \"|>\");" << '\n'
                << indent << arrow << "->SetArrowSize(0.01);" << '\n'
                << indent << arrow << "->Draw();" << '\n';
        }
        out << '\n';
    }
    out << '\n';
    return out.str();
}

/**
 * @param s A 2-d graph with errors
 * @param indent The indentation of every line
 * @return The statements that draw it
 */
std::string graph2EDFooter(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const auto arrays = arraysOf(s); // x, ex, y, ey
    const std::string graph = legendObjectOf(s);
    markerLine(out, s);
    out << indent << "TGraphErrors *" << graph << " = new TGraphErrors(" << pointsOf(s) << ", "
        << arrays[0] << ", " << arrays[2] << ", " << arrays[1] << " ," << arrays[3] << ");" << '\n';
    graph2DLook(out, s, graph, indent);
    out << indent << graph << "->Draw(\"" << drawOptionOf(s) << "\");" << '\n' << '\n';
    return out.str();
}

/**
 * The points' shadows: every point again, small and grey, at the floor of the plot box, which the
 * 3-d view of the pad already in place defines.
 *
 * @param s A 3-d graph
 * @param graph The name of its graph
 * @param indent The indentation of every line
 * @return The statements that draw the shadows
 */
std::string floorShadow(const rootSeries &s, const std::string &graph, const std::string &indent) {
    const auto arrays = arraysOf(s);
    const std::string shadow = "shadow_" + graph;
    EmitStream out;
    out << indent << "{" << '\n'
        << indent
        << "   const double z_floor = gPad->GetView() ? gPad->GetView()->GetRmin()[2] : 0.;" << '\n'
        << indent << "   TPolyMarker3D *" << shadow << " = new TPolyMarker3D(" << pointsOf(s)
        << ");" << '\n'
        << indent << "   for(int k = 0; k < " << pointsOf(s) << "; ++k) {" << '\n'
        << indent << "      " << shadow << "->SetPoint(k, " << arrays[0] << "[k], " << arrays[1]
        << "[k], z_floor);" << '\n'
        << indent << "   }" << '\n'
        << indent << "   " << shadow << "->SetMarkerStyle(20);" << '\n'
        << indent << "   " << shadow << "->SetMarkerSize(0.35);" << '\n'
        << indent << "   " << shadow << "->SetMarkerColor(kGray + 1);" << '\n'
        << indent << "   " << shadow << "->Draw();" << '\n'
        << indent << "}" << '\n';
    return out.str();
}

/**
 * A projected surface: the data as a colour-mapped surface, and its colour map on the ceiling
 * (ROOT's own SURF3 option) or on the floor of the plot box.
 *
 * ROOT has no floor counterpart of SURF3, so the floor map is drawn here: once the surface has been
 * painted and the 3-d view exists, every cell of the surface's interpolated histogram is projected
 * onto the floor plane through the view and filled with the palette colour of its value. The surface
 * is then painted again on top, so it hides the parts of the map that lie behind it.
 *
 * The z range of the box is pinned to the data's own minimum and maximum, so that the map lies at
 * exactly those heights and an overlay drawn there sits on it.
 *
 * @param s A 3-d graph with a projection (the primary of its pad)
 * @param graph The name of its graph
 * @param indent The indentation of every line
 * @return The statements that draw the projected surface
 */
std::string
projectedSurface(const rootSeries &s, const std::string &graph, const std::string &indent) {
    if(s.secondary) {
        throw gem_exception(
            g_error_streamer()
            << "a surface projection was requested for an overlay -- only the primary plot of a "
               "pad "
            << "can be drawn as a projected surface" << '\n'
        );
    }
    if(not s.spec.drawing_args.empty()) {
        throw gem_exception(
            g_error_streamer()
            << "a surface projection decides the drawing itself and cannot be combined with the "
            << "custom drawing arguments \"" << s.spec.drawing_args << "\"" << '\n'
        );
    }
    if(pointsOf(s) < 3) {
        throw gem_exception(
            g_error_streamer() << "a surface needs at least three points, got " << pointsOf(s)
                               << '\n'
        );
    }

    const auto [z_min_it, z_max_it] = std::ranges::minmax_element(doublesOf(s, 2));
    const double z_min = *z_min_it;
    const double z_max = *z_max_it;
    const surfaceProjection projection = s.spec.projection.value_or(surfaceProjection::NONE);
    const bool surface = s.spec.surface.value_or(true);

    EmitStream out;
    out << indent << graph << "->SetMinimum(" << z_min << ");" << '\n'
        << indent << graph << "->SetMaximum(" << z_max << ");" << '\n';

    if(projection == surfaceProjection::CEILING && surface) {
        out << indent << graph << "->Draw(\"SURF3Z\");" << '\n';
        return out.str();
    }

    // The map is drawn cell by cell onto the floor (or, without the surface, the ceiling): paint once
    // to obtain the view, draw the map, and paint the surface again on top if it is wanted. Without
    // the surface, the first paint draws invisible markers only -- it sets up the box and the view
    // without a surface and without the colour scale that comes with one.
    const double plane = (projection == surfaceProjection::CEILING) ? z_max : z_min;
    const std::string h = "h_" + graph;
    const std::string v = "v_" + graph;
    if(surface) {
        out << indent << graph << "->Draw(\"SURF1Z\");" << '\n';
    }
    else {
        out << indent << graph << "->SetMarkerSize(0);" << '\n'
            << indent << graph << "->Draw(\"P\");" << '\n';
    }
    out << indent << "gPad->Update();" << '\n'
        << indent << "{" << '\n'
        << indent << "   TH2 *" << h << " = " << graph << "->GetHistogram();" << '\n'
        << indent << "   TView *" << v << " = gPad->GetView();" << '\n'
        << indent << "   const int n_colours = gStyle->GetNumberOfColors();" << '\n'
        << indent << "   const double z_lo = " << z_min << ";" << '\n'
        << indent << "   const double z_hi = " << z_max << ";" << '\n'
        << indent << "   for(int i = 1; i <= " << h << "->GetNbinsX(); ++i) {" << '\n'
        << indent << "      for(int j = 1; j <= " << h << "->GetNbinsY(); ++j) {" << '\n'
        << indent << "         const double x0 = " << h << "->GetXaxis()->GetBinLowEdge(i);" << '\n'
        << indent << "         const double x1 = " << h << "->GetXaxis()->GetBinUpEdge(i);" << '\n'
        << indent << "         const double y0 = " << h << "->GetYaxis()->GetBinLowEdge(j);" << '\n'
        << indent << "         const double y1 = " << h << "->GetYaxis()->GetBinUpEdge(j);" << '\n'
        << indent << "         const double z_p = " << plane << ";" << '\n'
        << indent
        << "         const double corners[4][3] = {{x0, y0, z_p}, {x1, y0, z_p}, {x1, y1, z_p}, "
           "{x0, y1, z_p}};"
        << '\n'
        << indent << "         double px[5];" << '\n'
        << indent << "         double py[5];" << '\n'
        << indent << "         for(int k = 0; k < 4; ++k) {" << '\n'
        << indent << "            double ndc[3];" << '\n'
        << indent << "            " << v << "->WCtoNDC(corners[k], ndc);" << '\n'
        << indent << "            px[k] = ndc[0];" << '\n'
        << indent << "            py[k] = ndc[1];" << '\n'
        << indent << "         }" << '\n'
        << indent << "         px[4] = px[0];" << '\n'
        << indent << "         py[4] = py[0];" << '\n'
        << indent << "         double frac = (" << h
        << "->GetBinContent(i, j) - z_lo) / (z_hi - z_lo);" << '\n'
        << indent << "         frac = frac < 0. ? 0. : (frac > 1. ? 1. : frac);" << '\n'
        << indent
        << "         const int colour = gStyle->GetColorPalette(int(frac * (n_colours - 1)));"
        << '\n'
        << indent << "         TPolyLine *cell = new TPolyLine(5, px, py);" << '\n'
        << indent << "         cell->SetFillColor(colour);" << '\n'
        << indent << "         cell->SetLineColor(colour);" << '\n'
        << indent << "         cell->Draw(\"f\");" << '\n'
        << indent << "      }" << '\n'
        << indent << "   }" << '\n'
        << indent << "}" << '\n';
    if(surface) {
        out << indent << graph << "->Draw(\"SURF1 same\");" << '\n';
    }
    return out.str();
}

/**
 * @param s A 3-d graph
 * @param indent The indentation of every line
 * @return The statements that draw it: its points, their shadows, a projected surface, a line
 *         through the points
 */
std::string graph3DFooter(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const auto arrays = arraysOf(s);
    const std::string graph = "graph_" + suffixOf(s);
    const std::size_t n = pointsOf(s);
    const bool shadow = s.spec.floor_shadow.value_or(false);
    markerLine(out, s);
    out << indent << "TGraph2D *" << graph << " = new TGraph2D(" << n << ", " << arrays[0] << ", "
        << arrays[1] << ", " << arrays[2] << ");" << '\n'
        << indent << graph << "->SetMarkerStyle(20);" << '\n'
        << indent << graph << "->SetMarkerSize(1);" << '\n'
        << indent << graph << "->SetMarkerColor(2);" << '\n';
    emitRootTitle(out, indent, graph, s.spec.name);

    // The draw, then a pad update, then the axis titles -- and that order is load-bearing. A
    // TGraph2D owns no axes of its own: GetXaxis() reaches into an internal histogram that ROOT
    // REBUILDS when the graph is painted with a surface or colour-map option (SURF*, COLZ, TRI*).
    // Titles set before the painting are discarded with the old histogram, which left every
    // scanned landscape with unlabelled axes. Update() forces the painting to happen here, so the
    // titles below land on the histogram that is finally drawn -- for every drawing option, plain
    // point clouds included.
    // A shadow is drawn under the points: an overlay's shadow before its points (the primary has
    // made the 3-d view already); a primary's after it has painted itself and the view exists, with
    // the points painted again on top.
    if(s.spec.projection.value_or(surfaceProjection::NONE) == surfaceProjection::NONE) {
        if(shadow and s.secondary) {
            out << floorShadow(s, graph, indent);
        }
        out << indent << graph << "->Draw(\"" << drawOptionOf(s) << "\");" << '\n';
        if(shadow and not s.secondary) {
            out << indent << "gPad->Update();" << '\n'
                << floorShadow(s, graph, indent) << indent << graph << "->Draw(\"P,same\");"
                << '\n';
        }
    }
    else {
        out << projectedSurface(s, graph, indent);
    }
    // The titles of the two floor axes are centred on their axis and set clear of the tick labels:
    // ROOT puts them at the far end of the axis by default, where the x title ran into the colour
    // palette of a projected surface and the y title into the tick labels of its axis.
    out << indent << "gPad->Update();" << '\n'
        << indent << graph << "->GetXaxis()->SetTitle(\"" << rootEscape(s.spec.x_label) << "\");"
        << '\n'
        << indent << graph << "->GetXaxis()->CenterTitle();" << '\n'
        << indent << graph << "->GetXaxis()->SetTitleOffset(" << GRAPH3DXTITLEOFFSET << ");" << '\n'
        << indent << graph << "->GetYaxis()->SetTitle(\"" << rootEscape(s.spec.y_label) << "\");"
        << '\n'
        << indent << graph << "->GetYaxis()->CenterTitle();" << '\n'
        << indent << graph << "->GetYaxis()->SetTitleOffset(" << GRAPH3DYTITLEOFFSET << ");" << '\n'
        << indent << graph << "->GetZaxis()->SetTitle(\"" << rootEscape(s.spec.z_label) << "\");"
        << '\n'
        << indent << graph << "->GetZaxis()->SetTitleOffset(1.5);" << '\n'
        << '\n';

    if(curveOf(s)) {
        const auto &x = doublesOf(s, 0);
        const auto &y = doublesOf(s, 1);
        const auto &z = doublesOf(s, 2);
        const std::string lines = "lines_" + graph;
        const std::string sample = legendObjectOf(s);
        out << indent << "TPolyLine3D *" << lines << " = new TPolyLine3D(" << n << ");" << '\n'
            << '\n';
        for(std::size_t i = 0; i < n; ++i) {
            out << indent << lines << "->SetPoint(" << i << ", " << x[i] << ", " << y[i] << ", "
                << z[i] << ");";
        }
        // The legend's sample of a curve: the graph's markers and the poly-line's line, in an
        // object of its own that is never drawn, so that the drawn objects stay as they are.
        out << '\n'
            << indent << lines << "->SetLineWidth(3);" << '\n'
            << indent << lines << "->Draw();" << '\n'
            << indent << "TGraph *" << sample << " = new TGraph();" << '\n'
            << indent << sample << "->SetMarkerStyle(" << graph << "->GetMarkerStyle());" << '\n'
            << indent << sample << "->SetMarkerSize(" << graph << "->GetMarkerSize());" << '\n'
            << indent << sample << "->SetMarkerColor(" << graph << "->GetMarkerColor());" << '\n'
            << indent << sample << "->SetLineColor(" << lines << "->GetLineColor());" << '\n'
            << indent << sample << "->SetLineWidth(" << lines << "->GetLineWidth());" << '\n'
            << '\n';
    }
    return out.str();
}

/**
 * A 4-d graph: a marker per point in a 3-d frame, sized by the point's w value. The points are taken
 * in the order of their w (an index permutation; the columns stay put), the best one first, larger
 * and blue; with n_best only that many are drawn.
 *
 * @param s A 4-d graph
 * @param pad The series of its pad (the frame of the primary holds the points of its overlays)
 * @param indent The indentation of every line
 * @return The statements that draw it
 */
std::string
graph4DFooter(const rootSeries &s, const std::vector<rootSeries> &pad, const std::string &indent) {
    const auto &x_col = doublesOf(s, 0);
    const auto &y_col = doublesOf(s, 1);
    const auto &z_col = doublesOf(s, 2);
    const auto &w_col = doublesOf(s, 3);
    const std::size_t data_size = pointsOf(s);
    const auto [min_marker_size, max_marker_size] =
        s.spec.marker_size.value_or(std::make_tuple(DEFMINMARKERSIZE, DEFMAXMARKERSIZE));
    const bool small_w_large_marker = s.spec.small_w_large_marker.value_or(true);
    const std::size_t n_best = s.spec.n_best.value_or(0);
    const std::string suffix = suffixOf(s);

    // The w-ordered index permutation, so that the n best are the first
    std::vector<std::size_t> order =
        std::views::iota(0uz, data_size) | std::ranges::to<std::vector<std::size_t>>();
    if(small_w_large_marker) {
        std::ranges::sort(order, [&w_col](std::size_t a, std::size_t b) -> bool {
            return (w_col[a] < w_col[b]);
        });
    }
    else {
        std::ranges::sort(order, [&w_col](std::size_t a, std::size_t b) -> bool {
            return (w_col[a] > w_col[b]);
        });
    }

    if(data_size < static_cast<std::size_t>(2)) {
        throw gem_exception(
            g_error_streamer() << "Got vector of invalid size " << data_size << '\n'
        );
    }
    const auto [w_min_it, w_max_it] = std::ranges::minmax_element(w_col);
    const double w_min = *w_min_it;
    const double w_max = *w_max_it;

    EmitStream out;

    // The frame of a pad's primary holds the points of every 4-d graph laid over it: an overlay
    // draws no frame of its own, and its points outside the primary's would lie outside the box.
    if(not s.secondary) {
        auto [x_lo, x_hi] = columnRange(s.columns[0]);
        auto [y_lo, y_hi] = columnRange(s.columns[1]);
        auto [z_lo, z_hi] = columnRange(s.columns[2]);
        for(const auto &overlay : pad | std::views::drop(1)) {
            if(overlay.columns.size() != 4) {
                continue;
            }
            const auto [sx_lo, sx_hi] = columnRange(overlay.columns[0]);
            const auto [sy_lo, sy_hi] = columnRange(overlay.columns[1]);
            const auto [sz_lo, sz_hi] = columnRange(overlay.columns[2]);
            x_lo = std::min(x_lo, sx_lo);
            x_hi = std::max(x_hi, sx_hi);
            y_lo = std::min(y_lo, sy_lo);
            y_hi = std::max(y_hi, sy_hi);
            z_lo = std::min(z_lo, sz_lo);
            z_hi = std::max(z_hi, sz_hi);
        }
        // The frame's name carries the plot's suffix, so two 4-d plots on one canvas do not collide.
        const std::string frame = "fr_" + suffix;
        out << indent << "TH3F *" << frame << " = new TH3F(\"" << frame << "\",\"" << frame << "\","
            << "10, " << x_lo << ", " << x_hi << ", "
            << "10, " << y_lo << ", " << y_hi << ", "
            << "10, " << z_lo << ", " << z_hi << ");" << '\n'
            << indent << frame << "->SetTitle(\" \");" << '\n'
            << indent << frame << "->GetXaxis()->SetTitle(\"" << rootEscape(s.spec.x_label)
            << "\");" << '\n'
            << indent << frame << "->GetXaxis()->SetTitleOffset(1.6);" << '\n'
            << indent << frame << "->GetYaxis()->SetTitle(\"" << rootEscape(s.spec.y_label)
            << "\");" << '\n'
            << indent << frame << "->GetYaxis()->SetTitleOffset(1.6);" << '\n'
            << indent << frame << "->GetZaxis()->SetTitle(\"" << rootEscape(s.spec.z_label)
            << "\");" << '\n'
            << indent << frame << "->GetZaxis()->SetTitleOffset(1.6);" << '\n'
            << '\n'
            << indent << frame << "->Draw();" << '\n';
    }

    const double w_range = w_max - w_min;
    std::size_t pos = 0;
    for(std::size_t const idx : order) {
        const std::string marker = "pm3d_" + suffix + "_" + to_string(pos);
        const double w = w_col[idx];

        // The w value as a marker size: by default, the smallest w gets the largest marker
        double marker_size = 0.;
        if(0 == pos) {
            marker_size = 2 * max_marker_size;
        }
        else if(small_w_large_marker) {
            marker_size = min_marker_size + ((max_marker_size - min_marker_size) *
                                             std::pow((1. - ((w - w_min) / w_range)), 8.));
        }
        else {
            marker_size = min_marker_size + ((max_marker_size - min_marker_size) *
                                             std::pow(((w - w_min) / w_range), 8));
        }

        // The marker's only point: a TPolyMarker3D(1) given point n > 0 grows to n + 1 points, the
        // ones before at the origin, which every marker after the first painted there.
        out << indent << "TPolyMarker3D *" << marker << " = new TPolyMarker3D(1);" << '\n'
            << indent << marker << "->SetPoint(0, " << x_col[idx] << ", " << y_col[idx] << ", "
            << z_col[idx] << "); // w = " << w << '\n'
            << indent << marker << "->SetMarkerSize(" << marker_size << ");" << '\n'
            << indent << marker << "->SetMarkerColor(" << (0 == pos ? 4 : 2) << ");" << '\n'
            << indent << marker << "->SetMarkerStyle(8);" << '\n'
            << indent << marker << "->Draw();" << '\n'
            << '\n';

        pos++;
        if(n_best > 0 and pos >= n_best) {
            break;
        }
    }
    out << '\n';
    return out.str();
}

/******************************************************************************/
// The histograms: declared with their bins and range, filled entry by entry.

/**
 * @param s A histogram
 * @param indent The indentation of every line
 * @return The declaration of the histogram: its bins over its fixed range, or over the range of
 *         its data
 */
std::string histogramHeader(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const std::string hist = legendObjectOf(s);
    const std::string comment = dsMarkerComment(s.plotter->dsMarker());
    const std::size_t n_x = s.spec.n_bins_x.value_or(Gem::Common::DEFAULTNBINSGPD);
    const auto range = [&s](
                           const std::optional<std::tuple<double, double>> &fixed,
                           std::size_t column,
                           std::size_t n_bins
                       ) {
        if(fixed.has_value()) {
            return *fixed;
        }
        requireData(s);
        return rangeHolding(s.columns[column], n_bins);
    };
    if(s.spec.kind == plotKind::hist_2d) {
        const std::size_t n_y = s.spec.n_bins_y.value_or(Gem::Common::DEFAULTNBINSGPD);
        // A fixed range needs both axes; otherwise both come from the data.
        const bool fixed = s.spec.range_x.has_value() and s.spec.range_y.has_value();
        const auto [lo_x, hi_x] = range(fixed ? s.spec.range_x : std::nullopt, 0, n_x);
        const auto [lo_y, hi_y] = range(fixed ? s.spec.range_y : std::nullopt, 1, n_y);
        out << indent << "TH2D *" << hist << " = new TH2D(\"" << hist << "\", \"" << hist << "\","
            << n_x << ", " << lo_x << ", " << hi_x << "," << n_y << ", " << lo_y << ", " << hi_y
            << ");" << comment << '\n'
            << '\n';
    }
    else {
        const auto [lo_x, hi_x] = range(s.spec.range_x, 0, n_x);
        out << indent << (s.spec.kind == plotKind::hist_1i ? "TH1I *" : "TH1D *") << hist
            << " = new " << (s.spec.kind == plotKind::hist_1i ? "TH1I" : "TH1D") << "(\"" << hist
            << "\", \"" << hist << "\"," << n_x << ", " << lo_x << ", " << hi_x << ");" << comment
            << '\n'
            << '\n';
    }
    return out.str();
}

/**
 * @param s A histogram
 * @param indent The indentation of every line
 * @return The statements that fill it, an entry per line, the first carrying the data-structure
 *         marker
 */
std::string histogramBody(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const std::string hist = legendObjectOf(s);
    const std::string comment = dsMarkerComment(s.plotter->dsMarker());
    for(std::size_t i = 0; i < pointsOf(s); ++i) {
        out << indent << hist << "->Fill(";
        switch(s.spec.kind) {
        case plotKind::hist_1i:
            std::visit([&out, i](const auto *values) { out << (*values)[i]; }, s.columns[0]);
            break;
        case plotKind::hist_2d:
            out << std::showpoint << doublesOf(s, 0)[i] << ", " << doublesOf(s, 1)[i];
            break;
        default:
            out << std::showpoint << doublesOf(s, 0)[i];
            break;
        }
        out << ");" << (i == 0 ? comment : std::string{}) << '\n';
    }
    out << '\n';
    return out.str();
}

/**
 * @param s A histogram
 * @param indent The indentation of every line
 * @return The statements that draw it
 */
std::string histogramFooter(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const std::string hist = legendObjectOf(s);
    emitRootTitle(out, indent, hist, s.spec.name);
    markerLine(out, s);
    out << indent << hist << "->GetXaxis()->SetTitle(\"" << rootEscape(s.spec.x_label) << "\");"
        << '\n'
        << indent << hist << "->GetYaxis()->SetTitle(\"" << rootEscape(s.spec.y_label) << "\");"
        << '\n'
        << indent << hist << "->Draw(\"" << drawOptionOf(s) << "\");" << '\n'
        << '\n';
    return out.str();
}

/******************************************************************************/
// The functions: ROOT samples their formula itself.

/**
 * @param s A function
 * @param indent The indentation of every line
 * @return The declaration of the function over its range
 */
std::string functionHeader(const rootSeries &s, const std::string &indent) {
    const bool two_d = s.spec.kind == plotKind::function_2d;
    if(s.spec.function.empty() or not s.spec.range_x.has_value() or
       (two_d and not s.spec.range_y.has_value())) {
        throw gem_exception(
            g_error_streamer() << "GRootEmitter: the function \"" << s.spec.name
                               << "\" reports no formula or no range to draw it over" << '\n'
        );
    }
    const auto [lo_x, hi_x] = *s.spec.range_x;
    if(lo_x >= hi_x) {
        throw gem_exception(
            g_error_streamer() << "lower boundary" << (two_d ? "(x)" : "") << " >= upper boundary"
                               << (two_d ? "(x)" : "") << ": " << lo_x << " / " << hi_x << '\n'
        );
    }
    EmitStream out;
    const std::string function = legendObjectOf(s);
    out << indent << (two_d ? "TF2 *" : "TF1 *") << function << " = new " << (two_d ? "TF2" : "TF1")
        << "(\"" << function << "\", \"" << rootEscape(s.spec.function) << "\"," << lo_x << ", "
        << hi_x;
    if(two_d) {
        const auto [lo_y, hi_y] = *s.spec.range_y;
        if(lo_y >= hi_y) {
            throw gem_exception(
                g_error_streamer()
                << "lower boundary(y) >= upper boundary(y): " << lo_y << " / " << hi_y << '\n'
            );
        }
        out << ", " << lo_y << ", " << hi_y;
    }
    out << ");" << dsMarkerComment(s.plotter->dsMarker()) << '\n';
    return out.str();
}

/**
 * @param s A function
 * @param indent The indentation of every line
 * @return The statements that draw it
 */
std::string functionFooter(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const bool two_d = s.spec.kind == plotKind::function_2d;
    const std::string function = legendObjectOf(s);
    const std::string comment = dsMarkerComment(s.plotter->dsMarker());
    out << indent << function << "->GetXaxis()->SetTitle(\"" << rootEscape(s.spec.x_label) << "\");"
        << '\n'
        << indent << function << "->GetYaxis()->SetTitle(\"" << rootEscape(s.spec.y_label) << "\");"
        << '\n';
    if(two_d) {
        out << indent << function << "->GetZaxis()->SetTitle(\"" << rootEscape(s.spec.z_label)
            << "\");" << '\n';
    }
    out << indent << function << "->SetNpx(" << s.spec.n_bins_x.value_or(DEFNSAMPLES) << ");"
        << '\n';
    if(two_d) {
        out << indent << function << "->SetNpy(" << s.spec.n_bins_y.value_or(DEFNSAMPLES) << ");"
            << '\n';
    }
    emitRootTitle(out, indent, function, s.spec.name);
    out << indent << function << "->Draw(\"" << drawOptionOf(s) << "\");" << comment << '\n'
        << '\n';
    return out.str();
}

/******************************************************************************/
// The three sections of a series: its header declares what holds its data, its body fills it, and
// its footer draws it.

/**
 * @param s The series
 * @param pad The series of its pad
 * @param indent The indentation of every line
 * @return The declarations of the series' data
 */
std::string headerOf(
    const rootSeries &s,
    [[maybe_unused]] const std::vector<rootSeries> &pad,
    const std::string &indent
) {
    switch(s.spec.kind) {
    case plotKind::graph_2d:
    case plotKind::graph_2d_err:
    case plotKind::graph_3d:
        return graphHeader(s, indent);
    case plotKind::graph_4d:
        return {}; // drawn point by point in the footer
    case plotKind::hist_1d:
    case plotKind::hist_1i:
    case plotKind::hist_2d:
        return histogramHeader(s, indent);
    case plotKind::function_1d:
    case plotKind::function_2d:
        return functionHeader(s, indent);
    }
    return {};
}

/**
 * @param s The series
 * @param pad The series of its pad
 * @param indent The indentation of every line
 * @return The statements that fill the series' data
 */
std::string bodyOf(
    const rootSeries &s,
    [[maybe_unused]] const std::vector<rootSeries> &pad,
    const std::string &indent
) {
    switch(s.spec.kind) {
    case plotKind::graph_2d:
    case plotKind::graph_2d_err:
    case plotKind::graph_3d:
        return graphBody(s, indent);
    case plotKind::hist_1d:
    case plotKind::hist_1i:
    case plotKind::hist_2d:
        return histogramBody(s, indent);
    case plotKind::graph_4d:
    case plotKind::function_1d:
    case plotKind::function_2d:
        return {}; // a 4-d graph is drawn point by point; ROOT samples a function itself
    }
    return {};
}

/**
 * @param s The series
 * @param pad The series of its pad
 * @param indent The indentation of every line
 * @return The statements that draw the series
 */
std::string
footerOf(const rootSeries &s, const std::vector<rootSeries> &pad, const std::string &indent) {
    switch(s.spec.kind) {
    case plotKind::graph_2d:
        return graph2DFooter(s, indent);
    case plotKind::graph_2d_err:
        return graph2EDFooter(s, indent);
    case plotKind::graph_3d:
        return graph3DFooter(s, indent);
    case plotKind::graph_4d:
        return graph4DFooter(s, pad, indent);
    case plotKind::hist_1d:
    case plotKind::hist_1i:
    case plotKind::hist_2d:
        return histogramFooter(s, indent);
    case plotKind::function_1d:
    case plotKind::function_2d:
        return functionFooter(s, indent);
    }
    return {};
}

/******************************************************************************/
/**
 * The series of the pad of a primary plotter: the primary first, then its overlays in their order.
 *
 * @param primary The pad's primary plotter
 * @return The pad's series
 */
std::vector<rootSeries> padOf(const GBasePlotter &primary) {
    const auto &overlays = primary.secondaryPlotters();
    std::vector<rootSeries> pad;
    pad.push_back(
        {.plotter = &primary,
         .spec = primary.plotSpec(),
         .columns = primary.dataColumns(),
         .secondary = false,
         .parent_id = 0,
         .own_id = primary.id(),
         .shares_pad = not overlays.empty()}
    );
    for(const auto &[pos, overlay] : overlays | std::views::enumerate) {
        pad.push_back(
            {.plotter = overlay.get(),
             .spec = overlay->plotSpec(),
             .columns = overlay->dataColumns(),
             .secondary = true,
             .parent_id = primary.id(),
             .own_id = static_cast<std::size_t>(pos),
             .shares_pad = true}
        );
    }
    for(const auto &s : pad) {
        if(s.columns.size() < columnsNeeded(s.spec.kind)) {
            throw gem_exception(
                g_error_streamer()
                << "GRootEmitter: " << s.plotter->getPlotterName() << " reports "
                << s.columns.size() << " data columns; a plot of kind \"" << to_string(s.spec.kind)
                << "\" needs " << columnsNeeded(s.spec.kind) << '\n'
            );
        }
    }
    return pad;
}

/**
 * One section of a pad -- its declarations, its data or its drawing -- for each of its series, each
 * introduced by a comment that names it.
 *
 * @param pad The series of the pad
 * @param section What the section holds ("Header", "Body", "Footer")
 * @param of The section of one series
 * @param indent The indentation of every line
 * @return The section
 */
std::string padSection(
    const std::vector<rootSeries> &pad,
    const std::string &section,
    std::string (*of)(const rootSeries &, const std::vector<rootSeries> &, const std::string &),
    const std::string &indent
) {
    EmitStream out;
    out << indent << "// " << section << " data for primary plotter" << '\n'
        << of(pad[0], pad, indent);
    for(const auto &s : pad | std::views::drop(1)) {
        out << indent << "// " << section << " data for secondary plotter " << s.own_id << " of "
            << pad[0].plotter->getPlotterName() << '\n'
            << of(s, pad, indent) << '\n';
    }
    return out.str();
}

/**
 * What a pad draws after its series: one frame and the legend for a pad of several series, the
 * notes a reader of the macro checks it by, and a pad title set apart from the first series' label.
 *
 * @param pad The series of the pad
 * @param indent The indentation of every line
 * @return The statements
 */
std::string padFinish(const std::vector<rootSeries> &pad, const std::string &indent) {
    EmitStream out;
    const GBasePlotter &primary = *pad[0].plotter;
    const GPlotSpec &spec = pad[0].spec;
    const bool three_d = isThreeDimensional(spec);
    const bool room = spec.legend_room.value_or(false);
    // Several plotters share this pad: give them one frame, and the legend the description lists,
    // each sample drawn from its series' object.
    const auto entries = primary.legendEntries();
    if(pad.size() > 1) {
        std::vector<rootLegendEntry> root_entries;
        for(const auto &e : entries) {
            root_entries.push_back(
                {legendObjectOf(pad.at(e.series)), e.label, e.sample, e.series, e.line_width}
            );
        }
        emitRootOverlayFrameAndLegend(
            out,
            indent,
            primary.legendPlace(),
            root_entries,
            three_d,
            room
        );
    }
    // Whether the frame leaves the legend a band of its own, for the same reader.
    out << indent << "// legend room: " << (room ? 1 : 0) << '\n';
    // The number of rows ROOT draws of the description (the band of a 3-d pad shows at most
    // LEGEND3DMAXROWS), for a reader of the script that checks what ROOT drew (the validity test
    // of the plotting library).
    const bool band = primary.legendPlace() == legendPosition::AUTO and three_d;
    out << indent << "// legend entries: "
        << (band ? std::min(entries.size(), LEGEND3DMAXROWS) : entries.size()) << '\n';
    // A pad title set apart from the first series' label replaces that label as the title, once
    // the legend has taken the label.
    if(not spec.pad_title.empty()) {
        emitRootPadTitle(out, indent, spec.pad_title);
    }
    return out.str();
}

/******************************************************************************/
/**
 * The canvas of the macro: ROOT's style, the canvas, its label (the title of the figure, in a box
 * above the pads; without it the pads take the whole canvas) and the pad divided into the grid of
 * the plots. The label's text has one size (CANVASLABELTEXTSIZE), whatever its letters, unless it is
 * too wide for the box: then it is drawn just small enough (ROOT measures it, as TPaveLabel would,
 * at the size TPaveLabel derives from the box's height in pixels).
 *
 * @param c_dim The canvas' width and height in pixels
 * @param c_div The number of pads in x and y direction
 * @param label The canvas label
 * @param show_label Whether the label is shown
 * @param indent The indentation of every line
 * @return The statements
 */
std::string canvasOf(
    const std::tuple<std::uint32_t, std::uint32_t> &c_dim,
    const std::tuple<std::size_t, std::size_t> &c_div,
    const std::string &label,
    bool show_label,
    const std::string &indent
) {
    EmitStream result;

    result << indent << "gROOT->Reset();" << '\n'
           << indent << "gStyle->SetCanvasColor(0);" << '\n'
           << indent << "gStyle->SetStatBorderSize(1);" << '\n'
           << indent << "gStyle->SetOptStat(0);" << '\n'
           << '\n'
           << indent << R"(TCanvas *cc = new TCanvas("cc", "cc",0,0,)" << std::get<0>(c_dim) << ","
           << std::get<1>(c_dim) << ");" << '\n'
           << '\n';
    if(show_label) {
        std::ostringstream size;
        size.imbue(std::locale::classic());
        size << CANVASLABELTEXTSIZE;
        result << indent << "TPaveLabel* canvasTitle = new TPaveLabel(0.2,0.95,0.8,0.99, \""
               << rootEscape(label) << "\");" << '\n'
               << indent << "{ // one text size, whatever the letters; smaller only if too wide"
               << '\n'
               << indent << "   const double x1 = 0.2, y1 = 0.95, x2 = 0.8, y2 = 0.99;" << '\n'
               << indent << "   const double pad_w = gPad->XtoPixel(gPad->GetX2());" << '\n'
               << indent << "   const double pad_h = gPad->YtoPixel(gPad->GetY1());" << '\n'
               << indent << "   const double room = 0.95 * (x2 - x1);" << '\n'
               << indent << "   TLatex label(0., 0., canvasTitle->GetLabel());" << '\n'
               << indent << "   label.SetTextFont(canvasTitle->GetTextFont());" << '\n'
               << indent << "   const auto width_at = [&](double size) { // of the box's height"
               << '\n'
               << indent << "      const double box_h = (y2 - y1) * pad_h;" << '\n'
               << indent << "      label.SetTextSize(size * box_h / TMath::Min(pad_w, pad_h));"
               << '\n'
               << indent << "      return label.GetXsize() / (gPad->GetX2() - gPad->GetX1());"
               << '\n'
               << indent << "   };" << '\n'
               << indent << "   double size = " << size.str() << ";" << '\n'
               << indent << "   double width = width_at(size);" << '\n'
               << indent << "   for(int pass = 0; pass < 4 && width > room; ++pass) {" << '\n'
               << indent << "      size *= room / width; // a width is not quite proportional"
               << '\n'
               << indent << "      width = width_at(size);" << '\n'
               << indent << "   }" << '\n'
               << indent << "   canvasTitle->SetTextSize(size);" << '\n'
               << indent << "}" << '\n'
               << indent << "canvasTitle->Draw();" << '\n'
               << '\n'
               << indent
               << R"(TPad* graphPad = new TPad("Graphs", "Graphs", 0.01, 0.01, 0.99, 0.94);)"
               << '\n';
    }
    else {
        result << indent
               << R"(TPad* graphPad = new TPad("Graphs", "Graphs", 0.01, 0.01, 0.99, 0.99);)"
               << '\n';
    }
    result << indent << "graphPad->Draw();" << '\n'
           << indent << "graphPad->Divide(" << std::get<0>(c_div) << "," << std::get<1>(c_div)
           << ");" << '\n'
           << '\n';

    return result.str();
}

} // anonymous namespace

/******************************************************************************/
/**
 * The ROOT-macro file extension.
 *
 * @return The string ".C"
 */
std::string GRootEmitter::fileExtension() const {
    return std::string(".C");
}

/******************************************************************************/
/**
 * Emits the overall plot as a ROOT macro: the canvas and its pads, then, for every pad, the
 * declarations of its series' data, the data, and the drawing, each written from what the series'
 * plotters report (their plot specs, columns and draw styles), as the other backends do.
 *
 * @param gpd The designer holding the plotters and canvas configuration
 * @return The complete ROOT macro source code for the canvas and all registered plotters
 */
std::string GRootEmitter::emitDocument(const GPlotDesigner &gpd) const {
    const std::filesystem::path &plot_name = gpd.pending_plot_name_;
    const std::string indent = gpd.indent();

    EmitStream result;
    std::size_t const max_plots = gpd.c_x_div_ * gpd.c_y_div_;

    warnPadOverflow(
        "GRootEmitter::emitDocument()",
        gpd.getCanvasLabel(),
        gpd.plotters_cnt_.size(),
        max_plots
    );

    std::vector<std::vector<rootSeries>> pads;
    for(const auto &p : gpd.plotters_cnt_ | std::views::take(max_plots)) {
        pads.push_back(padOf(*p));
    }

    result << "{" << '\n'
           << canvasOf(
                  {gpd.c_x_dim_, gpd.c_y_dim_},
                  {gpd.c_x_div_, gpd.c_y_div_},
                  gpd.canvas_label_,
                  gpd.show_canvas_label_,
                  indent
              )
           << '\n';

    result << indent << "//===================  Header Section ====================" << '\n'
           << '\n';
    for(const auto &pad : pads) {
        result << padSection(pad, "Header", headerOf, indent) << '\n';
    }

    result << indent << "//===================  Data Section ======================" << '\n'
           << '\n';
    for(const auto &pad : pads) {
        result << padSection(pad, "Body", bodyOf, indent) << '\n';
    }

    result << indent << "//===================  Plot Section ======================" << '\n'
           << '\n';
    for(const auto &[idx, pad] : pads | std::views::enumerate) {
        const GPlotSpec &spec = pad[0].spec;
        result << indent << "graphPad->cd(" << static_cast<std::size_t>(idx) + 1 << ");"
               << '\n'; /* cd starts at 1 */
        // The scale of an axis belongs to the pad and is set before anything is drawn into it.
        if(spec.x_log) {
            result << indent << "gPad->SetLogx(1);" << '\n';
        }
        if(spec.y_log) {
            result << indent << "gPad->SetLogy(1);" << '\n';
        }
        result << padSection(pad, "Footer", footerOf, indent) << padFinish(pad, indent);
        // The labels of such an axis can only be arranged once the pad knows its range.
        if(spec.x_log or spec.y_log) {
            emitRootLogAxisLabels(result, indent);
        }
        result << '\n';
    }

    result << indent << "graphPad->cd();" << '\n' << indent << "cc->cd();" << '\n';

    // Check if we are supposed to output a png file
    if(gpd.add_print_command_ && plot_name.string() != "empty" && not(plot_name.string()).empty()) {
        // The picture is named by the macro's file name alone, without its directory, so that the
        // macro works from wherever it lies (and is moved); make sure there are no white spaces
        std::string const plot_name_local =
            Gem::Common::trimWhitespace(plot_name.filename().string());
        result << '\n'
               << indent << "// Print out the data of this file to a png file" << '\n'
               << indent << "cc->Print(\"" << plot_name_local << ".png\");" << '\n';
    }

    result << "}" << '\n';

    return result.str();
}

/******************************************************************************/
} /* namespace Gem::Dietrich */
