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

#include "common/record/GRecorder.hpp"

// Standard header files go here
#include <algorithm>
#include <exception>
#include <iterator>
#include <set>
#include <string_view>

// Geneva header files go here
#include "common/GCommonHelperFunctions.hpp" // timeAndPlace()
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GSingletonT.hpp"
#include "common/record/GCsvSink.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * The recorder is reached from any thread and while the program ends (a stream handle that is a
 * static, a launcher that closes the streams): it is never destroyed, like the logger.
 */
template <>
struct gsingleton_never_destroy<Record::GRecorder> : std::true_type {};

} /* namespace Gem::Common */

namespace Gem::Common::Record {

namespace {

/******************************************************************************/
/**
 * @brief A place of the source, for messages
 * @param where The place
 * @return "file:line (function)"
 */
std::string place(std::source_location const &where) {
    return std::string(where.file_name()) + ":" + std::to_string(where.line()) + " (" +
           where.function_name() + ")";
}

/******************************************************************************/
/**
 * @brief Throws the refusal of a declaration
 * @param name The name of the stream
 * @param reason Why it is refused
 */
[[noreturn]] void refuse(std::string const &name, std::string const &reason) {
    throw gem_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In GRecorder::stream(): Error!" << '\n'
        << "The stream \"" << name << "\" cannot be declared: " << reason << '\n'
    );
}

} /* namespace */

/******************************************************************************/
/**
 * Declares a stream with columns given at run time. Refused: an empty name or one that is no
 * plain file name; an empty or a repeated column name; a column "time" in a stream with a time
 * column; flushEvery 0; a resumeColumn that is no numeric column of the stream; and a name of a
 * stream that is open, with both declarers named.
 *
 * @param name The name of the stream, also that of its files
 * @param columns The columns
 * @param time Whether a time column ("time") comes first
 * @param options The sinks, resume and flush
 * @param declarer Where the stream is declared
 * @return The handle of the stream
 */
GStream GRecorder::stream(
    std::string name,
    std::vector<GColumn> columns,
    withTime time,
    GStreamOptions options,
    std::source_location declarer
) {
    if(name.empty() || name == "." || name == ".." ||
       name.find_first_of("/\\") != std::string::npos) {
        refuse(name, "a stream needs a name that can be the name of a file");
    }
    if(time == withTime::wall) {
        columns.insert(columns.begin(), GColumn{.name = "time", .type = columnType::TIMESTAMP});
    }
    else if(time == withTime::steady) {
        columns.insert(columns.begin(), GColumn{.name = "time", .type = columnType::FLOATING});
    }
    std::set<std::string_view> names;
    for(auto const &column : columns) {
        if(column.name.empty()) {
            refuse(name, "a column has no name");
        }
        if(not names.insert(column.name).second) {
            refuse(
                name,
                "the column \"" + column.name + "\" is named twice" +
                    (column.name == "time" && time != withTime::none
                         ? " (the stream has a time column of that name)"
                         : "")
            );
        }
    }
    if(options.flushEvery == 0) {
        refuse(name, "flushEvery must be at least 1");
    }
    if(not options.resumeColumn.empty()) {
        const auto it = std::ranges::find(columns, options.resumeColumn, &GColumn::name);
        if(it == columns.end() ||
           (it->type != columnType::INTEGER && it->type != columnType::UNSIGNED &&
            it->type != columnType::FLOATING)) {
            refuse(
                name,
                "the column \"" + options.resumeColumn +
                    "\" by which a resumed file is cut back is no numeric column of the stream"
            );
        }
    }

    if(options.sinks.empty()) {
        options.sinks.push_back(std::make_shared<GCsvSink>());
    }

    const std::scoped_lock lk(mutex_);
    if(const auto it = streams_.find(name); it != streams_.end()) {
        refuse(
            name,
            "it is declared at " + place(declarer) + ", but a stream of that name is open, " +
                "declared at " + place(it->second->declarer()) +
                ". Two declarers -- also two objects of one class -- need different stream "
                "names; a name is free again once its stream is closed."
        );
    }
    if(const auto added = added_sinks_.find(name); added != added_sinks_.end()) {
        std::ranges::move(added->second, std::back_inserter(options.sinks));
        added_sinks_.erase(added);
    }
    auto core = std::make_shared<detail::GStreamCore>(
        GStreamInfo{.name = name, .columns = std::move(columns), .directory = directory_},
        time,
        std::move(options),
        declarer
    );
    streams_.emplace(std::move(name), core);
    return GStream(std::move(core));
}

/******************************************************************************/
/**
 * Sets the directory of the streams declared from now on; a stream keeps the directory it was
 * declared in. The directory is created when a sink first writes into it.
 *
 * @param directory The directory
 */
void GRecorder::setDirectory(std::filesystem::path directory) {
    const std::scoped_lock lk(mutex_);
    directory_ = std::move(directory);
}

/******************************************************************************/
/**
 * The directory of the streams declared from now on.
 *
 * @return It; "./records" unless setDirectory() changed it
 */
std::filesystem::path GRecorder::directory() const {
    const std::scoped_lock lk(mutex_);
    return directory_;
}

/******************************************************************************/
/**
 * Closes every open stream. Called at the end of a run (courtier's launcher does so), also after
 * an exception, so a failure to close one stream is reported through the log and does not keep
 * the others open.
 */
void GRecorder::closeAll() {
    std::vector<std::shared_ptr<detail::GStreamCore>> open;
    {
        const std::scoped_lock lk(mutex_);
        for(auto const &[name, core] : streams_) {
            open.push_back(core);
        }
    }
    // Outside the lock: a closing stream frees its name (forget_())
    for(auto const &core : open) {
        try {
            core->close();
        }
        catch(std::exception const &e) {
            glogger << "In GRecorder::closeAll(): the stream \"" << core->info().name
                    << "\" could not be closed cleanly:" << '\n'
                    << e.what() << '\n'
                    << GWARNING;
        }
    }
}

/******************************************************************************/
/**
 * Adds a sink to the next declaration of a stream, by its name.
 *
 * @param name The name of the stream
 * @param sink The sink; it is opened, written and closed by that stream alone
 */
void GRecorder::addSink(std::string const &name, std::shared_ptr<GSink> sink) {
    if(not sink) {
        refuse(name, "addSink() was given no sink");
    }
    const std::scoped_lock lk(mutex_);
    if(const auto it = streams_.find(name); it != streams_.end()) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GRecorder::addSink(): Error!" << '\n'
            << "The stream \"" << name << "\" is open already (declared at "
            << place(it->second->declarer())
            << "): a sink is added before the stream is declared, so that it gets every row"
            << '\n'
        );
    }
    added_sinks_[name].push_back(std::move(sink));
}

/******************************************************************************/
/**
 * Frees the name of a stream that has closed.
 *
 * @param core The stream
 */
void GRecorder::forget_(detail::GStreamCore const *core) {
    const std::scoped_lock lk(mutex_);
    if(const auto it = streams_.find(core->info().name);
       it != streams_.end() && it->second.get() == core) {
        streams_.erase(it);
    }
}

/******************************************************************************/
/**
 * The recorder of this process.
 *
 * @return It
 */
GRecorder &recorder() {
    return *Gem::Common::GSingletonT<GRecorder>::instance();
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
