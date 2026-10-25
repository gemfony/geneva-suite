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
            case '\'':           out += "''"; break;
            case '\n': case '\r': out += ' '; break;
            default:             out += c;    break;
        }
    }
    return out;
}

/** @brief Emit the Octave plotting call(s) for ONE plotter into the current subplot (a
 *  `hold on` is already in effect, so secondaries overlay). Data are written as inline
 *  row-vector literals; bin counts come from the plot spec. */
std::string octPlotCall(const GBasePlotter &p, scriptKind k) {
    EmitStream call; // NOLINT(cppcoreguidelines-init-variables)
    const std::string label = octaveEscape(p.plotLabel());
    // Columns in storage order (see plotSpec().columns). GGraph2ED's stored order is
    // (x, ex, y, ey).
    const auto cols = p.dataColumns();
    const GPlotSpec spec = p.plotSpec();
    switch(k) {
        case scriptKind::g2d: {
            call << "plot(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[1])
                 << ", '-o', 'DisplayName', '" << label << "');" << '\n';
        } break;
        case scriptKind::g2ed: {
            // Y-error bars only: a single errorbar() carrying BOTH x- and y-error is not
            // portable across base Octave and MATLAB, so the x-error column is omitted here.
            call << "errorbar(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[2]) << ", "
                 << bracketedRowCol(cols[3]) << ", 'o', 'DisplayName', '" << label << "');" << '\n';
        } break;
        case scriptKind::g3d: {
            call << "plot3(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[1]) << ", "
                 << bracketedRowCol(cols[2]) << ", 'DisplayName', '" << label << "');" << '\n'
                 // With `hold on` already in effect Octave keeps the flat 2-d view, so a 3-d curve
                 // would be drawn as its x-y shadow; the 3-d view is requested explicitly.
                 << "view(3);" << '\n';
        } break;
        case scriptKind::g4d: {
            // 3-d scatter coloured by the w-component, with a colourbar.
            call << "scatter3(" << bracketedRowCol(cols[0]) << ", " << bracketedRowCol(cols[1]) << ", "
                 << bracketedRowCol(cols[2]) << ", 36, " << bracketedRowCol(cols[3]) << ", 'filled');" << '\n'
                 << "colorbar;" << '\n';
        } break;
        case scriptKind::hist1d:
        case scriptKind::hist1i: {
            // A 1-d histogram of the raw samples (float64 or int32); same base call.
            // A histogram spec always carries its bin counts; the fallback is unreachable
            call << "hist(" << bracketedRowCol(cols[0]) << ", "
                 << spec.n_bins_x.value_or(Gem::Common::DEFAULTNBINSGPD) << ");" << '\n';
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
                 << "imagesc([min(_x) max(_x)], [min(_y) max(_y)], _H'); axis xy; colorbar;"
                 << '\n';
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
 * geneva_exception. The script creates a figure and one `subplot` per pad, sets each
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

    EmitStream result; // NOLINT(cppcoreguidelines-init-variables)

    // Validate ALL plotters (and their secondaries) up front, so a partial script is
    // never produced for an unsupported plotter type; a 2-d and a 3-d plotter cannot
    // share one pad.
    validateScriptPads(gpd.plotters_cnt_, true, "OctaveEmitter::emitDocument()", "Octave");
    warnPadOverflow(
        "OctaveEmitter::emitDocument()", gpd.getCanvasLabel(), gpd.plotters_cnt_.size(), max_plots
    );

    // Preamble: the canvas title as a leading comment (sgtitle is version-fragile, so it is
    // not emitted as a call), then the figure.
    result << "% Canvas: " << octaveEscape(gpd.getCanvasLabel()) << '\n'
           << "figure();" << '\n' << '\n';

    // Per pad: select the subplot, set labels/title, then plot the pad's primary-and-
    // secondary plotters into it (secondaries overlay via the active `hold on`).
    for(const auto &[idx, p] :
        gpd.plotters_cnt_ | std::views::enumerate | std::views::take(max_plots)) {
        const std::size_t pad_idx = static_cast<std::size_t>(idx) + 1; // subplot indices start at 1

        const scriptKind k =
            classifyForScript(*p, true, "OctaveEmitter::emitDocument()", "Octave");
        const bool three_d = isThreeDimensionalScript(k);

        result << "subplot(" << rows << ", " << cols << ", " << pad_idx << "); hold on;" << '\n';

        // Per-pad axis labels and title.
        result << "xlabel('" << octaveEscape(p->xAxisLabel()) << "');" << '\n'
               << "ylabel('" << octaveEscape(p->yAxisLabel()) << "');" << '\n';
        if(three_d) {
            result << "zlabel('" << octaveEscape(p->zAxisLabel()) << "');" << '\n';
        }
        result << "title('" << octaveEscape(p->plotLabel()) << "');" << '\n';

        // The primary plotter, then any secondary plotters overlaid in the same pad.
        result << octPlotCall(*p, k);
        for(const auto &sp : p->secondaryPlotters()) {
            result << octPlotCall(
                *sp, classifyForScript(*sp, true, "OctaveEmitter::emitDocument()", "Octave")
            );
        }
        result << '\n';
    }

    return result.str();
}


/******************************************************************************/
} /* namespace Gem::Dietrich */
