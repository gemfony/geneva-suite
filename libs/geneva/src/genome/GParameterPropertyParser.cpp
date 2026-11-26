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
#include "common/GChecks.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp" // parseWhole()
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
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
//   args (d/f/i) := index ',' lower ',' upper [ ',' nSteps ]
//   args (b)     := index [ ',' lower ',' upper ] [ ',' nSteps ]
//   args (s)     := nItems
// Parameters are addressed by 'index' (their position in the flat parameter vector of
// the corresponding type). A specification carries no name or label: a token where the
// step count stands that is not a step count is refused with a message naming it (a
// progress plot names its variables with setVariableNames()).
namespace {

/**
 * @brief Splits a string on commas, trimming whitespace from each resulting token.
 *
 * An empty input yields a single empty token; a trailing comma yields a trailing empty token.
 * Gem::Common::splitString() is not used: it drops empty fields, and a spec such as "d(0,,1)" must be
 * refused instead of being read as "d(0,1)". Trimming is common's (the same characters).
 *
 * @param s The string to split (the comma-separated argument list of a spec)
 * @return The list of trimmed tokens between commas, in order
 */
std::vector<std::string> splitOnComma(const std::string &s) {
    std::vector<std::string> out;
    std::string_view const sv{s};
    std::string_view::size_type start = 0;
    for(;;) {
        auto const pos = sv.find(',', start);
        out.push_back(Gem::Common::trimWhitespace(sv.substr(start, pos - start)));
        if(pos == std::string_view::npos) {
            return out;
        }
        start = pos + 1;
    }
}

/**
 * @brief Aborts parsing by throwing a gem_exception that reports the offending fragment.
 *
 * Marked [[noreturn]]: it always throws and never returns to the caller.
 *
 * @param raw The raw (sub)string that could not be parsed, included verbatim in the error message
 */
[[noreturn]] void fail(const std::string &raw) {
    throw gem_exception(
        g_error_streamer() << "Could not parse parameter-property fragment: " << raw << '\n'
    );
}

/**
 * @brief Reads a token as a whole as a value of par_type (Gem::Common::parseWhole()), failing the
 * parse otherwise: a remainder ("1.5x"), a number out of range or of another kind ("1.5" for an
 * integer) and an empty token are refused. A bool is "true", "false", "1" or "0".
 *
 * @tparam par_type The type of the value
 * @param s The token to convert
 * @param raw The enclosing raw fragment, forwarded to fail() for error reporting on failure
 * @return The value
 */
template <typename par_type>
par_type readToken(const std::string &s, const std::string &raw) {
    std::optional<par_type> value = Gem::Common::parseWhole<par_type>(s);
    if(not value) {
        fail(raw);
    }
    return *value;
}

/**
 * @brief Reads the optional step count of a spec: the tokens from @p first on. At most one
 * token may follow, and it must be an unsigned number; anything else -- a label, a name -- is
 * refused with a message that names it.
 *
 * @param tok The comma-split argument tokens of the spec
 * @param first The position of the step count in @p tok
 * @param raw The raw fragment content, for the error messages
 * @return The step count, or GPP_DEF_NSTEPS if none is given
 */
std::size_t
readNSteps(const std::vector<std::string> &tok, std::size_t first, const std::string &raw) {
    for(std::size_t k = first; k < tok.size(); ++k) {
        if(not Gem::Common::parseWhole<std::size_t>(tok[k])) {
            throw gem_exception(
                g_error_streamer()
                << "\"" << tok[k] << "\" in (" << raw << ") is not a step count." << '\n'
                << "A parameter-scan specification carries no label or name: a variable is"
                << " addressed by its index (a progress plot names its variables with"
                << " setVariableNames())." << '\n'
            );
        }
    }
    if(tok.size() > first + 1) {
        fail(raw);
    }
    return tok.size() == first + 1 ? readToken<std::size_t>(tok[first], raw) : GPP_DEF_NSTEPS;
}

/**
 * @brief Builds a parPropSpec for a numeric (d/f/i) parameter from its argument tokens.
 *
 * Fills the index, lower/upper boundaries and step count of the spec. The first three tokens
 * are the mandatory index, lower boundary and upper boundary; an optional fourth is the step
 * count (GPP_DEF_NSTEPS when none is given). See readNSteps() for what else is refused.
 *
 * @tparam par_type The parameter value type of the spec (double, float or std::int32_t)
 * @param tok The comma-split argument tokens of the spec (at least index, lower, upper)
 * @param raw The raw fragment content, forwarded to fail() for error reporting
 * @return A fully populated parPropSpec<par_type> describing the parameter scan
 */
template <typename par_type>
parPropSpec<par_type> makeNumericSpec(const std::vector<std::string> &tok, const std::string &raw) {
    if(tok.size() < 3) {
        fail(raw);
    }
    parPropSpec<par_type> spec;
    spec.index = readToken<std::size_t>(tok[0], raw);
    spec.lowerBoundary = readToken<par_type>(tok[1], raw);
    spec.upperBoundary = readToken<par_type>(tok[2], raw);
    spec.nSteps = readNSteps(tok, 3, raw);
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
        throw gem_exception(
            g_error_streamer() << "Found " << s_spec_vec_.size() << " simple scan entries where a"
                               << '\n'
                               << "maximum of 1 is allowed" << '\n'
        );
    }
    if(s_spec_vec_.size() ==
       1) { // If we did find a "simple scan" entry, we will discard the other entries.
        if(not d_spec_vec_.empty() || not f_spec_vec_.empty() || not i_spec_vec_.empty() ||
           not b_spec_vec_.empty()) {
            glogger << "You have specified both a simple-scan component and explicit" << '\n'
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
        while(i < n &&
              (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == ',')) {
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
        d_spec_vec_.push_back(makeNumericSpec<double>(tok, content));
    }
    else if(type == 'f') {
        f_spec_vec_.push_back(makeNumericSpec<float>(tok, content));
    }
    else if(type == 'i') {
        i_spec_vec_.push_back(makeNumericSpec<std::int32_t>(tok, content));
    }
    else if(type == 'b') {
        applyBoolFragment(tok, content);
    }
    else if(type == 's') {
        if(tok.empty()) {
            fail(content);
        }
        simpleScanSpec spec{};
        spec.nItems = readToken<std::size_t>(tok[0], content);
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
 * Layout: index[,lowerBoundary,upperBoundary][,nSteps]. nSteps (default GPP_DEF_NSTEPS) is the
 * number of draws of a random scan; a grid scan visits the two boolean values whatever it says.
 * See readNSteps() for what else is refused.
 *
 * @param tok The comma-split tokens of the fragment content
 * @param content The original fragment content (for error messages)
 */
void GParameterPropertyParser::applyBoolFragment(
    std::vector<std::string> const &tok,
    std::string const &content
) {
    if(tok.empty() || tok[0].empty()) {
        fail(content);
    }
    parPropSpec<bool> spec;
    spec.index = readToken<std::size_t>(tok[0], content);
    spec.lowerBoundary = false;
    spec.upperBoundary = true;
    std::size_t n_steps_at = 1; // b(index[, nSteps])
    if(tok.size() >= 3) {       // b(index, lower, upper[, nSteps])
        spec.lowerBoundary = readToken<bool>(tok[1], content);
        spec.upperBoundary = readToken<bool>(tok[2], content);
        n_steps_at = 3;
    }
    spec.nSteps = readNSteps(tok, n_steps_at, content);
    b_spec_vec_.push_back(spec);
}

/******************************************************************************/
/**
 * @brief Retrieve the number of "simple scan" items requested by the parsed string.
 *
 * This additionally guards against more than one simple-scan entry having
 * slipped through (which would be an internal inconsistency).
 *
 * @return The nItems of the (single) simple-scan entry, or 0 if no simple-scan entry was specified
 */
std::size_t GParameterPropertyParser::getNSimpleScanItems() const {
    if(s_spec_vec_.empty()) {
        return static_cast<std::size_t>(0);
    }
    // Return the data of the first item
    Gem::Common::checkThat(
        s_spec_vec_.size() <= 1,
        "Found ",
        s_spec_vec_.size(),
        " simple scan entries where at most 1 is allowed"
    );

    return (s_spec_vec_.front()).nItems;
}

/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
