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

#include "common/record/GJsonLinesSink.hpp"

// Standard header files go here
#include <cmath>
#include <format>
#include <fstream>
#include <string_view>
#include <type_traits>
#include <variant>

// Boost header files go here
#include <boost/json.hpp>

// Geneva header files go here
#include "common/GJsonIO.hpp" // parseJsonString()

namespace Gem::Common::Record {

namespace {

/******************************************************************************/
/**
 * @brief A JSON string: in double quotes, with a double quote, a backslash and the control
 * characters escaped
 * @param text The text (UTF-8)
 * @return The JSON string
 */
std::string jsonString(std::string_view text) {
    std::string out = "\"";
    for(char const c : text) {
        switch(c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if(static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned int>(c));
            }
            else {
                out += c;
            }
            break;
        }
    }
    out += '"';
    return out;
}

} /* namespace */

/******************************************************************************/
/**
 * The extension of a JSON Lines file.
 *
 * @return ".jsonl"
 */
std::string GJsonLinesSink::extension_() const {
    return ".jsonl";
}

/******************************************************************************/
/**
 * A JSON Lines file begins with its first row; nothing is written.
 *
 * @param out The file
 * @param columns The columns
 */
void GJsonLinesSink::writeStart_(
    [[maybe_unused]] std::ostream &out,
    [[maybe_unused]] std::vector<GColumn> const &columns
) { /* nothing */
}

/******************************************************************************/
/**
 * Writes one row as one JSON object on one line, its members the columns in column order.
 *
 * @param out The file
 * @param row The row
 */
void GJsonLinesSink::writeRow_(std::ostream &out, GRow const &row) {
    auto const &columns = this->streamColumns_();
    std::string line = "{";
    for(std::size_t i = 0; i < row.size(); ++i) {
        line += (i == 0 ? "" : ",") + jsonString(columns.at(i).name) + ":";
        std::visit(
            [&line](auto const &value) {
                using V = std::remove_cvref_t<decltype(value)>;
                if constexpr(std::is_same_v<V, bool>) {
                    line += value ? "true" : "false";
                }
                else if constexpr(std::is_same_v<V, std::string>) {
                    line += jsonString(value);
                }
                else if constexpr(std::is_same_v<V, GTimeStamp>) {
                    line += jsonString(std::format("{:%FT%TZ}", value));
                }
                else if constexpr(std::is_same_v<V, double>) {
                    line += std::isfinite(value) ? std::format("{}", value) : std::string("null");
                }
                else {
                    line += std::format("{}", value);
                }
            },
            row[i]
        );
    }
    line += "}";
    out << line << '\n';
}

/******************************************************************************/
/**
 * Whether the members of the first object of an existing file are the columns, in their order.
 *
 * @param existing The file
 * @param columns The columns
 * @return Whether they are
 */
bool GJsonLinesSink::holdsColumns_(
    std::filesystem::path const &existing,
    std::vector<GColumn> const &columns
) const {
    std::ifstream in(existing);
    std::string first;
    if(not std::getline(in, first)) {
        return false;
    }
    try {
        boost::json::value const jv = Gem::Common::parseJsonString(first, existing.string());
        if(not jv.is_object()) {
            return false;
        }
        auto const &object = jv.as_object();
        if(object.size() != columns.size()) {
            return false;
        }
        std::size_t i = 0;
        for(auto const &member : object) {
            if(member.key() != columns[i++].name) {
                return false;
            }
        }
        return true;
    }
    catch(std::exception const &) {
        return false; // not a JSON object: not this stream's file
    }
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
