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

#include "geneva/genome/GParameterPropertyParser.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace Gem::Geneva::Genome {

constexpr std::size_t GPP_DEF_NSTEPS = 100; // The default number of steps for a given parameter

/******************************************************************************/
// Small hand-written parsing helpers. They replace the former Boost.Spirit grammar:
// the parameter-property syntax is now strictly positional, so a tiny tokenizer is
// both sufficient and far simpler.
//
// Grammar (whitespace is ignored):
//   spec-list := spec ( ',' spec )*
//   spec      := type '(' args ')'
//   type      := 'd' | 'f' | 'i' | 'b' | 's'
//   args (d/f/i) := index ',' lower ',' upper [ ',' nSteps ] [ ',' label ]
//   args (b)     := index [ ',' lower ',' upper [ ',' nSteps ] ] [ ',' label ]
//   args (s)     := nItems
// Parameters are addressed by 'index' (their position in the flat parameter vector of
// the corresponding type). The optional 'label' is a free-form display name used by
// monitors (e.g. GProgressPlotter axis labels); it does NOT identify a parameter.
namespace {

/**
 * @brief Strips leading and trailing whitespace (space, tab, newline, carriage return) from a string.
 *
 * @param s The string to trim
 * @return A copy of @p s with surrounding whitespace removed, or an empty string if @p s is all whitespace
 */
std::string trim(const std::string &s) {
    std::size_t const b = s.find_first_not_of(" \t\n\r");
    if(b == std::string::npos) {
        return std::string{};
    }
    std::size_t const e = s.find_last_not_of(" \t\n\r");
    return s.substr(b, e - b + 1);
}

/**
 * @brief Splits a string on commas, trimming whitespace from each resulting token.
 *
 * An empty input yields a single empty token; a trailing comma yields a trailing empty token.
 *
 * @param s The string to split (the comma-separated argument list of a spec)
 * @return The list of trimmed tokens between commas, in order
 */
std::vector<std::string> splitOnComma(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for(char const c : s) {
        if(c == ',') {
            out.push_back(trim(cur));
            cur.clear();
        }
        else {
            cur.push_back(c);
        }
    }
    out.push_back(trim(cur));
    return out;
}

/**
 * @brief Checks whether a string consists solely of decimal digits (i.e. is a valid unsigned integer literal).
 *
 * @param s The token to test
 * @return true if @p s is non-empty and contains only characters '0'-'9', false otherwise
 */
bool isUnsigned(const std::string &s) {
    if(s.empty()) {
        return false;
    }
    return std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

/**
 * @brief Aborts parsing by throwing a geneva_exception that reports the offending fragment.
 *
 * Marked [[noreturn]]: it always throws and never returns to the caller.
 *
 * @param raw The raw (sub)string that could not be parsed, included verbatim in the error message
 */
[[noreturn]] void fail(const std::string &raw) {
    throw geneva_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In GParameterPropertyParser::parse(): Error!" << '\n'
        << "Could not parse parameter-property fragment: " << raw << '\n'
    );
}

/**
 * @brief Converts a token to an unsigned integer, failing the parse if it is not a valid unsigned literal.
 *
 * @param s The token to convert
 * @param raw The enclosing raw fragment, forwarded to fail() for error reporting on failure
 * @return The parsed value as a std::size_t
 */
std::size_t toUnsigned(const std::string &s, const std::string &raw) {
    if(not isUnsigned(s)) {
        fail(raw);
    }
    return static_cast<std::size_t>(std::stoul(s));
}

/**
 * @brief Converts a token to a boolean, accepting "true"/"1" and "false"/"0", and failing the parse otherwise.
 *
 * @param s The token to convert
 * @param raw The enclosing raw fragment, forwarded to fail() for error reporting on an unrecognized token
 * @return true for "true"/"1", false for "false"/"0"
 */
bool toBool(const std::string &s, const std::string &raw) {
    if(s == "true" || s == "1") {
        return true;
    }
    if(s == "false" || s == "0") {
        return false;
    }
    fail(raw);
}

/**
 * @brief Builds a parPropSpec for a numeric (d/f/i) parameter from its argument tokens.
 *
 * Fills the index, lower/upper boundaries, step count and optional label of the spec. The
 * first three tokens are the mandatory index, lower boundary and upper boundary. Any further
 * tokens are interpreted positionally-by-content: a purely numeric token sets nSteps, anything
 * else is taken as the free-form display label. When no nSteps token is given, GPP_DEF_NSTEPS
 * is used.
 *
 * @tparam par_type The parameter value type of the spec (double, float or std::int32_t)
 * @tparam ConvFun Callable type used to convert a boundary token into a par_type value
 * @param tok The comma-split argument tokens of the spec (at least index, lower, upper)
 * @param convertBound Functor converting a boundary token to par_type; invoked as convertBound(token, raw)
 * @param raw The raw fragment content, forwarded to fail() for error reporting
 * @return A fully populated parPropSpec<par_type> describing the parameter scan
 */
template <typename par_type, typename ConvFun>
parPropSpec<par_type> makeNumericSpec(
    const std::vector<std::string> &tok,
    ConvFun convertBound,
    const std::string &raw
) {
    if(tok.size() < 3) {
        fail(raw);
    }
    parPropSpec<par_type> spec;
    const std::size_t index = toUnsigned(tok[0], raw);
    spec.lowerBoundary = convertBound(tok[1], raw);
    spec.upperBoundary = convertBound(tok[2], raw);
    spec.nSteps = GPP_DEF_NSTEPS;
    std::string label;
    for(std::size_t k = 3; k < tok.size(); ++k) {
        if(isUnsigned(tok[k])) {
            spec.nSteps = static_cast<std::size_t>(std::stoul(tok[k]));
        }
        else {
            label = tok[k];
        }
    }
    spec.var = NAMEANDIDTYPE(label.empty() ? 0 : 2, label, index);
    return spec;
}

} /* anonymous namespace */

/******************************************************************************/
/**
 * @brief The standard constructor -- stores the raw parameter-property string and parses it immediately.
 *
 * @param rw The raw parameter-property description string (see the grammar at the top of this file)
 */
GParameterPropertyParser::GParameterPropertyParser(const std::string &rw)
  : raw_(rw)
  , parsed_(false) {
    this->parse();
}

/******************************************************************************/
/**
 * @brief Retrieves the raw parameter description string that was supplied to the parser.
 *
 * @return The unparsed raw parameter-property string
 */
std::string GParameterPropertyParser::getRawParameterDescription() const {
    return raw_;
}

/******************************************************************************/
/**
 * @brief Allows to check whether parsing has already taken place.
 *
 * @return true if the raw string has been parsed, false otherwise
 */
bool GParameterPropertyParser::isParsed() const {
    return parsed_;
}

/******************************************************************************/
/**
 * @brief Resets the internal spec vectors and parses a new parameter-property string.
 *
 * Clears all previously parsed s/d/f/i/b specifications, replaces the raw string, marks
 * the parser as not-yet-parsed and re-runs parse().
 *
 * @param raw The new raw parameter-property description string to store and parse
 */
void GParameterPropertyParser::setNewParameterDescription(std::string raw) {
    raw_ = std::move(raw);

    s_spec_vec_.clear();
    d_spec_vec_.clear();
    f_spec_vec_.clear();
    i_spec_vec_.clear();
    b_spec_vec_.clear();

    parsed_ = false;

    // Update the information
    this->parse();
}

/******************************************************************************/
/**
 * @brief Initiates parsing of the raw_ string into the typed spec vectors.
 *
 * Tokenizes raw_ into type'('content')' fragments, dispatches each to the appropriate
 * numeric/bool/simple-scan handler and populates the corresponding spec vector. At most one
 * "simple scan" ('s') entry is allowed (more than one is a hard error); if a simple-scan entry
 * is present, any explicit d/f/i/b components are discarded with a warning. Does nothing if the
 * string has already been parsed. The grammar is documented at the top of this file.
 */
void GParameterPropertyParser::parse() {
    // Do nothing if the string has already been parsed
    if(parsed_) {
        return;
    }

    // Tokenize the raw string into (type, content) fragments, then apply each to its per-type spec vector.
    for(auto const &[type, content] : this->tokenizeRaw()) {
        this->applyFragment(type, content);
    }

    // We only accept a single "simple-scan" entry. Complain, if more than one was found.
    if(s_spec_vec_.size() > 1) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParameterPropertyParser::parse(): Error!" << '\n'
            << "Found " << s_spec_vec_.size() << " simple scan entries where a" << '\n'
            << "maximum of 1 is allowed" << '\n'
        );
    }
    if(s_spec_vec_.size() == 1) { // If we did find a "simple scan" entry, we will discard the other entries.
        if(not d_spec_vec_.empty() || not f_spec_vec_.empty() || not i_spec_vec_.empty() ||
                not b_spec_vec_.empty()) {
            glogger << "In GParameterPropertyParser::parse(): Warning!" << '\n'
                    << "You have specified both a simple-scan component and explicit" << '\n'
                    << "scan-components. The explicit components will be discarded." << '\n'
                    << GWARNING;

            d_spec_vec_.clear();
            f_spec_vec_.clear();
            i_spec_vec_.clear();
            b_spec_vec_.clear();
        }
    }

    // Prevent further use of this function
    parsed_ = true;
}

/******************************************************************************/
/**
 * @brief Tokenizes raw_ into (type, content) fragments of the form type'('content')', separated by
 * whitespace/commas. Throws (via fail()) on an unknown type char or a missing '('/')'.
 *
 * @return The parsed fragments in source order
 */
std::vector<std::pair<char, std::string>> GParameterPropertyParser::tokenizeRaw() const {
    std::vector<std::pair<char, std::string>> fragments;

    const std::string &s = raw_;
    std::size_t i = 0;
    const std::size_t n = s.size();
    auto skipSep = [&]() {
        while(i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == ',')) {
            ++i;
        }
    };
    skipSep();
    while(i < n) {
        const char type = s[i];
        if(type != 'd' && type != 'f' && type != 'i' && type != 'b' && type != 's') {
            fail(s.substr(i));
        }
        ++i;
        if(i >= n || s[i] != '(') {
            fail(s.substr(i));
        }
        ++i; // consume '('
        std::string content;
        while(i < n && s[i] != ')') {
            content.push_back(s[i]);
            ++i;
        }
        if(i >= n) { // no closing ')'
            fail(s);
        }
        ++i; // consume ')'
        fragments.emplace_back(type, content);
        skipSep();
    }

    return fragments;
}

/******************************************************************************/
/**
 * @brief Applies one parsed (type, content) fragment to the matching per-type spec vector.
 *
 * @param type The fragment's type character (one of d/f/i/b/s)
 * @param content The fragment's comma-separated content
 */
void GParameterPropertyParser::applyFragment(char type, std::string const &content) {
    const std::vector<std::string> tok = splitOnComma(content);

    if(type == 'd') {
        d_spec_vec_.push_back(makeNumericSpec<double>(
            tok,
            [](const std::string &t, const std::string &raw) { return t.empty() ? (fail(raw), 0.0) : std::stod(t); },
            content
        ));
    }
    else if(type == 'f') {
        f_spec_vec_.push_back(makeNumericSpec<float>(
            tok,
            [](const std::string &t, const std::string &raw) { return t.empty() ? (fail(raw), 0.0f) : std::stof(t); },
            content
        ));
    }
    else if(type == 'i') {
        i_spec_vec_.push_back(makeNumericSpec<std::int32_t>(
            tok,
            [](const std::string &t, const std::string &raw) { return t.empty() ? (fail(raw), static_cast<std::int32_t>(0)) : static_cast<std::int32_t>(std::stoi(t)); },
            content
        ));
    }
    else if(type == 'b') {
        applyBoolFragment(tok, content);
    }
    else if(type == 's') {
        if(tok.empty()) {
            fail(content);
        }
        simpleScanSpec spec{};
        spec.nItems = toUnsigned(tok[0], content);
        s_spec_vec_.push_back(spec);
    }
    else {
        fail(content);
    }
}

/******************************************************************************/
/**
 * @brief applyFragment() 'b' branch: parse a boolean parameter-scan spec.
 *
 * Layout: index[,lowerBoundary,upperBoundary][,nSteps][,label] -- the optional trailing tokens
 * (nSteps as an unsigned, everything else a label) may appear in either order.
 *
 * @param tok The comma-split tokens of the fragment content
 * @param content The original fragment content (for error messages)
 */
void GParameterPropertyParser::applyBoolFragment(
    std::vector<std::string> const &tok, std::string const &content
) {
    if(tok.empty() || tok[0].empty()) {
        fail(content);
    }
    parPropSpec<bool> spec;
    const std::size_t index = toUnsigned(tok[0], content);
    spec.lowerBoundary = false;
    spec.upperBoundary = true;
    spec.nSteps = GPP_DEF_NSTEPS;
    std::string label;
    if(tok.size() >= 3) {
        spec.lowerBoundary = toBool(tok[1], content);
        spec.upperBoundary = toBool(tok[2], content);
        for(std::size_t k = 3; k < tok.size(); ++k) {
            if(isUnsigned(tok[k])) {
                spec.nSteps = static_cast<std::size_t>(std::stoul(tok[k]));
            }
            else {
                label = tok[k];
            }
        }
    }
    else if(tok.size() == 2) {
        if(isUnsigned(tok[1])) {
            spec.nSteps = static_cast<std::size_t>(std::stoul(tok[1]));
        }
        else {
            label = tok[1];
        }
    }
    spec.var = NAMEANDIDTYPE(label.empty() ? 0 : 2, label, index);
    b_spec_vec_.push_back(spec);
}

/******************************************************************************/
/**
 * @brief Retrieve the number of "simple scan" items requested by the parsed string.
 *
 * In DEBUG builds this additionally guards against more than one simple-scan entry having
 * slipped through (which would be an internal inconsistency).
 *
 * @return The nItems of the (single) simple-scan entry, or 0 if no simple-scan entry was specified
 */
std::size_t GParameterPropertyParser::getNSimpleScanItems() const {
    if(s_spec_vec_.empty()) {
        return static_cast<std::size_t>(0);
    }
    // Return the data of the first item
#ifdef DEBUG
    if(s_spec_vec_.size() > 1) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParameterPropertyParser::getNSimpleScanItems() const: Error!" << '\n'
            << "Found " << s_spec_vec_.size() << " simple scan entries where a" << '\n'
            << "maximum of 1 is allowed" << '\n'
        );
    }
#endif

    return (s_spec_vec_.front()).nItems;
}

/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
