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

// Standard headers go here
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <typeinfo>
#include <vector>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief Touches a file in a given path, optionally with content, and returns its last-write time.
 */
std::filesystem::file_time_type touch_time(
    std::filesystem::path const &path,
    std::string const &content = "",
    bool remove_if_not_present = false
);

/******************************************************************************/
/**
 * @brief Determines a suitable number of threads for the current architecture.
 */
unsigned int getNHardwareThreads();

/******************************************************************************/
/**
 * @brief 64 random bits for identifiers, from an engine per thread seeded with 256 bits of
 * std::random_device.
 */
[[nodiscard]] std::uint64_t randomBits64();

/******************************************************************************/
/**
 * @brief Loads textual (ASCII) data from an external file.
 */
std::string loadTextDataFromFile(std::filesystem::path const &p);

/******************************************************************************/
/**
 * @brief Loads textual (ASCII) data from an external file, putting it line by line into a std::vector.
 */
std::vector<std::string> loadTextLinesFromFile(std::filesystem::path const &p);

/******************************************************************************/
/**
 * @brief Runs an external program and waits for it to end.
 *
 * The program is started directly, without a shell: each element of the argument list reaches it as
 * one argument, unchanged -- spaces and quotes included. Shell syntax (pipes, redirections, \$VAR,
 * globs, quoting) is therefore not interpreted; a program that needs it is given a script to run. A
 * program name without a directory part is looked up in PATH. Standard input is inherited; standard
 * output and standard error go to the output file when one is given, otherwise they are inherited.
 * It changes no process-wide signal disposition; several threads may call it at once.
 */
int runExternalCommand(
    std::filesystem::path const &program,
    std::vector<std::string> const &arguments,
    std::filesystem::path const &command_output_file_name,
    std::string &full_command
);

/******************************************************************************/
/**
 * @brief Returns a textual label for a given serialization mode.
 *
 * constexpr / string_view: the labels are fixed strings, so a constexpr
 * lookup avoids per-call std::string allocation. Returning string_view
 * lets call sites stream the result directly (it implicitly converts to
 * std::string where one is required).
 *
 * @param s The serialization mode to describe
 * @return A textual label for the given serialization mode (empty for invalid enumerators)
 */
constexpr std::string_view serializationModeToString(serializationMode s) noexcept {
    switch(s) {
    case serializationMode::GEM_BINARY:
        return "GArchive binary mode";
    case serializationMode::GEM_JSON:
        return "GArchive JSON mode";
    }
    return {}; // unreachable for valid enumerator inputs
}

/******************************************************************************/
/**
 * @brief Trims leading and trailing whitespace (spaces, tabs, CR, LF) from a string.
 */
std::string trimWhitespace(std::string_view s);

/******************************************************************************/
/**
 * @brief Splits a string into a vector of strings, according to a separator character.
 */
std::vector<std::string> splitString(std::string const &str, const char *sep);

/******************************************************************************/
/**
 * @brief Splits a string into a vector of unsigned int values, if possible (throws on failure).
 */
std::vector<unsigned int> stringToUIntVec(std::string const &raw, char sep = ',');

/******************************************************************************/
/**
 * @brief Splits a string into a vector of double values, if possible (throws on failure).
 */
std::vector<double> stringToDoubleVec(std::string const &raw);

/******************************************************************************/
/**
 * @brief Splits a string of the form "(1,2), (3,4)" into a vector of unsigned int-tuples (throws on failure).
 */
std::vector<std::tuple<unsigned int, unsigned int>> stringToUIntTupleVec(std::string const &raw);

/******************************************************************************/
/**
 * @brief Translates a string of the type "00:10:30" into a std::chrono::duration<double>.
 */
std::chrono::duration<double> duration_from_string(std::string const &duration_string);

/******************************************************************************/
/**
 * @brief Returns the number of milliseconds since 1.1.1970.
 */
std::string getMSSince1970();

/******************************************************************************/
/**
 * @brief Moves an existing output file out of the way, so a fresh one can be written.
 *
 * If (and only if) @p file_name names an existing file, it is renamed to
 * "<file_name>.bak_<milliseconds-since-1970>" and a warning naming both the original and the
 * backup is emitted. Doing nothing when the file is absent is the normal case, not an error.
 *
 * This exists as one shared function rather than as a block repeated at each output site: the
 * hand-copied form let a site test one file name while reporting another (a diagnostic naming
 * a file it had not touched), which is impossible here because the caller supplies the name
 * exactly once.
 */
bool backupExistingFile(const std::string &file_name, std::string_view context);

/******************************************************************************/
/**
 * @brief Replaces a file as a whole: what @p write puts into the stream it is given becomes the
 * content of @p target, or nothing changes.
 *
 * @p write fills a temporary file beside @p target (".<name>.tmp" in the same directory, so on the
 * same filesystem), which is then renamed over @p target. A reader, and a process that is ended while
 * it writes, sees the old file or the new one, never a part of the new one. If @p write throws or the
 * stream fails, the temporary file is removed, @p target is left as it was and the error is passed on
 * (a failed stream as a gem_exception naming @p context).
 *
 * @param target The file to replace (it need not exist yet; its directory must)
 * @param write Writes the new content into the stream it is given
 * @param context The caller, named in the messages of the exceptions
 */
void replaceFileAtomically(
    std::filesystem::path const &target,
    std::function<void(std::ostream &)> const &write,
    std::string_view context
);

/******************************************************************************/
/**
 * @brief Converts a std::chrono::high_resolution_clock::time_point into an arithmetic number.
 */
std::chrono::milliseconds::rep
time_point_to_milliseconds(std::chrono::high_resolution_clock::time_point const &val);

/******************************************************************************/
/**
 * @brief Converts an arithmetic number into a std::chrono::high_resolution_clock::time_point.
 */
std::chrono::high_resolution_clock::time_point
milliseconds_to_time_point(std::chrono::milliseconds::rep const &val);

/******************************************************************************/
/**
 * @brief Raises an exception if a given define wasn't set.
 */
void condnotset(std::string const &f, std::string const &d);

/******************************************************************************/

} /* namespace Gem::Common */
