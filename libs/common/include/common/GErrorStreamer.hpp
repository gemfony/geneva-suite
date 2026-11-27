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

// Standard header files go here
#include <ios>
#include <source_location>
#include <sstream>
#include <string>

// Boost header files go here

// Geneva header files go here

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief Converts the current time to a string.
 */
std::string currentTimeAsString();

/******************************************************************************/
/**
 * Returns a "Recorded on <time> / in File <file> at line <line> (<function>)"
 * string describing the call site via C++20 std::source_location. The defaulted
 * source_location captures the caller. g_error_streamer puts it in front of every
 * error message, so a message names its place without a hand-typed prefix.
 *
 * @param loc The call-site source location (defaulted to std::source_location::current(), i.e. the caller)
 * @return A string describing the current time and the call site (file, line, function)
 */
[[nodiscard]] inline std::string
timeAndPlace(std::source_location const &loc = std::source_location::current()) {
    return std::string("Recorded on ") + currentTimeAsString() + "\n" + "in File " +
           loc.file_name() + " at line " + std::to_string(loc.line()) + " (" + loc.function_name() +
           ") :\n";
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The one way to raise an error in the collection:
 *
 *     throw gem_exception(g_error_streamer() << "What went wrong" << '\n');
 *
 * A simple wrapper around a std::ostringstream that records where it was made. Its conversion
 * to a string, done when the exception is built, puts the time and the place of the throw
 * (timeAndPlace(): file, line and function) in front of the streamed message. Nothing is
 * logged here: an error is logged where it is finally handled -- the fatal report for an
 * error that ends the program (common/GFatalReport.hpp), GEM_ERROR (common/GLogger.hpp) where an
 * error ends a task -- so an error that a caller catches and handles prints nothing.
 */
class g_error_streamer {
public:
    /**************************************************************************/
    /**
	  * @brief Constructs the streamer and records the time and place of the call.
	  *
	  * @param loc The place of the throw (defaulted to the caller's location)
	  */
    //! [coding-empty-body]
    explicit g_error_streamer(std::source_location const &loc = std::source_location::current())
      : where_and_when_(Gem::Common::timeAndPlace(loc)) { /* nothing */
    }
    //! [coding-empty-body]

    /*************************************************************************/
    // Deleted or defaulted constructors, destructor and assignment operators

    g_error_streamer(const g_error_streamer &) = delete;
    g_error_streamer &operator=(const g_error_streamer &) = delete;
    g_error_streamer(g_error_streamer &&) = delete;
    g_error_streamer &operator=(g_error_streamer &&) = delete;

    ~g_error_streamer() = default;

    /**************************************************************************/
    /**
	  * @brief Streams virtually any streamable value into this object.
	  *
	  * This function allows us to stream virtually any type of streamable data
	  * to this class.
	  *
	  * @tparam value_type The type of the value streamed into the class
	  * @param val The value streamed into this class
	  * @return A reference to this object (to allow chaining)
	  */
    template <typename value_type>
    g_error_streamer &operator<<(const value_type &val) {
        ostream_ << val;
        return *this;
    }

    /******************************************************************************/
    /**
	  * @brief Streams a std::ostream manipulator (e.g. std::endl) into this object.
	  *
	  * Needed for stringstream.
	  *
	  * @param val A std::ostream manipulator function to be applied to the wrapped stream
	  * @return A reference to this object (to allow chaining)
	  */
    g_error_streamer &operator<<(std::ostream &(*val)(std::ostream &)) {
        ostream_ << val;
        return *this;
    }

    /******************************************************************************/
    /**
	  * @brief Streams a std::ios manipulator into this object.
	  *
	  * Needed for stringstream.
	  *
	  * @param val A std::ios manipulator function to be applied to the wrapped stream
	  * @return A reference to this object (to allow chaining)
	  */
    g_error_streamer &operator<<(std::ios &(*val)(std::ios &)) {
        ostream_ << val;
        return *this;
    }

    /******************************************************************************/
    /**
	  * @brief Streams a std::ios_base manipulator into this object.
	  *
	  * Needed for stringstream.
	  *
	  * @param val A std::ios_base manipulator function to be applied to the wrapped stream
	  * @return A reference to this object (to allow chaining)
	  */
    g_error_streamer &operator<<(std::ios_base &(*val)(std::ios_base &)) {
        ostream_ << val;
        return *this;
    }

    /**************************************************************************/
    /**
	  * @brief Returns the streamed message without the time and place.
	  *
	  * @return A copy of the currently accumulated message string
	  */
    [[nodiscard]] std::string content() const {
        return ostream_.str();
    }

    /**************************************************************************/
    /**
	  * @brief The message of the exception: the time and place of the throw, then the streamed
	  * message.
	  *
	  * @return The time and place of the throw followed by the streamed message
	  */
    operator std::string() const { // NOLINT -- implicit on purpose: gem_exception is built from it
        return where_and_when_ + ostream_.str();
    }

private:
    /**************************************************************************/
    // Data
    std::ostringstream ostream_;
    std::string where_and_when_; ///< The time and place of the throw (timeAndPlace())

    /**************************************************************************/
};

/******************************************************************************/

} /* namespace Gem::Common */

/******************************************************************************/
