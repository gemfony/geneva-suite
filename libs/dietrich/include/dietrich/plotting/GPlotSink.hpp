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

// Standard header files go here
#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

// Geneva header files go here
#include "common/record/GSink.hpp"
#include "dietrich/plotting/GDataLog.hpp"
#include "dietrich/plotting/GPlotEmitter.hpp" // plotBackend

namespace Gem::Dietrich {

/******************************************************************************/
/**
 * A sink of the recording framework (Gem::Common::Record) that turns a stream into a plot: the
 * rows become the data of a GDataLog, which is written at the stream's end through a backend
 * chosen per sink (ROOT by default), into `<directory>/<stream name><extension of the backend>`
 * -- an existing file is backed up first (Gem::Common::backupExistingFile()).
 *
 * The plot is described by a GDataLog without data (the layout): its canvas, and a series per
 * plot whose GPlotSpec::columns name the stream's columns it is drawn from, in the order the plot
 * wants them (x, y, ...). Each row of the stream adds one row to every such series. Integer, bool
 * and floating-point columns are plotted as numbers, a time stamp as seconds since the epoch; a
 * text column cannot be plotted. A series none of whose columns is a stream column is not drawn
 * from the stream; a derived sink fills it at the end (completeLog_()). A series that names some
 * stream columns but not all is refused when the stream opens.
 *
 * A histogram series without a bin count for an axis gets one bin per distinct value of that
 * axis at the end, so a histogram over iterations has one bin per recorded iteration.
 *
 * A resuming stream (Gem::Common::Record::GStreamOptions::resume) plots the whole run: when it
 * opens, the sink reads the earlier rows from the stream's own CSV file
 * (Gem::Common::Record::GStreamInfo::csvFile) -- only a file written for the same columns, and only
 * the rows up to the resume value (GStreamOptions::resumeColumn), as the CSV sink cuts it back --
 * and then adds the new rows. Without a GCsvSink on the stream there is nothing it may read: it
 * plots the rows after the resume only, and the earlier plot is backed up.
 *
 * The DATA backend is refused: it writes the data rather than a plot, which the stream's
 * GCsvSink or GJsonLinesSink do, and its file would be theirs.
 *
 * A program adds the sink to a stream by name (Gem::Common::Record::GRecorder::addSink()), also to
 * a stream that another library declares.
 */
class GPlotSink : public Gem::Common::Record::GSink {
public:
    /** @brief A sink that writes the given plot through the given backend */
    explicit GPlotSink(GDataLog layout, plotBackend backend = plotBackend::ROOT);

    /** @brief Matches the series to the stream's columns */
    void open(Gem::Common::Record::GStreamInfo const &info) override;
    /** @brief Adds the rows to the series */
    void write(std::span<Gem::Common::Record::GRow const> rows) override;
    /** @brief Completes the plot and writes it */
    void close() override;

    /** @brief The file of the plot, once the sink is open */
    [[nodiscard]] std::filesystem::path const &path() const;

protected:
    /** @brief Called at the end, before the plot is written: a derived sink may add data or
     *  change the specs. The default does nothing. */
    virtual void completeLog_(GDataLog &log);

private:
    /** @brief Adds the rows of the stream's CSV file that a resumed stream keeps */
    void plotEarlierRows_(Gem::Common::Record::GStreamInfo const &info);

    GDataLog log_;        ///< The plot
    plotBackend backend_; ///< The backend it is written through
    std::vector<std::vector<std::size_t>>
        feeds_;                  ///< Per series: the stream columns it is drawn from
    std::filesystem::path path_; ///< The file
    bool opened_ = false;        ///< open() was called
};

/******************************************************************************/

} /* namespace Gem::Dietrich */
