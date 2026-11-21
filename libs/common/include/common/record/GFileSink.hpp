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
 * whose columns are its own, and backs it up only otherwise. Every write is flushed to the file.
 *
 * A derived sink says how a file of its kind begins and how a row is written.
 */
class GFileSink : public GSink {
public:
    /** @brief Opens the file by the output rule @param info The stream */
    void open(GStreamInfo const &info) final;
    /** @brief Writes the rows, one line each, and flushes them @param rows The rows */
    void write(std::span<GRow const> rows) final;
    /** @brief Closes the file */
    void close() final;

    /** @brief The file, once the sink is open @return Its path */
    [[nodiscard]] std::filesystem::path const &path() const;

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

    /** @brief The columns of the stream, once the sink is open @return They */
    [[nodiscard]] std::vector<GColumn> const &streamColumns_() const;

private:
    std::filesystem::path path_;   ///< The file
    std::vector<GColumn> stream_columns_; ///< The columns of the stream
    std::ofstream out_;            ///< The open file
};

/******************************************************************************/

} /* namespace Gem::Common::Record */
