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
#include <algorithm>
#include <format>
#include <fstream>
#include <istream>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "common/GCommonHelperFunctionsT.hpp" // parseWhole()

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

/******************************************************************************/
/**
 * @brief Reads one CSV record (RFC 4180): a line, and while a quoted field of it is open, the
 * lines that follow, joined by their line breaks. The one reader of a record of a CSV file.
 * @param in The file
 * @param record The record, without its last line break
 * @return Whether a complete record was read; false at the end of the file, and for a last record
 *         that ends before its line break (a row cut by a crash)
 */
bool readCsvRecord(std::istream &in, std::string &record) {
    if(not std::getline(in, record)) {
        return false;
    }
    auto quotes = std::ranges::count(record, '"');
    for(std::string more; quotes % 2 != 0 && not in.eof() && std::getline(in, more);) {
        quotes += std::ranges::count(more, '"');
        record += '\n';
        record += more;
    }
    return not in.eof() && quotes % 2 == 0;
}

/******************************************************************************/
/**
 * @brief The fields of a CSV record, by RFC 4180
 * @param line The record (readCsvRecord())
 * @return Its fields, unquoted
 */
std::vector<std::string> fieldsOf(std::string_view line) {
    std::vector<std::string> fields(1);
    bool quoted = false;
    for(std::size_t i = 0; i < line.size(); ++i) {
        char const c = line[i];
        if(quoted && c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
            fields.back() += '"';
            ++i;
        }
        else if(c == '"') {
            quoted = not quoted;
        }
        else if(c == ',' && not quoted) {
            fields.emplace_back();
        }
        else {
            fields.back() += c;
        }
    }
    return fields;
}

} /* namespace */

/******************************************************************************/
/**
 * The rows of a CSV file that a GCsvSink wrote for the given columns: its header must be
 * theirs. A row is read as a whole, also where a text of it spans several lines; a row with
 * another number of fields, and a last row without its line break (a row cut by a crash), is
 * left out.
 *
 * @param file The file
 * @param columns The columns of the stream
 * @return The fields of each row, as text, in column order; none if there is no such file
 */
std::optional<std::vector<std::vector<std::string>>>
GCsvSink::readRows(std::filesystem::path const &file, std::vector<GColumn> const &columns) {
    std::ifstream in(file);
    std::string line;
    if(not readCsvRecord(in, line) || line != headerLine(columns)) {
        return std::nullopt;
    }
    std::vector<std::vector<std::string>> rows;
    while(readCsvRecord(in, line)) {
        std::vector<std::string> fields = fieldsOf(line);
        if(fields.size() == columns.size()) {
            rows.push_back(std::move(fields));
        }
    }
    return rows;
}

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
    if(not readCsvRecord(in, first)) {
        return false;
    }
    return first == headerLine(columns);
}

/******************************************************************************/
/**
 * Reads the next CSV record, also one whose quoted text spans several lines (readCsvRecord()).
 *
 * @param in The file
 * @param record The record, without its last line break
 * @return Whether a complete one was read
 */
bool GCsvSink::readRecord_(std::istream &in, std::string &record) const {
    return readCsvRecord(in, record);
}

/******************************************************************************/
/**
 * A CSV file begins with its header line.
 *
 * @return 1
 */
std::size_t GCsvSink::startLines_() const {
    return 1;
}

/******************************************************************************/
/**
 * The value of one column of a CSV row, as a number.
 *
 * @param line The row (readRecord_())
 * @param column The position of the column
 * @return The value; none if the row has another number of fields than the stream has columns
 *         (a row cut by a crash) or the field is no number
 */
std::optional<double> GCsvSink::numberIn_(std::string const &line, std::size_t column) const {
    const std::vector<std::string> fields = fieldsOf(line);
    if(fields.size() != this->streamColumns_().size() || column >= fields.size()) {
        return std::nullopt;
    }
    return parseWhole<double>(fields[column]);
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
