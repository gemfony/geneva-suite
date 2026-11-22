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
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/record/GRecordTypes.hpp"
#include "common/record/GSink.hpp"

namespace Gem::Common::Record {

/******************************************************************************/
/**
 * How a stream is declared, beyond its name, its columns and its time column.
 */
struct GStreamOptions {
    /** The sinks the rows go to; none: one GCsvSink */
    std::vector<std::shared_ptr<GSink>> sinks;
    /** The stream continues a run that was resumed from a checkpoint: a file sink appends to an
     *  existing file whose columns are the stream's, and backs it up and begins a new one otherwise.
     *  Before it appends, it cuts the file back to the checkpoint if resumeColumn names a column */
    bool resume = false;
    /** With resume: a numeric column by which the file is cut back -- every row whose value in it
     *  is greater than resumeAfter (the rows the earlier run wrote after its checkpoint; after a
     *  crash, also a row cut in the middle) is dropped before the first append. Empty: nothing is
     *  dropped */
    std::string resumeColumn;
    /** With resume and resumeColumn: the last value whose rows are kept (for a run, the iteration of
     *  its checkpoint) */
    double resumeAfter = 0.;
    /** The rows are handed to the sinks every this many rows, and when the stream closes; 1 (the
     *  default) loses at most the row being written when a run crashes */
    std::size_t flushEvery = 1;
};

class GRecorder;

namespace detail {

/******************************************************************************/
/**
 * The state of one stream, shared by its handle and the recorder until the stream is closed.
 * One mutex guards it; the sinks are called under it, so that a row is stored whole and the rows
 * reach the sinks in the order they were appended (GRecorder, "Threads").
 */
class GStreamCore {
public:
    /** @brief A stream as declared */
    GStreamCore(
        GStreamInfo info,
        withTime time,
        GStreamOptions options,
        std::source_location declarer
    );

    GStreamCore(GStreamCore const &) = delete;
    GStreamCore(GStreamCore &&) = delete;
    GStreamCore &operator=(GStreamCore const &) = delete;
    GStreamCore &operator=(GStreamCore &&) = delete;
    /** @brief The destructor; the stream was closed before */
    ~GStreamCore() = default;

    /** @brief Appends a row (without the time column) */
    void append(GRow row);
    /** @brief Hands the rows held back to the sinks and closes them; again: nothing */
    bool close();
    /** @brief Whether the stream is open */
    [[nodiscard]] bool isOpen() const;

    /** @brief The name and the columns */
    [[nodiscard]] GStreamInfo const &info() const;
    /** @brief Where the stream was declared */
    [[nodiscard]] std::source_location const &declarer() const;

private:
    /** @brief Hands the rows held back to the sinks, opening them first; under mutex_ */
    void flushLocked_();

    mutable std::mutex mutex_; ///< Guards everything below
    GStreamInfo info_;         ///< Name, columns, directory, resume
    withTime time_;            ///< The time column
    std::vector<std::shared_ptr<GSink>> sinks_; ///< Where the rows go
    std::size_t flush_every_;                   ///< Rows per flush
    std::source_location declarer_;             ///< Where the stream was declared
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now(); ///< Declared
    std::vector<GRow> held_;   ///< Rows not yet handed to the sinks
    bool sinks_open_ = false;  ///< The sinks were opened
    bool closed_ = false;      ///< The stream was closed
};

} /* namespace detail */

/******************************************************************************/
/**
 * The handle of a stream whose columns are given at run time (GRecorder::stream() with a list of
 * GColumn), for a tenant that knows its columns only when it declares them -- the parameters of
 * a problem, for example. Rows are checked against the columns when they are appended.
 *
 * A handle is moved, not copied; destroying it closes its stream. A closed stream (by close(), by
 * the handle's end or by GRecorder::closeAll()) refuses further rows. An empty handle (default
 * constructed, or moved from) has no stream.
 */
class GStream {
public:
    /** @brief An empty handle */
    GStream() = default;
    /** @brief Takes over the stream of another handle */
    GStream(GStream &&cp) noexcept = default;
    /** @brief Closes this handle's stream and takes over that of another */
    GStream &operator=(GStream &&cp) noexcept;
    /** @brief Closes the stream; a failure is reported through the log, not thrown */
    ~GStream();

    GStream(GStream const &) = delete;
    GStream &operator=(GStream const &) = delete;

    /** @brief Appends a row, from any thread */
    void append(GRow row);
    /** @brief Closes the stream: the rows held back go to the sinks, which close */
    void close();
    /** @brief Whether the handle has a stream that is open */
    [[nodiscard]] bool isOpen() const;
    /** @brief The name of the stream */
    [[nodiscard]] std::string name() const;
    /** @brief The columns of the stream, the time column included */
    [[nodiscard]] std::vector<GColumn> columns() const;

private:
    friend class GRecorder;
    /** @brief A handle of a declared stream */
    explicit GStream(std::shared_ptr<detail::GStreamCore> core);

    std::shared_ptr<detail::GStreamCore> core_; ///< The stream; empty for an empty handle
};

/******************************************************************************/
/**
 * The handle of a stream whose columns are typed at compile time (GRecorder::stream<Ts...>()):
 * one column per type, appended as one argument per column. Otherwise as GStream.
 *
 * @tparam Ts The C++ types of the columns (see columnTypeOf())
 */
template <typename... Ts>
class GStreamT {
public:
    /** @brief An empty handle */
    GStreamT() = default;

    /** @brief Appends a row, from any thread @param values One value per column */
    void append(Ts const &...values) {
        stream_.append(GRow{toValue<Ts>(values)...});
    }
    /** @brief Closes the stream (see GStream::close()) */
    void close() {
        stream_.close();
    }
    /** @brief Whether the handle has a stream that is open @return Whether it has */
    [[nodiscard]] bool isOpen() const {
        return stream_.isOpen();
    }
    /** @brief The name of the stream @return It */
    [[nodiscard]] std::string name() const {
        return stream_.name();
    }

private:
    friend class GRecorder;
    /** @brief The typed handle of a declared stream @param stream Its untyped handle */
    explicit GStreamT(GStream stream)
      : stream_(std::move(stream)) {
    }

    GStream stream_; ///< The stream
};

/******************************************************************************/

} /* namespace Gem::Common::Record */
