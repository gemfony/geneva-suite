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
#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/record/GRecordTypes.hpp"
#include "common/record/GSink.hpp"
#include "common/record/GStream.hpp"

namespace Gem::Common::Record {

/******************************************************************************/
/**
 * The process-wide owner of the streams and of the output rule (recorder()). A tenant declares a
 * stream once and appends rows to it whenever it has something to record:
 *
 * @code
 * auto progress = recorder().stream<std::uint64_t, double>("progress", {"iteration", "best_fitness"});
 * progress.append(iteration, best);
 *
 * auto quotes = recorder().stream<std::string, double>("quotes", {"symbol", "price"}, withTime::wall);
 * quotes.append("ACME", 17.25);
 * @endcode
 *
 * **The output rule.** A file sink writes `<directory>/<stream name>.<extension>`; the directory
 * is `./records` unless setDirectory() changed it (courtier's launcher does so for --recordDir),
 * and a stream keeps the directory it was declared in. Two open streams of one name are refused,
 * and the refusal names both declarers; once a stream is closed its name is free again. An
 * existing file is backed up at a stream's first write, and a resuming stream appends to it when
 * its columns are the same (GFileSink). The format is chosen per stream, by its sinks
 * (GStreamOptions; default CSV); a name never implies a format.
 *
 * **Threads.** Streams may be declared, appended to and closed from any thread. A row is stored
 * whole or not at all, never interleaved with another; rows of one thread keep their order, and
 * rows of different threads are stored in the order they arrive. The recorder guards its streams
 * with one mutex, each stream its rows and its sinks with one mutex of its own, which is held
 * while the sinks write: so the order the stream guarantees is the order in the files.
 *
 * **The end.** The recorder is never destroyed, so that a stream may be closed during the end of
 * the program; streams are therefore closed explicitly: by their handle (close(), or its
 * destructor), or all at once by closeAll() -- which courtier's launcher calls when its run()
 * ends, also after an exception or a stop request; a program without the launcher calls it
 * itself. A file sink flushes every write, so a crashed run leaves its files complete up to the
 * last row written; a plot sink writes at the close.
 */
class GRecorder {
public:
    /** @brief The constructor; there is one recorder per process (recorder()) */
    GRecorder() = default;

    GRecorder(GRecorder const &) = delete;
    GRecorder(GRecorder &&) = delete;
    GRecorder &operator=(GRecorder const &) = delete;
    GRecorder &operator=(GRecorder &&) = delete;
    /** @brief The destructor; never runs for the process's recorder */
    ~GRecorder() = default;

    /**
     * @brief Declares a stream with columns typed at compile time
     * @tparam Ts The C++ types of the columns (columnTypeOf())
     * @param name The name of the stream, also that of its files
     * @param columns The names of the columns, one per type
     * @param time Whether a time column ("time") comes first
     * @param options The sinks, resume and flush
     * @param declarer Where the stream is declared (named by a refusal)
     * @return The handle of the stream
     */
    template <typename... Ts>
    [[nodiscard]] GStreamT<Ts...> stream(
        std::string name,
        std::array<std::string, sizeof...(Ts)> columns,
        withTime time = withTime::none,
        GStreamOptions options = {},
        std::source_location declarer = std::source_location::current()
    ) {
        constexpr std::array<columnType, sizeof...(Ts)> TYPES{columnTypeOf<Ts>()...};
        std::vector<GColumn> typed;
        typed.reserve(sizeof...(Ts));
        for(std::size_t i = 0; i < sizeof...(Ts); ++i) {
            typed.push_back(GColumn{.name = std::move(columns[i]), .type = TYPES[i]});
        }
        return GStreamT<Ts...>(
            this->stream(std::move(name), std::move(typed), time, std::move(options), declarer)
        );
    }

    /** @brief Declares a stream with columns given at run time (GStream) */
    [[nodiscard]] GStream stream(
        std::string name,
        std::vector<GColumn> columns,
        withTime time = withTime::none,
        GStreamOptions options = {},
        std::source_location declarer = std::source_location::current()
    );

    /** @brief Sets the directory of the streams declared from now on */
    void setDirectory(std::filesystem::path directory);
    /** @brief The directory of the streams declared from now on */
    [[nodiscard]] std::filesystem::path directory() const;

    /** @brief Closes every open stream; a failure is reported through the log, not thrown */
    void closeAll();

private:
    friend class detail::GStreamCore;
    /** @brief Frees the name of a stream that has closed */
    void forget_(detail::GStreamCore const *core);

    mutable std::mutex mutex_; ///< Guards the two members below
    std::filesystem::path directory_ = std::filesystem::path(".") / "records"; ///< See setDirectory()
    std::map<std::string, std::shared_ptr<detail::GStreamCore>, std::less<>>
        streams_; ///< The open streams, by name
};

/******************************************************************************/
/**
 * @brief The recorder of this process, created at the first call and never destroyed
 */
[[nodiscard]] GRecorder &recorder();

/******************************************************************************/

} /* namespace Gem::Common::Record */
