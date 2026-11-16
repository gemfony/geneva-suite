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
#include <array>
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
 * The ROOT colour of the n-th series of a pad that holds several: the primary plotter is series 0,
 * its overlays follow. ROOT draws every graph in black unless it is told otherwise; the other
 * backends choose their colours themselves.
 *
 * @param series The position of the series in its pad
 * @return A ROOT colour index
 */
inline int rootSeriesColour(std::size_t series) {
    static constexpr std::array<int, 8> COLOURS{4, 2, 8, 6, 7, 9, 28, 1}; // blue, red, green, ...
    return COLOURS.at(series % COLOURS.size());
}

/******************************************************************************/
/**
 * Emits the two ROOT lines that give a graph the colour of its series (line and marker).
 *
 * @param s The stream the ROOT macro lines are written to
 * @param indent The indentation prefix of the emitted lines
 * @param obj The ROOT object variable name
 * @param series The position of the series in its pad
 */
inline void emitRootSeriesColour(
    std::ostream &s,
    const std::string &indent,
    const std::string &obj,
    std::size_t series
) {
    s << indent << obj << "->SetLineColor(" << rootSeriesColour(series) << ");" << '\n'
      << indent << obj << "->SetMarkerColor(" << rootSeriesColour(series) << ");" << '\n';
}

/******************************************************************************/
/**
 * One entry of a ROOT legend: the entry of the description (GLegendEntry) together with the
 * name of the object in the script whose look its sample copies.
 */
struct rootLegendEntry {
    std::string object;  ///< the name of the series' object in the script (GBasePlotter::rootLegendObject_()); empty: none
    std::string label;   ///< the text of the entry
    legendSample sample; ///< what the sample shows
    std::size_t series = 0;       ///< the series' place in its pad (0 the primary), for the colour of a sample object
    std::uint32_t line_width = 0; ///< the width of a sample object's line (0: ROOT's default)
};

/**
 * @param sample A legend sample
 * @return ROOT's TLegend::AddEntry() option that draws it from the series' object
 */
inline std::string rootLegendOption(legendSample sample) {
    switch(sample) {
        case legendSample::LINE:            return "l";
        case legendSample::MARKER:          return "p";
        case legendSample::LINE_AND_MARKER: return "lp";
        case legendSample::FILL:            return "f";
        case legendSample::COLOUR_MAP:      return "f";
    }
    return "l";
}

/******************************************************************************/
/**
 * Emits the ROOT block that builds the legend of the current pad from its description
 * (GBasePlotter::legendEntries()): one AddEntry() per entry, in its order, with its text, and a
 * sample drawn from the series' own object as the description says -- a line, markers, both, the
 * series' fill, or, for a series drawn with a colour map, a box in the middle colour of the
 * palette (a small TGraph that is not drawn, for the sample alone; so too for a series that names no
 * object of its own, in its colour). TPad::BuildLegend() is not used:
 * it makes an entry of every object with line, marker or fill attributes (a projected map alone
 * gave it hundreds of cells) and shows line, marker and fill for each.
 *
 * AUTO is ROOT's own placement on a 2-d pad, as BuildLegend() makes it: a box of 0.3 x 0.21 of the
 * pad, put where it covers the least at painting time, with its text sized to the box. On a 3-d
 * pad ROOT's placement sees only the pad's 2-d grid and put the box over the front-left axis;
 * there AUTO is the band at the top left (legendBox3D()). A corner puts the box of legendBox()
 * there.
 *
 * Every box but ROOT's own has a fixed text size, so that pads of one size show their legends
 * alike: LEGENDTEXTSIZE in a corner, LEGENDMINTEXTSIZE in the 3-d band, which is low. A label too
 * long for the box at that size would run out of it over the pad; then the legend is drawn
 * smaller, just small enough for its widest label (ROOT measures the labels), but never smaller
 * than LEGENDMINTEXTSIZE, ROOT's default size of axis labels. Where the widest label does not fit
 * even at that size, the box grows instead, away from the side of the pad it is placed at (a box
 * at the left grows to the right, one at the right to the left) and not beyond the pad; the
 * samples keep their width.
 *
 * @param s The stream the ROOT macro lines are written to
 * @param indent The indentation prefix of the emitted lines
 * @param position Where the legend lies
 * @param entries The entries (not empty)
 * @param three_d Whether the pad is a 3-d box (isThreeDimensional())
 */
inline void emitRootLegend(
    std::ostream &s,
    const std::string &indent,
    legendPosition position,
    const std::vector<rootLegendEntry> &entries,
    bool three_d
) {
    s << indent << "{ // the legend: one entry per series of the pad that carries a label" << '\n';
    const bool roots_own = position == legendPosition::AUTO and not three_d;
    double box_x1 = 0.;
    double box_x2 = 0.;
    if(roots_own) {
        s << indent << "   TLegend *legend = new TLegend(0.3, 0.21); // placed by ROOT" << '\n';
    } else {
        const bool band = position == legendPosition::AUTO;
        const auto [x1, y1, x2, y2] = band ? legendBox3D(entries.size()) : legendBox(position, entries.size());
        box_x1 = x1;
        box_x2 = x2;
        std::ostringstream box;
        box.imbue(std::locale::classic());
        box << "new TLegend(" << x1 << ", " << y1 << ", " << x2 << ", " << y2 << ");";
        std::ostringstream text_size;
        text_size.imbue(std::locale::classic());
        text_size << (band ? LEGENDMINTEXTSIZE : LEGENDTEXTSIZE);
        s << indent << "   TLegend *legend = " << box.str()
          << (band ? " // a 3-d pad: the band at the top left" : "") << '\n'
          << indent << "   legend->SetTextSize(" << text_size.str() << ");" << '\n';
    }
    for(const auto &e : entries) {
        const std::string label = "\"" + rootEscape(e.label) + "\"";
        if(e.sample == legendSample::COLOUR_MAP) {
            s << indent << "   { // a colour map: a box in the middle colour of the palette, a sample object never drawn" << '\n'
              << indent << "      TGraph *sample = new TGraph();" << '\n'
              << indent << "      sample->SetName(\"sample\");" << '\n'
              << indent << "      sample->SetFillColor(gStyle->GetColorPalette(gStyle->GetNumberOfColors() / 2));" << '\n'
              << indent << "      sample->SetFillStyle(1001);" << '\n'
              << indent << "      sample->SetLineColor(0);" << '\n'
              << indent << "      legend->AddEntry(sample, " << label << ", \"f\");" << '\n'
              << indent << "   }" << '\n';
        } else if(e.object.empty()) {
            // A series without an object of its own to copy: a sample object in the series' colour,
            // drawn as the description says, never drawn itself.
            s << indent << "   { // a sample object for a series that names no object of its own" << '\n'
              << indent << "      TGraph *sample = new TGraph();" << '\n'
              << indent << "      sample->SetName(\"sample\");" << '\n'
              << indent << "      sample->SetLineColor(" << rootSeriesColour(e.series) << ");" << '\n'
              << indent << "      sample->SetMarkerColor(" << rootSeriesColour(e.series) << ");" << '\n'
              << indent << "      sample->SetFillColor(" << rootSeriesColour(e.series) << ");" << '\n'
              << indent << "      sample->SetMarkerStyle(20);" << '\n';
            if(e.line_width > 0) {
                s << indent << "      sample->SetLineWidth(" << e.line_width << ");" << '\n';
            }
            s << indent << "      legend->AddEntry(sample, " << label << ", \"" << rootLegendOption(e.sample) << "\");" << '\n'
              << indent << "   }" << '\n';
        } else {
            s << indent << "   legend->AddEntry(" << e.object << ", " << label << ", \""
              << rootLegendOption(e.sample) << "\");" << '\n';
        }
    }
    if(not roots_own) {
        // The room of a label is the box less the margin of the samples, and a little air. The
        // box is written out: the legend knows its coordinates in the pad only once it is painted.
        const bool grows_left = position == legendPosition::TOP_RIGHT or position == legendPosition::BOTTOM_RIGHT
            or position == legendPosition::CENTER_RIGHT;
        std::ostringstream box;
        box.imbue(std::locale::classic());
        box << "const double x1 = " << box_x1 << ", x2 = " << box_x2 << ", floor_size = " << LEGENDMINTEXTSIZE << ";";
        s << indent << "   { // labels too long for the box are drawn smaller, down to a readable size; then the box grows" << '\n'
          << indent << "      " << box.str() << '\n'
          << indent << "      const double sample_width = legend->GetMargin() * (x2 - x1);" << '\n'
          << indent << "      const double room = 0.95 * ((x2 - x1) - sample_width);" << '\n'
          << indent << "      const auto widest_at = [&](double size) {" << '\n'
          << indent << "         double widest = 0.;" << '\n'
          << indent << "         TIter next_label(legend->GetListOfPrimitives());" << '\n'
          << indent << "         while(TLegendEntry *e = static_cast<TLegendEntry *>(next_label())) {" << '\n'
          << indent << "            TLatex label(0., 0., e->GetLabel());" << '\n'
          << indent << "            label.SetTextFont(legend->GetTextFont());" << '\n'
          << indent << "            label.SetTextSize(size);" << '\n'
          << indent << "            widest = TMath::Max(widest, label.GetXsize() / (gPad->GetX2() - gPad->GetX1()));" << '\n'
          << indent << "         }" << '\n'
          << indent << "         return widest;" << '\n'
          << indent << "      };" << '\n'
          << indent << "      double size = legend->GetTextSize();" << '\n'
          << indent << "      double widest = widest_at(size);" << '\n'
          << indent << "      for(int pass = 0; pass < 8 && widest > room && size > floor_size; ++pass) { // a font's width is not quite proportional to its size" << '\n'
          << indent << "         size = TMath::Max(floor_size, size * room / widest);" << '\n'
          << indent << "         widest = widest_at(size);" << '\n'
          << indent << "      }" << '\n'
          << indent << "      legend->SetTextSize(size);" << '\n'
          << indent << "      if(widest > room) { // too wide even at the smallest size: the box grows, the samples keep their width" << '\n'
          << indent << "         const double width = (widest / 0.95) + sample_width;" << '\n';
        if(grows_left) {
            s << indent << "         legend->SetX1(TMath::Max(0.01, x2 - width));" << '\n'
              << indent << "         legend->SetMargin(sample_width / (x2 - legend->GetX1()));" << '\n';
        } else {
            s << indent << "         legend->SetX2(TMath::Min(0.99, x1 + width));" << '\n'
              << indent << "         legend->SetMargin(sample_width / (legend->GetX2() - x1));" << '\n';
        }
        s << indent << "      }" << '\n'
          << indent << "   }" << '\n';
    }
    s << indent << "   legend->Draw();" << '\n'
      << indent << "}" << '\n';
}

/******************************************************************************/
/**
 * Emits what a ROOT pad needs to show a title set apart from the label of its first series
 * (GBasePlotter::setPadTitle()): ROOT takes a pad's title from the first object drawn into it,
 * the object whose title is also its legend entry. The emitted block runs after the legend has
 * been built (the legend keeps its own copy of each entry's text) and gives the first object
 * of the pad the pad's title.
 *
 * @param s The stream the ROOT macro lines are written to
 * @param indent The indentation prefix of the emitted lines
 * @param title The title of the pad
 */
inline void emitRootPadTitle(std::ostream &s, const std::string &indent, const std::string &title) {
    s << indent << "{ // the title of the pad, apart from the label of its first series" << '\n'
      << indent << "   TIter next(gPad->GetListOfPrimitives());" << '\n'
      << indent << "   while(TObject *o = next()) {" << '\n'
      << indent << "      if(TNamed *n = dynamic_cast<TNamed *>(o)) {" << '\n'
      << indent << "         n->SetTitle(\"" << rootEscape(title) << "\");" << '\n'
      << indent << "         break;" << '\n'
      << indent << "      }" << '\n'
      << indent << "   }" << '\n'
      << indent << "   gPad->Modified();" << '\n'
      << indent << "}" << '\n';
}

/******************************************************************************/
/**
 * Emits what a ROOT pad with overlaid graphs needs after all of them are drawn: one frame that
 * holds every graph, and a legend. ROOT takes the frame from the first graph alone, so a graph
 * drawn "same" that reaches beyond it would be cut off. The emitted block looks at the graphs of
 * the current pad (it needs no names), takes in their points and their error bars, widens the
 * first one's frame by a twentieth -- by a factor on a logarithmic axis -- and, if the pad's
 * legend has entries, builds it (see emitRootLegend()).
 *
 * @param s The stream the ROOT macro lines are written to
 * @param indent The indentation prefix of the emitted lines
 * @param position Where the legend lies (AUTO: where ROOT puts it)
 * @param entries The entries of the pad's legend (GBasePlotter::legendEntries()); none: the pad
 *        gets no legend
 * @param three_d Whether the pad is a 3-d box (isThreeDimensional()), for where AUTO puts the legend
 */
inline void emitRootOverlayFrameAndLegend(
    std::ostream &s,
    const std::string &indent,
    legendPosition position,
    const std::vector<rootLegendEntry> &entries,
    bool three_d
) {
    s << indent << "{ // one frame for all graphs of this pad" << (entries.empty() ? "" : ", and a legend")
      << '\n'
      << indent << "   TGraph *first = nullptr;" << '\n'
      << indent << "   double xlo = 0., xhi = 0., ylo = 0., yhi = 0.;" << '\n'
      << indent << "   TIter next(gPad->GetListOfPrimitives());" << '\n'
      << indent << "   while(TObject *o = next()) {" << '\n'
      << indent << "      TGraph *g = dynamic_cast<TGraph *>(o);" << '\n'
      << indent << "      if(!g || g->GetN() == 0) continue;" << '\n'
      << indent << "      double gxlo = 0., gxhi = 0., gylo = 0., gyhi = 0.;" << '\n'
      << indent << "      for(int i = 0; i < g->GetN(); ++i) {" << '\n'
      << indent
      << "         // the error bars belong into the frame (a graph without them reports -1)"
      << '\n'
      << indent << "         const double x = g->GetX()[i], y = g->GetY()[i];" << '\n'
      << indent << "         double xl = x - TMath::Max(0., g->GetErrorXlow(i));" << '\n'
      << indent << "         double yl = y - TMath::Max(0., g->GetErrorYlow(i));" << '\n'
      << indent << "         const double xh = x + TMath::Max(0., g->GetErrorXhigh(i));" << '\n'
      << indent << "         const double yh = y + TMath::Max(0., g->GetErrorYhigh(i));" << '\n'
      << indent << "         if(gPad->GetLogx() && xl <= 0.) xl = x; // no room below zero" << '\n'
      << indent << "         if(gPad->GetLogy() && yl <= 0.) yl = y;" << '\n'
      << indent
      << "         if(i == 0) { gxlo = xl; gxhi = xh; gylo = yl; gyhi = yh; continue; }"
      << '\n'
      << indent << "         gxlo = TMath::Min(gxlo, xl); gxhi = TMath::Max(gxhi, xh);" << '\n'
      << indent << "         gylo = TMath::Min(gylo, yl); gyhi = TMath::Max(gyhi, yh);" << '\n'
      << indent << "      }" << '\n'
      << indent << "      if(!first) { first = g; xlo = gxlo; xhi = gxhi; ylo = gylo; yhi = gyhi; }"
      << '\n'
      << indent << "      xlo = TMath::Min(xlo, gxlo); xhi = TMath::Max(xhi, gxhi);" << '\n'
      << indent << "      ylo = TMath::Min(ylo, gylo); yhi = TMath::Max(yhi, gyhi);" << '\n'
      << indent << "   }" << '\n'
      << indent << "   if(first) {" << '\n'
      << indent << "      if(gPad->GetLogx()) first->GetXaxis()->SetLimits(xlo / 1.1, xhi * 1.1);"
      << '\n'
      << indent
      << "      else first->GetXaxis()->SetLimits(xlo - 0.05 * (xhi - xlo), xhi + 0.05 * (xhi - "
         "xlo));"
      << '\n'
      << indent
      << "      if(gPad->GetLogy()) { first->SetMinimum(ylo / 1.1); first->SetMaximum(yhi * 1.1); }"
      << '\n'
      << indent
      << "      else { first->SetMinimum(ylo - 0.05 * (yhi - ylo)); first->SetMaximum(yhi + 0.05 * "
         "(yhi - ylo)); }"
      << '\n'
      << indent << "   }" << '\n';
    if(not entries.empty()) {
        emitRootLegend(s, indent + "   ", position, entries, three_d);
    }
    s << indent << "   gPad->Modified();" << '\n'
      << indent << "}" << '\n';
}

/******************************************************************************/
/**
 * Emits what a ROOT pad with a logarithmic axis needs after everything is drawn. ROOT labels a
 * logarithmic axis at the powers of ten only, so an axis that lies between two of them would
 * carry no number at all. For an axis of fewer than three decades the emitted block asks for the
 * labels in between and has them written out (300 instead of 3x10^2). It looks at the objects of
 * the current pad and needs no names.
 *
 * @param s The stream the ROOT macro lines are written to
 * @param indent The indentation prefix of the emitted lines
 */
inline void emitRootLogAxisLabels(std::ostream &s, const std::string &indent) {
    s << indent << "{ // a logarithmic axis over few decades: more labels, written out" << '\n'
      << indent << "   gPad->Update();" << '\n'
      << indent
      << "   const bool few_x = gPad->GetLogx() && gPad->GetUxmax() - gPad->GetUxmin() < 3.;"
      << '\n'
      << indent
      << "   const bool few_y = gPad->GetLogy() && gPad->GetUymax() - gPad->GetUymin() < 3.;"
      << '\n'
      << indent << "   TIter next(gPad->GetListOfPrimitives());" << '\n'
      << indent << "   while(TObject *o = next()) {" << '\n'
      << indent << "      TH1 *h = dynamic_cast<TH1 *>(o);" << '\n'
      << indent << "      if(TGraph *g = dynamic_cast<TGraph *>(o)) h = g->GetHistogram();" << '\n'
      << indent << "      if(TF1 *f = dynamic_cast<TF1 *>(o)) h = f->GetHistogram();" << '\n'
      << indent << "      if(!h) continue;" << '\n'
      << indent
      << "      if(few_x) { h->GetXaxis()->SetMoreLogLabels(); h->GetXaxis()->SetNoExponent(); }"
      << '\n'
      << indent
      << "      if(few_y) { h->GetYaxis()->SetMoreLogLabels(); h->GetYaxis()->SetNoExponent(); }"
      << '\n'
      << indent << "   }" << '\n'
      << indent << "   gPad->Modified();" << '\n'
      << indent << "}" << '\n';
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

/**
 * @brief Whether a projected 3-d graph draws its surface as well as its colour map.
 * @param p The plotter
 * @return false if only the map is to be drawn
 */
inline bool drawsSurface(const GBasePlotter &p) {
    return p.plotSpec().surface.value_or(true);
}

/**
 * @brief Whether a 3-d graph draws its points' shadows on the floor of the plot box.
 * @param p The plotter
 * @return true if the shadows are to be drawn
 */
inline bool castsFloorShadow(const GBasePlotter &p) {
    return p.plotSpec().floor_shadow.value_or(false);
}

/**
 * @brief How the script backends (matplotlib, Octave, and the external renderer of the data
 * export) draw a series: its draw style (GBasePlotter::drawStyle()), as far as they can draw it.
 * They draw a 1-d histogram always as bars and a 2-d histogram always as a colour map, whatever
 * ROOT draw option it carries; everything else as the draw style says. Their drawing and their
 * legend samples both come from this.
 * @param p The plotter
 * @return How the script backends draw the series
 */
inline GDrawStyle scriptStyle(const GBasePlotter &p) {
    GDrawStyle style = p.drawStyle();
    const plotKind kind = p.plotSpec().kind;
    if(kind == plotKind::hist_1d or kind == plotKind::hist_1i) {
        style = GDrawStyle{.fill = true, .line_width = style.line_width};
    } else if(kind == plotKind::hist_2d) {
        style = GDrawStyle{.colour_map = true, .line_width = style.line_width};
    }
    return style;
}

/**
 * @brief The legend of a pad as the script backends draw it: the pad's description
 * (GBasePlotter::legendEntries()) with each sample taken from scriptStyle(), so that it shows
 * what these backends draw.
 * @param primary The pad's primary plotter
 * @return The entries; empty: the pad has no legend
 */
inline std::vector<GLegendEntry> scriptLegendEntries(const GBasePlotter &primary) {
    auto entries = primary.legendEntries();
    for(auto &e : entries) {
        const GBasePlotter &series = e.series == 0 ? primary : *primary.secondaryPlotters()[e.series - 1];
        e.sample = legendSampleOf(scriptStyle(series));
    }
    return entries;
}

/******************************************************************************/

} // namespace Gem::Dietrich
