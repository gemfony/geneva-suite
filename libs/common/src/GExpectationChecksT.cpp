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

#include "common/GExpectationChecksT.hpp"
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include <cstddef>
#include <ostream>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

namespace Gem::Common {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
// Identifies test counter and success counter
constexpr std::size_t TESTCOUNTER = 0;
constexpr std::size_t SUCCESSCOUNTER = 1;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The standard constructor -- initialization with class name and expectation
 *
 * @param caller The name of the class / function performing the comparison (used in messages)
 * @param e The expectation (equality, similarity or inequality) the comparisons should fulfil
 */
GToken::GToken(std::string caller, Gem::Common::expectation e)
  : test_counter_(std::make_tuple(0uz, 0uz))
  , caller_(std::move(caller))
  , e_(e) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Increments the counter of performed tests
 */
void GToken::incrTestCounter() {
    std::get<TESTCOUNTER>(test_counter_) += 1;
}

/******************************************************************************/
/**
 * @brief Increments the counter of tests that met the expectation
 */
void GToken::incrSuccessCounter() {
    std::get<SUCCESSCOUNTER>(test_counter_) += 1;
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the current state of the success counter
 *
 * @return The number of tests that have met the expectation so far
 */
std::size_t GToken::getSuccessCounter() const {
    return std::get<SUCCESSCOUNTER>(test_counter_);
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the current state of the test counter
 *
 * @return The number of tests that have been performed so far
 */
std::size_t GToken::getTestCounter() const {
    return std::get<TESTCOUNTER>(test_counter_);
}

/******************************************************************************/
/**
 * @brief Allows to check whether the expectation was met
 *
 * @return true if the registered expectation has been satisfied, false otherwise
 */
bool GToken::expectationMet() const {
    switch(e_) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY:
        if(std::get<TESTCOUNTER>(test_counter_) == std::get<SUCCESSCOUNTER>(test_counter_)) {
            return true;
        }
        break;

    case Gem::Common::expectation::INEQUALITY:
        if(std::get<SUCCESSCOUNTER>(test_counter_) > 0) {
            return true;
        }
        break;
    }

    return false;
}

/******************************************************************************/
/**
 * @brief Conversion to a boolean indicating whether the expectation was met
 *
 * @return true if the registered expectation has been satisfied, false otherwise
 */
GToken::operator bool() const {
    return this->expectationMet();
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the expectation token
 *
 * @return The expectation (equality, similarity or inequality) this token enforces
 */
Gem::Common::expectation GToken::getExpectation() const {
    return e_;
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the expectation token as a string
 *
 * @return The expectation as a string ("FP_SIMILARITY", "EQUALITY", "INEQUALITY", or "unknown")
 */
std::string GToken::getExpectationStr() const {
    switch(e_) {
    case Gem::Common::expectation::FP_SIMILARITY:
        return "FP_SIMILARITY";
        break;

    case Gem::Common::expectation::EQUALITY:
        return "EQUALITY";
        break;

    case Gem::Common::expectation::INEQUALITY:
        return "INEQUALITY";
        break;
    default:
        return "unknown";
        break;
    }
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the name of the caller
 *
 * @return The name of the class / function that created this token
 */
std::string GToken::getCallerName() const {
    return caller_;
}

/******************************************************************************/
/**
 * @brief Allows to register an error message e.g. obtained from a failed check
 *
 * @param m The (non-empty) error message to store; an empty message triggers an exception
 */
void GToken::registerErrorMessage(std::string const &m) {
    if(not m.empty()) {
        error_messages_.push_back(m);
    }
    else {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GToken::registerErrorMessage(): Error" << '\n'
            << "Tried to register empty error message" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief Allows to register an exception obtained from a failed check
 *
 * @param g The expectation-violation exception whose description should be stored
 */
void GToken::registerErrorMessage(g_expectation_violation const &g) {
    error_messages_.emplace_back(g.what());
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the currently registered error messages
 *
 * @return The concatenation of all registered error messages, prefixed with a header line
 */
std::string GToken::getErrorMessages() const {
    std::string result; // NOLINT(cppcoreguidelines-init-variables)
    result = "Registered errors:\n";
    for(auto const &error : error_messages_) {
        result += error;
    }
    return result;
}

/******************************************************************************/
/**
 * @brief Conversion to a string indicating success or failure
 *
 * @return A human-readable description of whether the expectation was met, plus any failure details
 */
std::string GToken::toString() const {
    std::string result = "Expectation of ";

    switch(e_) {
    case Gem::Common::expectation::FP_SIMILARITY: {
        result += std::string("FP_SIMILARITY was ");
    } break;

    case Gem::Common::expectation::EQUALITY: {
        result += std::string("EQUALITY was ");
    } break;

    case Gem::Common::expectation::INEQUALITY: {
        result += std::string("INEQUALITY was ");
    } break;
    }

    if(this->expectationMet()) {
        result += std::string("met in ") + caller_ + std::string("\n");
    }
    else {
        result += std::string("not met in ") + caller_ + std::string("\n");
        // We only add specific information about failed checks for the expectation
        // "inequality", so we do not swamp the user with useless information.
        // For the inequality information, just one out of many checks for data-
        // inequality must be met, so the "equal" comparisons are of no concern.
        // Only the information "everything is equal while inequality was expected"
        // is important. If equality or similarity were expected, every single
        // deviation from equality is of interest.
        if(Gem::Common::expectation::INEQUALITY != e_) {
            for(auto const &error : error_messages_) {
                result += error;
            }
        }
    }

    return result;
}

/******************************************************************************/
/**
 * @brief Evaluates the information in this object, throwing if the expectation was not met
 */
void GToken::evaluate() const {
    if(not this->expectationMet()) {
        throw(g_expectation_violation(this->toString()));
    }
}

/******************************************************************************/
/**
 * @brief Easy output of GToken objects
 *
 * @param s The output stream to write to
 * @param g The token whose state should be streamed
 * @return A reference to the output stream, to allow chaining
 */
std::ostream &operator<<(std::ostream &s, GToken const &g) {
    s << "GToken for caller " << g.getCallerName() << " with expectation  " << g.getExpectationStr()
      << ":" << '\n'
      << "Test counter:     " << g.getTestCounter() << '\n'
      << "Success counter:  " << g.getSuccessCounter() << '\n'
      << g.getErrorMessages() << '\n';
    return s;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief This function checks whether two objects of type Gem::Common::tribool meet a given expectation.
 *
 * @param x The first tribool to be compared
 * @param y The second tribool to be compared
 * @param x_name The name of the first parameter (used in the violation message)
 * @param y_name The name of the second parameter (used in the violation message)
 * @param e The expectation both parameters need to fulfill (equality / similarity / inequality)
 * @param limit The (unused) floating-point comparison limit; present for interface uniformity with numeric overloads
 */
void compare(
    Gem::Common::tribool const &x,
    Gem::Common::tribool const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    [[maybe_unused]] double limit
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY:
        expectation_str = "FP_SIMILARITY / EQUALITY";
        if((x == Gem::Common::tribool::True  && y == Gem::Common::tribool::True) ||
           (x == Gem::Common::tribool::False && y == Gem::Common::tribool::False) ||
           (x == Gem::Common::tribool::Indeterminate && y == Gem::Common::tribool::Indeterminate)) {
            expectation_met = true;
        }
        break;

    case Gem::Common::expectation::INEQUALITY:
        expectation_str = "INEQUALITY";
        if((x != Gem::Common::tribool::True  || y != Gem::Common::tribool::True) &&
           (x != Gem::Common::tribool::False || y != Gem::Common::tribool::False) &&
           (x != Gem::Common::tribool::Indeterminate || y != Gem::Common::tribool::Indeterminate)) {
            expectation_met = true;
        }
        break;
    };

    if(not expectation_met) {
        std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)
        error << "Expectation of " << expectation_str << " was violated for parameters "
              << '\n'
              << "[" << '\n'
              << x_name << " = " << x << '\n'
              << y_name << " = " << y << '\n'
              << "]" << '\n';
        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common */
