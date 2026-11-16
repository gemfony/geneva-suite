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

#include "dietrich/GPlotDesigner.hpp"
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
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
#include "dietrich/plotting/detail/GPlotDetail.hpp"


namespace Gem::Dietrich {

// The plotting library builds on common's facilities (logging, serialization helpers,
// exception types, make_member, EmitStream, ...); make them visible here without
// per-name qualification. This affects lookup only within Gem::Dietrich.
using namespace Gem::Common;
// Dietrich declares its own to_string(plotKind), which would otherwise shadow common's
// numeric to_string(...) for unqualified calls; merge common's overloads back in.
using Gem::Common::to_string;

/******************************************************************************/
/**
 * The matplotlib-script file extension.
 *
 * @return The string ".py"
 */
std::string MatplotlibEmitter::fileExtension() const {
    return std::string(".py");
}

namespace {

/******************************************************************************/
// Helpers backing the matplotlib backend. The classification / validation / column-literal
// skeleton is the shared script-backend driver in detail/GPlotDetail.hpp; only the
// matplotlib-specific plotting calls live here.

/** @brief The keyword argument for a line width that was set (", linewidth=N"), or nothing */
std::string lineWidthArgument(const GBasePlotter &p) {
    return p.lineWidth() > 0 ? ", linewidth=" + std::to_string(p.lineWidth()) : std::string{};
}

/** @brief The keyword arguments of a line plot drawn as a style says: a line, markers, or both */
std::string mplLineAndMarker(const GDrawStyle &style) {
    return std::string(", marker=\"") + (style.markers ? "o" : "none") + "\""
        + (style.line ? "" : ", linestyle=\"none\"");
}

/** @brief matplotlib's format string of a style: "-o", "o", "-", or "none" */
std::string mplFormat(const GDrawStyle &style) {
    if(style.line and style.markers) {
        return "-o";
    }
    if(style.line or style.markers) {
        return style.line ? "-" : "o";
    }
    return "none";
}

/** @brief matplotlib's name of a legend corner ("lower right", ...); AUTO has none */
std::string matplotlibLegendLocation(legendPosition position) {
    switch(position) {
        case legendPosition::TOP_LEFT:     return "upper left";
        case legendPosition::TOP_RIGHT:    return "upper right";
        case legendPosition::BOTTOM_LEFT:  return "lower left";
        case legendPosition::BOTTOM_RIGHT: return "lower right";
        case legendPosition::CENTER_RIGHT: return "center right";
        case legendPosition::AUTO:         break;
        case legendPosition::FREEST:       break; // resolved before (GBasePlotter::legendPlace())
    }
    return {};
}

/** @brief Emit the matplotlib plotting call(s) for ONE plotter into the named Axes `ax`. The data are
 *  written as inline Python list literals; `ax` is a 2-d Axes for g2d/g2ed/hist*, a 3-d Axes for
 *  g3d/g4d. `fig` is the figure name (used for the g4d/hist2d colourbar). All statements are emitted at
 *  column 0 -- Python is whitespace-sensitive, so no indentation is applied. */
std::string mplPlotCall(
    const GBasePlotter &p,
    scriptKind k,
    const std::string &ax,
    const std::string &fig
) {
    EmitStream call; // NOLINT(cppcoreguidelines-init-variables)
    const std::string label = backslashEscape(p.plotLabel());
    // Columns in storage order (see plotSpec().columns); histogram bin counts come
    // from the same spec. GGraph2ED's stored order is (x, ex, y, ey) -> errorbar wants
    // x, y with xerr/yerr.
    const auto cols = p.dataColumns();
    const GPlotSpec spec = p.plotSpec();
    // How the series is drawn: the one description its legend sample comes from as well.
    const GDrawStyle style = scriptStyle(p);
    switch(k) {
        case scriptKind::g2d: {
            if(style.fill) {
                call << "_a = " << ax << ".fill_between(" << bracketedRowCol(cols[0]) << ", "
                     << bracketedRowCol(cols[1]) << ", label=\"" << label << "\")" << '\n'
                     << "_c = _a.get_facecolor()[0]" << '\n';
            } else {
                call << "_a = " << ax << ".plot(" << bracketedRowCol(cols[0]) << ", "
                     << bracketedRowCol(cols[1]) << mplLineAndMarker(style) << ", label=\"" << label
                     << "\"" << lineWidthArgument(p) << ")" << '\n'
                     << "_c = _a[0].get_color()" << '\n';
            }
        } break;
        case scriptKind::g2ed: {
            call << "_a = " << ax << ".errorbar(" << bracketedRowCol(cols[0]) << ", "
                 << bracketedRowCol(cols[2]) << ", xerr=" << bracketedRowCol(cols[1])
                 << ", yerr=" << bracketedRowCol(cols[3]) << ", fmt=\"" << mplFormat(style)
                 << "\", label=\"" << label << "\"" << lineWidthArgument(p) << ")" << '\n'
                 << "_c = _a[0].get_color()" << '\n';
        } break;
        case scriptKind::g3d: {
            if(const surfaceProjection proj = projectionOf(p); proj != surfaceProjection::NONE) {
                // A surface over the scattered (x, y) points, and the same colour map as filled
                // contours on the floor or ceiling of the box; the z range is pinned to the data so
                // the map lies exactly at its minimum or maximum.
                auto [z_lo, z_hi] = columnRange(cols[2]);
                if(not(z_hi > z_lo)) {
                    z_hi = z_lo + 1.; // a flat surface still needs a z range to be drawn in
                }
                const double plane = (proj == surfaceProjection::FLOOR) ? z_lo : z_hi;
                // matplotlib sorts 3-d artists by a computed depth, which puts a map and a path drawn
                // at the same height in arbitrary order; a fixed order is used instead: the map, then
                // the surface, then the pad's overlays (default zorder 2), which therefore stay visible.
                call << ax << ".computed_zorder = False" << '\n'
                     << "_x = " << bracketedRowCol(cols[0]) << '\n'
                     << "_y = " << bracketedRowCol(cols[1]) << '\n'
                     << "_z = " << bracketedRowCol(cols[2]) << '\n';
                if(drawsSurface(p)) {
                    call << "_surf = " << ax
                         << ".plot_trisurf(_x, _y, _z, cmap=\"viridis\", linewidth=0, alpha=0.85, zorder=1, label=\""
                         << label << "\")" << '\n';
                }
                call << ax << ".tricontourf(_x, _y, _z, zdir=\"z\", offset=" << plane
                     << ", levels=20, cmap=\"viridis\", zorder=0)" << '\n'
                     << ax << ".set_zlim(" << z_lo << ", " << z_hi << ")" << '\n';
                if(drawsSurface(p)) {
                    call << fig << ".colorbar(_surf, ax=" << ax << ", shrink=0.6)" << '\n';
                }
                call << "_c = matplotlib.colormaps[\"viridis\"](0.5)" << '\n';
            }
            else if(style.colour_map) {
                // A colour-mapped surface of its own drawing arguments (ROOT's SURF, COL, ...).
                call << "_surf = " << ax << ".plot_trisurf(" << bracketedRowCol(cols[0]) << ", "
                     << bracketedRowCol(cols[1]) << ", " << bracketedRowCol(cols[2])
                     << ", cmap=\"viridis\", linewidth=0, alpha=0.85, zorder=1)" << '\n'
                     << fig << ".colorbar(_surf, ax=" << ax << ", shrink=0.6)" << '\n'
                     << "_c = matplotlib.colormaps[\"viridis\"](0.5)" << '\n';
            }
            else {
                if(castsFloorShadow(p)) {
                    // The points' shadows on the floor, small and grey, drawn before the points.
                    call << ax << ".scatter(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[1])
                         << ", zs=" << ax << ".get_zlim()[0], s=2, c=\"0.6\", zorder=1)" << '\n';
                }
                // Points (red, as ROOT draws them), a line through them, or a line alone.
                call << "_a = " << ax << ".plot(" << bracketedRowCol(cols[0]) << ", "
                     << bracketedRowCol(cols[1]) << ", " << bracketedRowCol(cols[2]) << mplLineAndMarker(style)
                     << (style.markers ? ", markersize=3" : "") << (style.line ? "" : ", color=\"red\"")
                     << lineWidthArgument(p) << ", label=\"" << label << "\", zorder=2)" << '\n'
                     << "_c = _a[0].get_color()" << '\n';
            }
        } break;
        case scriptKind::g4d: {
            // 3-d scatter coloured by the w-component, with a colourbar.
            call << "_sc = " << ax << ".scatter(" << bracketedRowCol(cols[0]) << ", "
                 << bracketedRowCol(cols[1]) << ", " << bracketedRowCol(cols[2])
                 << ", c=" << bracketedRowCol(cols[3]) << ", cmap=\"viridis\", label=\"" << label
                 << "\")" << '\n'
                 << fig << ".colorbar(_sc, ax=" << ax << ")" << '\n'
                 << "_c = matplotlib.colormaps[\"viridis\"](0.5)" << '\n';
        } break;
        case scriptKind::hist1d:
        case scriptKind::hist1i: {
            // A 1-d histogram of the raw samples (float64 or int32); same matplotlib call.
            // A histogram spec always carries its bin counts; the fallback is unreachable
            call << "_a = " << ax << ".hist(" << bracketedRowCol(cols[0]) << ", bins="
                 << spec.n_bins_x.value_or(Gem::Common::DEFAULTNBINSGPD) << ", label=\"" << label
                 << "\")" << '\n'
                 << "_c = _a[2][0].get_facecolor()" << '\n';
        } break;
        case scriptKind::hist2d: {
            // A histogram spec always carries its bin counts; the fallbacks are unreachable
            call << "_h = " << ax << ".hist2d(" << bracketedRowCol(cols[0]) << ", "
                 << bracketedRowCol(cols[1]) << ", bins=["
                 << spec.n_bins_x.value_or(Gem::Common::DEFAULTNBINSGPD) << ", "
                 << spec.n_bins_y.value_or(Gem::Common::DEFAULTNBINSGPD) << "])" << '\n'
                 << fig << ".colorbar(_h[3], ax=" << ax << ")" << '\n'
                 << "_c = matplotlib.colormaps[\"viridis\"](0.5)" << '\n';
        } break;
    }
    return call.str();
}

} // anonymous namespace

/******************************************************************************/
/**
 * Emits a self-contained Python/matplotlib script for the graph plotters (GGraph2D /
 * GGraph2ED / GGraph3D / GGraph4D) and the histogram plotters (GHistogram1D /
 * GHistogram1I / GHistogram2D). Any other plotter type (e.g. a function plotter) triggers a clear
 * geneva_exception. The script selects the headless Agg backend, creates a `fig` and
 * one Axes per pad (a 3-d Axes for the 3-d graphs), sets each pad's axis labels and
 * title and plots the pad's primary-and-secondary plotters into it. It deliberately
 * ends WITHOUT `fig.savefig(...)`, so it is terminal-agnostic: a validity harness (or
 * the caller) appends its own `fig.savefig('out.png')`.
 *
 * @param gpd The designer holding the plotters and canvas configuration
 * @return The complete matplotlib (Python) script as a string
 */
std::string MatplotlibEmitter::emitDocument(const GPlotDesigner &gpd) const {
    const std::size_t cols = gpd.c_x_div_;
    const std::size_t rows = gpd.c_y_div_;
    const std::size_t max_plots = cols * rows;

    EmitStream result; // NOLINT(cppcoreguidelines-init-variables)

    // Validate ALL plotters (and their secondaries) up front, so a partial script is
    // never produced for an unsupported plotter type; a 2-d and a 3-d plotter cannot
    // share one pad (one is a flat Axes, the other an mplot3d Axes).
    validateScriptPads(gpd.plotters_cnt_, true, "MatplotlibEmitter::emitDocument()", "matplotlib");
    warnPadOverflow(
        "MatplotlibEmitter::emitDocument()", gpd.getCanvasLabel(), gpd.plotters_cnt_.size(), max_plots
    );

    // NOTE: Python is whitespace-sensitive, so every statement below is emitted at column 0 (the
    // GPlotDesigner indentation setting does not apply to a Python script).

    // Script preamble: select the headless Agg backend BEFORE importing pyplot, then create the
    // figure. Pads are added individually via fig.add_subplot(rows, cols, idx[, projection='3d']) so a
    // 3-d pad gets an mplot3d Axes while 2-d pads stay flat -- a single plt.subplots() grid cannot mix
    // the two.
    result << "import matplotlib" << '\n'
           << "matplotlib.use(\"Agg\")" << '\n'
           << "import matplotlib.pyplot as plt" << '\n'
           << "from matplotlib.lines import Line2D" << '\n'
           << "from matplotlib.patches import Patch" << '\n'
           << "from mpl_toolkits.mplot3d import Axes3D  # noqa: F401 (registers the 3d projection)"
           << '\n' << '\n'
           // A legend sample made from the pad's legend description: a line, markers, both, or a
           // filled box, in the colour its series is drawn in.
           << "def _legend_sample(colour, sample, width):" << '\n'
           << "    if sample in (\"fill\", \"colour_map\"):" << '\n'
           << "        return Patch(facecolor=colour)" << '\n'
           << "    line = sample in (\"line\", \"line_and_marker\")" << '\n'
           << "    marker = sample in (\"marker\", \"line_and_marker\")" << '\n'
           << "    extra = {\"linewidth\": width} if width > 0 else {}" << '\n'
           << "    return Line2D([], [], color=colour, linestyle=\"-\" if line else \"none\","
           << " marker=\"o\" if marker else \"none\", **extra)" << '\n'
           << '\n'
           << "fig = plt.figure(figsize=(" << (cols == 0 ? 1 : cols) * 5 << ", "
           << (rows == 0 ? 1 : rows) * 4 << "))" << '\n';
    if(gpd.getShowCanvasLabel()) {
        result << "fig.suptitle(\"" << backslashEscape(gpd.getCanvasLabel()) << "\")" << '\n';
    }
    result << '\n';

    // Per pad: create the Axes with the right projection, set labels/title, then plot the pad's
    // primary-and-secondary plotters into it (secondaries overlay on the same Axes).
    for(const auto &[idx, p] :
        gpd.plotters_cnt_ | std::views::enumerate | std::views::take(max_plots)) {
        const std::size_t pad_idx = static_cast<std::size_t>(idx) + 1; // matplotlib subplot indices start at 1

        const scriptKind k =
            classifyForScript(*p, true, "MatplotlibEmitter::emitDocument()", "matplotlib");
        const bool three_d = isThreeDimensionalScript(k);

        result << "ax = fig.add_subplot(" << rows << ", " << cols << ", " << pad_idx
               << (three_d ? ", projection=\"3d\"" : "") << ")" << '\n';

        // Per-pad axis labels and title.
        result << "ax.set_xlabel(\"" << backslashEscape(p->xAxisLabel()) << "\")" << '\n'
               << "ax.set_ylabel(\"" << backslashEscape(p->yAxisLabel()) << "\")" << '\n';
        if(three_d) {
            result << "ax.set_zlabel(\"" << backslashEscape(p->zAxisLabel()) << "\")" << '\n';
        }
        // Logarithmic axes, for two-dimensional plots: matplotlib's 3-d axes do not support them.
        if(not three_d && p->xAxisLogarithmic()) {
            result << "ax.set_xscale(\"log\")" << '\n';
        }
        if(not three_d && p->yAxisLogarithmic()) {
            result << "ax.set_yscale(\"log\")" << '\n';
        }
        result << "ax.set_title(\"" << backslashEscape(p->padTitleOrLabel()) << "\")" << '\n';

        // The primary plotter, then any secondary plotters overlaid on the same Axes; the colour
        // each series is drawn in is kept for its legend sample.
        result << "_colours = []" << '\n' << mplPlotCall(*p, k, "ax", "fig") << "_colours.append(_c)" << '\n';
        for(const auto &sp : p->secondaryPlotters()) {
            result << mplPlotCall(
                          *sp,
                          classifyForScript(*sp, true, "MatplotlibEmitter::emitDocument()", "matplotlib"),
                          "ax",
                          "fig"
                      )
                   << "_colours.append(_c)" << '\n';
        }
        // The legend the description lists (GBasePlotter::legendEntries()), with samples made from
        // it: no artist that is not listed can enter, and a series whose artist cannot carry a
        // legend entry (a filled contour map) is listed all the same.
        if(const auto entries = scriptLegendEntries(*p); not entries.empty()) {
            result << "ax.legend(handles=[";
            for(const auto &[i, e] : entries | std::views::enumerate) {
                result << (i == 0 ? "" : ", ") << "_legend_sample(_colours[" << e.series << "], \""
                       << to_string(e.sample) << "\", " << e.line_width << ")";
            }
            result << "], labels=[";
            for(const auto &[i, e] : entries | std::views::enumerate) {
                result << (i == 0 ? "" : ", ") << "\"" << backslashEscape(e.label) << "\"";
            }
            result << "]";
            // AUTO on a 3-d Axes is the upper left: matplotlib's "best" place is computed in the
            // flat projection and put the legend over the box and the axis labels.
            std::string location = matplotlibLegendLocation(p->legendPlace());
            if(location.empty() and three_d) {
                location = "upper left";
            }
            if(not location.empty()) {
                result << ", loc=\"" << location << "\"";
            }
            result << ")" << '\n';
        }
        result << '\n';
    }

    result << "fig.tight_layout()" << '\n';

    return result.str();
}


/******************************************************************************/
} /* namespace Gem::Dietrich */
