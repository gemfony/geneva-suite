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

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "dietrich/plotting/GPlotEmitter.hpp" // plotBackend
#include "dietrich/plotting/GPlotSpec.hpp"

namespace Gem::Dietrich {


class GPlotDesigner; // forward declaration

/******************************************************************************/
/**
 * The modern, plotter-object-free way to produce a plot: DECLARE what to draw (a
 * GPlotSpec per series) and PUSH the data, instead of building concrete plotter
 * objects (GGraph2D / GHistogram ...) and mutating them. This is the API production
 * code -- the optimization monitors -- programs against, so it expresses INTENT
 * (the separated plot-choice value) decoupled from the rendering object model.
 *
 * A GDataLog carries the canvas (title + pad grid + optional pixel dimensions) and an
 * ordered list of series. A primary series occupies its own pad; an overlay series
 * shares a primary's pad. Data is appended row by row (one value per column). At the
 * end the log is realized into a GPlotDesigner (each series' plotter is built from its
 * spec via makePlotter() and filled via the generic appendRow()), which then renders
 * through any backend -- so the backend choice is orthogonal to the data collection.
 */
class GDataLog {
public:
    /** @brief Identifies a declared series within this log */
    using SeriesId = std::size_t;

    /***************************************************************************/
    /**
     * @brief Constructs an empty log for a canvas with the given title and pad grid
     * @param canvas_label The canvas title
     * @param c_x_div The number of pad columns
     * @param c_y_div The number of pad rows
     */
    GDataLog(std::string canvas_label, std::size_t c_x_div, std::size_t c_y_div);

    /***************************************************************************/
    /**
     * @brief Declares a primary series (occupying its own pad) from its plot spec
     * @param spec The plot specification (kind, labels, columns, bins, plot mode)
     * @return The id of the newly declared series
     */
    SeriesId declareSeries(GPlotSpec spec);

    /**
     * @brief Declares an overlay series sharing an existing primary series' pad
     * @param primary The id of the primary series whose pad this overlays
     * @param spec The plot specification for the overlay
     * @return The id of the newly declared overlay series
     */
    SeriesId overlaySeries(SeriesId primary, GPlotSpec spec);

    /***************************************************************************/
    /**
     * @brief Appends one data row (one value per column, in column order) to a series
     * @param id The series to append to
     * @param row One value per column (size must match the series' column count)
     */
    void append(SeriesId id, std::span<const double> row);
    /** @brief Convenience append for a 2-column (x, y) series */
    void append(SeriesId id, double x, double y);
    /** @brief Convenience append for a 3-column (x, y, z) series */
    void append(SeriesId id, double x, double y, double z);
    /** @brief Convenience append for a 4-column (x, y, z, w) series */
    void append(SeriesId id, double x, double y, double z, double w);
    /** @brief Convenience append for a 3-column series from an (x, y, z) tuple */
    void append(SeriesId id, const std::tuple<double, double, double> &row);
    /** @brief Convenience append for a 4-column series from an (x, y, z, w) tuple */
    void append(SeriesId id, const std::tuple<double, double, double, double> &row);

    /**
     * @brief Marks a series so its data is sorted by the first column before rendering
     * (the GDataLog equivalent of GGraph2D::sortX()).
     * @param id The series to sort
     */
    void sortByFirstColumn(SeriesId id);

    /***************************************************************************/
    /**
     * @brief Sets the canvas pixel dimensions (forwarded to the built GPlotDesigner)
     * @param x_dim The canvas width in pixels
     * @param y_dim The canvas height in pixels
     */
    void setCanvasDimensions(std::uint32_t x_dim, std::uint32_t y_dim);

    /**
     * @brief Forwards the "add a print command" flag to the built GPlotDesigner (only
     * applied when set, so an untouched log emits byte-identically to a plain designer)
     * @param add_print_command Whether the designer should add a print / save command
     */
    void setAddPrintCommand(bool add_print_command);

    /***************************************************************************/
    /**
     * @brief Realizes the log into a GPlotDesigner: builds each series' plotter from its
     * spec (makePlotter) and fills it (appendRow), attaching overlays as secondary
     * plotters of their primary. The result renders through any backend.
     *
     * @return A populated GPlotDesigner ready to emit / write
     */
    [[nodiscard]] GPlotDesigner toDesigner() const;

    /**
     * @brief Realizes the log and writes it to a file through the given backend
     * @param path The output file path
     * @param backend The rendering backend (defaults to ROOT)
     */
    void writeToFile(const std::filesystem::path &path, plotBackend backend = plotBackend::ROOT) const;

    /**
     * @brief The number of declared series (primaries + overlays)
     * @return The series count
     */
    [[nodiscard]] std::size_t nSeries() const;

private:
    /***************************************************************************/
    /** @brief One declared series: its spec, its appended rows, and (for an overlay) the
     *  primary whose pad it shares. */
    struct Series {
        GPlotSpec spec;                      ///< the plot-choice value for this series
        std::vector<std::vector<double>> rows; ///< appended data rows (one value per column)
        std::optional<SeriesId> primary;     ///< set if this series overlays a primary's pad
        bool sort_first_column = false;      ///< sort rows by the first column before rendering
    };

    std::string canvas_label_; ///< the canvas title
    std::size_t c_x_div_;      ///< number of pad columns
    std::size_t c_y_div_;      ///< number of pad rows
    std::optional<std::tuple<std::uint32_t, std::uint32_t>> dims_; ///< optional canvas pixel dimensions
    std::optional<bool> add_print_command_; ///< forwarded to the designer only when set
    std::vector<Series> series_; ///< the declared series, in declaration order
};

/******************************************************************************/

} /* namespace Gem::Dietrich */
