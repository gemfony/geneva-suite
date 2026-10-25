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
 * The gnuplot-script file extension.
 *
 * @return The string ".gp"
 */
std::string GnuplotEmitter::fileExtension() const {
    return std::string(".gp");
}

namespace {

/** Grid nodes per axis onto which a projected surface's scattered points are interpolated. */
constexpr int GNUPLOT_SURFACE_GRID = 50;

/******************************************************************************/
// Shared helpers for reading a GPlotColumn (the type-tagged float64 / int32 view a
// plotter exposes via dataColumns()). These let every non-ROOT backend consume a
// column generically; an int32 column streams as integers and exports as a real
// int32 numpy array, while a double column is unchanged. Declared first so the
// gnuplot, matplotlib and data sections below can all use them.


/** @brief The double-typed value vector behind a column. Only valid for a column known
 *  to be float64 (the graph plotters, whose axes are always double); throws otherwise. */
const std::vector<double> &asDoubleColumn(const GPlotColumn &c) {
    return *std::get<const std::vector<double> *>(c);
}

/******************************************************************************/
// Helpers backing the gnuplot backend. A "dataset" is one plotter's inline data
// block (`x y ...` rows terminated by `e`); a "spec" is the leading
// `'-' with <style> title "..."` fragment naming that dataset inside a (s)plot
// command. The graph plotters expose their columns via the public const
// column<I>() accessor added to GDataCollectorT.

/** @brief The `with <style> ...` spec for a plotter's dataset (the dataset source -- a named
 *  datablock -- is prepended by the caller). gnuplot cannot read inline `'-'` data inside a
 *  `set multiplot` block, so each dataset is emitted as a `$Dn` datablock and referenced here. */
std::string datasetSpec(const GBasePlotter &p, scriptKind k) {
    EmitStream spec; // NOLINT(cppcoreguidelines-init-variables)
    const std::string title = backslashEscape(p.plotLabel());
    switch(k) {
        case scriptKind::g2d:
            spec << "with linespoints title \"" << title << "\"";
            break;
        case scriptKind::g2ed:
            spec << "with xyerrorbars title \"" << title << "\"";
            break;
        case scriptKind::g3d:
            spec << "with linespoints title \"" << title << "\"";
            break;
        case scriptKind::g4d:
            spec << "using 1:2:3:4 with points palette pointtype 7 title \"" << title << "\"";
            break;
        default: // unreachable: validateScriptPads rejected the histogram kinds up front
            break;
    }
    return spec.str();
}

/** @brief A plotter's data rows (one point per line). Used as the body of a gnuplot `$Dn << EOD`
 *  datablock, so it carries no `e`/`EOD` terminator -- the caller adds it. */
std::string datasetRows(const GBasePlotter &p, scriptKind k, const std::string &indent) {
    EmitStream rows; // NOLINT(cppcoreguidelines-init-variables)
    // Columns come back in storage order (see plotSpec().columns); the gnuplot row
    // layout is the same EXCEPT GGraph2ED, whose stored (x, ex, y, ey) is re-ordered
    // to the `x y xdelta ydelta` that the xyerrorbars style expects.
    // Graph plotters are always float64, so each column is read as a double vector.
    const auto cols = p.dataColumns();
    switch(k) {
        case scriptKind::g2d: {
            const auto &x = asDoubleColumn(cols[0]);
            const auto &y = asDoubleColumn(cols[1]);
            for(std::size_t i = 0; i < x.size(); ++i) {
                rows << indent << x[i] << ' ' << y[i] << '\n';
            }
        } break;
        case scriptKind::g2ed: {
            const auto &x  = asDoubleColumn(cols[0]);
            const auto &ex = asDoubleColumn(cols[1]);
            const auto &y  = asDoubleColumn(cols[2]);
            const auto &ey = asDoubleColumn(cols[3]);
            for(std::size_t i = 0; i < x.size(); ++i) {
                rows << indent << x[i] << ' ' << y[i] << ' ' << ex[i] << ' ' << ey[i] << '\n';
            }
        } break;
        case scriptKind::g3d: {
            const auto &x = asDoubleColumn(cols[0]);
            const auto &y = asDoubleColumn(cols[1]);
            const auto &z = asDoubleColumn(cols[2]);
            for(std::size_t i = 0; i < x.size(); ++i) {
                rows << indent << x[i] << ' ' << y[i] << ' ' << z[i] << '\n';
            }
        } break;
        case scriptKind::g4d: {
            const auto &x = asDoubleColumn(cols[0]);
            const auto &y = asDoubleColumn(cols[1]);
            const auto &z = asDoubleColumn(cols[2]);
            const auto &w = asDoubleColumn(cols[3]);
            for(std::size_t i = 0; i < x.size(); ++i) {
                rows << indent << x[i] << ' ' << y[i] << ' ' << z[i] << ' ' << w[i] << '\n';
            }
        } break;
        default: // unreachable: validateScriptPads rejected the histogram kinds up front
            break;
    }
    return rows.str();
}

/******************************************************************************/
/**
 * @brief Emits the `$Dn` datablocks -- one per primary-or-secondary graph, in plot order -- up front.
 *
 * gnuplot cannot read an inline `'-'` dataset from inside a `set multiplot` block (it warns "Reading from
 * '-' inside a multiplot not supported" and renders nothing), so every dataset is emitted here as a named
 * `$Dn` datablock and referenced from the plot commands later. The datablock counter advances in the SAME
 * order emitMultiplot() references it, so `$Dn` lines up with its data.
 *
 * @param result The script stream to append to
 * @param plotters The designer's plotters (primary graphs; each may carry secondary plotters)
 * @param max_plots The pad-grid capacity; plotters beyond it are skipped
 */
void emitDatablocks(
    EmitStream &result,
    const std::vector<std::shared_ptr<GBasePlotter>> &plotters,
    std::size_t max_plots
) {
    std::size_t db_idx = 0;
    std::size_t n_plots = 0;
    for(const auto &p : plotters) {
        if(n_plots++ >= max_plots) {
            break;
        }
        std::vector<const GBasePlotter *> pad;
        pad.push_back(p.get());
        for(const auto &sp : p->secondaryPlotters()) {
            pad.push_back(sp.get());
        }
        for(const auto *dp : pad) {
            result << "$D" << db_idx << " << EOD" << '\n'
                   << datasetRows(*dp, classifyForScript(*dp, false, "GnuplotEmitter::emitDocument()", "gnuplot"), "") << "EOD" << '\n';
            // A projected surface is drawn with pm3d, which needs gridded data: the scattered points
            // are interpolated onto a regular grid ONCE, into $Gn, and the gridding is switched off
            // again so that the pad's overlays keep their own points.
            if(projectionOf(*dp) != surfaceProjection::NONE) {
                result << "set dgrid3d " << GNUPLOT_SURFACE_GRID << "," << GNUPLOT_SURFACE_GRID << '\n'
                       << "set table $G" << db_idx << '\n'
                       << "splot $D" << db_idx << " using 1:2:3" << '\n'
                       << "unset table" << '\n'
                       << "unset dgrid3d" << '\n';
            }
            ++db_idx;
        }
    }
}

/******************************************************************************/
/**
 * @brief Emits the `set multiplot` grid: the layout header, then per pad the axis labels / title and a
 * `plot`/`splot` referencing the datablocks emitted by emitDatablocks(), then `unset multiplot`.
 *
 * The datablock reference counter advances in the SAME order as emitDatablocks(), so `$Dn` lines up with
 * its data.
 *
 * @param result The script stream to append to
 * @param plotters The designer's plotters (primary graphs; each may carry secondary plotters)
 * @param max_plots The pad-grid capacity; plotters beyond it are skipped
 * @param rows The number of pad rows (c_y_div)
 * @param cols The number of pad columns (c_x_div)
 * @param canvas_label The overall canvas title
 * @param indent The per-line indentation string
 */
void emitMultiplot(
    EmitStream &result,
    const std::vector<std::shared_ptr<GBasePlotter>> &plotters,
    std::size_t max_plots,
    std::size_t rows,
    std::size_t cols,
    const std::string &canvas_label,
    const std::string &indent
) {
    // The multiplot grid (rows = c_y_div, cols = c_x_div).
    result << "set multiplot layout " << rows << "," << cols << " title \""
           << backslashEscape(canvas_label) << "\"" << '\n' << '\n';

    // Per pad: axis labels / title, then a plot/splot referencing the datablocks. The datablock counter
    // advances in the SAME order as emitDatablocks(), so $Dn lines up with its data.
    std::size_t db_ref = 0;
    std::size_t n_plots = 0;
    for(const auto &p : plotters) {
        if(n_plots++ >= max_plots) {
            break;
        }

        const scriptKind k =
            classifyForScript(*p, false, "GnuplotEmitter::emitDocument()", "gnuplot");
        const bool three_d = isThreeDimensionalScript(k);

        // Per-pad axis labels and title.
        result << indent << "set xlabel \"" << backslashEscape(p->xAxisLabel()) << "\"" << '\n'
               << indent << "set ylabel \"" << backslashEscape(p->yAxisLabel()) << "\"" << '\n';
        if(three_d) {
            result << indent << "set zlabel \"" << backslashEscape(p->zAxisLabel()) << "\"" << '\n';
        }
        result << indent << "set title \"" << backslashEscape(p->plotLabel()) << "\"" << '\n';

        // GGraph4D maps its w-component to a colour palette.
        if(k == scriptKind::g4d) {
            result << indent << "set palette" << '\n';
        }

        // Collect this pad's datasets (primary first, then any secondary plotters).
        std::vector<const GBasePlotter *> pad;
        pad.push_back(p.get());
        for(const auto &sp : p->secondaryPlotters()) {
            pad.push_back(sp.get());
        }

        // The (s)plot command: one `$Dn <style>` reference per dataset, comma-separated.
        // A projected surface: pm3d draws the gridded data as a coloured surface (s) and its colour
        // map on the base (b) or top (t) of the box. The z range is pinned to the data and the base
        // plane put at its minimum, so the map lies exactly at the data's minimum or maximum.
        const surfaceProjection proj = projectionOf(*p);
        if(proj != surfaceProjection::NONE) {
            auto [z_lo, z_hi] = columnRange(p->dataColumns()[2]);
            if(not(z_hi > z_lo)) {
                z_hi = z_lo + 1.;
            }
            result << indent << "set pm3d at " << (proj == surfaceProjection::FLOOR ? "bs" : "ts") << '\n'
                   << indent << "set zrange [" << z_lo << ":" << z_hi << "]" << '\n'
                   << indent << "set xyplane at " << z_lo << '\n';
        }

        result << indent << (three_d ? "splot " : "plot ");
        for(std::size_t i = 0; i < pad.size(); ++i) {
            result << (i == 0 ? "" : ", ");
            if(i == 0 && proj != surfaceProjection::NONE) {
                result << "$G" << db_ref << " with pm3d title \"" << backslashEscape(pad[i]->plotLabel()) << "\"";
            }
            else {
                result << "$D" << db_ref << ' '
                       << datasetSpec(*pad[i], classifyForScript(*pad[i], false, "GnuplotEmitter::emitDocument()", "gnuplot"));
            }
            ++db_ref;
        }
        result << '\n' << '\n';

        if(proj != surfaceProjection::NONE) {
            // Restore the defaults for the next pad of the multiplot.
            result << indent << "unset pm3d" << '\n'
                   << indent << "set autoscale z" << '\n'
                   << indent << "set xyplane relative 0.5" << '\n';
        }
    }

    result << "unset multiplot" << '\n';
}

} // anonymous namespace

/******************************************************************************/
/**
 * Emits a gnuplot script for the graph plotters (GGraph2D / GGraph2ED / GGraph3D /
 * GGraph4D). Any other plotter type triggers a clear geneva_exception. The script
 * is terminal-agnostic (the caller / validity harness prepends `set terminal` and
 * `set output`): it lays the pads out as a `set multiplot` grid and, per pad, sets
 * the axis labels and title and emits a `plot`/`splot` with one inline dataset per
 * primary-or-secondary plotter sharing that pad.
 *
 * @param gpd The designer holding the graph plotters and canvas configuration
 * @return The complete gnuplot script as a string
 */
std::string GnuplotEmitter::emitDocument(const GPlotDesigner &gpd) const {
    const std::size_t cols = gpd.c_x_div_;
    const std::size_t rows = gpd.c_y_div_;
    const std::size_t max_plots = cols * rows;

    EmitStream result; // NOLINT(cppcoreguidelines-init-variables)

    // Validate ALL plotters (and their secondaries) up front, so a partial script is
    // never produced for an unsupported plotter type (the gnuplot backend supports only
    // the graph plotters); a 2-d and a 3-d graph cannot share one pad.
    validateScriptPads(gpd.plotters_cnt_, false, "GnuplotEmitter::emitDocument()", "gnuplot");
    warnPadOverflow(
        "GnuplotEmitter::emitDocument()", gpd.getCanvasLabel(), gpd.plotters_cnt_.size(), max_plots
    );

    // gnuplot cannot read an inline `'-'` dataset from inside a `set multiplot` block, so every dataset
    // is emitted up front as a named `$Dn` datablock (pass 1) and referenced from the plot commands in
    // the multiplot grid (pass 2). Both passes walk the plotters in the same order, so `$Dn` lines up.
    emitDatablocks(result, gpd.plotters_cnt_, max_plots);
    result << '\n';
    emitMultiplot(result, gpd.plotters_cnt_, max_plots, rows, cols, gpd.getCanvasLabel(), gpd.indent());

    return result.str();
}


/******************************************************************************/
} /* namespace Gem::Dietrich */
