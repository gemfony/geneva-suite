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
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Gem::Dietrich {

// The plotting library builds on common's facilities (logging, serialization helpers,
// exception types, make_member, EmitStream, ...); make them visible here without
// per-name qualification. This affects lookup only within Gem::Dietrich.
using namespace Gem::Common;
// Dietrich declares its own to_string(plotKind), which would otherwise shadow common's
// numeric to_string(...) for unqualified calls; merge common's overloads back in.
using Gem::Common::to_string;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
// GDataLog: the modern, plotter-object-free plot API (declare a GPlotSpec per series +
// push data; realize into a GPlotDesigner via makePlotter() + appendRow()).

/******************************************************************************/
/**
 * Constructs an empty log for the given canvas title and pad grid.
 *
 * @param canvas_label The canvas title
 * @param c_x_div The number of pad columns
 * @param c_y_div The number of pad rows
 */
GDataLog::GDataLog(std::string canvas_label, std::size_t c_x_div, std::size_t c_y_div)
  : canvas_label_(std::move(canvas_label)), c_x_div_(c_x_div), c_y_div_(c_y_div) { /* nothing */ }

/******************************************************************************/
/**
 * Declares a primary series (its own pad) from its plot spec.
 *
 * @param spec The plot specification
 * @return The id of the new series
 */
GDataLog::SeriesId GDataLog::declareSeries(GPlotSpec spec) {
    series_.push_back(Series{std::move(spec), {}, std::nullopt});
    return series_.size() - 1;
}

/******************************************************************************/
/**
 * Declares an overlay series sharing an existing primary series' pad.
 *
 * @param primary The id of the primary series whose pad this overlays
 * @param spec The plot specification for the overlay
 * @return The id of the new overlay series
 */
GDataLog::SeriesId GDataLog::overlaySeries(SeriesId primary, GPlotSpec spec) {
    if(primary >= series_.size() || series_[primary].primary.has_value()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GDataLog::overlaySeries(): Error!" << '\n'
            << "primary id " << primary << " is not a valid primary series" << '\n'
        );
    }
    series_.push_back(Series{std::move(spec), {}, primary});
    return series_.size() - 1;
}

/******************************************************************************/
/**
 * Appends one data row (one value per column) to a series.
 *
 * @param id The series to append to
 * @param row One value per column
 */
void GDataLog::append(SeriesId id, std::span<const double> row) {
    if(id >= series_.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GDataLog::append(): Error!" << '\n'
            << "series id " << id << " is out of range" << '\n'
        );
    }
    series_[id].rows.emplace_back(row.begin(), row.end());
}

/******************************************************************************/
/** @brief Convenience append for a 2-column (x, y) series */
void GDataLog::append(SeriesId id, double x, double y) {
    const double row[] = {x, y};
    this->append(id, std::span<const double>(row));
}

/******************************************************************************/
/** @brief Convenience append for a 3-column (x, y, z) series */
void GDataLog::append(SeriesId id, double x, double y, double z) {
    const double row[] = {x, y, z};
    this->append(id, std::span<const double>(row));
}

/******************************************************************************/
/** @brief Convenience append for a 4-column (x, y, z, w) series */
void GDataLog::append(SeriesId id, double x, double y, double z, double w) {
    const double row[] = {x, y, z, w};
    this->append(id, std::span<const double>(row));
}

/******************************************************************************/
/** @brief Convenience append for a 3-column series from an (x, y, z) tuple */
void GDataLog::append(SeriesId id, const std::tuple<double, double, double> &row) {
    this->append(id, std::get<0>(row), std::get<1>(row), std::get<2>(row));
}

/******************************************************************************/
/** @brief Convenience append for a 4-column series from an (x, y, z, w) tuple */
void GDataLog::append(SeriesId id, const std::tuple<double, double, double, double> &row) {
    this->append(id, std::get<0>(row), std::get<1>(row), std::get<2>(row), std::get<3>(row));
}

/******************************************************************************/
/**
 * Sets the canvas pixel dimensions, forwarded to the built GPlotDesigner.
 *
 * @param x_dim The canvas width in pixels
 * @param y_dim The canvas height in pixels
 */
void GDataLog::setCanvasDimensions(std::uint32_t x_dim, std::uint32_t y_dim) {
    dims_ = std::make_tuple(x_dim, y_dim);
}

/******************************************************************************/
/**
 * Marks a series so its rows are sorted by the first column before rendering.
 *
 * @param id The series to sort
 */
void GDataLog::sortByFirstColumn(SeriesId id) {
    if(id >= series_.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GDataLog::sortByFirstColumn(): Error!" << '\n'
            << "series id " << id << " is out of range" << '\n'
        );
    }
    series_[id].sort_first_column = true;
}

/******************************************************************************/
/**
 * Forwards the "add a print command" flag to the built GPlotDesigner.
 *
 * @param add_print_command Whether the designer should add a print / save command
 */
void GDataLog::setAddPrintCommand(bool add_print_command) {
    add_print_command_ = add_print_command;
}

/******************************************************************************/
/**
 * Realizes the log into a populated GPlotDesigner: each series' plotter is built from
 * its spec (makePlotter) and filled (appendRow); an overlay is attached as a secondary
 * plotter of its primary. Primaries are registered in declaration order, so the emitted
 * output matches an equivalent hand-built designer.
 *
 * @return The populated GPlotDesigner
 */
GPlotDesigner GDataLog::toDesigner() const {
    GPlotDesigner gpd(canvas_label_, c_x_div_, c_y_div_);
    if(dims_.has_value()) {
        gpd.setCanvasDimensions(*dims_);
    }
    if(add_print_command_.has_value()) {
        gpd.setAddPrintCommand(*add_print_command_);
    }

    // Build every series' plotter (primary or overlay) from its spec + rows, sorting
    // by the first column where requested.
    std::vector<std::shared_ptr<GBasePlotter>> built(series_.size());
    for(auto &&[s, p] : std::views::zip(series_, built)) {
        p = makePlotter(s.spec);
        for(const auto &row : s.rows) {
            p->appendRow(row);
        }
        if(s.sort_first_column) {
            p->sortByFirstColumn();
        }
    }

    // Attach overlays to their primary.
    for(std::size_t i = 0; i < series_.size(); ++i) {
        if(const auto &primary = series_[i].primary; primary.has_value()) {
            built[*primary]->registerSecondaryPlotter(built[i]);
        }
    }

    // Register the primaries in declaration order.
    for(std::size_t i = 0; i < series_.size(); ++i) {
        if(not series_[i].primary.has_value()) {
            gpd.registerPlotter(built[i]);
        }
    }
    return gpd;
}

/******************************************************************************/
} /* namespace Gem::Dietrich */
