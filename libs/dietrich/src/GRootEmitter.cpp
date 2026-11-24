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

/******************************************************************************/
/**
 * The name of the object in the macro whose look the legend sample of a series copies: the graph
 * its footer draws.
 *
 * @param s The series
 * @return The name of the object
 */
std::string legendObjectOf(const rootSeries &s) {
    switch(s.spec.kind) {
    case plotKind::graph_2d:
        return "graph" + suffixOf(s);
    default:
        break;
    }
    throw gem_exception(
        g_error_streamer() << "GRootEmitter: a plot of kind \"" << to_string(s.spec.kind)
                           << "\" is not yet drawn from its plot spec" << '\n'
    );
}

/**
 * ROOT's draw option of a series: the plotter's own drawing arguments, or one made from its draw
 * style, with "same" for an overlay.
 *
 * @param s The series
 * @return The draw option
 */
std::string drawOptionOf(const rootSeries &s) {
    const std::string &own = s.spec.drawing_args;
    if(not own.empty()) {
        return own;
    }
    // Markers, and a line through them for a curve, as the series' draw style says.
    const GDrawStyle style = s.plotter->drawStyle();
    const std::string d_a = std::string(style.markers ? "P" : "") + (style.line ? "L" : "");
    return s.secondary ? d_a + ",same" : "A" + d_a;
}

/******************************************************************************/
// The three sections of a series: its header declares what holds its data, its body fills it, and
// its footer draws it.

/**
 * @param s The series
 * @param indent The indentation of every line
 * @return The declarations of the series' data
 */
std::string headerOf(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const std::string array = "_array_" + suffixOf(s);
    const std::size_t n = pointsOf(s);
    out << indent << "double x" << array << "[" << to_string(n) << "];"
        << dsMarkerComment(s.plotter->dsMarker()) << '\n'
        << indent << "double y" << array << "[" << to_string(n) << "];" << '\n'
        << '\n';
    return out.str();
}

/**
 * @param s The series
 * @param indent The indentation of every line
 * @return The statements that fill the series' data
 */
std::string bodyOf(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const std::string array = "_array_" + suffixOf(s);
    markerLine(out, s);
    const auto &x = doublesOf(s, 0);
    const auto &y = doublesOf(s, 1);
    for(std::size_t i = 0; i < x.size(); ++i) {
        out << indent << "x" << array << "[" << i << "] = " << x[i] << ";"
            << "\t" << "y" << array << "[" << i << "] = " << y[i] << ";" << '\n';
    }
    out << '\n';
    return out.str();
}

/**
 * @param s The series
 * @param indent The indentation of every line
 * @return The statements that draw the series
 */
std::string footerOf(const rootSeries &s, const std::string &indent) {
    EmitStream out;
    const std::string array = "_array_" + suffixOf(s);
    const std::string graph = legendObjectOf(s);
    const std::size_t n = pointsOf(s);
    markerLine(out, s);
    out << indent << "TGraph *" << graph << " = new TGraph(" << n << ", x" << array << ", y"
        << array << ");" << '\n'
        << indent << graph << "->GetXaxis()->SetTitle(\"" << rootEscape(s.spec.x_label) << "\");"
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
    std::string (*of)(const rootSeries &, const std::string &),
    const std::string &indent
) {
    EmitStream out;
    out << indent << "// " << section << " data for primary plotter" << '\n' << of(pad[0], indent);
    for(const auto &s : pad | std::views::drop(1)) {
        out << indent << "// " << section << " data for secondary plotter " << s.own_id << " of "
            << pad[0].plotter->getPlotterName() << '\n'
            << of(s, indent) << '\n';
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

/**
 * @param primary The primary plotter of a pad
 * @return true if the pad is drawn from the plot specs of its series
 */
bool drawnFromSpec(const GBasePlotter &primary) {
    return primary.plotSpec().kind == plotKind::graph_2d;
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
        pads.push_back(drawnFromSpec(*p) ? padOf(*p) : std::vector<rootSeries>{});
    }

    result << "{" << '\n' << gpd.staticHeader(indent) << '\n';

    result << indent << "//===================  Header Section ====================" << '\n'
           << '\n';
    for(const auto &[idx, p] :
        gpd.plotters_cnt_ | std::views::enumerate | std::views::take(max_plots)) {
        const auto &pad = pads[static_cast<std::size_t>(idx)];
        result << (pad.empty() ? p->headerData(indent)
                               : padSection(pad, "Header", headerOf, indent))
               << '\n';
    }

    result << indent << "//===================  Data Section ======================" << '\n'
           << '\n';
    for(const auto &[idx, p] :
        gpd.plotters_cnt_ | std::views::enumerate | std::views::take(max_plots)) {
        const auto &pad = pads[static_cast<std::size_t>(idx)];
        result << (pad.empty() ? p->bodyData(indent) : padSection(pad, "Body", bodyOf, indent))
               << '\n';
    }

    result << indent << "//===================  Plot Section ======================" << '\n'
           << '\n';
    for(const auto &[idx, p] :
        gpd.plotters_cnt_ | std::views::enumerate | std::views::take(max_plots)) {
        const auto &pad = pads[static_cast<std::size_t>(idx)];
        result << indent << "graphPad->cd(" << static_cast<std::size_t>(idx) + 1 << ");"
               << '\n'; /* cd starts at 1 */
        // The scale of an axis belongs to the pad and is set before anything is drawn into it.
        if(p->xAxisLogarithmic()) {
            result << indent << "gPad->SetLogx(1);" << '\n';
        }
        if(p->yAxisLogarithmic()) {
            result << indent << "gPad->SetLogy(1);" << '\n';
        }
        result
            << (pad.empty() ? p->footerData(indent)
                            : padSection(pad, "Footer", footerOf, indent) + padFinish(pad, indent));
        // The labels of such an axis can only be arranged once the pad knows its range.
        if(p->xAxisLogarithmic() || p->yAxisLogarithmic()) {
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
