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

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "dietrich/plotting/GPlotEnums.hpp" // graphPlotMode, surfaceProjection

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

} /* namespace Gem::Dietrich */
