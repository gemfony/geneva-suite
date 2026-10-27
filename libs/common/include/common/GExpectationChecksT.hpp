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
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <type_traits>
#include <typeinfo>
#include <vector>

// Boost headers go here

// Gemfony headers go here
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GMemberReflectionT.hpp"
#include "common/GTypeTraitsT.hpp"

namespace Gem::Common {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A token to be handed to different comparators, so they can signal the violation
 * of expectations
 */
class GToken // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
    /**
     * @brief The standard constructor -- initialization with class name and expectation.
     *
     * @param caller The name of the calling class, recorded for use in diagnostic messages
     * @param e The expectation (EQUALITY / FP_SIMILARITY / INEQUALITY) all checks fed to this token must fulfill
     */
    GToken(std::string caller, Gem::Common::expectation e);

    /*************************************************************************/
    // Defaulted or deleted constructors, destructor and assignment operators
    // We enforce the usage of a signle constructor and prevent any assignment.

    GToken() = delete;
    GToken(GToken const &) = delete;
    GToken(GToken &&) = delete;

    GToken &operator=(GToken const &) = delete;
    GToken &operator=(GToken &&) = delete;

    /*************************************************************************/

    /** @brief Increments the test counter */
    void incrTestCounter();
    /** @brief Increments the counter of tests that met the expectation */
    void incrSuccessCounter();

    /**
     * @brief Allows to retrieve the current state of the success counter.
     * @return The number of tests that have met the expectation so far
     */
    [[nodiscard]] std::size_t getSuccessCounter() const;
    /**
     * @brief Allows to retrieve the current state of the test counter.
     * @return The total number of tests performed so far
     */
    [[nodiscard]] std::size_t getTestCounter() const;

    /**
     * @brief Allows to check whether the expectation was met.
     * @return true if all recorded tests met the expectation, false otherwise
     */
    [[nodiscard]] bool expectationMet() const;
    /**
     * @brief Conversion to a boolean indicating whether the expectation was met.
     * @return true if all recorded tests met the expectation, false otherwise
     */
    operator bool() const; // NOLINT

    /**
     * @brief Allows to retrieve the expectation token.
     * @return The expectation (EQUALITY / FP_SIMILARITY / INEQUALITY) this token enforces
     */
    [[nodiscard]] Gem::Common::expectation getExpectation() const;
    /**
     * @brief Allows to retrieve the expectation token as a string.
     * @return A human-readable string representation of the enforced expectation
     */
    [[nodiscard]] std::string getExpectationStr() const;
    /**
     * @brief Allows to retrieve the name of the caller.
     * @return The name of the calling class passed to the constructor
     */
    [[nodiscard]] std::string getCallerName() const;

    /**
     * @brief Allows to register an error message e.g. obtained from a failed check.
     * @param error_message The error message to append to this token's collection
     */
    void registerErrorMessage(std::string const &m);
    /**
     * @brief Allows to register an exception obtained from a failed check.
     * @param g The expectation-violation exception whose message is appended to this token's collection
     */
    void registerErrorMessage(g_expectation_violation const &g);

    /**
     * @brief Allows to retrieve the currently registered error messages.
     * @return A concatenation of all error messages recorded for failed checks
     */
    [[nodiscard]] std::string getErrorMessages() const;

    /**
     * @brief Conversion to a string indicating success or failure.
     * @return A human-readable summary of this token's success / failure state
     */
    [[nodiscard]] std::string toString() const;

    /** @brief Evaluates the information in this object */
    void evaluate() const;

private:
    /** @brief Counts all tests vs. tests that have met the expectation */
    std::tuple<std::size_t, std::size_t> test_counter_;
    /** @brief Error messages obtained from failed checks */
    std::vector<std::string> error_messages_;

    /** @brief The name of the calling class */
    const std::string caller_;
    /** @brief The expectation to be met */
    const Gem::Common::expectation e_ = Gem::Common::expectation::INEQUALITY;
};

/******************************************************************************/
/**
 * @brief This function facilitates the output of GToken objects, mostly for debugging purposes.
 *
 * @param s The output stream the token is written to
 * @param g The GToken object to be streamed
 * @return A reference to the (modified) output stream, for chaining
 */
std::ostream &operator<<(std::ostream &s, GToken const &g);

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

#define BASENAME(B) std::string(#B)

/**
 * This struct facilitates transfer of comparable items to comparators
 *
 * @tparam T The type of the two items (held by const reference) to be compared
 */
template <typename T>
struct identity // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
    /***************************************************************************/
    /**
     * @brief The standard constructor.
     *
     * @param x_var The first item to be compared (held by const reference)
     * @param y_var The second item to be compared (held by const reference)
     * @param x_name_var A human-readable name for the first item (moved into the struct)
     * @param y_name_var A human-readable name for the second item (moved into the struct)
     * @param l_var The maximum allowed deviation for "similar" floating point values
     */
    identity(
        T const &x_var,
        T const &y_var,
        std::string x_name_var,
        std::string y_name_var,
        double l_var
    )
      : x(x_var)
      , y(y_var)
      , x_name(std::move(x_name_var))
      , y_name(std::move(y_name_var))
      , limit(l_var) { /* nothing */
    }

    /***************************************************************************/
    // Deleted and defaulted functions / rule of five

    identity() = delete;
    identity(identity const &) = default;
    identity(identity &&) = default;

    // identity holds reference and const members, so the assignment operators
    // are implicitly deleted by the language. Declare the deletion explicitly
    // to surface that fact (a previous `= default` was misleading: defaulted
    // assignment on such a class is implicitly deleted, not user-provided).
    identity &operator=(identity const &) = delete;
    identity &operator=(identity &&) = delete;

    /***************************************************************************/
    /**
     * Explicit conversion to a base-class identity. Mark explicit so the
     * conversion only fires where intentionally requested (`identity<Base>{id}`
     * or `static_cast<identity<Base>>(id)`); implicit conversion would
     * widen the set of overload-resolution paths in surprising ways and
     * is rarely what the caller wants.
     *
     * @tparam base_type A base class of T that the held items are re-viewed as
     * @return An identity object whose items are the base-class views of this object's items
     */
    template <typename base_type>
    explicit operator identity<base_type>() const {
        // We use an internal function for the actual conversion
        // so we may check whether base_type is an actual base of T
        return to<base_type>();
    }

    /***************************************************************************/
    // The actual data
    const T &x;
    const T &y;
    const std::string x_name;
    const std::string y_name;
    const double limit;

private:
    /***************************************************************************/
    /**
     * @brief Does the actual conversion, including a check that base_type is indeed a base of T.
     *
     * @tparam base_type A base class of T (enforced via a requires-clause) the items are cast to
     * @return An identity object whose items are dynamic_cast base-class references of this object's items,
     *         with the names prefixed by the base-type name and the same limit retained
     */
    template <typename base_type>
        requires std::is_base_of_v<base_type, T>
    identity<base_type>
    to() const {
        auto const &x_conv = dynamic_cast<base_type const &>(x);
        auto const &y_conv = dynamic_cast<base_type const &>(y);

        const std::string x_name_conv = "(" + BASENAME(base_type) + ")" + x_name;
        const std::string y_name_conv = "(" + BASENAME(base_type) + ")" + y_name;

        return identity<base_type>(x_conv, y_conv, x_name_conv, y_name_conv, limit);
    }
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Easy output of an identity object.
 *
 * @tparam T The type of the items held by the identity object
 * @param s The output stream the identity object is written to
 * @param i The identity object whose item names are streamed
 * @return A reference to the (modified) output stream, for chaining
 */
template <typename T>
std::ostream &operator<<(std::ostream &s, identity<T> const &i) {
    s << "Identity:" << '\n'
      << "x_name = " << i.x_name << '\n'
      << "y_name = " << i.y_name << '\n';
    return s;
}

/******************************************************************************/
/**
 * @brief Returns an identity object. The function is needed as automatic type
 * deduction does not work for structs / classes. We assume a central default
 * value for the maximum allowed difference for "similar" floating point values.
 *
 * @tparam T The type of the two items to be compared
 * @param x_var The first item to be compared
 * @param y_var The second item to be compared
 * @param x_name_var A human-readable name for the first item
 * @param y_name_var A human-readable name for the second item
 * @return An identity object wrapping the two items, their names and the default similarity difference
 */
template <typename T>
identity<T> getIdentity(
    T const &x_var,
    T const &y_var,
    std::string const &x_name_var,
    std::string const &y_name_var
) {
    return identity<T>(
        x_var,
        y_var,
        x_name_var,
        y_name_var,
        Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
    );
}

/******************************************************************************/
/**
 * This macro facilitates the creation of an identity object including
 * variable names
 */
#define IDENTITY(x, y) Gem::Common::getIdentity((x), (y), std::string(#x), std::string(#y))

/******************************************************************************/
/**
 * @brief Returns an identity object for base types of T.
 *
 * @tparam T The concrete type of the two items being compared
 * @tparam base_type The base class of T the items are dynamic_cast to before being wrapped
 * @param x_var The first item to be compared (cast to base_type const &)
 * @param y_var The second item to be compared (cast to base_type const &)
 * @param x_name_var A human-readable name for the first item
 * @param y_name_var A human-readable name for the second item
 * @return An identity object holding the base-class views of the two items and the default similarity difference
 */
template <typename T, typename base_type>
identity<base_type> getBaseIdentity(
    T const &x_var,
    T const &y_var,
    std::string const &x_name_var,
    std::string const &y_name_var
) {
    auto const &x_var_base = dynamic_cast<const base_type &>(x_var);
    auto const &y_var_base = dynamic_cast<const base_type &>(y_var);

    return identity<base_type>(
        x_var_base,
        y_var_base,
        x_name_var,
        y_name_var,
        Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
    );
}

/******************************************************************************/
/**
 * This macro helps to cast an object to its parent class before creating the identity object
 */
#define IDENTITY_CAST(t, x, y)                                                                     \
    Gem::Common::getBaseIdentity<decltype(x), t>(                                                  \
        (x),                                                                                       \
        (y),                                                                                       \
        std::string("(const " #t "&)" #x),                                                         \
        std::string("(const " #t "&)" #y)                                                          \
    )

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This function checks whether two "basic" types fulfill a given expectation.
 * It assumes that x and y understand the == and != operators and may be streamed.
 * If they do not fulfill this requirement, you need to provide a specialization
 * of these functions. A check for similarity is treated the same as a check for
 * equality. A specialization of this function is provided for floating point values.
 * The function will throw a g_expectation_violation exception if the expectation
 * was violated.
 *
 * @tparam basic_type The (non-floating-point, non-Gemfony) type of the items being compared
 * @param x The first parameter to be compared
 * @param y The second parameter to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 *
 * @note The trailing double `limit` argument is an unused tolerance value,
 *       present only to match the common compare() signature for non-FP types.
 */
template <typename basic_type>
    requires (!std::is_floating_point_v<basic_type> &&
              !Gem::Common::has_gemfony_common_interface<basic_type>::value)
void compare(
    basic_type const &x,
    basic_type const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    [[maybe_unused]] double limit = 0.
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY:
        expectation_str = "FP_SIMILARITY / EQUALITY";
        if(x == y) {
            expectation_met = true;
        }
        break;

    case Gem::Common::expectation::INEQUALITY:
        expectation_str = "INEQUALITY";
        if(x != y) {
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
/**
 * This function checks whether two std::chrono::time_point types fulfill a given
 * expectation. A check for similarity is treated the same as a check for
 * equality. The function will throw a g_expectation_violation exception if the expectation
 * was violated.
 *
 * @tparam Clock The clock type of the compared time points
 * @tparam Duration The duration type of the compared time points (defaults to Clock::duration)
 * @param x The first parameter to be compared
 * @param y The second parameter to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 *
 * @note The trailing double `limit` argument is an unused tolerance value,
 *       present only to match the common compare() signature.
 */
template <typename Clock, typename Duration = typename Clock::duration>
void compare(
    std::chrono::time_point<Clock, Duration> const &x,
    std::chrono::time_point<Clock, Duration> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    [[maybe_unused]] double limit = 0.
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY:
        expectation_str = "FP_SIMILARITY / EQUALITY";
        if(x == y) {
            expectation_met = true;
        }
        break;

    case Gem::Common::expectation::INEQUALITY:
        expectation_str = "INEQUALITY";
        if(x != y) {
            expectation_met = true;
        }
        break;
    };

    if(not expectation_met) {
        std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)
        error << "Expectation of " << expectation_str << " was violated for parameters "
              << '\n'
              << "[" << '\n'
              << x_name << " = " << x.time_since_epoch().count() << '\n'
              << y_name << " = " << y.time_since_epoch().count() << '\n'
              << "]" << '\n';
        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This function checks whether two std::chrono::duration types fulfill a given
 * expectation. A check for similarity is treated the same as a check for
 * equality. The function will throw a g_expectation_violation exception if the expectation
 * was violated.
 *
 * @tparam Rep The arithmetic representation type of the compared durations
 * @tparam Period The std::ratio tick period of the compared durations (defaults to std::ratio<1>)
 * @param x The first parameter to be compared
 * @param y The second parameter to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 *
 * @note The trailing double `limit` argument is an unused tolerance value,
 *       present only to match the common compare() signature.
 */
template <typename Rep, typename Period = std::ratio<1>>
void compare(
    std::chrono::duration<Rep, Period> const &x,
    std::chrono::duration<Rep, Period> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    [[maybe_unused]] double limit = 0.
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY:
        expectation_str = "FP_SIMILARITY / EQUALITY";
        if(x == y) {
            expectation_met = true;
        }
        break;

    case Gem::Common::expectation::INEQUALITY:
        expectation_str = "INEQUALITY";
        if(x != y) {
            expectation_met = true;
        }
        break;
    };

    if(not expectation_met) {
        std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)
        error << "Expectation of " << expectation_str << " was violated for parameters "
              << '\n'
              << "[" << '\n'
              << x_name << " = " << x.count() << '\n'
              << y_name << " = " << y.count() << '\n'
              << "]" << '\n';
        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
/**
 * This function checks whether two floating point types meet a given expectation.
 * The function will throw a g_expectation_violation exception if the expectation
 * was violated.
 *
 * @tparam fp_type The floating point type of the two values being compared
 * @param x The first parameter to be compared
 * @param y The second parameter to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed deviation of two floating point values (only used for FP_SIMILARITY)
 */
template <std::floating_point fp_type>
void compare(
    fp_type const &x,
    fp_type const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = CE_DEF_SIMILARITY_DIFFERENCE
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
        expectation_str = "FP_SIMILARITY";
        if(std::abs(x - y) < Gem::Common::narrow<fp_type>(limit)) {
            expectation_met = true;
        }
        break;
    case Gem::Common::expectation::EQUALITY:
        expectation_str = "EQUALITY";
        if(x == y) {
            expectation_met = true;
        }
        break;
    case Gem::Common::expectation::INEQUALITY:
        expectation_str = "INEQUALITY";
        if(x != y) {
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
/**
 * This function checks whether two containers of "basic" types meet a given expectation. It assumes that
 * these types understand the == and != operators. If they do not fulfill this requirement, you need to provide
 * a specialization of this function. A check for similarity is treated the same as a check for equality.
 * A specialization of this function is provided for floating point values. The function will throw a
 * g_expectation_violation exception if the expectation was violated.
 *
 * @tparam base_type The (non-floating-point) element type held by the containers
 * @tparam c_type The sequence container template (e.g. std::vector, std::deque) parameterised by element and allocator
 * @param x The first container to be compared
 * @param y The second container to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 *
 * @note The trailing double `limit` argument is an unused tolerance value,
 *       present only to match the common compare() signature for non-FP element types.
 */
template <typename base_type, template <typename, typename> class c_type>
    requires (!std::is_floating_point_v<base_type>)
// NOLINTNEXTLINE(readability-function-size) -- one coherent expectation-comparison sweep (switch-on-expectation + per-element diff reporting) for non-FP sequence containers; splitting would scatter the tightly-coupled error-message assembly
void compare(
    c_type<base_type, std::allocator<base_type>> const &x,
    c_type<base_type, std::allocator<base_type>> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    [[maybe_unused]] double limit = 0.
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY:
        expectation_str = "FP_SIMILARITY / EQUALITY";
        if(x == y) {
            expectation_met = true;
        }
        break;

    case Gem::Common::expectation::INEQUALITY:
        expectation_str = "INEQUALITY";
        if(x != y) {
            expectation_met = true;
        }
        break;
    };

    if(not expectation_met) {
        std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)
        error << "Expectation of " << expectation_str << " was violated for parameters " << x_name
              << " and " << y_name << "!" << '\n';

        if(Gem::Common::expectation::FP_SIMILARITY == e ||
           Gem::Common::expectation::EQUALITY == e) {
            if(x.size() != y.size()) {
                error << "Sizes of containers differ:" << '\n'
                      << x_name << ".size() == " << x.size() << " / " << y_name
                      << ".size() == " << y.size() << '\n';
            }
            else { // Some data member differs
                // Find out about the first entry that differs
                typename c_type<base_type, std::allocator<base_type>>::const_iterator x_it;
                typename c_type<base_type, std::allocator<base_type>>::const_iterator y_it;
                std::size_t failed_index = 0;
                for(x_it = x.begin(), y_it = y.begin(); x_it != x.end();
                    ++x_it, ++y_it, ++failed_index) {
                    if(*x_it != *y_it) {
                        error << "Found inequality at index " << failed_index << ": " << x_name
                              << "[" << failed_index << "] = " << *x_it << "; " << y_name << "["
                              << failed_index << "] = " << *y_it;
                        break; // break the loop
                    }
                }
            }
        }
        else { // Gem::Common::expectation::INEQUALITY == e
            error << "The two containers " << x_name << " and " << y_name << " are equal "
                  << "even though differences were expected" << '\n';
        }

        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
/**
 * This function checks whether two containers with a std::set-template interface holding "basic" types meet
 * a given expectation. It assumes that these types understand the == and != operators. If they do not fulfill
 * this requirement, you need to provide a specialization of this function. A check for similarity is treated
 * the same as a check for equality. A specialization of this function is provided for floating point values.
 * The function will throw a g_expectation_violation exception if the expectation was violated.
 *
 * @tparam base_type The (non-floating-point) element type held by the set-like containers
 * @tparam s_type The set container template (e.g. std::set) parameterised by element, comparator and allocator
 * @param x The first container to be compared
 * @param y The second container to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 *
 * @note The trailing double `limit` argument is an unused tolerance value,
 *       present only to match the common compare() signature for non-FP element types.
 */
template <typename base_type, template <typename, typename, typename> class s_type>
    requires (!std::is_floating_point_v<base_type>)
// NOLINTNEXTLINE(readability-function-size) -- one coherent expectation-comparison sweep (switch-on-expectation + per-element diff reporting) for non-FP set-like containers; splitting would scatter the tightly-coupled error-message assembly
void compare(
    s_type<base_type, std::less<base_type>, std::allocator<base_type>> const &x,
    s_type<base_type, std::less<base_type>, std::allocator<base_type>> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    [[maybe_unused]] double limit = 0.
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY:
        expectation_str = "FP_SIMILARITY / EQUALITY";
        if(x == y) {
            expectation_met = true;
        }
        break;

    case Gem::Common::expectation::INEQUALITY:
        expectation_str = "INEQUALITY";
        if(x != y) {
            expectation_met = true;
        }
        break;
    };

    if(not expectation_met) {
        std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)
        error << "Expectation of " << expectation_str << " was violated for parameters " << x_name
              << " and " << y_name << "!" << '\n';

        if(Gem::Common::expectation::FP_SIMILARITY == e ||
           Gem::Common::expectation::EQUALITY == e) {
            if(x.size() != y.size()) {
                error << "Sizes of containers differ:" << '\n'
                      << x_name << ".size() == " << x.size() << " / " << y_name
                      << ".size() == " << y.size() << '\n';
            }
            else { // Some data member differs
                // Find out about the first entry that differs
                typename s_type<base_type, std::less<base_type>, std::allocator<base_type>>::
                    const_iterator x_it;
                typename s_type<base_type, std::less<base_type>, std::allocator<base_type>>::
                    const_iterator y_it;
                std::size_t failed_index = 0;
                for(x_it = x.begin(), y_it = y.begin(); x_it != x.end();
                    ++x_it, ++y_it, ++failed_index) {
                    if(*x_it != *y_it) {
                        error << "Found inequality at index " << failed_index << ": " << x_name
                              << "[" << failed_index << "] = " << *x_it << "; " << y_name << "["
                              << failed_index << "] = " << *y_it;
                        break; // break the loop
                    }
                }
            }
        }
        else { // Gem::Common::expectation::INEQUALITY == e
            error << "The two containers " << x_name << " and " << y_name << " are equal "
                  << "even though differences were expected" << '\n';
        }

        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
/**
 * This function checks whether two containers of floating point types meet a given expectation.
 *
 * @tparam fp_type The floating point element type held by the containers
 * @tparam c_type The sequence container template (e.g. std::vector, std::deque) parameterised by element and allocator
 * @param x The first vector to be compared
 * @param y The second vector to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed per-element deviation of two floating point values (only used for FP_SIMILARITY)
 */
template <std::floating_point fp_type, template <typename, typename> class c_type>
// NOLINTNEXTLINE(readability-function-size) -- one coherent expectation-comparison sweep (switch-on-expectation + per-element FP-similarity/equality diff reporting) for FP sequence containers; splitting would scatter the tightly-coupled error-message assembly
void compare(
    c_type<fp_type, std::allocator<fp_type>> const &x,
    c_type<fp_type, std::allocator<fp_type>> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = CE_DEF_SIMILARITY_DIFFERENCE
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)
    std::size_t deviation_pos = 0;
    std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY: {
        if(Gem::Common::expectation::FP_SIMILARITY == e) {
            expectation_str = "FP_SIMILARITY";
        }
        else { // May only be FP_SIMILARITY or EQUALITY in this case
            expectation_str = "EQUALITY";
        }

        if(x.size() != y.size()) {
            error << "Different vector-sizes found : " << x_name << ".size() = " << x.size()
                  << '\n'
                  << y_name << ".size() = " << y.size() << '\n';
            break; // expectationMet is false here
        }

        // Do a per-position comparison
        bool found_deviation = false;
        typename c_type<fp_type, std::allocator<fp_type>>::const_iterator x_it;
        typename c_type<fp_type, std::allocator<fp_type>>::const_iterator y_it;
        if(Gem::Common::expectation::FP_SIMILARITY == e) {
            for(x_it = x.begin(), y_it = y.begin(); x_it != x.end(); ++x_it, ++y_it) {
                if(std::abs(*x_it - *y_it) >= Gem::Common::narrow<fp_type>(limit)) {
                    found_deviation = true;
                    deviation_pos =
                        Gem::Common::narrow<std::size_t>(std::distance(x.begin(), x_it));
                    error << "Found deviation between containers:" << '\n'
                          << x_name << "[" << deviation_pos << "] = " << *x_it << "; " << '\n'
                          << y_name << "[" << deviation_pos << "] = " << *y_it << "; " << '\n'
                          << "limit = " << Gem::Common::narrow<fp_type>(limit) << "; " << '\n'
                          << "deviation = " << std::abs(*x_it - *y_it) << '\n';
                    break; // break the loop
                }
            }
        }
        else { // May only be FP_SIMILARITY or EQUALITY in this case
            for(x_it = x.begin(), y_it = y.begin(); x_it != x.end(); ++x_it, ++y_it) {
                if(*x_it != *y_it) {
                    found_deviation = true;
                    deviation_pos =
                        Gem::Common::narrow<std::size_t>(std::distance(x.begin(), x_it));
                    error << "Found deviation between containers:" << '\n'
                          << x_name << "[" << deviation_pos << "] = " << *x_it << "; " << '\n'
                          << y_name << "[" << deviation_pos << "] = " << *y_it << "; " << '\n';
                    break; // break the loop
                }
            }
        }

        if(not found_deviation) {
            expectation_met = true;
        }
    } break;

    case Gem::Common::expectation::INEQUALITY:
        expectation_str = "INEQUALITY";
        if(x != y) {
            expectation_met = true;
        }
        else {
            error << "The containers " << x_name << " and " << y_name << '\n'
                  << "do not differ even though they should" << '\n';
        }
        break;
    };

    if(not expectation_met) {
        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
/**
 * This function checks whether two containers with a std::set template interface,
 * holding floating point types meet a given expectation.
 *
 * @tparam fp_type The floating point element type held by the set-like containers
 * @tparam s_type The set container template (e.g. std::set) parameterised by element, comparator and allocator
 * @param x The first vector to be compared
 * @param y The second vector to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed per-element deviation of two floating point values (only used for FP_SIMILARITY)
 */
template <std::floating_point fp_type, template <typename, typename, typename> class s_type>
// NOLINTNEXTLINE(readability-function-size) -- one coherent expectation-comparison sweep (switch-on-expectation + per-element FP-similarity/equality diff reporting) for FP set-like containers; splitting would scatter the tightly-coupled error-message assembly
void compare(
    s_type<fp_type, std::less<fp_type>, std::allocator<fp_type>> const &x,
    s_type<fp_type, std::less<fp_type>, std::allocator<fp_type>> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = CE_DEF_SIMILARITY_DIFFERENCE
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)
    std::size_t deviation_pos = 0;
    std::ostringstream error; // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY: {
        if(Gem::Common::expectation::FP_SIMILARITY == e) {
            expectation_str = "FP_SIMILARITY";
        }
        else { // May only be FP_SIMILARITY or EQUALITY in this case
            expectation_str = "EQUALITY";
        }

        if(x.size() != y.size()) {
            error << "Different vector-sizes found : " << x_name << ".size() = " << x.size()
                  << '\n'
                  << y_name << ".size() = " << y.size() << '\n';
            break; // expectationMet is false here
        }

        // Do a per-position comparison
        bool found_deviation = false;
        typename s_type<fp_type, std::less<fp_type>, std::allocator<fp_type>>::const_iterator x_it;
        typename s_type<fp_type, std::less<fp_type>, std::allocator<fp_type>>::const_iterator y_it;
        if(Gem::Common::expectation::FP_SIMILARITY == e) {
            for(x_it = x.begin(), y_it = y.begin(); x_it != x.end(); ++x_it, ++y_it) {
                if(std::abs(*x_it - *y_it) >= Gem::Common::narrow<fp_type>(limit)) {
                    found_deviation = true;
                    deviation_pos =
                        Gem::Common::narrow<std::size_t>(std::distance(x.begin(), x_it));
                    error << "Found deviation between containers:" << '\n'
                          << x_name << "[" << deviation_pos << "] = " << *x_it << "; " << '\n'
                          << y_name << "[" << deviation_pos << "] = " << *y_it << "; " << '\n'
                          << "limit = " << Gem::Common::narrow<fp_type>(limit) << "; " << '\n'
                          << "deviation = " << std::abs(*x_it - *y_it) << '\n';
                    break; // break the loop
                }
            }
        }
        else { // May only be FP_SIMILARITY or EQUALITY in this case
            for(x_it = x.begin(), y_it = y.begin(); x_it != x.end(); ++x_it, ++y_it) {
                if(*x_it != *y_it) {
                    found_deviation = true;
                    deviation_pos =
                        Gem::Common::narrow<std::size_t>(std::distance(x.begin(), x_it));
                    error << "Found deviation between containers:" << '\n'
                          << x_name << "[" << deviation_pos << "] = " << *x_it << "; " << '\n'
                          << y_name << "[" << deviation_pos << "] = " << *y_it << "; " << '\n';
                    break; // break the loop
                }
            }
        }

        if(not found_deviation) {
            expectation_met = true;
        }
    } break;

    case Gem::Common::expectation::INEQUALITY:
        expectation_str = "INEQUALITY";
        if(x != y) {
            expectation_met = true;
        }
        else {
            error << "The containers " << x_name << " and " << y_name << '\n'
                  << "do not differ even though they should" << '\n';
        }
        break;
    };

    if(not expectation_met) {
        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
/**
 * This function checks whether two complex types meet a given expectation. It is assumed that
 * these types have the standard Geneva interface with corresponding "compare" functions.
 *
 * @tparam geneva_type A type satisfying the Gemfony common interface (provides a compare() member)
 * @param x The first parameter to be compared
 * @param y The second parameter to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed deviation of two floating point values, forwarded to the member compare()
 */
template <typename geneva_type>
    requires Gem::Common::gemfony_common_interface<geneva_type>
void compare(
    geneva_type const &x,
    geneva_type const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)
    std::ostringstream error;    // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY: {
        expectation_str = "FP_SIMILARITY / EQUALITY";

        // If we reach this line, then both pointers have content

        { // Check whether the content differs
            try {
                x.compare(y, e, limit);
            }
            catch(g_expectation_violation &g) {
                error << "Content of " << x_name << " and " << y_name << " differ." << '\n'
                      << "Thus the expectation of " << expectation_str
                      << " was violated:" << '\n'
                      << g.what() << '\n';
                break; // Terminate the switch statement
            }

            // If we reach this line, the expectation was met
            expectation_met = true;
        }
    } break;

    case Gem::Common::expectation::INEQUALITY: {
        expectation_str = "INEQUALITY";

        // Check whether the content differs
        try {
            x.compare(y, e, limit);
        }
        catch(g_expectation_violation &g) {
            // If we catch an expectation violation for expectation "inequality",
            // we simply break the switch statement so that expectationMet remains to be false
            error << "Content of " << x_name << " and " << y_name << " are equal/similar."
                  << '\n'
                  << "Thus the expectation of " << expectation_str << " was violated:" << '\n'
                  << g.what() << '\n';
            break;
        }
        expectation_met = true;
    } break;
    };

    if(not expectation_met) {
        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
/**
 * This function checks whether two smart pointers to complex types meet a given expectation.
 * It is assumed that these types have the standard Geneva interface with corresponding "compare"
 * functions.
 *
 * @tparam geneva_type A type satisfying the Gemfony common interface (provides a compare() member)
 * @param x The first parameter to be compared (a smart pointer; may be empty)
 * @param y The second parameter to be compared (a smart pointer; may be empty)
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed deviation of two floating point values, forwarded to the pointee compare()
 */
template <typename geneva_type>
    requires Gem::Common::gemfony_common_interface<geneva_type>
void compare(
    std::shared_ptr<geneva_type> const &x,
    std::shared_ptr<geneva_type> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)
    std::ostringstream error;    // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY: {
        expectation_str = "FP_SIMILARITY / EQUALITY";

        // Check whether the pointers hold content
        if(x && not y) {
            error << "Smart pointer " << x_name << " holds content while " << y_name << " does not."
                  << '\n'
                  << "Thus the expectation of " << expectation_str << " was violated" << '\n';
            break; //
        }
        if(not x && y) {
            error << "Smart pointer " << x_name << " doesn't hold content while " << y_name
                  << " does." << '\n'
                  << "Thus the expectation of " << expectation_str << " was violated" << '\n';
            break; // The expectation was clearly not met
        }
        if(not x &&
                not y) { // No content to check. Both smart pointers can be considered equal
            expectation_met = true;
            break;
        }

        // If we reach this line, then both pointers have content

        { // Check whether the content differs
            try {
                x->compare(*y, e, limit);
            }
            catch(g_expectation_violation &g) {
                error << "Content of " << x_name << " and " << y_name << " differ." << '\n'
                      << "Thus the expectation of " << expectation_str
                      << " was violated:" << '\n'
                      << g.what() << '\n';
                break; // Terminate the switch statement
            }

            // If we reach this line, the expectation was met
            expectation_met = true;
        }
    } break;

    case Gem::Common::expectation::INEQUALITY: {
        expectation_str = "INEQUALITY";

        // Check whether the pointers hold content
        if((x && not y) || (not x && y)) {
            expectation_met = true;
            break;
        }
        if(not x &&
                not y) { // No content to check. Both smart pointers can be considered equal
            error << "Both smart pointers are empty and are thus considered equal." << '\n'
                  << "Thus the expectation of " << expectation_str << " was violated:" << '\n';
            break; // The expectation was not met
        }

        // Check whether the content differs
        try {
            x->compare(*y, e, limit);
        }
        catch(g_expectation_violation &g) {
            // If we catch an expectation violation for expectation "inequality",
            // we simply break the switch statement so that expectationMet remains to be false
            error << "Content of " << x_name << " and " << y_name << " are equal/similar."
                  << '\n'
                  << "Thus the expectation of " << expectation_str << " was violated:" << '\n'
                  << g.what() << '\n';
            break;
        }
        expectation_met = true;
    } break;
    };

    if(not expectation_met) {
        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
/**
 * unique_ptr counterpart of the shared_ptr compare() above. A uniquely-owned member is deep-compared
 * by its pointee, exactly like the shared_ptr case -- without it, compare_t(IDENTITY(unique_ptr, ...))
 * would fall back to comparing the raw pointer addresses, so two independent clones would always be
 * reported as unequal. The pointees are viewed through NON-OWNING shared_ptrs (no-op deleter) so the
 * full shared_ptr comparison logic (null handling, EQUALITY / INEQUALITY) is reused verbatim.
 *
 * @tparam geneva_type A type satisfying the Gemfony common interface (provides a compare() member)
 * @param x The first parameter to be compared (a uniquely-owned pointer; may be empty)
 * @param y The second parameter to be compared (a uniquely-owned pointer; may be empty)
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed deviation of two floating point values, forwarded to the pointee compare()
 */
template <typename geneva_type>
    requires Gem::Common::gemfony_common_interface<geneva_type>
void compare(
    std::unique_ptr<geneva_type> const &x,
    std::unique_ptr<geneva_type> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
) {
    compare(
        std::shared_ptr<geneva_type>(x.get(), [](geneva_type *) { /* non-owning */ }),
        std::shared_ptr<geneva_type>(y.get(), [](geneva_type *) { /* non-owning */ }),
        x_name,
        y_name,
        e,
        limit
    );
}

/******************************************************************************/
/**
 * This function checks whether two containers of smart pointers to complex types meet a given expectation.
 * It is assumed that these types have the standard Geneva interface with corresponding "compare"
 * functions. For an idea of what the template specifier does, search for "template template" in conjunction
 * with containers.
 *
 * @tparam geneva_type A type satisfying the Gemfony common interface (provides a compare() member)
 * @tparam c_type The sequence container template (e.g. std::vector) holding the smart pointers
 * @param x The first vector to be compared
 * @param y The second vector to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed deviation of two floating point values, forwarded to each element compare()
 */
template <typename geneva_type, template <typename, typename> class c_type>
    requires Gem::Common::gemfony_common_interface<geneva_type>
// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity) -- one coherent expectation-comparison sweep (switch-on-expectation + per-element null/content diff reporting via nested compare()) for containers of smart pointers to Geneva types; splitting would scatter the tightly-coupled error-message assembly
void compare(
    c_type<std::shared_ptr<geneva_type>, std::allocator<std::shared_ptr<geneva_type>>> const &x,
    c_type<std::shared_ptr<geneva_type>, std::allocator<std::shared_ptr<geneva_type>>> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
) {
    bool expectation_met = false;
    std::string expectation_str; // NOLINT(cppcoreguidelines-init-variables)
    std::ostringstream error;    // NOLINT(cppcoreguidelines-init-variables)

    switch(e) {
    case Gem::Common::expectation::FP_SIMILARITY:
    case Gem::Common::expectation::EQUALITY: {
        expectation_str = "FP_SIMILARITY / EQUALITY";

        // First check sizes
        if(x.size() != y.size()) {
            error << "containers " << x_name << " and " << y_name << " have different sizes "
                  << x.size() << " / " << y.size() << '\n'
                  << "Thus the expectation of " << expectation_str << " was violated" << '\n';
            // Terminate the switch statement. expectationMet will be false then
            break;
        }

        // Now loop over all members of the containers (equal sizes guaranteed above)
        bool found_deviation = false;
        for(auto const &[index, elems] : std::views::zip(x, y) | std::views::enumerate) {
            auto const &[x_ptr, y_ptr] = elems;
            // First check that both pointers have content
            // Check whether the pointers hold content
            if(x_ptr && not y_ptr) {
                error << "Smart pointer " << x_name << "[" << index << "] holds content while "
                      << y_name << "[" << index << "]  does not." << '\n'
                      << "Thus the expectation of " << expectation_str << " was violated"
                      << '\n';
                found_deviation = true;
                break; // terminate the loop
            }
            if(not x_ptr && y_ptr) {
                error << "Smart pointer " << x_name << "[" << index
                      << "] doesn't hold content while " << y_name << "[" << index << "]  does."
                      << '\n'
                      << "Thus the expectation of " << expectation_str << " was violated"
                      << '\n';
                found_deviation = true;
                break; // terminate the loop
            }
            if(not x_ptr &&
                    not y_ptr) { // No content to check. Both smart pointers can be considered equal
                continue;        // Go on with next iteration in the loop
            }

            // At this point we know that both pointers have content. We can now check the content
            // which is assumed to have the compare() function
            try {
                x_ptr->compare(*y_ptr, e, limit);
            }
            catch(g_expectation_violation &g) {
                error << "Content of " << x_name << "[" << index << "] and " << y_name << "["
                      << index << "] differs." << '\n'
                      << "Thus the expectation of " << expectation_str
                      << " was violated:" << '\n'
                      << g.what() << '\n';
                found_deviation = true;
                break; // Terminate the loop
            }
        }

        if(not found_deviation) {
            expectation_met = true;
        }
    } break;

    case Gem::Common::expectation::INEQUALITY: {
        expectation_str = "INEQUALITY";

        // First check sizes. The expectation of inequality will be met if they differ
        if(x.size() != y.size()) {
            expectation_met = true;
            break; // Terminate the switch statement
        }

        // Now loop over all members of the containers (sizes are equal here)
        bool found_inequality = false;
        for(auto const &[x_ptr, y_ptr] : std::views::zip(x, y)) {
            // First check that both pointers have content
            // Check whether the pointers hold content
            if((x_ptr && not y_ptr) || (not x_ptr && y_ptr)) {
                found_inequality = true;
                break; // terminate the loop
            }
            if(not x_ptr &&
                    not y_ptr) { // No content to check. Both smart pointers can be considered equal
                continue; // Go on with next iteration in the loop - there is nothing to check here
            }

            // At this point we know that both pointers have content. We can now check this content
            // which is assumed to have the compare() function
            try {
                x_ptr->compare(*y_ptr, e, limit);
                found_inequality = true;
                break; // terminate the loop
            }
            catch(g_expectation_violation &) {
                // Go on with the next item in the vector -- the content is equal or similar
                continue;
            }
        }

        if(found_inequality) {
            expectation_met = true;
        }
        else {
            error << "The two containers " << x_name << " and " << y_name << " are equal."
                  << '\n'
                  << "Thus the expectation of " << expectation_str << " was violated:" << '\n';
        }
    } break;
    };

    if(not expectation_met) {
        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
/**
 * unique_ptr counterpart of the shared_ptr container compare() above. A container of uniquely-owned
 * elements is deep-compared element-by-element, exactly like the shared_ptr case -- without it,
 * compare_t(IDENTITY(data_cnt_, ...)) for a unique_ptr container would fall back to comparing the raw
 * element addresses, so two independent clones would always be reported as unequal. Non-owning
 * shared_ptr views (no-op deleter) are built so the full shared_ptr container logic is reused verbatim.
 *
 * @tparam geneva_type A type satisfying the Gemfony common interface (provides a compare() member)
 * @tparam c_type The sequence container template (e.g. std::vector) holding the unique pointers
 * @param x The first vector to be compared
 * @param y The second vector to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed deviation of two floating point values, forwarded to each element compare()
 */
template <typename geneva_type, template <typename, typename> class c_type>
    requires Gem::Common::gemfony_common_interface<geneva_type>
void compare(
    c_type<std::unique_ptr<geneva_type>, std::allocator<std::unique_ptr<geneva_type>>> const &x,
    c_type<std::unique_ptr<geneva_type>, std::allocator<std::unique_ptr<geneva_type>>> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
) {
    std::vector<std::shared_ptr<geneva_type>> x_view;
    std::vector<std::shared_ptr<geneva_type>> y_view;
    x_view.reserve(x.size());
    y_view.reserve(y.size());
    for(auto const &p : x) {
        x_view.emplace_back(p.get(), [](geneva_type *) { /* non-owning */ });
    }
    for(auto const &p : y) {
        y_view.emplace_back(p.get(), [](geneva_type *) { /* non-owning */ });
    }
    compare(x_view, y_view, x_name, y_name, e, limit);
}

/******************************************************************************/
/**
 * @brief This function checks whether two objects of type Gem::Common::tribool meet a given expectation.
 *
 * The parameters, in signature order, are: the first tribool to compare, the second tribool to compare,
 * the name of the first tribool, the name of the second tribool, the expectation both must fulfill,
 * and the limit (the maximum allowed deviation of two floating point values; unused for tribool and
 * present only to match the common compare() signature). A g_expectation_violation exception is thrown
 * if the expectation is violated.
 *
 * @param limit The maximum allowed deviation of two floating point values (unused for tribool)
 */
void compare(
    Gem::Common::tribool const &x,
    Gem::Common::tribool const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = CE_DEF_SIMILARITY_DIFFERENCE
);

/******************************************************************************/
/**
 * @brief This function checks whether two types fulfill a given expectation.
 *
 * It increments the token's test counter, delegates to the appropriate compare() overload using the
 * items, names and limit held by the identity struct and the expectation taken from the token, and
 * records either success or a registered error message on the token. Non-expectation exceptions are
 * re-thrown as a geneva_exception.
 *
 * @tparam T The type of the two items wrapped by the identity struct
 * @param data The identity struct holding the two items, their names and the comparison limit
 * @param token The token holding the expectation and accumulating test / success counts and error messages
 */
template <typename T>
void compare_t(identity<T> const &data, GToken &token) {
    try {
        token.incrTestCounter();
        compare(data.x, data.y, data.x_name, data.y_name, token.getExpectation(), data.limit);
        token.incrSuccessCounter();
    }
    catch(const g_expectation_violation &g) {
        token.registerErrorMessage(g);
    }
    catch(const std::exception &e) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "Caught std::exception with message " << '\n'
            << e.what() << '\n'
        );
    }
    catch(...) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace()) << "Caught unknown exception" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief This function checks whether two base types fulfill a given expectation.
 *
 * It forces the use of base_type's own compare_() member (so only the base-class slice is compared),
 * incrementing the token's test counter and recording success or a registered error message on the
 * token. Non-expectation exceptions are re-thrown as a geneva_exception.
 *
 * @tparam base_type The base class whose compare_() member is invoked for the comparison
 * @param x The first object whose base_type slice is compared
 * @param y The second object whose base_type slice is compared
 * @param token The token holding the expectation and accumulating test / success counts and error messages
 */
template <typename base_type>
void compare_base_t(base_type const &x, base_type const &y, GToken &token) {
    try {
        token.incrTestCounter();
        x.base_type::compare_( // Force usage of the base-type compare_ function
            y
            , token.getExpectation()
            , Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
        );
        token.incrSuccessCounter();
    }
    catch(const g_expectation_violation &g) {
        token.registerErrorMessage(g);
    }
    catch(const std::exception &e) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "Caught std::exception with message" << '\n'
            << e.what() << '\n'
        );
    }
    catch(...) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace()) << "Caught unknown exception" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * The member-reflection machinery -- member_t / make_member() and the tagged
 * cloneable/atomic variants, plus g_load_members() and serialize_members() --
 * lives in GMemberReflectionT.hpp (included above). The compare derivation below
 * stays here because it depends on the comparison DSL (compare_t / getIdentity /
 * GToken) declared earlier in this header.
 */

/******************************************************************************/
/**
 * @brief Compares a single member descriptor pairwise by applying its compare policy.
 *
 * The compare axis is represented in GMemberReflectionT.hpp by opaque tag types (the compare
 * operation depends on the DSL declared in THIS header), so the dispatch happens here:
 *
 *  - cmp_value  : feeds the referenced values and names to compare_t() / getIdentity();
 *  - cmp_atomic : compares the LOADED values of a std::atomic member rather than the atomic objects;
 *  - cmp_skip   : compares nothing (the member is not part of the object's comparable identity).
 *
 * The two descriptors carry the same three policies (they come from the same localMembers()
 * declaration), but the referenced type differs (const vs non-const context), so A and B are
 * independent template parameters.
 *
 * @tparam A The referenced member type of the first descriptor
 * @tparam B The referenced member type of the second descriptor
 * @tparam Ser The (unused here) serialize policy
 * @tparam Load The (unused here) load policy
 * @tparam Cmp The compare policy tag dispatched on
 * @param a The first member descriptor (its .ref value and .name are compared)
 * @param b The second member descriptor (its .ref value and .name are compared)
 * @param token The token holding the expectation and recording the comparison result
 */
template <typename A, typename B, typename Ser, typename Load, typename Cmp>
void g_compare_one(
    const member_desc<A, Ser, Load, Cmp> &a,
    const member_desc<B, Ser, Load, Cmp> &b,
    GToken &token
) {
    if constexpr (std::same_as<Cmp, cmp_skip>) {
        // skipped: excluded from comparable identity
    } else if constexpr (std::same_as<Cmp, cmp_atomic>) {
        // Compare the loaded values rather than the atomic objects themselves.
        compare_t(getIdentity(a.ref.load(), b.ref.load(), a.name, b.name), token);
    } else { // cmp_value
        compare_t(getIdentity(a.ref, b.ref, a.name, b.name), token);
    }
}

/**
 * @brief Compares two tuples of member descriptors element-by-element via a fold over an index sequence.
 *
 * @tparam ATuple The tuple type of the first set of member descriptors
 * @tparam BTuple The tuple type of the second set of member descriptors
 * @tparam I The compile-time index pack enumerating the tuple elements to compare
 * @param a The first tuple of member descriptors
 * @param b The second tuple of member descriptors
 * @param token The token holding the expectation and accumulating the per-member comparison results
 *
 * @note The trailing std::index_sequence<I...> argument only carries the indices I... that
 *       drive the comparison fold; it has no runtime value.
 */
template <typename ATuple, typename BTuple, std::size_t... I>
void g_compare_members_impl(
    const ATuple &a, const BTuple &b, GToken &token, [[maybe_unused]] std::index_sequence<I...> indices
) {
    (g_compare_one(std::get<I>(a), std::get<I>(b), token), ...);
}

/**
 * @brief Compares each local member pairwise, recording results in the token.
 *
 * @tparam ATuple The tuple type of the first object's local member descriptors
 * @tparam BTuple The tuple type of the second object's local member descriptors
 * @param a The first object's tuple of local member descriptors (e.g. from localMembers())
 * @param b The second object's tuple of local member descriptors (e.g. from localMembers())
 * @param token The token holding the expectation and accumulating the per-member comparison results
 */
template <typename ATuple, typename BTuple>
void g_compare_members(ATuple a, BTuple b, GToken &token) {
    static_assert(
        std::tuple_size_v<ATuple> == std::tuple_size_v<BTuple>,
        "g_compare_members: localMembers() arity mismatch"
    );
    g_compare_members_impl(a, b, token, std::make_index_sequence<std::tuple_size_v<ATuple>>{});
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common */
