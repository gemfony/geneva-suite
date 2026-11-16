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
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
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
            }
            out += "\"";
        }
        if(line_width.has_value()) {
            out += ", \"line_width\": " + std::to_string(*line_width);
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
 * One entry of a pad's legend, as the ROOT, matplotlib and Octave writers draw it
 * (GBasePlotter::legendEntries()).
 */
struct GLegendEntry {
    std::size_t series = 0; ///< the series it names: 0 the pad's primary plotter, k its k-th secondary plotter
    std::string label;      ///< the text: the series' plot label
    legendSample sample = legendSample::LINE; ///< what the sample shows (legendSampleOf())
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
 * The one decision of what a series' legend sample shows, for every plotter type, from how the
 * series is drawn: its kind, its plot mode (scatter or curve), its projection, and its drawing
 * arguments where it has its own (they are ROOT's draw options; "L"/"C" draw a line, "P"/"*"
 * markers, "B"/"F" a fill, "SURF", "COL", "CONT", "LEGO", "TRI" a colour map, "SCAT" points).
 *
 * - a 2-d graph (with or without error bars): markers for a scatter plot, a line through markers
 *   for a curve;
 * - a 3-d graph: the colour map for a projected surface, markers for points, a line through
 *   markers for a curve;
 * - a 4-d graph: markers;
 * - a 1-d histogram: its bars (FILL); a 2-d histogram and a 2-d function: the colour map, or
 *   markers for a scatter plot;
 * - a 1-d function: a line.
 *
 * @param spec The series' plot spec (GBasePlotter::plotSpec())
 * @return What the series' legend sample shows
 */
[[nodiscard]] inline legendSample legendSampleOf(const GPlotSpec &spec) {
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
    const bool curve = spec.plot_mode == graphPlotMode::CURVE;

    switch(spec.kind) {
        case plotKind::graph_2d:
        case plotKind::graph_2d_err: {
            if(not own_args) {
                return curve or not spec.plot_mode.has_value() ? legendSample::LINE_AND_MARKER
                                                               : legendSample::MARKER;
            }
            if(has("B") or has("F")) {
                return legendSample::FILL;
            }
            const bool line = has("L") or has("C");
            const bool marker = has("P") or has("*");
            if(line and marker) {
                return legendSample::LINE_AND_MARKER;
            }
            return marker ? legendSample::MARKER : legendSample::LINE;
        }
        case plotKind::graph_3d: {
            if(spec.projection.value_or(surfaceProjection::NONE) != surfaceProjection::NONE or colour_map) {
                return legendSample::COLOUR_MAP;
            }
            if(own_args and has("LINE")) {
                return has("P") ? legendSample::LINE_AND_MARKER : legendSample::LINE;
            }
            return curve ? legendSample::LINE_AND_MARKER : legendSample::MARKER;
        }
        case plotKind::graph_4d:
            return legendSample::MARKER;
        case plotKind::hist_1d:
        case plotKind::hist_1i:
            return legendSample::FILL;
        case plotKind::hist_2d:
        case plotKind::function_2d:
            return has("SCAT") ? legendSample::MARKER : legendSample::COLOUR_MAP;
        case plotKind::function_1d:
            return has("P") ? legendSample::MARKER : legendSample::LINE;
    }
    return legendSample::LINE;
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

} /* namespace Gem::Dietrich */
