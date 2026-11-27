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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/record/GSink.hpp"

namespace Gem::Common::Record {

/******************************************************************************/
/**
 * The common part of the sinks that write a text file, one line per row (GCsvSink,
 * GJsonLinesSink): the output rule. The file is `<directory>/<stream name><extension>`; the
 * directory is created when needed. An existing file is backed up when the sink opens, that is at
 * the stream's first write (Gem::Common::backupExistingFile()), so nothing is overwritten
 * silently -- except that a resuming stream (GStreamInfo::resume) appends to an existing file
 * whose columns are its own, and backs it up only otherwise. Before it appends, it cuts the file
 * back to the checkpoint: with GStreamInfo::resumeColumn, every row whose value in that column is
 * greater than GStreamInfo::resumeAfter, or which cannot be read (a row cut by a crash), is
 * dropped -- a row is read as a whole (readRecord_()), also where it spans several lines -- and the file is rewritten as a whole (Gem::Common::replaceFileAtomically()). Every write
 * is flushed to the file.
 *
 * A derived sink says how a file of its kind begins and how a row is written.
 */
class GFileSink : public GSink {
public:
    /** @brief Opens the file by the output rule */
    void open(GStreamInfo const &info) final;
    /** @brief Writes the rows, one line each, and flushes them */
    void write(std::span<GRow const> rows) final;
    /** @brief Closes the file */
    void close() final;

    /** @brief The file, once the sink is open */
    [[nodiscard]] std::filesystem::path const &path() const;
    /** @brief The file this sink writes for a stream */
    [[nodiscard]] std::filesystem::path fileFor(GStreamInfo const &info) const;

protected:
    /** @brief The extension of the file, with its dot @return E.g. ".csv" */
    [[nodiscard]] virtual std::string extension_() const = 0;
    /** @brief Writes what begins a new file (a header line, or nothing) @param out The file
     *  @param columns The columns of the stream */
    virtual void writeStart_(std::ostream &out, std::vector<GColumn> const &columns) = 0;
    /** @brief Writes one row as one line @param out The file @param row The row */
    virtual void writeRow_(std::ostream &out, GRow const &row) = 0;
    /** @brief Whether an existing file of this kind holds the given columns, so that a resuming
     *  stream may append to it @param existing The file @param columns The columns of the stream
     *  @return Whether they are the file's */
    [[nodiscard]] virtual bool
    holdsColumns_(std::filesystem::path const &existing, std::vector<GColumn> const &columns) const = 0;
    /** @brief The number of lines a file of this kind begins with before its rows (a header)
     *  @return The number */
    [[nodiscard]] virtual std::size_t startLines_() const = 0;
    /** @brief The value of one column in a row of a file of this kind, as a number
     *  @param line The row, as readRecord_() read it @param column The position of the column
     *  @return The value; none if the line is no complete row or the value no number */
    [[nodiscard]] virtual std::optional<double>
    numberIn_(std::string const &line, std::size_t column) const = 0;
    /** @brief Reads the next row of a file of this kind, or a line it begins with: one line,
     *  unless the kind lets a row span several */
    [[nodiscard]] virtual bool readRecord_(std::istream &in, std::string &record) const;

    /** @brief The columns of the stream, once the sink is open */
    [[nodiscard]] std::vector<GColumn> const &streamColumns_() const;

private:
    /** @brief Cuts the file back to the rows a resumed stream keeps */
    void cutBack_(GStreamInfo const &info) const;

    std::filesystem::path path_;   ///< The file
    std::vector<GColumn> stream_columns_; ///< The columns of the stream
    std::ofstream out_;            ///< The open file
};

/******************************************************************************/

} /* namespace Gem::Common::Record */
