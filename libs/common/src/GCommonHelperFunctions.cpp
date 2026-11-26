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
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sched.h> // sched_getaffinity
#endif

// Boost.Process v2, built with boost.process.fs=std (INSTALL): BOOST_PROCESS_USE_STD_FS is set for
// this library in its CMakeLists.txt, so the process paths are std::filesystem paths
#include <boost/asio/io_context.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>

// Other Geneva headers whose symbols are used directly
#include "common/GChecks.hpp"
#include "common/GCommonEnums.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

namespace {
/******************************************************************************/
/**
 * Asks the system how many hardware threads this process may run on. On Linux that is the size
 * of the affinity mask (a batch system or taskset may assign a job a few cores of a large node);
 * when the mask cannot be read (more than CPU_SETSIZE processors), and on other systems, it is
 * std::thread::hardware_concurrency().
 *
 * @return The number of assigned hardware threads, or 0 if the system does not say
 */
unsigned int askNHardwareThreads() {
#if defined(__linux__)
    cpu_set_t assigned;
    CPU_ZERO(&assigned);
    if(0 == sched_getaffinity(0, sizeof(assigned), &assigned)) {
        const int n = CPU_COUNT(&assigned);
        if(n > 0) {
            return static_cast<unsigned int>(n);
        }
    }
#endif
    return std::thread::hardware_concurrency();
}

/******************************************************************************/
/**
 * Parses a non-empty, separator-delimited list of numbers, tolerating arbitrary
 * surrounding whitespace -- a hand-written replacement for the former
 * `qi::phrase_parse(from, to, (num % sep), qi::space, result)`. Returns true on a
 * full parse; on failure it returns false and reports the unconsumed remainder in
 * @p rest (so the caller can say where the text stops being a list).
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
    std::string_view s,
    char sep,
    std::vector<num_type> &out,
    std::string &rest
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

/******************************************************************************/
/**
 * Writes one word of a command line as a POSIX shell would need it: unchanged if it consists of
 * characters the shell does not interpret, otherwise in single quotes. Used for messages only.
 *
 * @param word The word to show
 * @return The word, quoted where needed
 */
std::string shellQuotedForDisplay(std::string const &word) {
    constexpr std::string_view plain_characters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@%+=:,./-";
    if(not word.empty() and word.find_first_not_of(plain_characters) == std::string::npos) {
        return word;
    }
    std::string quoted{"'"};
    for(char const c : word) {
        if(c == '\'') {
            quoted += "'\\''"; // end the quote, an escaped quote, reopen
        }
        else {
            quoted += c;
        }
    }
    return quoted + "'";
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
 * @return The time of the last write to the file (usable as a filesystem-based time marker)
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
 * Draws 64 random bits for an identifier (a file name, a connection id, the salt of a process).
 * Each thread has an engine of its own, seeded on first use with eight 32-bit words of
 * std::random_device through std::seed_seq. A single word of std::random_device (2^32 seeds)
 * is not enough: with n engines two share a seed with a probability of about n^2 / 2^33, which
 * reaches tens of percent for the threads of a thousand nodes, and two engines with one seed
 * draw the same identifiers. That probability cannot be shown by a unit test; the tests pin
 * that the identifiers of many threads differ.
 *
 * @return 64 random bits
 */
std::uint64_t randomBits64() {
    thread_local std::mt19937_64 engine = [] {
        std::random_device device;
        std::array<std::random_device::result_type, 8> entropy{};
        std::ranges::generate(entropy, std::ref(device));
        std::seed_seq seed(entropy.begin(), entropy.end());
        return std::mt19937_64(seed);
    }();
    return engine();
}

/******************************************************************************/
/**
 * The number of hardware threads assigned to this process (hyperthreads count as threads): on
 * Linux the size of its affinity mask, elsewhere std::thread::hardware_concurrency(). It is read
 * once, on the first call, from the mask of the calling thread (normally the one the process
 * started with), and then stays the same. If the system says nothing, a warning is logged and
 * DEFAULTNHARDWARETHREADS is used. A request for 0 threads throughout the collection means this
 * number. Thread-safe.
 *
 * @return The number of hardware threads assigned to this process, at least 1
 */
unsigned int getNHardwareThreads() {
    static const unsigned int N_THREADS = [] {
        const unsigned int n = askNHardwareThreads();
        if(n == 0) {
            glogger << "The system does not say how many hardware threads this process may use."
                    << '\n'
                    << "Using the default value " << DEFAULTNHARDWARETHREADS << " instead." << '\n'
                    << GWARNING;
            return DEFAULTNHARDWARETHREADS;
        }
        return n;
    }();
    return N_THREADS;
}

/******************************************************************************/
/**
 * This function loads textual (ASCII) data from an external file. Note that this function is
 * not currently meant to be used with binary data, although it should work with that as well
 * (at least in theory -- this is untested). There is no check whether the data loaded represents
 * a text.
 *
 * @param p The name of the file to be loaded
 * @return The data contained in the file, as a single string
 */
std::string loadTextDataFromFile(std::filesystem::path const &p) {
    // Check that the file exists
    if(not std::filesystem::exists(p)) {
        throw gem_exception(
            g_error_streamer() << "Tried to load data from file " << p.string() << '\n'
                               << "which does not exist" << '\n'
        );
    }

    std::ifstream source_file_stream(p);

    if(not source_file_stream) {
        throw gem_exception(
            g_error_streamer() << "Stream from file " << p.string() << '\n'
                               << "is not valid" << '\n'
        );
    }

    std::string source_file(
        std::istreambuf_iterator<char>(source_file_stream),
        (std::istreambuf_iterator<char>())
    );
    return source_file;
}

/******************************************************************************/
/**
 * This function loads textual (ASCII) data from an external file line by line and emits
 * a std::vector of strings, each holding a single line
 *
 * @param p The name of the file to be loaded
 * @return A std::vector holding the non-empty lines of the file
 */
std::vector<std::string> loadTextLinesFromFile(std::filesystem::path const &p) {
    // Check that the file exists
    if(not std::filesystem::exists(p)) {
        throw gem_exception(
            g_error_streamer() << "Tried to load data from file " << p.string() << '\n'
                               << "which does not exist" << '\n'
        );
    }

    std::ifstream source_file_stream(p);

    if(not source_file_stream) {
        throw gem_exception(
            g_error_streamer() << "Stream from file " << p.string() << '\n'
                               << "is not valid" << '\n'
        );
    }

    std::string line;
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
 * Runs an external program without a shell and waits for it to end (see the declaration for the
 * contract).
 *
 * @param program The program to run; a name without a directory part is looked up in PATH
 * @param arguments The arguments, each passed to the program as one argument, unchanged
 * @param command_output_file_name A file that receives the program's standard output and standard
 * error (truncated first); empty to let the program inherit both
 * @param full_command Output parameter: the command written as a shell would show it (words quoted
 * where needed), for messages only
 * @return 0 on success, else the program's exit status or the number of the signal that ended it
 * @throws gem_exception if the program is not found, the output file cannot be opened, or the
 * program cannot be started or waited for
 */
int runExternalCommand(
    std::filesystem::path const &program,
    std::vector<std::string> const &arguments,
    std::filesystem::path const &command_output_file_name,
    std::string &full_command
) {
    namespace bp = boost::process::v2;

    full_command = shellQuotedForDisplay(program.string());
    for(auto const &argument : arguments) {
        full_command += ' ' + shellQuotedForDisplay(argument);
    }
    if(not command_output_file_name.empty()) {
        full_command += " > " + shellQuotedForDisplay(command_output_file_name.string()) + " 2>&1";
    }

    // A name without a directory part is looked up in PATH, as the shell used to do
    std::filesystem::path const executable =
        program.has_parent_path() ? program : bp::environment::find_executable(program);
    if(executable.empty()) {
        throw gem_exception(
            g_error_streamer() << "In runExternalCommand(): program " << program.string()
                               << " was not found in PATH" << '\n'
                               << "Command: " << full_command << '\n'
        );
    }

    // Standard output and standard error share one open file, as "> file 2>&1" did
    std::unique_ptr<std::FILE, int (*)(std::FILE *)> output_file{nullptr, &std::fclose};
    if(not command_output_file_name.empty()) {
        output_file.reset(std::fopen(command_output_file_name.string().c_str(), "w"));
        if(not output_file) {
            int const open_error = errno;
            throw gem_exception(
                g_error_streamer() << "In runExternalCommand(): cannot open the output file "
                                   << command_output_file_name.string() << ": "
                                   << std::generic_category().message(open_error) << '\n'
                                   << "Command: " << full_command << '\n'
            );
        }
    }

    // One I/O context per call: concurrent calls share no state. The child closes every inherited
    // file descriptor but its standard streams, so a file another thread opens meanwhile does not
    // leak into it.
    boost::asio::io_context context;
    boost::system::error_code ec;
    auto child = bp::default_process_launcher()(
        context,
        ec,
        executable,
        arguments,
        bp::process_stdio{
            .in = {},
            .out = output_file ? output_file.get() : stdout,
            .err = output_file ? output_file.get() : stderr
        }
    );
    int exit_code = 0;
    if(not ec) {
        exit_code = child.wait(ec);
    }
    if(ec) {
        throw gem_exception(
            g_error_streamer() << "In runExternalCommand(): the program could not be run: "
                               << ec.message() << '\n'
                               << "Command: " << full_command << '\n'
        );
    }

    return exit_code;
}

/******************************************************************************/
/**
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
 * @param sep A single-character C-string holding the separator character
 * @return A std::vector holding the (whitespace-trimmed, non-empty) fragments
 */
std::vector<std::string> splitString(std::string const &str, const char *sep) {
    std::vector<std::string> result;

    checkThat(1 == std::string(sep).size(), "separator \"", sep, "\" is not one character");

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
 * @param sep The separator character between entries (default: ',')
 * @return A std::vector holding the parsed unsigned int values
 * @throws gem_exception naming the text and where it stops being such a list
 */
std::vector<unsigned int> stringToUIntVec(std::string const &raw, char sep) {
    std::vector<unsigned int> result;
    std::string rest;
    const bool parsed = parseSeparatedNumbers(std::string_view(raw), sep, result, rest);
    checkThat(
        parsed,
        "the text \"",
        raw,
        "\" is not a list of integers >= 0 separated by '",
        sep,
        "'; it stops being one at \"",
        rest,
        "\""
    );
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
 * @throws gem_exception naming the text and where it stops being such a list
 */
std::vector<double> stringToDoubleVec(std::string const &raw) {
    std::vector<double> result;
    std::string rest;
    const bool parsed = parseSeparatedNumbers(std::string_view(raw), ',', result, rest);
    checkThat(
        parsed,
        "the text \"",
        raw,
        "\" is not a list of numbers separated by ','; it stops being one at \"",
        rest,
        "\""
    );
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
        throw gem_exception(
            g_error_streamer() << "the text \"" << raw
                               << "\" is not a list of pairs (a,b) of integers >= 0 separated by"
                               << " ','; it stops being one at \"" << s.substr(i) << "\"" << '\n'
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
        throw gem_exception(
            g_error_streamer() << "Invalid number of fields present in \"" << duration_string
                               << "\": " << timings.size() << '\n'
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
    std::ostringstream oss;
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
 *   (e.g. "GMyLogger::write(): Warning!")
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
 * Replaces a file as a whole through a temporary file beside it; documented at the declaration.
 */
void replaceFileAtomically(
    std::filesystem::path const &target,
    std::function<void(std::ostream &)> const &write,
    std::string_view context
) {
    // The same directory, so the same filesystem: the rename below is atomic there. A name of
    // its own per call, so that writers of one target never share a temporary file; noreplace
    // refuses a name that exists already instead of filling another writer's file.
    std::filesystem::path const tmp_path =
        target.parent_path() /
        std::format(".{}.{:016x}.tmp", target.filename().string(), randomBits64());
    auto const fail = [&](std::string const &what) {
        std::error_code ignore;
        std::filesystem::remove(tmp_path, ignore);
        throw gem_exception(
            g_error_streamer() << "In " << context << ": Error!" << '\n'
                               << what << '\n'
        );
    };

    {
        std::ofstream ofs(tmp_path, std::ofstream::out | std::ofstream::noreplace);
        if(not ofs) {
            // Not fail(): removing the name could remove a file that is not ours
            throw gem_exception(
                g_error_streamer()
                << "In " << context << ": Error!" << '\n'
                << "Could not create the temporary file " << tmp_path.string() << '\n'
            );
        }
        try {
            write(ofs);
        }
        catch(...) {
            ofs.close();
            std::error_code ignore;
            std::filesystem::remove(tmp_path, ignore);
            throw;
        }
        ofs.close(); // flushes: a full disk shows here, before the target is touched
        if(ofs.fail()) {
            fail("Could not write the temporary file " + tmp_path.string());
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, target, ec);
    if(ec) {
        fail(
            "Could not rename " + tmp_path.string() + " over " + target.string() + ": " +
            ec.message()
        );
    }
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
    std::ostringstream error;
    error << '\n'
          << "================================================" << '\n'
          << "In function " << f << " Error!" << '\n'
          << "Function was called even though " << d << " hasn't been set." << '\n'
          << "================================================" << '\n';
    throw(gem_exception(error.str()));
}

/******************************************************************************/

} /* namespace Gem::Common */
