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
    switch(k) {
        case scriptKind::g2d: {
            call << ax << ".plot(" << bracketedRowCol(cols[0]) << ", "
                 << bracketedRowCol(cols[1]) << ", marker=\"o\", label=\"" << label << "\")" << '\n';
        } break;
        case scriptKind::g2ed: {
            call << ax << ".errorbar(" << bracketedRowCol(cols[0]) << ", "
                 << bracketedRowCol(cols[2]) << ", xerr=" << bracketedRowCol(cols[1])
                 << ", yerr=" << bracketedRowCol(cols[3]) << ", fmt=\"o\", label=\"" << label << "\")"
                 << '\n';
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
                     << "_z = " << bracketedRowCol(cols[2]) << '\n'
                     << "_surf = " << ax
                     << ".plot_trisurf(_x, _y, _z, cmap=\"viridis\", linewidth=0, alpha=0.85, zorder=1, label=\""
                     << label << "\")" << '\n'
                     << ax << ".tricontourf(_x, _y, _z, zdir=\"z\", offset=" << plane
                     << ", levels=20, cmap=\"viridis\", zorder=0)" << '\n'
                     << ax << ".set_zlim(" << z_lo << ", " << z_hi << ")" << '\n'
                     << fig << ".colorbar(_surf, ax=" << ax << ", shrink=0.6)" << '\n';
            }
            else {
                call << ax << ".plot(" << bracketedRowCol(cols[0]) << ", "
                     << bracketedRowCol(cols[1]) << ", " << bracketedRowCol(cols[2])
                     << ", label=\"" << label << "\")" << '\n';
            }
        } break;
        case scriptKind::g4d: {
            // 3-d scatter coloured by the w-component, with a colourbar.
            call << "_sc = " << ax << ".scatter(" << bracketedRowCol(cols[0]) << ", "
                 << bracketedRowCol(cols[1]) << ", " << bracketedRowCol(cols[2])
                 << ", c=" << bracketedRowCol(cols[3]) << ", cmap=\"viridis\", label=\"" << label
                 << "\")" << '\n'
                 << fig << ".colorbar(_sc, ax=" << ax << ")" << '\n';
        } break;
        case scriptKind::hist1d:
        case scriptKind::hist1i: {
            // A 1-d histogram of the raw samples (float64 or int32); same matplotlib call.
            // A histogram spec always carries its bin counts; the fallback is unreachable
            call << ax << ".hist(" << bracketedRowCol(cols[0]) << ", bins="
                 << spec.n_bins_x.value_or(Gem::Common::DEFAULTNBINSGPD) << ", label=\"" << label
                 << "\")" << '\n';
        } break;
        case scriptKind::hist2d: {
            // A histogram spec always carries its bin counts; the fallbacks are unreachable
            call << "_h = " << ax << ".hist2d(" << bracketedRowCol(cols[0]) << ", "
                 << bracketedRowCol(cols[1]) << ", bins=["
                 << spec.n_bins_x.value_or(Gem::Common::DEFAULTNBINSGPD) << ", "
                 << spec.n_bins_y.value_or(Gem::Common::DEFAULTNBINSGPD) << "])" << '\n'
                 << fig << ".colorbar(_h[3], ax=" << ax << ")" << '\n';
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
           << "from mpl_toolkits.mplot3d import Axes3D  # noqa: F401 (registers the 3d projection)"
           << '\n' << '\n'
           << "fig = plt.figure(figsize=(" << (cols == 0 ? 1 : cols) * 5 << ", "
           << (rows == 0 ? 1 : rows) * 4 << "))" << '\n'
           << "fig.suptitle(\"" << backslashEscape(gpd.getCanvasLabel()) << "\")" << '\n' << '\n';

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
        result << "ax.set_title(\"" << backslashEscape(p->plotLabel()) << "\")" << '\n';

        // The primary plotter, then any secondary plotters overlaid on the same Axes.
        result << mplPlotCall(*p, k, "ax", "fig");
        for(const auto &sp : p->secondaryPlotters()) {
            result << mplPlotCall(
                *sp,
                classifyForScript(*sp, true, "MatplotlibEmitter::emitDocument()", "matplotlib"),
                "ax",
                "fig"
            );
        }
        result << '\n';
    }

    result << "fig.tight_layout()" << '\n';

    return result.str();
}


/******************************************************************************/
} /* namespace Gem::Dietrich */
