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

#include "common/record/GCsvSink.hpp"

// Standard header files go here
#include <format>
#include <fstream>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <variant>

namespace Gem::Common::Record {

namespace {

/******************************************************************************/
/**
 * @brief A field as RFC 4180 has it: enclosed in double quotes (and a double quote written twice)
 * when it holds a comma, a double quote or a line break, or begins or ends with a space
 * @param text The field
 * @return The field as written
 */
std::string csvField(std::string_view text) {
    const bool quote = text.find_first_of(",\"\r\n") != std::string_view::npos ||
                       (not text.empty() && (text.front() == ' ' || text.back() == ' ' ||
                                             text.front() == '\t' || text.back() == '\t'));
    if(not quote) {
        return std::string(text);
    }
    std::string out = "\"";
    for(char const c : text) {
        if(c == '"') {
            out += '"';
        }
        out += c;
    }
    out += '"';
    return out;
}

/******************************************************************************/
/**
 * @brief The header line of the columns, without its line break
 * @param columns The columns
 * @return Their names, separated by commas
 */
std::string headerLine(std::vector<GColumn> const &columns) {
    std::string line;
    for(std::size_t i = 0; i < columns.size(); ++i) {
        line += (i == 0 ? "" : ",") + csvField(columns[i].name);
    }
    return line;
}

} /* namespace */

/******************************************************************************/
/**
 * The extension of a CSV file.
 *
 * @return ".csv"
 */
std::string GCsvSink::extension_() const {
    return ".csv";
}

/******************************************************************************/
/**
 * Writes the header line.
 *
 * @param out The file
 * @param columns The columns
 */
void GCsvSink::writeStart_(std::ostream &out, std::vector<GColumn> const &columns) {
    out << headerLine(columns) << '\n';
}

/******************************************************************************/
/**
 * Writes one row as one line.
 *
 * @param out The file
 * @param row The row
 */
void GCsvSink::writeRow_(std::ostream &out, GRow const &row) {
    std::string line;
    for(std::size_t i = 0; i < row.size(); ++i) {
        if(i > 0) {
            line += ',';
        }
        std::visit(
            [&line](auto const &value) {
                using V = std::remove_cvref_t<decltype(value)>;
                if constexpr(std::is_same_v<V, bool>) {
                    line += value ? "true" : "false";
                }
                else if constexpr(std::is_same_v<V, std::string>) {
                    line += csvField(value);
                }
                else if constexpr(std::is_same_v<V, GTimeStamp>) {
                    line += std::format("{:%FT%TZ}", value);
                }
                else { // the integers and double: the shortest form that reads back unchanged
                    line += std::format("{}", value);
                }
            },
            row[i]
        );
    }
    out << line << '\n';
}

/******************************************************************************/
/**
 * Whether the header line of an existing file is that of the columns.
 *
 * @param existing The file
 * @param columns The columns
 * @return Whether it is
 */
bool GCsvSink::holdsColumns_(
    std::filesystem::path const &existing,
    std::vector<GColumn> const &columns
) const {
    std::ifstream in(existing);
    std::string first;
    if(not std::getline(in, first)) {
        return false;
    }
    return first == headerLine(columns);
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
