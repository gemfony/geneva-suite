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
 * NOTE THAT THE LICENSE MENTIONED ABOVE APPLIES TO THIS FILE ONLY, and not to
 * any other files in this repository, unless explicitly stated otherwise.
 *
 * For further information on Gemfony scientific, see http://www.gemfony.eu .
 *
 * The majority of files in Geneva was released under the Apache license v2.0
 * in February 2020, with copyright by Gemfony scientific.
 *
 ********************************************************************************/

#pragma once

/**
 * @file GPlotDetail.hpp
 * @brief Library-private helpers shared by the plotting implementation files (GPlotDesigner.cpp, the
 * plot-type implementations GGraphPlots.cpp / GHistogramPlots.cpp, and the per-backend emitters
 * GRootEmitter.cpp / GGnuplotEmitter.cpp / ...). These are implementation details of the dietrich
 * plotting library: the header lives under a `detail/` subdirectory and is deliberately NOT part of
 * the installed public API (it is not in the INSTALL list) -- do not include it from outside the
 * dietrich sources. It was extracted from the former monolithic GPlotDesigner.cpp when that was split
 * into several translation units, so every plotting TU shares one definition.
 */

#include "dietrich/plotting/GBasePlotter.hpp" // GPlotColumn

#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Gem::Dietrich {

/******************************************************************************/
/**
 * A locale-independent, full-round-trip-precision string builder for emitted ROOT macro text.
 * Every coordinate value must be emitted through such a stream: a default std::ostringstream prints
 * only 6 significant digits (lossy for optimization traces) and honours the global locale (a
 * comma-decimal locale would emit "12,5", which ROOT misparses as two arguments). Imbuing the
 * classic ("C") locale and setting max_digits10 precision once, in the constructor, fixes both for
 * all subsequent insertions.
 */
class EmitStream : public std::ostringstream {
public:
    EmitStream() {
        this->imbue(std::locale::classic());
        *this << std::setprecision(std::numeric_limits<double>::max_digits10);
    }
};

/******************************************************************************/
/**
 * Escape a user-supplied string for safe inclusion inside a ROOT C-string literal (axis/plot/canvas
 * labels, TF1/TF2 formulae) or a // comment. Without this, a label containing a double quote,
 * backslash or newline would produce a non-compiling macro (or, inside a comment, inject code past
 * the comment). Only the C-string metacharacters are touched, so ROOT TLatex markup (#frac, #sqrt,
 * braces) passes through unchanged.
 */
inline std::string rootEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for(char const c : in) {
        switch(c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r':                break; // drop bare CRs
            default:   out += c;      break;
        }
    }
    return out;
}

/******************************************************************************/
/**
 * Builds the "// <marker>" ROOT comment for a data-set marker, or an empty string when no marker was
 * set. This construction used to be copy-pasted (in an assignment and a direct-streaming form, plus
 * a redundant emptiness ternary at the use sites) throughout the graph / histogram plotters.
 *
 * @param ds_marker The (unescaped) data-set marker; may be empty
 * @return The escaped ROOT comment line content, or an empty string
 */
inline std::string dsMarkerComment(const std::string &ds_marker) {
    return ds_marker.empty() ? std::string{} : "// " + rootEscape(ds_marker);
}

/******************************************************************************/
/**
 * Emits one ROOT SetTitle line for @p obj into @p s: the escaped @p label if one was set, else the
 * single-space placeholder ROOT needs to suppress its default object title. This block used to be
 * copy-pasted into every graph / histogram plotter's footer.
 *
 * @param s The stream the ROOT macro line is written to
 * @param indent The indentation prefix of the emitted line
 * @param obj The ROOT object variable name the title is set on
 * @param label The (unescaped) plot label; may be empty
 */
inline void emitRootTitle(
    std::ostream &s,
    const std::string &indent,
    const std::string &obj,
    const std::string &label
) {
    if(!label.empty()) {
        s << indent << obj << "->SetTitle(\"" << rootEscape(label) << "\");" << '\n';
    }
    else {
        s << indent << obj << "->SetTitle(\" \");" << '\n';
    }
}

/******************************************************************************/
/**
 * Escape a user-supplied string for safe inclusion inside a DOUBLE-quoted string
 * literal that interprets C-style backslash escapes (\" \\ \n \t \r). Both gnuplot
 * and Python double-quoted literals share exactly this contract, so the gnuplot and
 * matplotlib backends use this one function. It is deliberately NOT rootEscape
 * (which targets a ROOT C-string literal and additionally drops bare CRs). The
 * result is always inserted between two `"` delimiters.
 */
inline std::string backslashEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for(char const c : in) {
        switch(c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:   out += c;      break;
        }
    }
    return out;
}

/******************************************************************************/
/**
 * @brief The number of elements in a column (visits whichever value vector it holds). Shared by the
 * gnuplot and DATA backends, so it lives here rather than in one backend's translation unit.
 */
inline std::size_t columnSize(const GPlotColumn &c) {
    return std::visit([](const auto *v) { return v->size(); }, c);
}

/******************************************************************************/
// The shared driver of the script backends (gnuplot / matplotlib / Octave). The three emitters used
// to carry a private copy of this whole skeleton each (kind enum, classification, the up-front pad
// validation, the pad-overflow warning, the column literal); only the per-kind plotting calls are
// genuinely backend-specific and stay in the emitters.

/**
 * @brief The plotter kinds the script backends understand. gnuplot supports only the graph kinds;
 * matplotlib and Octave additionally support the histograms.
 */
enum class scriptKind : std::uint8_t { g2d, g2ed, g3d, g4d, hist1d, hist1i, hist2d };

/**
 * @brief Classify a plotter for a script backend by its reported plotSpec().kind
 *
 * Throws for a plotter kind outside the backend's support set, directing the caller to the ROOT
 * backend (which supports every plotter type).
 *
 * @param p The plotter to classify
 * @param with_histograms Whether the backend supports the histogram kinds (false for gnuplot)
 * @param where The backend function reported in the error text (e.g. "MatplotlibEmitter::emitDocument()")
 * @param backend_name The backend noun reported in the error text (e.g. "matplotlib")
 * @return The backend-level kind of the plotter
 */
inline scriptKind classifyForScript(
    const GBasePlotter &p,
    bool with_histograms,
    std::string_view where,
    std::string_view backend_name
) {
    switch(p.plotSpec().kind) {
        case plotKind::graph_2d:     return scriptKind::g2d;
        case plotKind::graph_2d_err: return scriptKind::g2ed;
        case plotKind::graph_3d:     return scriptKind::g3d;
        case plotKind::graph_4d:     return scriptKind::g4d;
        case plotKind::hist_2d:
            if(with_histograms) { return scriptKind::hist2d; }
            break;
        case plotKind::hist_1d:
            if(with_histograms) { return scriptKind::hist1d; }
            break;
        case plotKind::hist_1i:
            if(with_histograms) { return scriptKind::hist1i; }
            break;
        default: break;
    }
    throw geneva_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In " << where << ": Error!" << '\n'
        << "the " << backend_name << " backend does not support " << p.getPlotterName()
        << "; use the ROOT backend for it" << '\n'
    );
}

/** @brief Whether a plotter of the given kind is drawn into a 3-d pad. */
inline bool isThreeDimensionalScript(scriptKind k) {
    return k == scriptKind::g3d || k == scriptKind::g4d;
}

/**
 * @brief Up-front validation of every registered plotter (and its secondaries) for a script backend
 *
 * Ensures every plotter is classifiable (so a partial script is never produced for an unsupported
 * plotter type) and that a 2-d and a 3-d plotter do not share one pad (one is a flat pad, the other
 * a 3-d pad).
 *
 * @param plotters The designer's registered plotters
 * @param with_histograms Whether the backend supports the histogram kinds (false for gnuplot)
 * @param where The backend function reported in error texts
 * @param backend_name The backend noun reported in error texts
 */
inline void validateScriptPads(
    const std::vector<std::shared_ptr<GBasePlotter>> &plotters,
    bool with_histograms,
    std::string_view where,
    std::string_view backend_name
) {
    for(const auto &p : plotters) {
        const scriptKind k = classifyForScript(*p, with_histograms, where, backend_name);
        for(const auto &sp : p->secondaryPlotters()) {
            const scriptKind sk = classifyForScript(*sp, with_histograms, where, backend_name);
            if(sp->plotSpec().projection.value_or(surfaceProjection::NONE) != surfaceProjection::NONE) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In " << where << ": Error!" << '\n'
                    << "an overlay (" << sp->getPlotterName() << ") requests a surface projection -- "
                    << "only the primary plot of a pad can be drawn as a projected surface" << '\n'
                );
            }
            if(isThreeDimensionalScript(k) != isThreeDimensionalScript(sk)) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In " << where << ": Error!" << '\n'
                    << "a 2-d and a 3-d graph cannot share a single " << backend_name << " pad ("
                    << p->getPlotterName() << " vs. " << sp->getPlotterName() << ")" << '\n'
                );
            }
        }
    }
}

/**
 * @brief Warns (once per emission) when more plots are registered than the canvas has pads
 *
 * @param where The backend function reported in the warning text
 * @param canvas_label The designer's canvas label
 * @param n_plotters The number of registered plotters
 * @param max_plots The number of pads (columns x rows)
 */
inline void warnPadOverflow(
    std::string_view where,
    const std::string &canvas_label,
    std::size_t n_plotters,
    std::size_t max_plots
) {
    if(n_plotters > max_plots) {
        glogger << "In " << where << " (Canvas label = \"" << canvas_label << "\":" << '\n'
                << "Warning! Found more plots than pads (" << n_plotters << " vs. " << max_plots
                << ")" << '\n'
                << "Some of the plots will be ignored" << '\n'
                << GWARNING;
    }
}

/**
 * @brief Emit one numeric column as a bracketed, comma-separated list literal `[v0, v1, ...]` at
 * full (round-trippable) precision -- the shared shape of a Python list and an Octave row vector.
 * Works for both float64 and int32 element types (an int column emits integer literals).
 */
template <typename T>
std::string bracketedRow(const std::vector<T> &col) {
    EmitStream out; // NOLINT(cppcoreguidelines-init-variables)
    out << '[';
    for(std::size_t i = 0; i < col.size(); ++i) {
        out << (i == 0 ? "" : ", ") << col[i];
    }
    out << ']';
    return out.str();
}

/** @brief Emit a type-tagged column (float64 or int32) as a bracketed list literal. */
inline std::string bracketedRowCol(const GPlotColumn &c) {
    return std::visit([](const auto *v) { return bracketedRow(*v); }, c);
}

/**
 * @brief The smallest and largest value of a (non-empty) column, as doubles
 * @param c The column
 * @return (minimum, maximum); (0, 0) for an empty column
 */
inline std::pair<double, double> columnRange(const GPlotColumn &c) {
    return std::visit(
        [](const auto *v) -> std::pair<double, double> {
            if(v->empty()) {
                return {0., 0.};
            }
            const auto [lo, hi] = std::ranges::minmax_element(*v);
            return {static_cast<double>(*lo), static_cast<double>(*hi)};
        },
        c
    );
}

/**
 * @brief Where a plotter's surface shows its colour map (NONE for anything that is not projected)
 * @param p The plotter
 * @return The plotter's surface projection
 */
inline surfaceProjection projectionOf(const GBasePlotter &p) {
    return p.plotSpec().projection.value_or(surfaceProjection::NONE);
}

/******************************************************************************/

} // namespace Gem::Dietrich
