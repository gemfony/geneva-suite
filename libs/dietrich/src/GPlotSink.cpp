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

#include "dietrich/plotting/GPlotSink.hpp"

// Standard header files go here
#include <algorithm>
#include <charconv>
#include <chrono>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

// Geneva header files go here
#include "common/GCommonHelperFunctions.hpp" // backupExistingFile()
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/record/GCsvSink.hpp"
#include "dietrich/plotting/GPlotDesigner.hpp"

namespace Gem::Dietrich {

using Gem::Common::Record::columnType;
using Gem::Common::Record::GRow;
using Gem::Common::Record::GStreamInfo;
using Gem::Common::Record::GTimeStamp;

namespace {

/******************************************************************************/
/**
 * @brief A value of a row as a number of the plot
 * @param value The value; not a text (refused when the stream opens)
 * @return The number
 */
double plotted(Gem::Common::Record::GValue const &value) {
    return std::visit(
        [](auto const &v) -> double {
            using V = std::remove_cvref_t<decltype(v)>;
            if constexpr(std::is_same_v<V, std::string>) {
                return 0.;
            }
            else if constexpr(std::is_same_v<V, GTimeStamp>) {
                return std::chrono::duration<double>(v.time_since_epoch()).count();
            }
            else {
                return static_cast<double>(v);
            }
        },
        value
    );
}

/******************************************************************************/
/**
 * @brief The number of distinct values of one column of a series' rows
 * @param rows The rows
 * @param column The column
 * @return The number, at least 1
 */
std::size_t distinctValues(std::vector<std::vector<double>> const &rows, std::size_t column) {
    std::set<double> values;
    for(auto const &row : rows) {
        if(column < row.size()) {
            values.insert(row[column]);
        }
    }
    return std::max<std::size_t>(values.size(), 1);
}

/******************************************************************************/
/**
 * @brief The value of a field of a CSV row as a number of the plot, read as GCsvSink writes it
 * @param text The field
 * @param type The type of its column
 * @return The number; none if the field cannot be read (text cannot be plotted)
 */
std::optional<double> numberOf(std::string const &text, columnType type) {
    switch(type) {
    case columnType::BOOLEAN:
        if(text == "true" || text == "false") {
            return text == "true" ? 1. : 0.;
        }
        return std::nullopt;
    case columnType::TIMESTAMP: {
        std::istringstream in(text);
        std::chrono::sys_time<std::chrono::nanoseconds> stamp;
        in >> std::chrono::parse("%FT%TZ", stamp);
        if(in.fail()) {
            return std::nullopt;
        }
        return std::chrono::duration<double>(stamp.time_since_epoch()).count();
    }
    case columnType::TEXT:
        return std::nullopt;
    default: {
        double value = 0.;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if(ec != std::errc{} || end != text.data() + text.size()) {
            return std::nullopt;
        }
        return value;
    }
    }
}

} /* namespace */

/******************************************************************************/
/**
 * A sink that writes the given plot through the given backend.
 *
 * @param layout The canvas and the series (their GPlotSpec::columns name stream columns), without data
 * @param backend The backend the plot is written through; not DATA (refused: the stream's data
 *        are written by its GCsvSink or GJsonLinesSink)
 */
GPlotSink::GPlotSink(GDataLog layout, plotBackend backend)
  : log_(std::move(layout))
  , backend_(backend) {
    if(backend_ == plotBackend::DATA) {
        throw gem_exception(
            g_error_streamer()
            << "The DATA backend writes the data, not a plot: the data of a stream are written by "
               "its GCsvSink or GJsonLinesSink (whose file a DATA plot would also take)"
            << '\n'
        );
    }
}

/******************************************************************************/
/**
 * Matches each series to the stream's columns and fixes the file.
 *
 * @param info The stream
 */
void GPlotSink::open(GStreamInfo const &info) {
    path_ = info.directory / (info.name + makeEmitter(backend_)->fileExtension());
    feeds_.assign(log_.nSeries(), {});
    for(std::size_t id = 0; id < log_.nSeries(); ++id) {
        auto const &wanted = log_.spec(id).columns;
        for(auto const &name : wanted) {
            const auto it = std::ranges::find(info.columns, name, &Gem::Common::Record::GColumn::name);
            if(it == info.columns.end()) {
                continue;
            }
            if(it->type == columnType::TEXT) {
                throw gem_exception(
                    g_error_streamer()
                    << "The column \"" << name << "\" of the stream \"" << info.name
                    << "\" holds text, which cannot be plotted" << '\n'
                );
            }
            feeds_[id].push_back(static_cast<std::size_t>(it - info.columns.begin()));
        }
        if(not feeds_[id].empty() && feeds_[id].size() != wanted.size()) {
            throw gem_exception(
                g_error_streamer()
                << "The series \"" << log_.spec(id).name << "\" of the plot of the stream \""
                << info.name << "\" names " << wanted.size() << " columns, of which only "
                << feeds_[id].size() << " are columns of the stream" << '\n'
            );
        }
    }
    if(info.resume && not info.csvFile.empty()) {
        this->plotEarlierRows_(info);
    }
    opened_ = true; // a refused stream writes no plot
}

/******************************************************************************/
/**
 * Adds the earlier rows of a resumed stream: those of the stream's CSV file (if it was written
 * for the same columns) up to the resume value, the rows the CSV sink keeps when it cuts the file
 * back. A row that cannot be read is left out.
 *
 * @param info The stream
 */
void GPlotSink::plotEarlierRows_(GStreamInfo const &info) {
    const auto rows = Gem::Common::Record::GCsvSink::readRows(info.csvFile, info.columns);
    if(not rows) {
        return;
    }
    std::optional<std::size_t> resume_column;
    if(not info.resumeColumn.empty()) {
        const auto it =
            std::ranges::find(info.columns, info.resumeColumn, &Gem::Common::Record::GColumn::name);
        resume_column = static_cast<std::size_t>(it - info.columns.begin());
    }
    std::vector<double> values;
    for(auto const &fields : *rows) {
        if(resume_column) {
            const auto value = numberOf(fields[*resume_column], info.columns[*resume_column].type);
            if(not value || *value > info.resumeAfter) {
                continue;
            }
        }
        for(std::size_t id = 0; id < feeds_.size(); ++id) {
            if(feeds_[id].empty()) {
                continue;
            }
            values.clear();
            for(std::size_t const column : feeds_[id]) {
                if(const auto value = numberOf(fields[column], info.columns[column].type)) {
                    values.push_back(*value);
                }
            }
            if(values.size() == feeds_[id].size()) {
                log_.append(id, values);
            }
        }
    }
}

/******************************************************************************/
/**
 * Adds each row to every series drawn from the stream.
 *
 * @param rows The rows
 */
void GPlotSink::write(std::span<GRow const> rows) {
    std::vector<double> values;
    for(auto const &row : rows) {
        for(std::size_t id = 0; id < feeds_.size(); ++id) {
            if(feeds_[id].empty()) {
                continue;
            }
            values.clear();
            for(std::size_t const column : feeds_[id]) {
                values.push_back(plotted(row.at(column)));
            }
            log_.append(id, values);
        }
    }
}

/******************************************************************************/
/**
 * Completes the plot (bins of histograms, then completeLog_()) and writes it, backing up an
 * existing file first. A sink whose open() failed writes nothing.
 */
void GPlotSink::close() {
    if(not opened_) {
        return;
    }
    for(std::size_t id = 0; id < log_.nSeries(); ++id) {
        GPlotSpec &spec = log_.spec(id);
        if(spec.kind != plotKind::hist_1d && spec.kind != plotKind::hist_2d) {
            continue;
        }
        if(not spec.n_bins_x) {
            spec.n_bins_x = distinctValues(log_.rows(id), 0);
        }
        if(spec.kind == plotKind::hist_2d && not spec.n_bins_y) {
            spec.n_bins_y = distinctValues(log_.rows(id), 1);
        }
    }
    this->completeLog_(log_);

    if(not path_.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path_.parent_path(), ec);
    }
    Gem::Common::backupExistingFile(path_.string(), "GPlotSink::close(): Warning!");
    GPlotDesigner designer = log_.toDesigner();
    designer.setEmitter(makeEmitter(backend_));
    designer.writeToFile(path_);
}

/******************************************************************************/
/**
 * The file of the plot.
 *
 * @return Its path, once the sink is open
 */
std::filesystem::path const &GPlotSink::path() const {
    return path_;
}

/******************************************************************************/
/**
 * Called at the end, before the plot is written. The default does nothing.
 *
 * @param log The plot, with its data
 */
void GPlotSink::completeLog_([[maybe_unused]] GDataLog &log) { /* nothing */
}

/******************************************************************************/

} /* namespace Gem::Dietrich */
