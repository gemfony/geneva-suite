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

#include "common/GCommonHelperFunctions.hpp"

// Standard library headers used directly in this translation unit
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// Other Geneva headers whose symbols are used directly
#include "common/GCommonEnums.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

namespace {
std::mutex g_hwt_read_mutex;         // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_hwt_read{false}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<unsigned int> g_nHardwareThreads{
    Gem::Common::DEFAULTNHARDWARETHREADS
}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/******************************************************************************/
/**
 * Parses a non-empty, separator-delimited list of numbers, tolerating arbitrary
 * surrounding whitespace -- a hand-written replacement for the former
 * `qi::phrase_parse(from, to, (num % sep), qi::space, result)`. Returns true on a
 * full parse; on failure it returns false and reports the unconsumed remainder in
 * @p rest (so the caller can reproduce the original "Stopped at ..." diagnostic).
 *
 * @tparam num_type The arithmetic type each element is parsed into (via std::from_chars)
 * @param s The input text to parse
 * @param sep The separator character expected between elements
 * @param out Output parameter: receives the successfully parsed values (appended)
 * @param rest Output parameter: on failure, receives the unconsumed remainder of the input
 * @return true on a full parse, false otherwise
 */
template <typename num_type>
bool parseSeparatedNumbers(
    std::string_view s, char sep, std::vector<num_type> &out, std::string &rest
) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    auto skipws = [&]() {
        while(i < n && std::isspace(static_cast<unsigned char>(s[i])) != 0) {
            ++i;
        }
    };

    skipws();
    if(i >= n) { // the grammar `num % sep` requires at least one element
        rest.assign(s.substr(i));
        return false;
    }

    while(true) {
        skipws();
        num_type value{};
        const char *first = s.data() + i;
        const char *last = s.data() + n;
        auto [ptr, ec] = std::from_chars(first, last, value);
        if(ec != std::errc() || ptr == first) {
            rest.assign(s.substr(i));
            return false;
        }
        i = static_cast<std::size_t>(ptr - s.data());
        out.push_back(value);

        skipws();
        if(i >= n) {
            break; // list fully consumed
        }
        if(s[i] != sep) { // trailing garbage -> the original would leave `from != to`
            rest.assign(s.substr(i));
            return false;
        }
        ++i; // consume the separator and parse the next element
    }

    return true;
}
} /* anonymous namespace */

namespace Gem::Common {

/******************************************************************************/
/**
 * This function is meant to determine the last-write-time of a file in a given path.
 * The file will be created, if it does not exist, filled with an optional text
 * and removed afterwards, if a) the file did not exist before and b) the user
 * wants the file to be removed. The time of the last write to the file will be
 * returned, so this function can be used to create a time marker from the file system.
 *
 * @param path The path (including file name) to the file to be touched
 * @param content An optional content for the file (default empty)
 * @param remove_if_not_present Indicates whether the file should be removed after having been touched (default false)
 * @return The time of the last write to the file
 */
std::filesystem::file_time_type touch_time(
    std::filesystem::path const &path,
    std::string const &content,
    bool remove_if_not_present
) {
    bool const file_already_existed = std::filesystem::exists(path);
    std::ofstream ofs(path);
    ofs << content;
    auto last_write_time = std::filesystem::last_write_time(path);
    if(remove_if_not_present && not file_already_existed) {
        std::filesystem::remove(path);
    }
    return last_write_time;
}

/******************************************************************************/
/**
 * This function retrieves the number of CPU cores on the system (possibly including "virtual cores" such
 * as in the case of hyperthreading). The function is thread-safe. When called multiple times, it will read the number of
 * hardware threads from a local cache.
 *
 * @return A guess at a suitable number of threads for this architecture
 */
unsigned int getNHardwareThreads() {
    if(not g_hwt_read) {
        std::scoped_lock const lock(g_hwt_read_mutex);
        if(not g_hwt_read) {
            g_nHardwareThreads.store(std::thread::hardware_concurrency());

            if(g_nHardwareThreads.load() == 0) { // We could not load the number of hardware threads
                glogger << "In getNHardwareThreads():"
                        << "Could not get information regarding suitable number of threads."
                        << '\n'
                        << "from hardware. Using the default value  = " << DEFAULTNHARDWARETHREADS
                        << " instead." << '\n'
                        << GWARNING;

                g_nHardwareThreads.store(DEFAULTNHARDWARETHREADS);
            }

            g_hwt_read = true;
        }
    } // exclusive access ends

    return g_nHardwareThreads.load();
}

/******************************************************************************/
/**
 * This function loads textual (ASCII) data from an external file. Note that this function is
 * not currently meant to be used with binary data, although it should work with that as well
 * (at least in theory -- this is untested). There is no check whether the data loaded represents
 * a text.
 *
 * @param p The name of the file to be loaded
 * @return The data contained in the file
 */
std::string loadTextDataFromFile(std::filesystem::path const &p) {
    // Check that the file exists
    if(not std::filesystem::exists(p)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In loadTextDataFromFile(): Error!" << '\n'
            << "Tried to load data from file " << p.string() << '\n'
            << "which does not exist" << '\n'
        );
    }

    std::ifstream source_file_stream(p);

    if(not source_file_stream) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In loadTextDataFromFile(): Error!" << '\n'
            << "Stream from file " << p.string() << '\n'
            << "is not valid" << '\n'
        );
    }

    std::string source_file( // NOLINT(cppcoreguidelines-init-variables)
			std::istreambuf_iterator<char>(source_file_stream), (std::istreambuf_iterator<char>())
		);
    return source_file;
}

/******************************************************************************/
/**
 * This function loads textual (ASCII) data from an external file line by line and emits
 * a std::vector of strings, each holding a single line
 *
 * @param p The name of the file to be loaded
 * @return The data contained in the file, as a std::vector
 */
std::vector<std::string> loadTextLinesFromFile(std::filesystem::path const &p) {
    // Check that the file exists
    if(not std::filesystem::exists(p)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In loadTextLinesFromFile(): Error!" << '\n'
            << "Tried to load data from file " << p.string() << '\n'
            << "which does not exist" << '\n'
        );
    }

    std::ifstream source_file_stream(p);

    if(not source_file_stream) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In loadTextLinesFromFile(): Error!" << '\n'
            << "Stream from file " << p.string() << '\n'
            << "is not valid" << '\n'
        );
    }

    std::string line; // NOLINT(cppcoreguidelines-init-variables)
    std::vector<std::string> str_result_vec;

    while(std::getline(source_file_stream, line)) {
        // Omit empty lines, store everything else in the vector
        if(not line.empty()) {
            str_result_vec.push_back(line);
        }
    }

    return str_result_vec;
}

/******************************************************************************/
/**
 * Execute an external command, reacting to possible errors.
 *
 * @param program The command (program) to be executed
 * @param arguments The list of arguments to be appended to the command
 * @param command_output_file_name The name of a file to which the command's output should be piped (empty to skip)
 * @param full_command Output parameter: receives the full command line that was assembled and executed
 * @return The error code returned by the executed command
 */
int runExternalCommand(
    std::filesystem::path const &program,
    std::vector<std::string> const &arguments,
    std::filesystem::path const &command_output_file_name,
    std::string &full_command
) {
    // Convert slashes to backslashes on Windows
    std::filesystem::path p_program = program;
    std::string local_command = (p_program.make_preferred()).string();

    // Add command line arguments
    for(auto const &argument : arguments) {
        local_command += (std::string(" ") + argument);
    }

    // If requested by the user, we want to send the command to an external file
    if(not command_output_file_name.empty()) {
        std::filesystem::path p_command_output_file_name = command_output_file_name;
        std::string const localcommand_output_file_name =
            (p_command_output_file_name.make_preferred()).string();

        local_command = std::string("(") + local_command + std::string(") > ") +
                        localcommand_output_file_name + std::string(" 2>&1");
    }

    // MOstly for external debugging
#ifdef GEM_COMMON_PRINT_COMMANDLINE
    std::cout << "Executing external command \"" << localCommand << "\" ...";
#endif /* GEM_COMMON_PRINT_COMMANDLINE */

    // Assign the full command (mostly needed for external error-evaluation)
    full_command = local_command;

    // Run the actual command.
    // NOLINTNEXTLINE(concurrency-mt-unsafe,cert-env33-c) -- deliberate: this IS the external-command runner, invoked from single-threaded orchestration
    int const error_code = system(local_command.c_str());

#ifdef GEM_COMMON_PRINT_COMMANDLINE
    std::cout << "... done." << '\n';
#endif /* GEM_COMMON_PRINT_COMMANDLINE */

    // The error code will be returned as the function valiue
    return error_code;
}

/******************************************************************************/
/**
 * Splits a string into a vector of strings, according to a seperator character.
 * Any trailing or leading white spaces are removed from the result strings.
 *
 * @param str The string to be split
 * @brief Trims leading and trailing whitespace (spaces, tabs, CR, LF) from a string.
 *
 * @param s The string to trim
 * @return The trimmed string; an empty string if @p s is whitespace-only
 */
std::string trimWhitespace(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if(b == std::string_view::npos) {
        return {};
    }
    const auto e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

/******************************************************************************/
/**
 * @brief Splits a string into a vector of strings, according to a separator character.
 *
 * @param str The string to be split
 * @param sep The separator character
 * @return A std::vector holding the fragments
 */
std::vector<std::string> splitString(std::string const &str, const char *sep) {
    std::vector<std::string> result;

#ifdef DEBUG
    if(1 != std::string(sep).size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In splitString(): Error!" << '\n'
            << "Supplied separator \"" << sep << "\" has invalid size " << std::string(sep).size()
            << '\n'
        );
    }
#endif /* DEBUG */

    const char sep_char = sep[0];
    const std::string_view sv{str};
    std::string_view::size_type start = 0;
    for(;;) {
        const auto pos = sv.find(sep_char, start);
        // One shared tail for every fragment, including the remainder after the last separator
        std::string frag = trimWhitespace(sv.substr(start, pos - start));
        if(not frag.empty()) {
            result.push_back(std::move(frag));
        }
        if(pos == std::string_view::npos) {
            break;
        }
        start = pos + 1;
    }

    return result;
}

/******************************************************************************/
/**
 * Splits a string into a vector of unsigned int, if possible, or throws
 * an exception. The list must at least contain one entry and must be
 * separated by the @p sep character.
 *
 * @param raw The string to be parsed (must contain at least one entry)
 * @param sep The separator character between entries
 * @return A std::vector holding the parsed unsigned int values
 */
std::vector<unsigned int> stringToUIntVec(std::string const &raw, char sep) {
    std::vector<unsigned int> result;
    std::string rest;

    if(not parseSeparatedNumbers(std::string_view(raw), sep, result, rest)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In stringToUIntVec(const std::string& raw): Error!" << '\n'
            << "Parsing failed." << '\n'
            << "Stopped at: \": " << rest << "\"" << '\n'
        );
    }

    return result;
}

/******************************************************************************/
/**
 * Splits a string into a vector of double values, if possible, or throws
 * an exception. The list must at least contain one entry and must be
 * comma-separated.
 *
 * @param raw The comma-separated string to be parsed (must contain at least one entry)
 * @return A std::vector holding the parsed double values
 */
std::vector<double> stringToDoubleVec(std::string const &raw) {
    std::vector<double> result;
    std::string rest;

    if(not parseSeparatedNumbers(std::string_view(raw), ',', result, rest)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In stringToDoubleVec(const std::string& raw): Error!" << '\n'
            << "Parsing failed." << '\n'
            << "Stopped at: \": " << rest << "\"" << '\n'
        );
    }

    return result;
}

/******************************************************************************/
/**
 * Splits a string into a vector of unsigned int-tuples, if possible, or
 * throws an exception. The string should have the form "(1,2), (3,4)" etc.
 *
 * @param raw The string to be parsed (must contain at least one "(a,b)" tuple)
 * @return A std::vector holding the parsed (unsigned int, unsigned int) tuples
 */
std::vector<std::tuple<unsigned int, unsigned int>> stringToUIntTupleVec(std::string const &raw) {
    // Hand-written replacement for the former Spirit grammar
    // (('(' >> uint_ >> ',' >> uint_ >> ')') % ','), qi::space skipper: a non-empty,
    // comma-separated list of "(a,b)" unsigned-int pairs with arbitrary whitespace.
    std::vector<std::tuple<unsigned int, unsigned int>> result;

    const std::string_view s(raw);
    std::size_t i = 0;
    const std::size_t n = s.size();

    auto skipws = [&]() {
        while(i < n && std::isspace(static_cast<unsigned char>(s[i])) != 0) {
            ++i;
        }
    };
    auto throwFail = [&]() {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In stringToUIntTupleVec(const std::string& raw): Error!" << '\n'
            << "Parsing failed." << '\n'
            << "Stopped at: \"" << std::string(s.substr(i)) << "\"" << '\n'
        );
    };
    auto expect = [&](char c) {
        skipws();
        if(i >= n || s[i] != c) {
            throwFail();
        }
        ++i;
    };
    auto parseUInt = [&]() -> unsigned int {
        skipws();
        unsigned int value = 0;
        const char *first = s.data() + i;
        auto [ptr, ec] = std::from_chars(first, s.data() + n, value);
        if(ec != std::errc() || ptr == first) {
            throwFail();
        }
        i = static_cast<std::size_t>(ptr - s.data());
        return value;
    };

    skipws();
    if(i >= n) { // the grammar requires at least one tuple
        throwFail();
    }

    while(true) {
        expect('(');
        unsigned int const a = parseUInt();
        expect(',');
        unsigned int const b = parseUInt();
        expect(')');
        result.emplace_back(a, b);

        skipws();
        if(i >= n) {
            break; // list fully consumed
        }
        if(s[i] != ',') { // trailing garbage
            throwFail();
        }
        ++i; // consume the tuple separator and parse the next pair
    }

    return result;
}

/******************************************************************************/
/**
 * Translates a string of the type "00:10:30" into a std::chrono::duration<double>
 * object denoting hours:minutes:seconds
 *
 * @param duration_string A "hours:minutes:seconds" style string (1, 2 or 3 colon-separated fields)
 * @return The corresponding duration as a std::chrono::duration<double>
 */
std::chrono::duration<double> duration_from_string(std::string const &duration_string) {
    std::vector<unsigned int> timings = stringToUIntVec(duration_string, ':');

    switch(timings.size()) {
    case 1:
        return std::chrono::seconds(timings.at(0));
        break;

    case 2:
        return (std::chrono::minutes(timings.at(0)) + std::chrono::seconds(timings.at(1)));
        break;

    case 3:
        return (
            std::chrono::hours(timings.at(0)) + std::chrono::minutes(timings.at(1)) +
            std::chrono::seconds(timings.at(2))
        );
        break;

    default:
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Common::duration_from_string(\"" << duration_string << "\"): Error!"
            << '\n'
            << "Invalid number of fields present: " << timings.size() << '\n'
        );
        break;
    }
}

/******************************************************************************/
/**
 * Converts the current time to a string
 *
 * @return The current local time, formatted as a human-readable string
 */
std::string currentTimeAsString() {
    std::ostringstream oss; // NOLINT(cppcoreguidelines-init-variables)
    std::time_t const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm timeinfo{};

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
    localtime_s(&timeinfo, &now);
#else // We assume a POSIX-compliant platform
    localtime_r(&now, &timeinfo);
#endif

    oss << std::put_time(&timeinfo, "%c");
    return oss.str();
}

/******************************************************************************/
/**
 * Returns the number of milliseconds since 1.1.1970
 *
 * @return The number of milliseconds elapsed since the Unix epoch, as a string
 */
std::string getMSSince1970() {
    std::chrono::time_point<std::chrono::system_clock> const p1; // 1970
    std::chrono::time_point<std::chrono::system_clock> const p2 = std::chrono::system_clock::now();
    std::chrono::milliseconds const ms_since_1970 =
        std::chrono::duration_cast<std::chrono::milliseconds>(p2 - p1);
    return std::to_string(
        ms_since_1970.count()
    ); // Cannot use Gem::Common::to_string here as we do not want to include GCommonHelperFunctionsT.hpp
}

/******************************************************************************/
/**
 * Moves an existing output file out of the way, so a fresh one can be written. See the
 * declaration for why this is a shared function rather than a per-site block.
 *
 * @param file_name The output file to back up if it exists
 * @param context The caller's identification, used verbatim as the first line of the warning
 * @return true if a backup was made, false if there was no such file
 */
bool backupExistingFile(const std::string &file_name, std::string_view context) {
    if(not std::filesystem::exists(file_name)) {
        return false;
    }

    std::string const new_file_name = file_name + ".bak_" + getMSSince1970();

    glogger << "In " << context << '\n'
            << "Attempt to output information to file " << file_name << '\n'
            << "which already exists. We will rename the old file to" << '\n'
            << new_file_name << '\n'
            << GWARNING;

    std::filesystem::rename(file_name, new_file_name);

    return true;
}

/******************************************************************************/
/**
 * Converts a std::chrono::high_resolution_clock::time_point into an arithmetic number
 *
 * @param val The time point to convert
 * @return The number of milliseconds since the clock's epoch
 */
std::chrono::milliseconds::rep
time_point_to_milliseconds(std::chrono::high_resolution_clock::time_point const &val) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(val.time_since_epoch()).count();
}

/******************************************************************************/
/**
 * Converts an arithmetic number into a std::chrono::high_resolution_clock::time_point
 *
 * @param val A number of milliseconds since the clock's epoch
 * @return The corresponding std::chrono::high_resolution_clock::time_point
 */
std::chrono::high_resolution_clock::time_point
milliseconds_to_time_point(std::chrono::milliseconds::rep const &val) {
    return std::chrono::high_resolution_clock::time_point(std::chrono::milliseconds(val));
}

/******************************************************************************/
/**
 * Raise an exception if a given define wasn't set. "F" stands for "function",
 * "D" for "define".
 *
 * @param f The name of the function that was called ("function")
 * @param d The name of the define that was expected to be set ("define")
 */
void condnotset(std::string const &f, std::string const &d) {
    std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)
    error << '\n'
          << "================================================" << '\n'
          << "In function " << f << " Error!" << '\n'
          << "Function was called even though " << d << " hasn't been set." << '\n'
          << "================================================" << '\n';
    throw(geneva_exception(error.str()));
}

/******************************************************************************/

} /* namespace Gem::Common */
