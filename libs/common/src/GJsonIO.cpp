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

#include "common/GJsonIO.hpp"

// Standard library headers used directly in this translation unit
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

// Other Geneva headers whose symbols are used directly
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"

namespace {

/******************************************************************************/
/**
 * The parse options Geneva uses for every JSON document it reads. Comments and trailing commas are
 * tolerated so that hand-edited configuration files remain forgiving, and numbers are parsed with
 * full precision so a floating-point value round-trips exactly.
 *
 * @return The shared parse options
 */
boost::json::parse_options genevaParseOptions() {
    boost::json::parse_options opts;
    opts.allow_comments = true;
    opts.allow_trailing_commas = true;
    opts.numbers = boost::json::number_precision::precise;
    return opts;
}

/******************************************************************************/
/**
 * Recursively writes @p jv to @p os with a four-space indent, placing every object member and every
 * array element on its own line. Empty objects/arrays are emitted compactly. Scalars are rendered by
 * Boost.JSON's own serializer (correct string escaping and shortest-round-trip numbers).
 *
 * @param os The stream to write to
 * @param jv The JSON value to serialize
 * @param depth The current indentation depth (number of four-space levels)
 */
void prettyPrintImpl(std::ostream &os, boost::json::value const &jv, std::size_t depth) {
    constexpr std::size_t indent_width = 4;
    const std::string pad(depth * indent_width, ' ');
    const std::string child_pad((depth + 1) * indent_width, ' ');

    switch(jv.kind()) {
    case boost::json::kind::object: {
        boost::json::object const &obj = jv.get_object();
        if(obj.empty()) {
            os << "{}";
            break;
        }
        os << "{\n";
        bool first = true;
        for(auto const &member : obj) {
            if(not first) {
                os << ",\n";
            }
            first = false;
            os << child_pad << boost::json::serialize(member.key()) << ": ";
            prettyPrintImpl(os, member.value(), depth + 1);
        }
        os << '\n' << pad << '}';
        break;
    }
    case boost::json::kind::array: {
        boost::json::array const &arr = jv.get_array();
        if(arr.empty()) {
            os << "[]";
            break;
        }
        os << "[\n";
        bool first = true;
        for(auto const &element : arr) {
            if(not first) {
                os << ",\n";
            }
            first = false;
            os << child_pad;
            prettyPrintImpl(os, element, depth + 1);
        }
        os << '\n' << pad << ']';
        break;
    }
    default:
        // string / int64 / uint64 / double / bool / null: let Boost.JSON render the scalar token.
        os << boost::json::serialize(jv);
        break;
    }
}

} /* anonymous namespace */

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief Parses a JSON document held in a string, tolerating comments and trailing commas.
 *
 * @param text The JSON text to parse
 * @param context An optional description of the source (used only in the error message)
 * @return The parsed JSON value
 */
boost::json::value parseJsonString(std::string_view text, std::string_view context) {
    std::error_code ec;
    boost::json::value result = boost::json::parse(text, ec, {}, genevaParseOptions());
    if(ec) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Common::parseJsonString(): Error!" << '\n'
            << "Could not parse JSON"
            << (context.empty() ? std::string() : (std::string(" from ") + std::string(context)))
            << ": " << ec.message() << '\n'
        );
    }
    return result;
}

/******************************************************************************/
/**
 * @brief Reads and parses a JSON document from a file, tolerating comments and trailing commas.
 *
 * @param path The path of the JSON file to read
 * @return The parsed JSON value
 */
boost::json::value parseJsonFile(std::filesystem::path const &path) {
    std::ifstream ifs(path, std::ios::binary);
    if(not ifs) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Common::parseJsonFile(): Error!" << '\n'
            << "Could not open JSON file " << path.string() << " for reading" << '\n'
        );
    }
    std::string const content(
        (std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>())
    );
    return parseJsonString(content, path.string());
}

/******************************************************************************/
/**
 * @brief Writes @p jv to @p os as indented JSON, one object member / array element per line.
 *
 * @param os The stream to write to
 * @param jv The JSON value to serialize
 */
void prettyPrintJson(std::ostream &os, boost::json::value const &jv) {
    prettyPrintImpl(os, jv, 0);
}

/******************************************************************************/
/**
 * @brief Returns @p jv as an indented JSON string.
 *
 * @param jv The JSON value to serialize
 * @return The indented JSON representation
 */
std::string prettyPrintJson(boost::json::value const &jv) {
    std::ostringstream oss;
    prettyPrintImpl(oss, jv, 0);
    return oss.str();
}

/******************************************************************************/
/**
 * @brief Writes @p jv to @p path as indented JSON, terminated by a single newline.
 *
 * @param path The file to write
 * @param jv The JSON value to serialize
 */
void writeJsonFile(std::filesystem::path const &path, boost::json::value const &jv) {
    std::ofstream ofs(path);
    if(not ofs) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Common::writeJsonFile(): Error!" << '\n'
            << "Could not open JSON file " << path.string() << " for writing" << '\n'
        );
    }
    prettyPrintImpl(ofs, jv, 0);
    ofs << '\n';
}

/******************************************************************************/
/**
 * @brief Overlays override values onto a generated configuration document, in place.
 *
 * @param base The generated configuration value to overlay onto (modified in place)
 * @param overrides An object mapping parameter/group names to replacement values
 */
void applyConfigOverrides(boost::json::value &base, boost::json::value const &overrides) {
    if(not overrides.is_object()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Common::applyConfigOverrides(): Error!" << '\n'
            << "The override document must be a JSON object, but is of kind "
            << static_cast<int>(overrides.kind()) << '\n'
        );
    }
    if(not base.is_object()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Common::applyConfigOverrides(): Error!" << '\n'
            << "The configuration to overlay onto must be a JSON object, but is of kind "
            << static_cast<int>(base.kind()) << '\n'
        );
    }

    boost::json::object &base_obj = base.get_object();
    for(auto const &member : overrides.get_object()) {
        std::string_view const key = member.key();
        boost::json::value const &override_value = member.value();

        auto const it = base_obj.find(key);
        if(it == base_obj.end()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In Gem::Common::applyConfigOverrides(): Error!" << '\n'
                << "The override names the parameter \"" << key << "\", which the configuration"
                << " does not contain (a stale or mistyped override)." << '\n'
            );
        }

        boost::json::value &target = it->value();
        if(target.is_object() && target.get_object().contains("value")) {
            // A parameter node: replace only its value, leaving the code-owned default/comment.
            target.get_object()["value"] = override_value;
        } else if(target.is_object() && override_value.is_object()) {
            // A group node: recurse into it with the nested override object.
            applyConfigOverrides(target, override_value);
        } else {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In Gem::Common::applyConfigOverrides(): Error!" << '\n'
                << "The override for \"" << key << "\" does not match the structure of the"
                << " configuration (neither a parameter node nor a group with a nested override)." << '\n'
            );
        }
    }
}

/******************************************************************************/

} /* namespace Gem::Common */
