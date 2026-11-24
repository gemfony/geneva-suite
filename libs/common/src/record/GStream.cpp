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

#include "common/record/GStream.hpp"

// Standard header files go here
#include <exception>
#include <format>

// Geneva header files go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/record/GCsvSink.hpp"
#include "common/record/GRecorder.hpp"

namespace Gem::Common::Record {

namespace detail {

/******************************************************************************/
/**
 * A stream as the recorder declared it, with its sinks (the recorder adds the default CSV sink
 * and the sinks of addSink()).
 *
 * @param info Its name, its columns (the time column included), its directory and resume
 * @param time Its time column
 * @param options Its sinks and how often it flushes
 * @param declarer Where it was declared
 */
GStreamCore::GStreamCore(
    GStreamInfo info,
    withTime time,
    GStreamOptions options,
    std::source_location declarer
)
  : info_(std::move(info))
  , time_(time)
  , sinks_(std::move(options.sinks))
  , flush_every_(options.flushEvery)
  , declarer_(declarer) {
    info_.resume = options.resume;
    info_.resumeColumn = options.resumeColumn;
    info_.resumeAfter = options.resumeAfter;
    // A plot sink of a resumed stream reads the earlier rows from the stream's own CSV file
    for(auto const &sink : sinks_) {
        if(auto const *csv = dynamic_cast<GCsvSink const *>(sink.get())) {
            info_.csvFile = csv->fileFor(info_);
            break;
        }
    }
}

/******************************************************************************/
/**
 * Appends a row, after checking it against the columns, and puts the time value in front of it
 * if the stream has a time column. Every flushEvery rows the rows are handed to the sinks.
 *
 * @param row One value per column (the time column excepted), in column order
 */
void GStreamCore::append(GRow row) {
    const std::size_t offset = time_ == withTime::none ? 0 : 1;
    if(row.size() + offset != info_.columns.size()) {
        throw gem_exception(
            g_error_streamer() << "In GStream::append(): Error!" << '\n'
                               << "The stream \"" << info_.name << "\" has "
                               << info_.columns.size() - offset << " columns, but a row of "
                               << row.size() << " values was appended" << '\n'
        );
    }
    for(std::size_t i = 0; i < row.size(); ++i) {
        GColumn const &column = info_.columns[i + offset];
        if(typeOf(row[i]) != column.type) {
            throw gem_exception(
                g_error_streamer()
                << "In GStream::append(): Error!" << '\n'
                << "The column \"" << column.name << "\" of the stream \"" << info_.name
                << "\" holds " << to_string(column.type) << " values, but a "
                << to_string(typeOf(row[i])) << " value was appended" << '\n'
            );
        }
    }

    const std::scoped_lock lk(mutex_);
    if(closed_) {
        throw gem_exception(
            g_error_streamer() << "In GStream::append(): Error!" << '\n'
                               << "The stream \"" << info_.name
                               << "\" is closed; it takes no further row" << '\n'
        );
    }
    if(time_ == withTime::wall) {
        row.insert(row.begin(), GValue(std::chrono::system_clock::now()));
    }
    else if(time_ == withTime::steady) {
        row.insert(
            row.begin(),
            GValue(std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count())
        );
    }
    held_.push_back(std::move(row));
    if(held_.size() >= flush_every_) {
        this->flushLocked_();
    }
}

/******************************************************************************/
/**
 * Closes the stream: the rows held back go to the sinks, the sinks are closed, and the recorder
 * frees the name. A failure of a sink is thrown after all of this has been done.
 *
 * @return Whether this call closed the stream (false: it was closed before)
 */
bool GStreamCore::close() {
    std::exception_ptr failure;
    {
        const std::scoped_lock lk(mutex_);
        if(closed_) {
            return false;
        }
        closed_ = true;
        try {
            this->flushLocked_();
        }
        catch(...) {
            failure = std::current_exception();
        }
        if(sinks_open_) {
            for(auto const &sink : sinks_) {
                try {
                    sink->close();
                }
                catch(...) {
                    if(not failure) {
                        failure = std::current_exception();
                    }
                }
            }
        }
    }
    recorder().forget_(this);
    if(failure) {
        std::rethrow_exception(failure);
    }
    return true;
}

/******************************************************************************/
/**
 * Whether the stream is open.
 *
 * @return Whether it is
 */
bool GStreamCore::isOpen() const {
    const std::scoped_lock lk(mutex_);
    return not closed_;
}

/******************************************************************************/
/**
 * The name, the columns, the directory and resume of the stream; fixed at its declaration.
 *
 * @return They
 */
GStreamInfo const &GStreamCore::info() const {
    return info_;
}

/******************************************************************************/
/**
 * Where the stream was declared.
 *
 * @return The place
 */
std::source_location const &GStreamCore::declarer() const {
    return declarer_;
}

/******************************************************************************/
/**
 * Hands the rows held back to the sinks, opening them before the first rows. Called under
 * mutex_. The sinks count as open once their opening has begun, so that a sink whose open()
 * failed is still closed.
 */
void GStreamCore::flushLocked_() {
    if(held_.empty()) {
        return;
    }
    if(not sinks_open_) {
        sinks_open_ = true;
        for(auto const &sink : sinks_) {
            sink->open(info_);
        }
    }
    std::vector<GRow> rows;
    rows.swap(held_);
    for(auto const &sink : sinks_) {
        sink->write(rows);
    }
}

} /* namespace detail */

/******************************************************************************/
/**
 * The handle of a declared stream.
 *
 * @param core The stream
 */
GStream::GStream(std::shared_ptr<detail::GStreamCore> core)
  : core_(std::move(core)) {
}

/******************************************************************************/
/**
 * Closes the stream of this handle and takes over that of another.
 *
 * @param cp The other handle
 * @return This handle
 */
GStream &GStream::operator=(GStream &&cp) noexcept {
    if(this != &cp) {
        try {
            this->close();
        }
        catch(std::exception const &e) {
            glogger << "In GStream::operator=(): the stream could not be closed cleanly:" << '\n'
                    << e.what() << '\n'
                    << GWARNING;
        }
        core_ = std::move(cp.core_);
    }
    return *this;
}

/******************************************************************************/
/**
 * Closes the stream. A failure is reported through the log, since a destructor does not throw.
 */
GStream::~GStream() {
    try {
        this->close();
    }
    catch(std::exception const &e) {
        glogger << "In GStream::~GStream(): the stream could not be closed cleanly:" << '\n'
                << e.what() << '\n'
                << GWARNING;
    }
}

/******************************************************************************/
/**
 * Appends a row, from any thread. The row must hold one value per column (the automatic time
 * column excepted), each of its column's type.
 *
 * @param row The row
 */
void GStream::append(GRow row) {
    if(not core_) {
        throw gem_exception(
            g_error_streamer() << "In GStream::append(): Error!" << '\n'
                               << "The handle has no stream" << '\n'
        );
    }
    core_->append(std::move(row));
}

/******************************************************************************/
/**
 * Closes the stream: the rows held back go to the sinks, which then close. Closing a closed
 * stream, or an empty handle, does nothing.
 */
void GStream::close() {
    if(core_) {
        core_->close();
    }
}

/******************************************************************************/
/**
 * Whether the handle has a stream that is open.
 *
 * @return Whether it has
 */
bool GStream::isOpen() const {
    return core_ && core_->isOpen();
}

/******************************************************************************/
/**
 * The name of the stream.
 *
 * @return It, or an empty string for an empty handle
 */
std::string GStream::name() const {
    return core_ ? core_->info().name : std::string();
}

/******************************************************************************/
/**
 * The columns of the stream, the automatic time column included.
 *
 * @return They, or none for an empty handle
 */
std::vector<GColumn> GStream::columns() const {
    return core_ ? core_->info().columns : std::vector<GColumn>();
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
