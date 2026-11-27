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
#include <algorithm>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <ranges>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

// Boost headers go here
#include <boost/core/demangle.hpp>

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
     * @brief The standard constructor -- initialization with class name, expectation and the
     * limit of floating point similarity.
     */
    GToken(std::string caller, Gem::Common::expectation e, double limit);

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
     */
    [[nodiscard]] std::size_t getSuccessCounter() const;
    /**
     * @brief Allows to retrieve the current state of the test counter.
     */
    [[nodiscard]] std::size_t getTestCounter() const;

    /**
     * @brief Allows to check whether the expectation was met.
     */
    [[nodiscard]] bool expectationMet() const;
    /**
     * @brief Conversion to a boolean indicating whether the expectation was met.
     */
    operator bool() const; // NOLINT

    /**
     * @brief Allows to retrieve the expectation token.
     */
    [[nodiscard]] Gem::Common::expectation getExpectation() const;
    /**
     * @brief Allows to retrieve the maximum deviation of two floating point values that are
     * similar (FP_SIMILARITY).
     */
    [[nodiscard]] double getLimit() const;
    /**
     * @brief Allows to retrieve the expectation token as a string.
     */
    [[nodiscard]] std::string getExpectationStr() const;
    /**
     * @brief Allows to retrieve the name of the caller.
     */
    [[nodiscard]] std::string getCallerName() const;

    /**
     * @brief Allows to register an error message e.g. obtained from a failed check.
     */
    void registerErrorMessage(std::string const &m);
    /**
     * @brief Allows to register an exception obtained from a failed check.
     */
    void registerErrorMessage(g_expectation_violation const &g);

    /**
     * @brief Allows to retrieve the currently registered error messages.
     */
    [[nodiscard]] std::string getErrorMessages() const;

    /**
     * @brief Conversion to a string indicating success or failure.
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
    /** @brief The maximum deviation of two floating point values that meet FP_SIMILARITY */
    const double limit_;
};

/******************************************************************************/
/**
 * @brief This function facilitates the output of GToken objects, mostly for debugging purposes.
 */
std::ostream &operator<<(std::ostream &s, GToken const &g);

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/**
 * This struct facilitates transfer of comparable items to comparators. The expectation and the
 * limit of floating point similarity come from the GToken the items are compared under (see
 * compare_t()).
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
     */
    identity(T const &x_var, T const &y_var, std::string x_name_var, std::string y_name_var)
      : x(x_var)
      , y(y_var)
      , x_name(std::move(x_name_var))
      , y_name(std::move(y_name_var)) { /* nothing */
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

private:
    /***************************************************************************/
    /**
     * @brief Does the actual conversion, including a check that base_type is indeed a base of T.
     *
     * @tparam base_type A base class of T (enforced via a requires-clause) the items are cast to
     * @return An identity object whose items are dynamic_cast base-class references of this object's items,
     *         with the names prefixed by the base-type name
     */
    template <typename base_type>
        requires std::is_base_of_v<base_type, T>
    identity<base_type> to() const {
        auto const &x_conv = dynamic_cast<base_type const &>(x);
        auto const &y_conv = dynamic_cast<base_type const &>(y);

        std::string const base_name = boost::core::demangle(typeid(base_type).name());
        std::string const x_name_conv = "(" + base_name + ")" + x_name;
        std::string const y_name_conv = "(" + base_name + ")" + y_name;

        return identity<base_type>(x_conv, y_conv, x_name_conv, y_name_conv);
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
    s << "Identity:" << '\n' << "x_name = " << i.x_name << '\n' << "y_name = " << i.y_name << '\n';
    return s;
}

/******************************************************************************/
/**
 * @brief Returns an identity object. The function is needed as automatic type
 * deduction does not work for structs / classes.
 *
 * @tparam T The type of the two items to be compared
 * @param x_var The first item to be compared
 * @param y_var The second item to be compared
 * @param x_name_var A human-readable name for the first item
 * @param y_name_var A human-readable name for the second item
 * @return An identity object wrapping the two items and their names
 */
template <typename T>
identity<T> getIdentity(
    T const &x_var,
    T const &y_var,
    std::string const &x_name_var,
    std::string const &y_name_var
) {
    return identity<T>(x_var, y_var, x_name_var, y_name_var);
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
 * @return An identity object holding the base-class views of the two items
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

    return identity<base_type>(x_var_base, y_var_base, x_name_var, y_name_var);
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
 * Satisfied by a smart pointer (std::shared_ptr or std::unique_ptr) to a type with the Gemfony common
 * interface. compare() follows such a pointer to the compare() of the object it points to.
 *
 * @tparam T The type checked
 */
template <typename T>
concept GemPointer =
    gemfony_common_interface<std::remove_const_t<typename T::element_type>> &&
    (std::same_as<T, std::shared_ptr<typename T::element_type>> ||
     std::same_as<T, std::unique_ptr<typename T::element_type, typename T::deleter_type>>);

/**
 * Satisfied by a container or other sized range that compare() takes element by element: a vector,
 * deque, list, set, map or array, also nested. An unordered container (a type with a hasher) is
 * compared with its own ==, which does not depend on the order of its elements. Strings (all types
 * with a traits_type) are compared as values, and an object with the Gemfony common interface through
 * its own compare(), even if it is a range as well.
 *
 * @tparam T The type checked
 */
template <typename T>
concept ComparedAsRange = std::ranges::input_range<T const> && std::ranges::sized_range<T const> &&
                          !requires { typename T::traits_type; } && !gemfony_common_interface<T>;

/**
 * Satisfied by the types compare() takes as values, with == and !=, and prints with operator<<: all
 * types that are neither floating point, nor objects with the Gemfony common interface or pointers to
 * them, nor ranges.
 *
 * @tparam T The type checked
 */
template <typename T>
concept ComparedByValue = !std::floating_point<T> && !gemfony_common_interface<T> &&
                          !GemPointer<T> && !ComparedAsRange<T>;

/******************************************************************************/
/**
 * @brief Tells whether two values meet an expectation. The one place of the floating point tolerance.
 *
 * Floating point values meet FP_SIMILARITY if they are equal or deviate by less than the limit. All
 * other cases compare with == (EQUALITY, and FP_SIMILARITY for values that are not floating point) or
 * != (INEQUALITY; floating point values too are then compared exactly). compare() checks the equality
 * of object states, so two floating point NaN are equal here, and a NaN differs from every number.
 *
 * @tparam T The type of the two values
 * @param x The first value
 * @param y The second value
 * @param e The expectation both values need to fulfill
 * @param limit The maximum allowed deviation of two floating point values (only used for FP_SIMILARITY)
 * @return true if the two values meet the expectation
 */
template <typename T>
[[nodiscard]] bool
valuesMeet(T const &x, T const &y, Gem::Common::expectation e, [[maybe_unused]] double limit) {
    if constexpr(std::floating_point<T>) {
        bool const equal = x == y || (std::isnan(x) && std::isnan(y));
        switch(e) {
        case Gem::Common::expectation::FP_SIMILARITY:
            return equal || std::abs(x - y) < Gem::Common::narrow<T>(limit);
        case Gem::Common::expectation::EQUALITY:
            return equal;
        case Gem::Common::expectation::INEQUALITY:
            return not equal;
        }
    }
    if(Gem::Common::expectation::INEQUALITY == e) {
        return x != y;
    }
    return x == y;
}

/******************************************************************************/
/**
 * @brief Composes the message that reports two values which violate an expectation.
 *
 * @tparam printable_type The type of the printed form of the two values
 * @param expectation_str The name of the violated expectation
 * @param x_name The name of the first value
 * @param x The printed form of the first value
 * @param y_name The name of the second value
 * @param y The printed form of the second value
 * @return The message, with the names and values of the two parameters
 */
template <typename printable_type>
[[nodiscard]] std::string valueViolation(
    std::string const &expectation_str,
    std::string const &x_name,
    printable_type const &x,
    std::string const &y_name,
    printable_type const &y
) {
    std::ostringstream error;
    error << "Expectation of " << expectation_str << " was violated for parameters " << '\n'
          << "[" << '\n';
    if constexpr(requires(std::ostream &out) { out << x; }) {
        error << x_name << " = " << x << '\n' << y_name << " = " << y << '\n';
    }
    else { // e.g. the std::pair elements of a map
        error << x_name << " and " << y_name << " (values that cannot be printed)" << '\n';
    }
    error << "]" << '\n';
    return error.str();
}

/******************************************************************************/
/**
 * @brief The name under which a comparison by value reports an expectation: a check for similarity is
 * one for equality there.
 *
 * @param e The expectation
 * @return "INEQUALITY" or "FP_SIMILARITY / EQUALITY"
 */
[[nodiscard]] inline std::string byValueExpectationName(Gem::Common::expectation e) {
    return Gem::Common::expectation::INEQUALITY == e ? "INEQUALITY" : "FP_SIMILARITY / EQUALITY";
}

/******************************************************************************/
/**
 * This function checks whether two values fulfill a given expectation. It assumes that x and y
 * understand the == and != operators and may be streamed. A check for similarity is treated the same
 * as a check for equality. Floating point values, objects with the Gemfony common interface, pointers
 * to them and ranges have overloads of their own. The function will throw a g_expectation_violation
 * exception if the expectation was violated.
 *
 * @tparam basic_type The type of the items being compared (see ComparedByValue)
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
    requires ComparedByValue<basic_type>
void compare(
    basic_type const &x,
    basic_type const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    [[maybe_unused]] double limit = 0.
) {
    if(not valuesMeet(x, y, e, limit)) {
        throw g_expectation_violation(
            valueViolation(byValueExpectationName(e), x_name, x, y_name, y)
        );
    }
}

/******************************************************************************/
/**
 * This function checks whether two std::chrono::time_point types fulfill a given
 * expectation. A check for similarity is treated the same as a check for
 * equality. The function will throw a g_expectation_violation exception if the expectation
 * was violated. The message shows the time points as counts since the epoch.
 *
 * @tparam clock_type The clock type of the compared time points
 * @tparam duration_type The duration type of the compared time points (defaults to clock_type::duration)
 * @param x The first parameter to be compared
 * @param y The second parameter to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 *
 * @note The trailing double `limit` argument is an unused tolerance value,
 *       present only to match the common compare() signature.
 */
template <typename clock_type, typename duration_type = typename clock_type::duration>
void compare(
    std::chrono::time_point<clock_type, duration_type> const &x,
    std::chrono::time_point<clock_type, duration_type> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    [[maybe_unused]] double limit = 0.
) {
    if(not valuesMeet(x, y, e, limit)) {
        throw g_expectation_violation(valueViolation(
            byValueExpectationName(e),
            x_name,
            x.time_since_epoch().count(),
            y_name,
            y.time_since_epoch().count()
        ));
    }
}

/******************************************************************************/
/**
 * This function checks whether two std::chrono::duration types fulfill a given
 * expectation. A check for similarity is treated the same as a check for
 * equality. The function will throw a g_expectation_violation exception if the expectation
 * was violated. The message shows the durations as counts of their ticks.
 *
 * @tparam rep_type The arithmetic representation type of the compared durations
 * @tparam period_type The std::ratio tick period of the compared durations (defaults to std::ratio<1>)
 * @param x The first parameter to be compared
 * @param y The second parameter to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 *
 * @note The trailing double `limit` argument is an unused tolerance value,
 *       present only to match the common compare() signature.
 */
template <typename rep_type, typename period_type = std::ratio<1>>
void compare(
    std::chrono::duration<rep_type, period_type> const &x,
    std::chrono::duration<rep_type, period_type> const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    [[maybe_unused]] double limit = 0.
) {
    if(not valuesMeet(x, y, e, limit)) {
        throw g_expectation_violation(
            valueViolation(byValueExpectationName(e), x_name, x.count(), y_name, y.count())
        );
    }
}

/******************************************************************************/
/**
 * This function checks whether two floating point types meet a given expectation.
 * The function will throw a g_expectation_violation exception if the expectation
 * was violated; under FP_SIMILARITY its message also shows the limit and the deviation.
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
    if(valuesMeet(x, y, e, limit)) {
        return;
    }
    if(Gem::Common::expectation::FP_SIMILARITY == e) {
        std::ostringstream deviation;
        deviation << "limit = " << Gem::Common::narrow<fp_type>(limit)
                  << ", deviation = " << std::abs(x - y) << '\n';
        throw g_expectation_violation(
            valueViolation(Gem::Common::enumName(e), x_name, x, y_name, y) + deviation.str()
        );
    }
    throw g_expectation_violation(valueViolation(Gem::Common::enumName(e), x_name, x, y_name, y));
}

/******************************************************************************/
/**
 * This function checks whether two complex types meet a given expectation. It is assumed that
 * these types have the Gemfony common interface with corresponding "compare" functions.
 *
 * @tparam gem_type A type satisfying the Gemfony common interface (provides a compare() member)
 * @param x The first parameter to be compared
 * @param y The second parameter to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed deviation of two floating point values, forwarded to the member compare()
 */
template <typename gem_type>
    requires Gem::Common::gemfony_common_interface<gem_type>
void compare(
    gem_type const &x,
    gem_type const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
) {
    try {
        x.compare(y, e, limit);
    }
    catch(g_expectation_violation const &g) {
        std::ostringstream error;
        error << "Content of " << x_name << " and " << y_name
              << (Gem::Common::expectation::INEQUALITY == e ? " are equal/similar." : " differ.")
              << '\n'
              << "Thus the expectation of " << byValueExpectationName(e) << " was violated:" << '\n'
              << g.what() << '\n';
        throw g_expectation_violation(error.str());
    }
}

/******************************************************************************/
/**
 * This function checks whether two smart pointers (std::shared_ptr or std::unique_ptr) to complex
 * types meet a given expectation. Two pointers that both hold an object are compared through the
 * compare() of these objects; two empty pointers are equal; an empty and a full pointer differ.
 *
 * @tparam pointer_type The smart pointer type (see GemPointer)
 * @param x The first parameter to be compared (may be empty)
 * @param y The second parameter to be compared (may be empty)
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed deviation of two floating point values, forwarded to the pointee compare()
 */
template <typename pointer_type>
    requires GemPointer<pointer_type>
void compare(
    pointer_type const &x,
    pointer_type const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
) {
    if(x && y) {
        compare(*x, *y, x_name, y_name, e, limit);
        return;
    }

    bool const both_empty = not x && not y;
    bool const inequality_expected = Gem::Common::expectation::INEQUALITY == e;
    if(both_empty != inequality_expected) {
        return; // Two empty pointers are equal, an empty and a full one differ
    }

    std::ostringstream error;
    if(both_empty) {
        error << "Both smart pointers are empty and are thus considered equal." << '\n';
    }
    else {
        error << "Smart pointer " << x_name << (x ? " holds" : " doesn't hold") << " content while "
              << y_name << (x ? " does not." : " does.") << '\n';
    }
    error << "Thus the expectation of " << byValueExpectationName(e) << " was violated" << '\n';
    throw g_expectation_violation(error.str());
}

/******************************************************************************/
/**
 * @brief Checks whether two ranges meet a given expectation, element by element (declaration; the
 * definition follows elementsMeet(), which it uses and which uses it for nested ranges).
 */
template <typename range_type>
    requires ComparedAsRange<range_type>
void compare(
    range_type const &x,
    range_type const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit = Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
);

/******************************************************************************/
/**
 * @brief Tells whether two elements of a range meet an expectation, without composing a message.
 *
 * Floating point values and values (see ComparedByValue) are compared directly; objects with the
 * Gemfony common interface, pointers to them and nested ranges through their compare() overload.
 *
 * @tparam T The element type of the range
 * @param x The first element
 * @param y The second element
 * @param e The expectation both elements need to fulfill
 * @param limit The maximum allowed deviation of two floating point values (only used for FP_SIMILARITY)
 * @return true if the two elements meet the expectation
 */
template <typename T>
[[nodiscard]] bool elementsMeet(T const &x, T const &y, Gem::Common::expectation e, double limit) {
    if constexpr(std::floating_point<T> || ComparedByValue<T>) {
        return valuesMeet(x, y, e, limit);
    }
    else {
        try {
            compare(x, y, std::string{}, std::string{}, e, limit);
            return true;
        }
        catch(g_expectation_violation const &) {
            return false;
        }
    }
}

/******************************************************************************/
/**
 * This function checks whether two ranges (see ComparedAsRange: containers of any shape, also nested)
 * meet a given expectation. EQUALITY and FP_SIMILARITY need ranges of the same size whose elements
 * meet the expectation pairwise; the message of the first pair that does not is the message of the
 * violation, its names indexed by the position (x_name[i]). INEQUALITY is met if the sizes differ or
 * one pair of elements differs. Elements are compared by their kind: floating point values with the
 * limit, objects with the Gemfony common interface and pointers to them through their compare(),
 * nested ranges by this function, all others as values. An unordered container (std::unordered_set,
 * std::unordered_map and their multi forms) has no order of its own: two equal ones may iterate in
 * different orders, so it is compared with its == (exactly, also under FP_SIMILARITY). The function
 * will throw a g_expectation_violation exception if the expectation was violated.
 *
 * @tparam range_type The type of the two ranges
 * @param x The first range to be compared
 * @param y The second range to be compared
 * @param x_name The name of the first parameter
 * @param y_name The name of the second parameter
 * @param e The expectation both parameters need to fulfill
 * @param limit The maximum allowed deviation of two floating point values (only used for FP_SIMILARITY)
 */
template <typename range_type>
    requires ComparedAsRange<range_type>
void compare(
    range_type const &x,
    range_type const &y,
    std::string const &x_name,
    std::string const &y_name,
    Gem::Common::expectation e,
    double limit
) {
    using element_type = std::ranges::range_value_t<range_type const>;
    auto const x_size = std::ranges::size(x);
    auto const y_size = std::ranges::size(y);
    auto const violation = [&] {
        return "Expectation of " + Gem::Common::enumName(e) + " was violated for parameters " +
               x_name + " and " + y_name;
    };

    if constexpr(requires { typename range_type::hasher; }) {
        bool const equal = x == y;
        if(equal == (Gem::Common::expectation::INEQUALITY == e)) {
            throw g_expectation_violation(
                violation() + (equal ? ": they do not differ\n" : ": they differ\n")
            );
        }
    }
    else {
        if(Gem::Common::expectation::INEQUALITY == e) {
            auto const differ = [limit](auto const &xy) {
                return elementsMeet<element_type>(
                    std::get<0>(xy),
                    std::get<1>(xy),
                    Gem::Common::expectation::INEQUALITY,
                    limit
                );
            };
            if(x_size != y_size || std::ranges::any_of(std::views::zip(x, y), differ)) {
                return;
            }
            throw g_expectation_violation(violation() + ": they do not differ\n");
        }

        if(x_size != y_size) {
            throw g_expectation_violation(
                violation() + ": different sizes " + x_name +
                ".size() = " + std::to_string(x_size) + ", " + y_name +
                ".size() = " + std::to_string(y_size) + '\n'
            );
        }

        for(auto const &[index, xy] : std::views::zip(x, y) | std::views::enumerate) {
            element_type const &x_element = std::get<0>(xy);
            element_type const &y_element = std::get<1>(xy);
            if(not elementsMeet(x_element, y_element, e, limit)) {
                compare(
                    x_element,
                    y_element,
                    x_name + "[" + std::to_string(index) + "]",
                    y_name + "[" + std::to_string(index) + "]",
                    e,
                    limit
                );
            }
        }
    }
}

/******************************************************************************/
/**
 * @brief This function checks whether two types fulfill a given expectation.
 *
 * It increments the token's test counter, delegates to the appropriate compare() overload using
 * the items and names held by the identity struct and the expectation and limit taken from the
 * token, and records either success or a registered error message on the token. Non-expectation
 * exceptions are re-thrown as a gem_exception.
 *
 * @tparam T The type of the two items wrapped by the identity struct
 * @param data The identity struct holding the two items and their names
 * @param token The token holding the expectation and the limit, accumulating test / success
 * counts and error messages
 */
template <typename T>
void compare_t(identity<T> const &data, GToken &token) {
    try {
        token.incrTestCounter();
        compare(data.x, data.y, data.x_name, data.y_name, token.getExpectation(), token.getLimit());
        token.incrSuccessCounter();
    }
    catch(const g_expectation_violation &g) {
        token.registerErrorMessage(g);
    }
    catch(...) {
        throw gem_exception(
            g_error_streamer() << "Caught an exception with message" << '\n'
                               << describeException(std::current_exception()) << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief This function checks whether two base types fulfill a given expectation.
 *
 * It forces the use of base_type's own compare_() member (so only the base-class slice is
 * compared) under the token's expectation and limit, incrementing the token's test counter and
 * recording success or a registered error message on the token. Non-expectation exceptions are
 * re-thrown as a gem_exception.
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
            , token.getLimit()
        );
        token.incrSuccessCounter();
    }
    catch(const g_expectation_violation &g) {
        token.registerErrorMessage(g);
    }
    catch(...) {
        throw gem_exception(
            g_error_streamer() << "Caught an exception with message" << '\n'
                               << describeException(std::current_exception()) << '\n'
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
 * @tparam ser_type The (unused here) serialize policy
 * @tparam load_type The (unused here) load policy
 * @tparam cmp_type The compare policy tag dispatched on
 * @param a The first member descriptor (its .ref value and .name are compared)
 * @param b The second member descriptor (its .ref value and .name are compared)
 * @param token The token holding the expectation and recording the comparison result
 */
template <typename A, typename B, typename ser_type, typename load_type, typename cmp_type>
void g_compare_one(
    const member_desc<A, ser_type, load_type, cmp_type> &a,
    const member_desc<B, ser_type, load_type, cmp_type> &b,
    GToken &token
) {
    if constexpr(std::same_as<cmp_type, cmp_skip>) {
        // skipped: excluded from comparable identity
    }
    else if constexpr(std::same_as<cmp_type, cmp_atomic>) {
        // Compare the loaded values rather than the atomic objects themselves.
        compare_t(getIdentity(a.ref.load(), b.ref.load(), a.name, b.name), token);
    }
    else { // cmp_value
        compare_t(getIdentity(a.ref, b.ref, a.name, b.name), token);
    }
}

/**
 * @brief Compares two tuples of member descriptors element-by-element via a fold over an index sequence.
 *
 * @tparam a_tuple_type The tuple type of the first set of member descriptors
 * @tparam b_tuple_type The tuple type of the second set of member descriptors
 * @tparam I The compile-time index pack enumerating the tuple elements to compare
 * @param a The first tuple of member descriptors
 * @param b The second tuple of member descriptors
 * @param token The token holding the expectation and accumulating the per-member comparison results
 *
 * @note The trailing std::index_sequence<I...> argument only carries the indices I... that
 *       drive the comparison fold; it has no runtime value.
 */
template <typename a_tuple_type, typename b_tuple_type, std::size_t... I>
void g_compare_members_impl(
    const a_tuple_type &a,
    const b_tuple_type &b,
    GToken &token,
    [[maybe_unused]] std::index_sequence<I...> indices
) {
    (g_compare_one(std::get<I>(a), std::get<I>(b), token), ...);
}

/**
 * @brief Compares each local member pairwise, recording results in the token.
 *
 * @tparam a_tuple_type The tuple type of the first object's local member descriptors
 * @tparam b_tuple_type The tuple type of the second object's local member descriptors
 * @param a The first object's tuple of local member descriptors (e.g. from localMembers())
 * @param b The second object's tuple of local member descriptors (e.g. from localMembers())
 * @param token The token holding the expectation and accumulating the per-member comparison results
 */
template <typename a_tuple_type, typename b_tuple_type>
void g_compare_members(a_tuple_type a, b_tuple_type b, GToken &token) {
    static_assert(
        std::tuple_size_v<a_tuple_type> == std::tuple_size_v<b_tuple_type>,
        "g_compare_members: localMembers() arity mismatch"
    );
    g_compare_members_impl(
        a,
        b,
        token,
        std::make_index_sequence<std::tuple_size_v<a_tuple_type>>{}
    );
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common */
