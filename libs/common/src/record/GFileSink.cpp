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

#include "common/record/GFileSink.hpp"

// Standard header files go here
#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// Geneva header files go here
#include "common/GCommonHelperFunctions.hpp" // backupExistingFile()
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"

namespace Gem::Common::Record {

/******************************************************************************/
/**
 * Opens `<directory>/<stream name><extension>`, creating the directory when needed. An existing
 * file is appended to if the stream resumes and the file holds its columns; otherwise it is
 * backed up and a new file begun.
 *
 * @param info The stream
 */
void GFileSink::open(GStreamInfo const &info) {
    stream_columns_ = info.columns;
    path_ = this->fileFor(info);

    if(not info.directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(info.directory, ec);
        if(ec) {
            throw gem_exception(
                g_error_streamer()
                << "The directory " << info.directory.string() << " of the stream \"" << info.name
                << "\" could not be created: " << ec.message() << '\n'
            );
        }
    }

    const bool append = info.resume && std::filesystem::exists(path_) &&
                        this->holdsColumns_(path_, stream_columns_);
    if(not append) {
        Gem::Common::backupExistingFile(
            path_.string(),
            "GFileSink::open() of the stream \"" + info.name + "\": Warning!"
        );
    }

    if(append && not info.resumeColumn.empty()) {
        this->cutBack_(info);
    }
    out_.open(path_, append ? std::ios::app : std::ios::trunc);
    if(not out_) {
        throw gem_exception(
            g_error_streamer() << "The file " << path_.string() << " of the stream \"" << info.name
                               << "\" could not be opened" << '\n'
        );
    }
    if(not append) {
        this->writeStart_(out_, stream_columns_);
        out_.flush();
    }
}

/******************************************************************************/
/**
 * Cuts the file of a resumed stream back to its checkpoint: keeps the lines a file begins with
 * and every row whose value in the resume column is at most the resume value, drops the rest --
 * the rows written after the checkpoint, and a row cut by a crash -- and rewrites the file as a
 * whole, so that a crash in the middle leaves the old file or the new one.
 *
 * @param info The stream, with its resume column and value
 */
void GFileSink::cutBack_(GStreamInfo const &info) const {
    const auto at = std::ranges::find(stream_columns_, info.resumeColumn, &GColumn::name);
    const auto column = static_cast<std::size_t>(at - stream_columns_.begin());
    std::vector<std::string> kept;
    {
        std::ifstream in(path_);
        std::size_t n = 0;
        for(std::string line; std::getline(in, line); ++n) {
            if(n < this->startLines_()) {
                kept.push_back(line);
                continue;
            }
            const std::optional<double> value = this->numberIn_(line, column);
            if(value && *value <= info.resumeAfter) {
                kept.push_back(line);
            }
        }
    }
    Gem::Common::replaceFileAtomically(
        path_,
        [&kept](std::ostream &out) {
            for(auto const &line : kept) {
                out << line << '\n';
            }
        },
        "GFileSink::open() of the stream \"" + info.name + "\""
    );
}

/******************************************************************************/
/**
 * Writes the rows, one line each, and flushes them to the file.
 *
 * @param rows The rows
 */
void GFileSink::write(std::span<GRow const> rows) {
    for(auto const &row : rows) {
        this->writeRow_(out_, row);
    }
    out_.flush();
    if(not out_) {
        throw gem_exception(
            g_error_streamer() << "Writing to " << path_.string() << " failed" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * Closes the file.
 */
void GFileSink::close() {
    out_.close();
}

/******************************************************************************/
/**
 * The file, once the sink is open.
 *
 * @return Its path
 */
std::filesystem::path const &GFileSink::path() const {
    return path_;
}

/******************************************************************************/
/**
 * The file this sink writes for a stream: `<directory>/<stream name><extension>`.
 *
 * @param info The stream
 * @return The path of the file
 */
std::filesystem::path GFileSink::fileFor(GStreamInfo const &info) const {
    return info.directory / (info.name + this->extension_());
}

/******************************************************************************/
/**
 * The columns of the stream, once the sink is open.
 *
 * @return They
 */
std::vector<GColumn> const &GFileSink::streamColumns_() const {
    return stream_columns_;
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
