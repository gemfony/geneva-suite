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
#include <system_error>

// Geneva header files go here
#include "common/GCommonHelperFunctions.hpp" // backupExistingFile(), timeAndPlace()
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
    path_ = info.directory / (info.name + this->extension_());

    if(not info.directory.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(info.directory, ec);
        if(ec) {
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileSink::open(): Error!" << '\n'
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

    out_.open(path_, append ? std::ios::app : std::ios::trunc);
    if(not out_) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GFileSink::open(): Error!" << '\n'
            << "The file " << path_.string() << " of the stream \"" << info.name
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
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GFileSink::write(): Error!" << '\n'
            << "Writing to " << path_.string() << " failed" << '\n'
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
 * The columns of the stream, once the sink is open.
 *
 * @return They
 */
std::vector<GColumn> const &GFileSink::streamColumns_() const {
    return stream_columns_;
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
