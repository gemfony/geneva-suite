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

#include "common/record/GTextSink.hpp"

// Standard header files go here
#include <chrono>
#include <format>
#include <type_traits>
#include <variant>

// Boost header files go here

// Geneva header files go here

namespace Gem::Common::Record {

/******************************************************************************/
/**
 * The extension of a text file.
 *
 * @return ".log"
 */
std::string GTextSink::extension_() const {
    return ".log";
}

/******************************************************************************/
/**
 * A text file begins with its first row; nothing is written.
 *
 * @param out The file
 * @param columns The columns
 */
void GTextSink::writeStart_(
    [[maybe_unused]] std::ostream &out,
    [[maybe_unused]] std::vector<GColumn> const &columns
) { /* nothing */
}

/******************************************************************************/
/**
 * Writes one row as one line: the values of its columns, separated by a space (none before a
 * value that begins with a line break), an empty text left out; a time stamp to the second, in
 * UTC. A line break at the end of the last value is the line's own.
 *
 * @param out The file
 * @param row The row
 */
void GTextSink::writeRow_(std::ostream &out, GRow const &row) {
    std::string line;
    for(auto const &value : row) {
        std::string const text = std::visit(
            [](auto const &v) -> std::string {
                using V = std::remove_cvref_t<decltype(v)>;
                if constexpr(std::is_same_v<V, bool>) {
                    return v ? "true" : "false";
                }
                else if constexpr(std::is_same_v<V, std::string>) {
                    return v;
                }
                else if constexpr(std::is_same_v<V, GTimeStamp>) {
                    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(v));
                }
                else {
                    return std::format("{}", v);
                }
            },
            value
        );
        if(not text.empty()) {
            line += (line.empty() || text.starts_with('\n') ? "" : " ") + text;
        }
    }
    if(line.ends_with('\n')) {
        line.pop_back();
    }
    out << line << '\n';
}

/******************************************************************************/
/**
 * A text file has no header by which it could be recognized as a stream's own.
 *
 * @param existing The file
 * @param columns The columns
 * @return false
 */
bool GTextSink::holdsColumns_(
    [[maybe_unused]] std::filesystem::path const &existing,
    [[maybe_unused]] std::vector<GColumn> const &columns
) const {
    return false;
}

/******************************************************************************/
/**
 * A text file begins with its first row.
 *
 * @return 0
 */
std::size_t GTextSink::startLines_() const {
    return 0;
}

/******************************************************************************/
/**
 * A text file holds no numbers by which it is cut back (holdsColumns_() never lets it resume).
 *
 * @param line The line of a row
 * @param column The position of a column
 * @return none
 */
std::optional<double> GTextSink::numberIn_(
    [[maybe_unused]] std::string const &line,
    [[maybe_unused]] std::size_t column
) const {
    return std::nullopt;
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
