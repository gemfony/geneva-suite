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
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>

// Boost headers go here
#include <boost/json.hpp>

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief The JSON I/O facilities shared by every Geneva subsystem that reads or writes JSON.
 *
 * This is the single seam over Boost.JSON. It provides the two things Boost.JSON does not offer
 * out of the box but which Geneva relies on:
 *
 *  - a filesystem-path-aware parser that accepts the lenient dialect Geneva emits (comments and
 *    trailing commas tolerated) and parses numbers with full round-trip precision, and
 *  - a pretty-printer that lays out every object member and every array element on its own line,
 *    so generated configuration files stay human-readable and hand-editable (in particular a
 *    `"comment"` array prints one comment line per line).
 *
 * Boost.JSON's own @c serialize() produces compact, single-line output with no pretty-printing,
 * which is why these helpers exist. All of Geneva's JSON producers (the configuration writer, the
 * individual-to-JSON dump, the external-evaluator protocol) route through prettyPrintJson().
 */

/******************************************************************************/
/**
 * @brief Parses a JSON document held in a string, tolerating comments and trailing commas.
 *
 * Numbers are parsed with full precision so a floating-point value round-trips exactly. On a parse
 * error a Gem::Common::geneva_exception is thrown.
 *
 * @param text The JSON text to parse
 * @param context An optional description of the source (used only in the error message)
 * @return The parsed JSON value
 */
boost::json::value parseJsonString(std::string_view text, std::string_view context = "");

/******************************************************************************/
/**
 * @brief Reads and parses a JSON document from a file, tolerating comments and trailing commas.
 *
 * A thin convenience wrapper around the Boost.JSON parser: it accepts a std::filesystem::path (which
 * Boost.JSON's parser does not) and reports the file name on error.
 *
 * @param path The path of the JSON file to read
 * @return The parsed JSON value
 */
boost::json::value parseJsonFile(std::filesystem::path const &path);

/******************************************************************************/
/**
 * @brief Writes @p jv to @p os as indented JSON, one object member / array element per line.
 *
 * Uses a four-space indent (matching Geneva's historical configuration layout). Empty objects and
 * arrays are emitted compactly (`{}` / `[]`). No trailing newline is appended (see writeJsonFile()).
 *
 * @param os The stream to write to
 * @param jv The JSON value to serialize
 */
void prettyPrintJson(std::ostream &os, boost::json::value const &jv);

/******************************************************************************/
/**
 * @brief Returns @p jv as an indented JSON string (see the streaming overload).
 *
 * @param jv The JSON value to serialize
 * @return The indented JSON representation
 */
std::string prettyPrintJson(boost::json::value const &jv);

/******************************************************************************/
/**
 * @brief Writes @p jv to @p path as indented JSON, terminated by a single newline.
 *
 * This is a plain (non-atomic) write. Callers that must not truncate a live file on failure
 * (e.g. the configuration update-in-place path) perform their own temp-file-plus-rename around
 * prettyPrintJson() instead.
 *
 * @param path The file to write
 * @param jv The JSON value to serialize
 */
void writeJsonFile(std::filesystem::path const &path, boost::json::value const &jv);

/******************************************************************************/
/**
 * @brief Overlays override values onto a generated configuration document, in place.
 *
 * This is the merge step of the "configs derived from code + a handful of per-example overrides"
 * model: a binary emits its parameter defaults as a canonical configuration (every parameter a
 * node carrying @c "comment", @c "default" and @c "value"), and this helper then layers the small
 * set of intentional non-default values on top, keyed by parameter name.
 *
 * A configuration is a JSON object whose members are either **parameter nodes** (objects carrying a
 * @c "value" member) or **groups** (objects whose members are themselves parameter nodes or nested
 * groups, e.g. @c touched_termination). An override document mirrors that shape but carries, for
 * each parameter it changes, only the bare replacement value (a group is a nested object recursing
 * into it). For every key in @p overrides:
 *
 *  - if the matching node in @p base is a parameter node, its @c "value" is replaced by the override
 *    value (an array value is replaced wholesale); the code-owned @c "default" and @c "comment" are
 *    left untouched, so an override stays valid across future default/comment changes;
 *  - if it is a group and the override value is an object, the overlay recurses into it;
 *  - otherwise the override names a key the configuration does not contain, or its shape does not
 *    match the target node, and a Gem::Common::geneva_exception is thrown — a stale or mistyped
 *    override fails loudly rather than being silently ignored.
 *
 * @param base The generated configuration value to overlay onto (modified in place)
 * @param overrides An object mapping parameter/group names to replacement values
 */
void applyConfigOverrides(boost::json::value &base, boost::json::value const &overrides);

/******************************************************************************/

} /* namespace Gem::Common */
