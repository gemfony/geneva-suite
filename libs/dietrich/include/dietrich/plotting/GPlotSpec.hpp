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

#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "dietrich/plotting/GPlotEnums.hpp" // graphPlotMode, surfaceProjection, legendPosition

namespace Gem::Dietrich {


/******************************************************************************/
/**
 * The kind of plot a plotter represents. This used to be encoded only implicitly,
 * by which concrete GBasePlotter subclass was instantiated (GGraph2D vs.
 * GHistogram2D ...). Making it a first-class value lets each plotter REPORT what it
 * is, so that the data-only backend's manifest (CSV/.npz) can fully describe each
 * plot for an external renderer without re-deriving the type.
 */
enum class plotKind {
    graph_2d,     ///< a 2-d (x, y) graph (GGraph2D)
    graph_2d_err, ///< a 2-d graph with x/y error bars (GGraph2ED)
    graph_3d,     ///< a 3-d (x, y, z) graph (GGraph3D)
    graph_4d,     ///< a 4-d (x, y, z, w) graph (GGraph4D)
    hist_1d,      ///< a 1-d histogram of double samples (GHistogram1D)
    hist_1i,      ///< a 1-d histogram of integer samples (GHistogram1I)
    hist_2d,      ///< a 2-d histogram (GHistogram2D)
    function_1d,  ///< a sampled 1-d function plot (GFunctionPlotter1D)
    function_2d   ///< a sampled 2-d function plot (GFunctionPlotter2D)
};

/******************************************************************************/
/**
 * Converts a plotKind to its stable string name (used in the manifest JSON and the
 * CSV section header).
 *
 * @param k The plot kind to convert
 * @return A stable, lower-snake-case string name for the kind
 */
[[nodiscard]] inline std::string to_string(plotKind k) {
    switch(k) {
        case plotKind::graph_2d:     return "graph_2d";
        case plotKind::graph_2d_err: return "graph_2d_err";
        case plotKind::graph_3d:     return "graph_3d";
        case plotKind::graph_4d:     return "graph_4d";
        case plotKind::hist_1d:      return "hist_1d";
        case plotKind::hist_1i:      return "hist_1i";
        case plotKind::hist_2d:      return "hist_2d";
        case plotKind::function_1d:  return "function_1d";
        case plotKind::function_2d:  return "function_2d";
    }
    return "unknown";
}

/******************************************************************************/
/**
 * Derives the DEFAULT semantic role for a plot kind. The role is a free-form string
 * (deliberately not a second enum) so that a future monitor can set a richer intent;
 * for now it is derived from the kind.
 *
 * @param k The plot kind whose default role is requested
 * @return The default role string (e.g. "xy", "distribution", "function")
 */
[[nodiscard]] inline std::string defaultRole(plotKind k) {
    switch(k) {
        case plotKind::graph_2d:     return "xy";
        case plotKind::graph_2d_err: return "xy_err";
        case plotKind::graph_3d:     return "xyz";
        case plotKind::graph_4d:     return "xyzw";
        case plotKind::hist_1d:      return "distribution";
        case plotKind::hist_1i:      return "distribution";
        case plotKind::hist_2d:      return "distribution_2d";
        case plotKind::function_1d:  return "function";
        case plotKind::function_2d:  return "function";
    }
    return "unknown";
}

/******************************************************************************/
/**
 * A first-class, serializable VALUE describing the CHOICE a plotter makes: what kind
 * of plot it is, its semantic role, its labels, its drawing arguments and its column
 * (axis) names. For histograms it also carries the per-axis bin counts. It is a pure
 * report derived from a plotter's existing state -- it carries no plotter and adds no
 * stored member to the plotters; see GBasePlotter::plotSpec().
 */
struct GPlotSpec {
    plotKind kind = plotKind::graph_2d; ///< what kind of plot this is

    std::string role; ///< semantic intent; defaults from kind (see defaultRole())

    std::string name; ///< the plot label

    std::string x_label; ///< the x-axis label
    std::string y_label; ///< the y-axis label
    std::string z_label; ///< the z-axis label

    bool x_log = false; ///< whether the x-axis is logarithmic
    bool y_log = false; ///< whether the y-axis is logarithmic

    std::string drawing_args; ///< the plotter's drawing arguments

    std::vector<std::string> columns; ///< axis/column names, e.g. {"x","y"} or {"x","ex","y","ey"}

    std::optional<std::size_t> n_bins_x; ///< number of x-bins (histograms only)
    std::optional<std::size_t> n_bins_y; ///< number of y-bins (histograms only)

    std::optional<std::tuple<double, double>>
        range_x; ///< fixed (min,max) x-axis range (histograms only; absent = auto-range from data)
    std::optional<std::tuple<double, double>>
        range_y; ///< fixed (min,max) y-axis range (2-d histograms only)

    std::optional<graphPlotMode> plot_mode; ///< scatter vs. curve (2-d graphs; for a 3-d graph CURVE adds a line)

    std::optional<surfaceProjection>
        projection; ///< 3-d graphs only: draw a surface with its colour map on the floor or ceiling

    std::optional<bool>
        surface; ///< projected 3-d graphs only: false draws the colour map alone, without the surface

    std::optional<bool>
        floor_shadow; ///< 3-d graphs only: true draws every point again, small and grey, on the floor

    std::string pad_title; ///< the title of the pad, apart from name (empty: name is the title)

    std::optional<legendPosition> legend; ///< where the pad's legend lies (absent: the backend's choice)

    std::optional<std::uint32_t> line_width; ///< 2-d graphs: the width of the line (absent: default)

    std::optional<bool> arrows; ///< 2-d graphs: true joins consecutive points by arrows (ROOT alone draws them)

    /***************************************************************************/
    /** @brief Default constructor (kind=graph_2d, role defaulted from it) */
    GPlotSpec() : role(defaultRole(kind)) { /* nothing */ }

    /**
     * @brief Constructs a spec for the given kind, defaulting the role from it.
     * @param k The plot kind this spec describes
     */
    explicit GPlotSpec(plotKind k) : kind(k), role(defaultRole(k)) { /* nothing */ }

    /***************************************************************************/
    /**
     * @brief Serializes this spec as a hand-rolled JSON object (no dependency).
     *
     * Strings are JSON-escaped; the bin counts appear only when populated. The
     * member order is stable (kind, role, name, labels, drawing_args, columns,
     * bins) so the output is deterministic.
     *
     * @return A JSON object string describing this plot spec
     */
    [[nodiscard]] std::string toJson() const {
        std::string out = "{";
        out += "\"kind\": \"" + jsonEscape(to_string(kind)) + "\"";
        out += ", \"role\": \"" + jsonEscape(role) + "\"";
        out += ", \"name\": \"" + jsonEscape(name) + "\"";
        out += ", \"x_label\": \"" + jsonEscape(x_label) + "\"";
        out += ", \"y_label\": \"" + jsonEscape(y_label) + "\"";
        out += ", \"z_label\": \"" + jsonEscape(z_label) + "\"";
        // A linear axis is the rule and is not written, so the JSON of a plot without a
        // logarithmic axis is what it was before these two fields existed.
        if(x_log) {
            out += ", \"x_log\": true";
        }
        if(y_log) {
            out += ", \"y_log\": true";
        }
        out += ", \"drawing_args\": \"" + jsonEscape(drawing_args) + "\"";
        out += ", \"columns\": [";
        for(std::size_t c = 0; c < columns.size(); ++c) {
            out += (c == 0 ? "" : ", ");
            out += "\"" + jsonEscape(columns[c]) + "\"";
        }
        out += "]";
        if(n_bins_x.has_value()) {
            out += ", \"n_bins_x\": " + std::to_string(*n_bins_x);
        }
        if(n_bins_y.has_value()) {
            out += ", \"n_bins_y\": " + std::to_string(*n_bins_y);
        }
        if(range_x.has_value()) {
            out += ", \"range_x\": [" + std::to_string(std::get<0>(*range_x)) + ", " +
                   std::to_string(std::get<1>(*range_x)) + "]";
        }
        if(range_y.has_value()) {
            out += ", \"range_y\": [" + std::to_string(std::get<0>(*range_y)) + ", " +
                   std::to_string(std::get<1>(*range_y)) + "]";
        }
        if(plot_mode.has_value()) {
            out += ", \"plot_mode\": \"";
            out += (*plot_mode == graphPlotMode::SCATTER ? "scatter" : "curve");
            out += "\"";
        }
        if(projection.has_value()) {
            out += ", \"projection\": \"";
            switch(*projection) {
                case surfaceProjection::NONE:    out += "none"; break;
                case surfaceProjection::FLOOR:   out += "floor"; break;
                case surfaceProjection::CEILING: out += "ceiling"; break;
            }
            out += "\"";
        }
        if(surface.has_value()) {
            out += std::string(", \"surface\": ") + (*surface ? "true" : "false");
        }
        if(floor_shadow.has_value()) {
            out += std::string(", \"floor_shadow\": ") + (*floor_shadow ? "true" : "false");
        }
        // The three settings of a pad and a line appear only when set, so the JSON of a plot that
        // does not use them is what it was before they existed.
        if(not pad_title.empty()) {
            out += ", \"pad_title\": \"" + jsonEscape(pad_title) + "\"";
        }
        if(legend.has_value()) {
            out += ", \"legend\": \"";
            switch(*legend) {
                case legendPosition::AUTO:         out += "auto"; break;
                case legendPosition::TOP_LEFT:     out += "top_left"; break;
                case legendPosition::TOP_RIGHT:    out += "top_right"; break;
                case legendPosition::BOTTOM_LEFT:  out += "bottom_left"; break;
                case legendPosition::BOTTOM_RIGHT: out += "bottom_right"; break;
                case legendPosition::CENTER_RIGHT: out += "center_right"; break;
                case legendPosition::FREEST:       out += "freest"; break;
            }
            out += "\"";
        }
        if(line_width.has_value()) {
            out += ", \"line_width\": " + std::to_string(*line_width);
        }
        if(arrows.value_or(false)) {
            out += ", \"arrows\": true";
        }
        out += "}";
        return out;
    }

    /***************************************************************************/
    /** @brief Minimal JSON string escaping (quotes, backslashes, control chars). Public and
     *  static so the DATA backend's hand-assembled manifest fields escape identically. */
    static std::string jsonEscape(const std::string &in) {
        std::string out;
        out.reserve(in.size() + 2);
        for(char const c : in) {
            switch(c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += c;      break;
            }
        }
        return out;
    }
};

/******************************************************************************/
/**
 * What the sample of a legend entry shows, so that it looks like its series: a line, markers,
 * both, a box filled (or outlined) as the series' bars are, or a box in the middle colour of the
 * colour map a surface, a map or a 2-d histogram is drawn with.
 */
enum class legendSample {
    LINE,            ///< a line (a curve, a function)
    MARKER,          ///< markers alone (points, a scatter plot)
    LINE_AND_MARKER, ///< a line through markers
    FILL,            ///< a box with the series' own fill and outline (the bars of a histogram)
    COLOUR_MAP       ///< a box in the middle colour of the colour map (a surface, a map)
};

/**
 * @param sample A legend sample
 * @return Its stable name ("line", "marker", "line_and_marker", "fill", "colour_map"), as the data
 *         export's manifest carries it
 */
[[nodiscard]] inline std::string to_string(legendSample sample) {
    switch(sample) {
        case legendSample::LINE:            return "line";
        case legendSample::MARKER:          return "marker";
        case legendSample::LINE_AND_MARKER: return "line_and_marker";
        case legendSample::FILL:            return "fill";
        case legendSample::COLOUR_MAP:      return "colour_map";
    }
    return "line";
}

/**
 * One entry of a pad's legend, as every backend draws it (GBasePlotter::legendEntries()).
 */
struct GLegendEntry {
    std::size_t series = 0; ///< the series it names: 0 the pad's primary plotter, k its k-th secondary plotter
    std::string label;      ///< the text: the series' plot label
    legendSample sample = legendSample::LINE; ///< what the sample shows (legendSampleOf())
    std::uint32_t line_width = 0; ///< the width of the series' line, which a line sample takes (0: the backend's default)
};

/**
 * @param label A plot label
 * @return true if the label names something: it has a character other than white space. A label
 *         of white space only is no label.
 */
[[nodiscard]] inline bool isLegendLabel(std::string_view label) {
    return std::ranges::any_of(label, [](unsigned char c) { return std::isspace(c) == 0; });
}

/**
 * How a series is drawn, resolved once from its plot spec (drawStyleOf()). Every backend takes both
 * its drawing of the series and the series' legend sample from it, so that a sample cannot show
 * what is not drawn; ROOT's draw option of a 2-d graph is made from it as well.
 */
struct GDrawStyle {
    bool line = false;            ///< a line through the points (a curve, a function)
    bool markers = false;         ///< a marker at every point
    bool error_bars = false;      ///< error bars at the points (a 2-d graph with errors)
    bool fill = false;            ///< bars or boxes in the series' own colours (a 1-d histogram, ROOT's BOX)
    bool colour_map = false;      ///< a surface or map coloured by the palette
    bool arrows = false;          ///< arrows between consecutive points (ROOT alone draws them)
    std::uint32_t line_width = 0; ///< the width of the line (0: the backend's default)
};

/**
 * The one decision of how a series is drawn, for every plotter type, from its kind, its plot mode
 * (scatter or curve), its arrows, its projection, its line width, and its drawing arguments where
 * it has its own -- ROOT's draw options, which the plotters report in their spec also where they
 * choose them themselves (a 2-d histogram's set2DOpt()): "L"/"C" draw a line, "P"/"*" markers,
 * "B"/"F" a fill, "E" error bars, "SURF", "COL", "CONT", "LEGO", "TRI" a colour map, "BOX" boxes,
 * "SCAT" points, "LINE" a 3-d line.
 *
 * - a 2-d graph: markers for a scatter plot (and where arrows join the points), a line through
 *   markers for a curve; a graph with errors adds its error bars;
 * - a 3-d graph: the colour map for a projected surface, markers for points, a line through
 *   markers for a curve;
 * - a 4-d graph: markers;
 * - a 1-d histogram: its bars, or markers with error bars for "P" / "E";
 * - a 2-d histogram: the colour map (ROOT's default, COL), boxes for "BOX", points for "SCAT",
 *   "ARR" and "TEXT";
 *   a 2-d function: the colour map;
 * - a 1-d function: a line.
 *
 * @param spec The series' plot spec (GBasePlotter::plotSpec())
 * @return How the series is drawn
 */
[[nodiscard]] inline GDrawStyle drawStyleOf(const GPlotSpec &spec) {
    std::string args;
    for(char const c : spec.drawing_args) {
        args += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    for(auto at = args.find("SAME"); at != std::string::npos; at = args.find("SAME")) {
        args.erase(at, 4); // "same" places a series in a pad; it does not say how it is drawn
    }
    const auto has = [&args](std::string_view token) { return args.find(token) != std::string::npos; };
    const bool colour_map = has("SURF") or has("COL") or has("CONT") or has("LEGO") or has("TRI");
    const bool own_args = args.find_first_not_of(", ") != std::string::npos;
    const bool curve = spec.plot_mode.value_or(graphPlotMode::CURVE) == graphPlotMode::CURVE;

    GDrawStyle style;
    style.line_width = spec.line_width.value_or(0);
    switch(spec.kind) {
        case plotKind::graph_2d:
        case plotKind::graph_2d_err: {
            style.error_bars = spec.kind == plotKind::graph_2d_err;
            if(own_args) {
                style.fill = has("B") or has("F");
                style.line = has("L") or has("C");
                style.markers = has("P") or has("*");
                if(not(style.fill or style.line or style.markers)) {
                    style.line = true; // ROOT joins the points of a graph without an option by a line
                }
            } else {
                style.arrows = spec.arrows.value_or(false);
                style.markers = true;
                style.line = curve and not style.arrows; // arrows join the points instead
            }
        } break;
        case plotKind::graph_3d: {
            if(spec.projection.value_or(surfaceProjection::NONE) != surfaceProjection::NONE or colour_map) {
                style.colour_map = true;
            } else if(own_args) {
                style.line = has("LINE");
                style.markers = has("P") or not style.line;
            } else {
                style.markers = true;
                style.line = spec.plot_mode == graphPlotMode::CURVE;
            }
        } break;
        case plotKind::graph_4d:
            style.markers = true;
            break;
        case plotKind::hist_1d:
        case plotKind::hist_1i:
            if(has("P") or has("E")) {
                style.markers = true;
                style.error_bars = has("E");
            } else {
                style.fill = true;
            }
            break;
        case plotKind::hist_2d:
            if(has("BOX")) {
                style.fill = true;
            } else if(has("SCAT") or has("ARR") or has("TEXT")) {
                style.markers = true; // points (or arrows, numbers) where the entries lie
            } else {
                style.colour_map = true; // ROOT's default for a 2-d histogram is COL
            }
            break;
        case plotKind::function_2d:
            style.colour_map = true;
            break;
        case plotKind::function_1d:
            style.markers = has("P");
            style.line = not style.markers;
            break;
    }
    return style;
}

/**
 * What a series' legend sample shows, from how the series is drawn (drawStyleOf()): the colour map,
 * the fill, a line through markers, a line, or markers (also for error bars alone).
 *
 * @param style How the series is drawn
 * @return What its legend sample shows
 */
[[nodiscard]] inline legendSample legendSampleOf(const GDrawStyle &style) {
    if(style.colour_map) {
        return legendSample::COLOUR_MAP;
    }
    if(style.fill) {
        return legendSample::FILL;
    }
    if(style.line and style.markers) {
        return legendSample::LINE_AND_MARKER;
    }
    return style.line ? legendSample::LINE : legendSample::MARKER;
}

/******************************************************************************/
/**
 * The box a legend placed in a corner takes in a ROOT pad, in coordinates of the pad (0 to 1,
 * inside ROOT's default margins of a tenth), sized to its number of entries. A program that
 * wants to know whether a corner is free of its data can compare its data with it.
 *
 * @param position The place (AUTO has no box of its own; it gets the lower right's)
 * @param n_series The number of entries
 * @return x1, y1, x2, y2
 */
[[nodiscard]] inline std::tuple<double, double, double, double>
legendBox(legendPosition position, std::size_t n_series) {
    constexpr double WIDTH = 0.32;
    constexpr double ENTRY_HEIGHT = 0.065;
    constexpr double PADDING = 0.03;
    const double height =
        (ENTRY_HEIGHT * static_cast<double>(n_series == 0 ? 1 : n_series)) + PADDING;
    const bool left = position == legendPosition::TOP_LEFT or position == legendPosition::BOTTOM_LEFT;
    const bool top = position == legendPosition::TOP_LEFT or position == legendPosition::TOP_RIGHT;
    const double x1 = left ? 0.12 : 0.88 - WIDTH;
    double y1 = top ? 0.88 - height : 0.13;
    if(position == legendPosition::CENTER_RIGHT) {
        y1 = 0.5 - (height / 2.);
    }
    return {x1, y1, x1 + WIDTH, y1 + height};
}

/******************************************************************************/
/** The text size of a legend placed in a corner, relative to the pad's height (ROOT) */
constexpr double LEGENDTEXTSIZE = 0.045;
/**
 * The smallest text size of a legend with a box of its own (ROOT): ROOT's default size of axis
 * labels, relative to the pad's height. A legend whose widest label does not fit its box at this
 * size gets a wider box rather than smaller text.
 */
constexpr double LEGENDMINTEXTSIZE = 0.035;
/**
 * The second, lower floor of a legend's text size (ROOT): the rows of a crowded band of a 3-d pad
 * (six or seven entries, see legendBox3D()) share its height at this size, and no legend's text
 * gets smaller.
 */
constexpr double LEGENDCROWDEDTEXTSIZE = 0.025;

/******************************************************************************/
/**
 * The box of the legend of a 3-d pad whose legend position is AUTO, in ROOT: the band at the top
 * left of the pad, left of and below the pad's title and above the back-left top edge of the 3-d
 * box and the top of the z axis, where ROOT's default 3-d view draws neither data nor an axis nor
 * the colour palette. ROOT's own placement (TPad::PlaceBox) knows only the pad's 2-d grid and put
 * the legend over the front-left axis and its labels. The box reaches from the top of that band
 * downwards, one entry height per entry, and a few entries fill it.
 *
 * @param n_series The number of entries
 * @return x1, y1, x2, y2 in coordinates of the pad (0 to 1)
 */
[[nodiscard]] inline std::tuple<double, double, double, double> legendBox3D(std::size_t n_series) {
    constexpr double X1 = 0.01;
    constexpr double X2 = 0.35;
    constexpr double TOP = 0.92;
    constexpr double ENTRY_HEIGHT = 0.04;
    constexpr double PADDING = 0.02;
    constexpr double BOTTOM = 0.70; // the lowest the band reaches: five entries
    const double height = (ENTRY_HEIGHT * static_cast<double>(n_series == 0 ? 1 : n_series)) + PADDING;
    return {X1, std::max(BOTTOM, TOP - height), X2, TOP};
}

/**
 * The place of a 2-d pad's legend that covers the least of its data: of the boxes legendBox() gives
 * the lower right, the middle of the right side, the top left, the lower left and the top right
 * (in this order of preference), the one that covers the least of the data: every point of a cloud
 * and every vertex of a curve counts one, and a straight segment of a curve that crosses the box
 * without a vertex inside counts one too (it is sampled along its length), so that a curve is
 * never cheaper to cover than a cloud; the data are placed in the pad
 * as ROOT frames them (the range of the data widened by a twentieth on each side, by a factor of
 * 1.1 on a logarithmic axis, inside margins of a tenth). It is the freest corner, not a free one:
 * where every corner covers data, the legend covers the fewest. A box narrower than @p width is
 * judged at that width, grown away from its side of the pad as a legend grows for long labels
 * (at most to the frame).
 *
 * @param curves Series drawn with a line, each its points in drawing order
 * @param points Series drawn as markers alone, each its points
 * @param n_series The number of entries of the legend
 * @param x_log Whether the x axis is logarithmic
 * @param y_log Whether the y axis is logarithmic
 * @param width The width the box will take at least (0: legendBox()'s own)
 * @return The freest of the five places
 */
[[nodiscard]] inline legendPosition freestLegendCorner(
    const std::vector<std::vector<std::pair<double, double>>> &curves,
    const std::vector<std::vector<std::pair<double, double>>> &points,
    std::size_t n_series,
    bool x_log = false,
    bool y_log = false,
    double width = 0.
) {
    const auto axis = [](double v, bool log) { return log ? std::log10(v) : v; };
    double x_lo = std::numeric_limits<double>::max();
    double x_hi = std::numeric_limits<double>::lowest();
    double y_lo = x_lo;
    double y_hi = x_hi;
    for(const auto *set : {&curves, &points}) {
        for(const auto &series : *set) {
            for(const auto &[x, y] : series) {
                if((x_log and x <= 0.) or (y_log and y <= 0.)) {
                    continue; // not shown on a logarithmic axis
                }
                x_lo = std::min(x_lo, axis(x, x_log));
                x_hi = std::max(x_hi, axis(x, x_log));
                y_lo = std::min(y_lo, axis(y, y_log));
                y_hi = std::max(y_hi, axis(y, y_log));
            }
        }
    }
    const auto to_pad = [](double v, double lo, double hi, bool log) {
        const double width = hi > lo ? hi - lo : 1.;
        const double widen = log ? std::log10(1.1) : 0.05 * width;
        return 0.1 + (0.8 * (v - (lo - widen)) / (width + (2. * widen)));
    };
    constexpr int SAMPLES = 200;
    // Every point of a cloud and every vertex of a curve counts one; a straight segment of a curve
    // that crosses a box without a vertex inside counts one as well (it is sampled along its length),
    // so that a curve is never cheaper to cover than a cloud of as many points.
    std::vector<std::pair<double, double>> vertices;
    std::vector<std::vector<std::pair<double, double>>> segments; // a segment's samples between its vertices
    const auto shown = [&](double x, double y) { return not((x_log and x <= 0.) or (y_log and y <= 0.)); };
    const auto in_pad = [&](double x, double y) {
        return std::pair{to_pad(axis(x, x_log), x_lo, x_hi, x_log), to_pad(axis(y, y_log), y_lo, y_hi, y_log)};
    };
    for(const auto *set : {&curves, &points}) {
        for(const auto &series : *set) {
            for(const auto &[x, y] : series) {
                if(shown(x, y)) {
                    vertices.push_back(in_pad(x, y));
                }
            }
        }
    }
    for(const auto &curve : curves) {
        for(std::size_t i = 0; i + 1 < curve.size(); i++) {
            std::vector<std::pair<double, double>> segment;
            for(int k = 1; k < SAMPLES; k++) {
                const double t = static_cast<double>(k) / SAMPLES;
                const double x = curve[i].first + (t * (curve[i + 1].first - curve[i].first));
                const double y = curve[i].second + (t * (curve[i + 1].second - curve[i].second));
                if(shown(x, y)) {
                    segment.push_back(in_pad(x, y));
                }
            }
            if(shown(curve[i].first, curve[i].second) and shown(curve[i + 1].first, curve[i + 1].second)) {
                segment.push_back(in_pad(curve[i].first, curve[i].second));     // the ends, to tell a
                segment.push_back(in_pad(curve[i + 1].first, curve[i + 1].second)); // crossing segment
            }
            segments.push_back(std::move(segment));
        }
    }
    legendPosition best = legendPosition::BOTTOM_RIGHT;
    double fewest = std::numeric_limits<double>::max();
    for(const auto corner :
        {legendPosition::BOTTOM_RIGHT,
         legendPosition::CENTER_RIGHT,
         legendPosition::TOP_LEFT,
         legendPosition::BOTTOM_LEFT,
         legendPosition::TOP_RIGHT}) {
        auto [x1, y1, x2, y2] = legendBox(corner, n_series);
        if(x2 - x1 < width) { // the box a long label grows, away from its side, at most to the frame
            const bool right = corner == legendPosition::TOP_RIGHT or corner == legendPosition::BOTTOM_RIGHT
                or corner == legendPosition::CENTER_RIGHT;
            if(right) {
                x1 = std::max(0.1, x2 - width);
            } else {
                x2 = std::min(0.9, x1 + width);
            }
        }
        const auto inside = [&](const std::pair<double, double> &p) {
            return p.first >= x1 and p.first <= x2 and p.second >= y1 and p.second <= y2;
        };
        double covered = static_cast<double>(std::ranges::count_if(vertices, inside));
        for(const auto &segment : segments) {
            const bool ends_inside = segment.size() >= 2
                and (inside(segment[segment.size() - 1]) or inside(segment[segment.size() - 2]));
            if(not ends_inside and std::ranges::any_of(segment, inside)) {
                covered += 1.; // crosses the box without a vertex inside
            }
        }
        if(covered < fewest - 1e-12) {
            fewest = covered;
            best = corner;
        }
    }
    return best;
}

/** The most rows the band of a 3-d pad shows (legendBox3D()); more series are summed up in the last */
constexpr std::size_t LEGEND3DMAXROWS = 7;

/**
 * The text size of the band of a 3-d pad for its number of rows: LEGENDMINTEXTSIZE for up to five,
 * which fill the band to its bottom, and LEGENDCROWDEDTEXTSIZE for six or seven, which share it.
 *
 * @param n_rows The number of rows (at most LEGEND3DMAXROWS)
 * @return The text size, relative to the pad's height
 */
[[nodiscard]] inline double legendTextSize3D(std::size_t n_rows) {
    return n_rows <= 5 ? LEGENDMINTEXTSIZE : LEGENDCROWDEDTEXTSIZE;
}

/**
 * @param spec The plot spec of a pad's primary plotter
 * @return true if the pad is drawn as a 3-d box: a 3-d or 4-d graph, or a 2-d histogram or
 *         function drawn as a surface or lego plot
 */
[[nodiscard]] inline bool isThreeDimensional(const GPlotSpec &spec) {
    if(spec.kind == plotKind::graph_3d or spec.kind == plotKind::graph_4d) {
        return true;
    }
    if(spec.kind == plotKind::hist_2d or spec.kind == plotKind::function_2d) {
        std::string args;
        for(char const c : spec.drawing_args) {
            args += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return args.find("SURF") != std::string::npos or args.find("LEGO") != std::string::npos
            or args.find("TRI") != std::string::npos;
    }
    return false;
}

/******************************************************************************/

} /* namespace Gem::Dietrich */
