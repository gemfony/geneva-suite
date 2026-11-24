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
#include <tuple>
#include <utility>
#include <vector>

namespace Gem::Dietrich {

// The plotting library builds on common's facilities (logging, serialization helpers,
// exception types, make_member, EmitStream, ...); make them visible here without
// per-name qualification. This affects lookup only within Gem::Dietrich.
using namespace Gem::Common;

/******************************************************************************/
/**
 * The Octave / MATLAB script file extension.
 *
 * @return The string ".m"
 */
std::string OctaveEmitter::fileExtension() const {
    return std::string(".m");
}

namespace {

/******************************************************************************/
// Helpers backing the Octave / MATLAB backend. The classification / validation /
// column-literal skeleton is the shared script-backend driver in detail/GPlotDetail.hpp;
// only the Octave-specific escaping and plotting calls live here. Everything emitted
// stays inside the Octave-and-MATLAB COMMON CORE (base functions only -- no toolboxes /
// packages), so a plain `octave` can run the script.

/** @brief Escape a user string for a single-quoted Octave / MATLAB string literal: a
 *  literal apostrophe doubles to '', and embedded newlines (which cannot appear inside a
 *  single-line literal) become spaces. The result is always inserted between two `'`. */
std::string octaveEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for(char const c : in) {
        switch(c) {
        case '\'':
            out += "''";
            break;
        case '\n':
        case '\r':
            out += ' ';
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

/** @brief The property for a line width that was set (", 'LineWidth', N"), or nothing */
std::string lineWidthArgument(const GBasePlotter &p) {
    return p.lineWidth() > 0 ? ", 'LineWidth', " + std::to_string(p.lineWidth()) : std::string{};
}

/** @brief Octave's name of a legend corner ('southeast', ...); AUTO has none */
std::string octaveLegendLocation(legendPosition position) {
    switch(position) {
    case legendPosition::TOP_LEFT:
        return "northwest";
    case legendPosition::TOP_RIGHT:
        return "northeast";
    case legendPosition::BOTTOM_LEFT:
        return "southwest";
    case legendPosition::BOTTOM_RIGHT:
        return "southeast";
    case legendPosition::CENTER_RIGHT:
        return "east";
    case legendPosition::AUTO:
        break;
    case legendPosition::FREEST:
        break; // resolved before (GBasePlotter::legendPlace())
    }
    return {};
}

/** @brief Octave's line specification of a style: "-o", "-", or "o" (also for a style of
 *  neither, which no plotter reports) */
std::string octaveFormat(const GDrawStyle &style) {
    if(style.line) {
        return style.markers ? "-o" : "-";
    }
    return "o";
}

/** @brief Emit the Octave plotting call(s) for ONE plotter into the current subplot (a
 *  `hold on` is already in effect, so secondaries overlay). Data are written as inline
 *  row-vector literals; bin counts come from the plot spec. The calls end with `c_`, the colour
 *  the series is drawn in, for its legend sample. The plotted objects carry no name: the legend
 *  is built from samples of its own (with the gnuplot toolkit a named object would enter the
 *  key a second time). */
std::string octPlotCall(const GBasePlotter &p, scriptKind k) {
    EmitStream call;
    // Columns in storage order (see plotSpec().columns). GGraph2ED's stored order is
    // (x, ex, y, ey).
    const auto cols = p.dataColumns();
    const GPlotSpec spec = p.plotSpec();
    // How the series is drawn: the one description its legend sample comes from as well.
    const GDrawStyle style = scriptStyle(p);
    switch(k) {
    case scriptKind::g2d: {
        if(style.fill) {
            call << "h_ = area(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[1])
                 << ");" << '\n'
                 << "c_ = get(h_, 'FaceColor');" << '\n'
                 << "if ischar(c_), co_ = get(gca, 'ColorOrder'); c_ = co_(1, :); end" << '\n';
        }
        else {
            call << "h_ = plot(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[1])
                 << ", '" << octaveFormat(style) << "'" << lineWidthArgument(p) << ");" << '\n'
                 << "c_ = get(h_, 'Color');" << '\n';
        }
    } break;
    case scriptKind::g2ed: {
        // Y-error bars only: a single errorbar() carrying BOTH x- and y-error is not
        // portable across base Octave and MATLAB, so the x-error column is omitted here.
        call << "h_ = errorbar(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[2])
             << ", " << bracketedRowCol(cols[3]) << ", '" << octaveFormat(style) << "'"
             << lineWidthArgument(p) << ");" << '\n'
             << "c_ = get(h_, 'Color');" << '\n';
    } break;
    case scriptKind::g3d: {
        if(const surfaceProjection proj = projectionOf(p); proj != surfaceProjection::NONE) {
            // A triangulated surface over the scattered (x, y) points, and the same colour map
            // as a flat triangulation at the floor or ceiling of the box (coloured by z); the z
            // range is pinned to the data so the map lies exactly at its minimum or maximum.
            auto [z_lo, z_hi] = columnRange(cols[2]);
            if(not(z_hi > z_lo)) {
                z_hi = z_lo + 1.; // a flat surface still needs a z range to be drawn in
            }
            const double plane = (proj == surfaceProjection::FLOOR) ? z_lo : z_hi;
            call << "x_ = " << bracketedRowCol(cols[0]) << ";" << '\n'
                 << "y_ = " << bracketedRowCol(cols[1]) << ";" << '\n'
                 << "z_ = " << bracketedRowCol(cols[2]) << ";" << '\n'
                 << "t_ = delaunay(x_, y_);" << '\n';
            if(drawsSurface(p)) {
                call << "trisurf(t_, x_, y_, z_, 'EdgeColor', 'none');" << '\n';
            }
            call << "trisurf(t_, x_, y_, " << plane
                 << " * ones(size(z_)), z_, 'EdgeColor', 'none');" << '\n'
                 << "zlim([" << z_lo << " " << z_hi << "]);" << '\n';
            if(drawsSurface(p)) {
                call << "colorbar;" << '\n';
            }
            call << "view(3);" << '\n' << "cm_ = colormap(); c_ = cm_(round(end / 2), :);" << '\n';
        }
        else if(style.colour_map) {
            // A colour-mapped surface of its own drawing arguments (ROOT's SURF, COL, ...).
            call << "x_ = " << bracketedRowCol(cols[0]) << ";" << '\n'
                 << "y_ = " << bracketedRowCol(cols[1]) << ";" << '\n'
                 << "z_ = " << bracketedRowCol(cols[2]) << ";" << '\n'
                 << "trisurf(delaunay(x_, y_), x_, y_, z_, 'EdgeColor', 'none');" << '\n'
                 << "colorbar;" << '\n'
                 << "view(3);" << '\n'
                 << "cm_ = colormap(); c_ = cm_(round(end / 2), :);" << '\n';
        }
        else {
            if(castsFloorShadow(p)) {
                // The points' shadows on the floor, small and grey, drawn before the points.
                call << "xs_ = " << bracketedRowCol(cols[0]) << ";" << '\n'
                     << "ys_ = " << bracketedRowCol(cols[1]) << ";" << '\n'
                     << "zl_ = zlim();" << '\n'
                     << "plot3(xs_, ys_, zl_(1) * ones(size(xs_)), '.', 'Color', [0.6 0.6 0.6], "
                        "'MarkerSize', 4);"
                     << '\n';
            }
            // Points (red, as ROOT draws them), a line through them, or a line alone.
            call << "h_ = plot3(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[1])
                 << ", " << bracketedRowCol(cols[2]) << ", '" << octaveFormat(style) << "'"
                 << (style.line ? "" : ", 'Color', 'r'")
                 << (style.markers ? ", 'MarkerSize', 3" : "") << lineWidthArgument(p) << ");"
                 << '\n'
                 << "c_ = get(h_, 'Color');"
                 << '\n'
                 // With `hold on` already in effect Octave keeps the flat 2-d view, so a 3-d
                 // curve would be drawn as its x-y shadow; the 3-d view is requested explicitly.
                 << "view(3);" << '\n';
        }
    } break;
    case scriptKind::g4d: {
        // 3-d scatter coloured by the w-component, with a colourbar.
        call << "scatter3(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[1]) << ", "
             << bracketedRowCol(cols[2]) << ", 36, " << bracketedRowCol(cols[3]) << ", 'filled');"
             << '\n'
             << "colorbar;" << '\n'
             << "cm_ = colormap(); c_ = cm_(round(end / 2), :);" << '\n';
    } break;
    case scriptKind::hist1d:
    case scriptKind::hist1i: {
        // A 1-d histogram of the raw samples (float64 or int32); same base call.
        // A histogram spec always carries its bin counts; the fallback is unreachable
        call << "hist(" << bracketedRowCol(cols[0]) << ", "
             << spec.n_bins_x.value_or(Gem::Common::DEFAULTNBINSGPD) << ");" << '\n'
             << "b_ = findobj(gca, 'Type', 'patch'); c_ = get(b_(1), 'FaceColor');"
             << '\n'
             // bars coloured from the colour map name no colour of their own
             << "if ischar(c_), cm_ = colormap(); c_ = cm_(round(end / 2), :); end" << '\n';
    } break;
    case scriptKind::hist2d: {
        // A base-only 2-d histogram: bin x and y with histc over linspace edges, sum
        // the counts with accumarray, and draw the grid with imagesc (hist3 is avoided
        // -- it needs the statistics toolbox / package).
        call << "_x = " << bracketedRowCol(cols[0]) << "; _y = " << bracketedRowCol(cols[1]) << ";"
             << '\n'
             << "_nbx = " << spec.n_bins_x.value_or(Gem::Common::DEFAULTNBINSGPD)
             << "; _nby = " << spec.n_bins_y.value_or(Gem::Common::DEFAULTNBINSGPD) << ";" << '\n'
             << "_ex = linspace(min(_x), max(_x), _nbx + 1); "
             << "_ey = linspace(min(_y), max(_y), _nby + 1);" << '\n'
             << "[~, _bx] = histc(_x, _ex); [~, _by] = histc(_y, _ey);" << '\n'
             << "_bx = min(max(_bx, 1), _nbx); _by = min(max(_by, 1), _nby);" << '\n'
             << "_H = accumarray([_bx(:), _by(:)], 1, [_nbx, _nby]);" << '\n'
             << "imagesc([min(_x) max(_x)], [min(_y) max(_y)], _H'); axis xy; colorbar;" << '\n'
             << "cm_ = colormap(); c_ = cm_(round(end / 2), :);" << '\n';
    } break;
    }
    return call.str();
}

} // anonymous namespace

/******************************************************************************/
/**
 * Emits a self-contained Octave / MATLAB (.m) script for the graph plotters (GGraph2D /
 * GGraph2ED / GGraph3D / GGraph4D) and the histogram plotters (GHistogram1D /
 * GHistogram1I / GHistogram2D). Any other plotter type (e.g. a function plotter) triggers a clear
 * gem_exception. The script creates a figure and one `subplot` per pad, sets each
 * pad's axis labels and title and plots the pad's primary-and-secondary plotters into it
 * (with `hold on`). Only base Octave-and-MATLAB functions are used. It deliberately ends
 * WITHOUT a `print` / `saveas`, so it is terminal-agnostic: a validity harness (or the
 * caller) appends its own `print('-dpng', 'out.png')`.
 *
 * @param gpd The designer holding the plotters and canvas configuration
 * @return The complete Octave / MATLAB script as a string
 */
std::string OctaveEmitter::emitDocument(const GPlotDesigner &gpd) const {
    const std::size_t cols = gpd.c_x_div_;
    const std::size_t rows = gpd.c_y_div_;
    const std::size_t max_plots = cols * rows;

    EmitStream result;

    // Validate ALL plotters (and their secondaries) up front, so a partial script is
    // never produced for an unsupported plotter type; a 2-d and a 3-d plotter cannot
    // share one pad.
    validateScriptPads(gpd.plotters_cnt_, true, "OctaveEmitter::emitDocument()", "Octave");
    warnPadOverflow(
        "OctaveEmitter::emitDocument()",
        gpd.getCanvasLabel(),
        gpd.plotters_cnt_.size(),
        max_plots
    );

    // Preamble: the canvas title as a leading comment (sgtitle is version-fragile, so it is
    // not emitted as a call; a figure that leaves the title out therefore looks the same), then
    // the figure.
    result << "% Canvas: " << octaveEscape(gpd.getCanvasLabel()) << '\n'
           << "figure();" << '\n'
           << '\n';

    // Per pad: select the subplot, set labels/title, then plot the pad's primary-and-
    // secondary plotters into it (secondaries overlay via the active `hold on`).
    for(const auto &[idx, p] :
        gpd.plotters_cnt_ | std::views::enumerate | std::views::take(max_plots)) {
        const std::size_t pad_idx = static_cast<std::size_t>(idx) + 1; // subplot indices start at 1

        const scriptKind k = classifyForScript(*p, true, "OctaveEmitter::emitDocument()", "Octave");
        const bool three_d = isThreeDimensionalScript(k);

        result << "subplot(" << rows << ", " << cols << ", " << pad_idx << "); hold on;" << '\n';

        // Per-pad axis labels and title.
        result << "xlabel('" << octaveEscape(p->xAxisLabel()) << "');" << '\n'
               << "ylabel('" << octaveEscape(p->yAxisLabel()) << "');" << '\n';
        if(three_d) {
            result << "zlabel('" << octaveEscape(p->zAxisLabel()) << "');" << '\n';
        }
        if(p->xAxisLogarithmic()) {
            result << "set(gca, 'XScale', 'log');" << '\n';
        }
        if(p->yAxisLogarithmic()) {
            result << "set(gca, 'YScale', 'log');" << '\n';
        }
        result << "title('" << octaveEscape(p->padTitleOrLabel()) << "');" << '\n';

        // The primary plotter, then any secondary plotters overlaid in the same pad; the colour
        // each series is drawn in is kept for its legend sample.
        result << "cs_ = {};" << '\n' << octPlotCall(*p, k) << "cs_{end + 1} = c_;" << '\n';
        for(const auto &sp : p->secondaryPlotters()) {
            result << octPlotCall(
                          *sp,
                          classifyForScript(*sp, true, "OctaveEmitter::emitDocument()", "Octave")
                      )
                   << "cs_{end + 1} = c_;" << '\n';
        }
        // The legend the description lists (scriptLegendEntries()). Its samples are line objects
        // of their own, drawn at no point (NaN) in the series' colour as the description says --
        // a line, markers, both, or a filled square for a fill or a colour map -- and handed to
        // legend() with the texts, so that no object that is not listed can enter and a series whose own object
        // carries no name (a map, a histogram) is listed all the same.
        if(const auto entries = scriptLegendEntries(*p); not entries.empty()) {
            result << "s_ = [];" << '\n';
            for(const auto &e : entries) {
                const std::string colour = "cs_{" + std::to_string(e.series + 1) + "}";
                if(e.sample == legendSample::FILL or e.sample == legendSample::COLOUR_MAP) {
                    // A filled square marker: with the gnuplot toolkit (the suite's and the manual's
                    // renderer) a patch without data never reaches the key, a line object does.
                    result << "s_(end + 1) = "
                           << (three_d ? "plot3(NaN, NaN, NaN" : "plot(NaN, NaN")
                           << ", 'LineStyle', 'none', 'Marker', 's', 'MarkerSize', 10, "
                              "'MarkerFaceColor', "
                           << colour << ", 'MarkerEdgeColor', " << colour << ");" << '\n';
                }
                else {
                    const bool line = e.sample != legendSample::MARKER;
                    const bool marker = e.sample != legendSample::LINE;
                    result << "s_(end + 1) = "
                           << (three_d ? "plot3(NaN, NaN, NaN" : "plot(NaN, NaN")
                           << ", 'LineStyle', '" << (line ? "-" : "none") << "', 'Marker', '"
                           << (marker ? "o" : "none") << "', 'Color', " << colour
                           << (e.line_width > 0 ? ", 'LineWidth', " + std::to_string(e.line_width)
                                                : std::string{})
                           << ");" << '\n';
                }
            }
            result << "legend(s_, {";
            for(const auto &[i, e] : entries | std::views::enumerate) {
                result << (i == 0 ? "" : ", ") << "'" << octaveEscape(e.label) << "'";
            }
            result << "});" << '\n';
            // AUTO on a 3-d pad is the upper left, as in the other backends (Octave's default, the
            // upper right, lies over the box).
            std::string location = octaveLegendLocation(p->legendPlace());
            if(location.empty() and three_d) {
                location = "northwest";
            }
            if(not location.empty()) {
                result << "legend('location', '" << location << "');" << '\n';
            }
        }
        result << '\n';
    }

    return result.str();
}

/******************************************************************************/
} /* namespace Gem::Dietrich */
