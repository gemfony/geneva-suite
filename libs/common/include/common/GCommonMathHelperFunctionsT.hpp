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
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <ranges>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <typeinfo>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief Enforces a value inside of a given range (both boundaries inclusive).
 *
 * Note that the value of @p val may change: if it falls outside [lower, upper]
 * it is clamped to the nearest boundary.
 *
 * @tparam fp_type The floating point type of the value and boundaries
 * @param val The value to be constrained; modified in place if out of range
 * @param lower The lower (inclusive) boundary of the allowed range
 * @param upper The upper (inclusive) boundary of the allowed range
 * @param caller Optional name of the calling context, used in log/error messages ("empty" suppresses the prefix)
 * @param verbose If true, emits a warning whenever the value is clamped
 * @return The (possibly clamped) value
 */
template <std::floating_point fp_type>
fp_type enforceRangeConstraint(
    fp_type &val,
    const fp_type &lower,
    const fp_type &upper,
    const std::string &caller = "empty",
    bool verbose = false
) {
    if(lower > upper) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << (caller == "empty" ? "" : ("[" + caller + "] "))
            << "In enforceRangeConstraint<fp_type>(): Error!" << '\n'
            << "Lower boundary > upper boundary: " << lower << " / " << upper << '\n'
        );
    }

    if(val < lower) {
        if(verbose) {
            glogger << (caller == "empty" ? "" : ("[" + caller + "] "))
                    << "In Gem::Common::enforceRangeConstraint(): " << '\n'
                    << "value " << val << " < lower boundary " << lower << '\n'
                    << "Will be adapted to " << lower << '\n'
                    << GWARNING;
        }
        val = lower;
    }
    else if(val > upper) {
        if(verbose) {
            glogger << (caller == "empty" ? "" : ("[" + caller + "] "))
                    << "In Gem::Common::enforceRangeConstraint(): " << '\n'
                    << "value " << val << " > upper boundary " << upper << '\n'
                    << "Will be adapted to " << upper << '\n'
                    << GWARNING;
        }
        val = upper;
    }

    return val;
}

/******************************************************************************/
/**
 * @brief Checks that a given floating point value is inside of a given set of boundaries (both inclusive).
 *
 * @tparam fp_type The floating point type of the value and boundaries
 * @param val The value to be checked
 * @param lower The lower (inclusive) boundary of the allowed range
 * @param upper The upper (inclusive) boundary of the allowed range
 * @param caller Optional name of the calling context, used in error messages ("empty" suppresses the prefix)
 * @return true if lower <= val <= upper, false otherwise
 */
template <std::floating_point fp_type>
bool checkRangeCompliance(
    const fp_type &val,
    const fp_type &lower,
    const fp_type &upper,
    const std::string &caller = "empty"
) {
    if(lower > upper) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << (caller == "empty" ? "" : ("[" + caller + "] "))
            << "In checkRangeCompliance<fp_type>(...): Error!" << '\n'
            << "Lower boundary > upper boundary: " << lower << " / " << upper << '\n'
        );
    }

    return not(val < lower || val > upper);
}

/******************************************************************************/
/**
 * @brief Checks that a given integral value is inside of a given set of boundaries (both inclusive).
 *
 * @tparam int_type The integral type of the value and boundaries
 * @param val The value to be checked
 * @param lower The lower (inclusive) boundary of the allowed range
 * @param upper The upper (inclusive) boundary of the allowed range
 * @param caller Optional name of the calling context, used in error messages ("empty" suppresses the prefix)
 * @return true if lower <= val <= upper, false otherwise
 */
template <std::integral int_type>
bool checkRangeCompliance(
    const int_type &val,
    const int_type &lower,
    const int_type &upper,
    const std::string &caller = "empty"
) {
    if(lower > upper) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << (caller == "empty" ? "" : ("[" + caller + "] "))
            << "In checkRangeCompliance<int_type>(...): Error!" << '\n'
            << "Lower boundary > upper boundary: " << lower << " / " << upper << '\n'
        );
    }

    return not(val < lower || val > upper);
}

/******************************************************************************/
/**
 * @brief Retrieves the worst known value for a given floating point type.
 *
 * @tparam fp_type The floating point type whose worst-case value is requested
 * @param max_mode If true, higher values are better (worst = lowest()); if false, lower is better (worst = max())
 * @return The worst representable value of fp_type for the given optimization direction
 */
template <std::floating_point fp_type>
fp_type getWorstCase(bool max_mode) {
    return (
        max_mode ? std::numeric_limits<fp_type>::lowest() : std::numeric_limits<fp_type>::max()
    );
}

/******************************************************************************/
/**
 * @brief Retrieves the best known value for a given floating point type.
 *
 * @tparam fp_type The floating point type whose best-case value is requested
 * @param max_mode If true, higher values are better (best = max()); if false, lower is better (best = lowest())
 * @return The best representable value of fp_type for the given optimization direction
 */
template <std::floating_point fp_type>
fp_type getBestCase(bool max_mode) {
    return (
        max_mode ? std::numeric_limits<fp_type>::max() : std::numeric_limits<fp_type>::lowest()
    );
}

/******************************************************************************/
/**
 * @brief Retrieves the worst known value for a given floating point type.
 *
 * @tparam fp_type The floating point type whose worst-case value is requested
 * @param sort_order The optimization direction; HIGHERISBETTER -> worst = lowest(), otherwise worst = max()
 * @return The worst representable value of fp_type for the given sort order
 */
template <std::floating_point fp_type>
fp_type getWorstCase(Gem::Common::sortOrder sort_order) {
    return (
        sort_order == Gem::Common::sortOrder::HIGHERISBETTER
            ? std::numeric_limits<fp_type>::lowest()
            : std::numeric_limits<fp_type>::max()
    );
}

/******************************************************************************/
/**
 * @brief Retrieves the best known value for a given floating point type.
 *
 * @tparam fp_type The floating point type whose best-case value is requested
 * @param sort_order The optimization direction; HIGHERISBETTER -> best = max(), otherwise best = lowest()
 * @return The best representable value of fp_type for the given sort order
 */
template <std::floating_point fp_type>
fp_type getBestCase(Gem::Common::sortOrder sort_order) {
    return (
        sort_order == Gem::Common::sortOrder::HIGHERISBETTER
            ? std::numeric_limits<fp_type>::max()
            : std::numeric_limits<fp_type>::lowest()
    );
}

/******************************************************************************/
/**
 * @brief Named convenience constants for the boundary-openness and warning flags of checkValueRange().
 */
constexpr bool GFPLOWERCLOSED = false;
constexpr bool GFPLOWEROPEN = true;
constexpr bool GFPUPPERCLOSED = false;
constexpr bool GFPUPPEROPEN = true;
constexpr bool GFNOWARNING = false;

/**
 * @brief Checks that a floating point value is contained in a given range, warning or throwing otherwise.
 *
 * @tparam fp_type The floating point type of the value and boundaries
 * @param val The value to be checked for containment
 * @param min The lower boundary (included unless lower_open)
 * @param max The upper boundary (included unless upper_open)
 * @param lower_open If true the lower boundary is exclusive (val must be strictly greater); default closed
 * @param upper_open If true the upper boundary is exclusive (val must be strictly smaller); default closed
 * @param warn_only If true, an out-of-range value only triggers a warning; otherwise it throws
 * @param var_name Optional variable name to include in the warning/error message
 * @return The value being checked (unchanged)
 */
template <std::floating_point fp_type>
fp_type checkValueRange(
    fp_type val,
    fp_type min,
    fp_type max,
    bool lower_open = false,
    bool upper_open = false,
    bool warn_only = false,
    const std::string& var_name = std::string()
) {
    bool in_value_range = true;

    if(lower_open) {
        if(val < std::nextafter(min, std::numeric_limits<fp_type>::infinity())) {
            in_value_range = false;
        }
    }
    else {
        if(val < min) {
            in_value_range = false;
        }
    }

    if(upper_open) {
        if(val > std::nextafter(max, -std::numeric_limits<fp_type>::infinity())) {
            in_value_range = false;
        }
    }
    else {
        if(val > max) {
            in_value_range = false;
        }
    }

    if(not in_value_range) {
        if(warn_only) {
            glogger << "In checkValueRange<fp_type>(): Error!" << '\n'
                    << "Value " << val << (var_name.empty() ? "" : (" of variable " + var_name))
                    << " outside of recommended range " << '\n'
                    << min << (lower_open ? " (open) - " : " (closed) - ") << max
                    << (upper_open ? " (open)" : " (closed)") << '\n'
                    << GWARNING;
        }
        else {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In checkValueRange<fp_type>(): Error!" << '\n'
                << "Value " << val << (var_name.empty() ? "" : (" of variable " + var_name))
                << " outside of allowed range " << '\n'
                << min << (lower_open ? " (open) - " : " (closed) - ") << max
                << (upper_open ? " (open)" : " (closed)") << '\n'
            );
        }
    }

    return val;
}

/******************************************************************************/
/**
 * @brief Checks that an integral value is contained in a given range, warning or throwing otherwise.
 *
 * @tparam int_type The integral type of the value and boundaries
 * @param val The value to be checked for containment
 * @param min The lower boundary (included unless lower_open)
 * @param max The upper boundary (included unless upper_open)
 * @param lower_open If true the lower boundary is exclusive (val must be strictly greater); default closed
 * @param upper_open If true the upper boundary is exclusive (val must be strictly smaller); default closed
 * @param warn_only If true, an out-of-range value only triggers a warning; otherwise it throws
 * @return The value being checked (unchanged)
 */
template <std::integral int_type>
int_type checkValueRange(
    int_type val,
    int_type min,
    int_type max,
    bool lower_open = false,
    bool upper_open = false,
    bool warn_only = false
) {
    bool in_value_range = true;

    if(lower_open) {
        if(val <= min) {
            in_value_range = false;
        }
    }
    else {
        if(val < min) {
            in_value_range = false;
        }
    }

    if(upper_open) {
        if(val >= max) {
            in_value_range = false;
        }
    }
    else {
        if(val > max) {
            in_value_range = false;
        }
    }

    if(not in_value_range) {
        if(warn_only) {
            glogger << "Warning:" << '\n'
                    << "In checkValueRange<int_type>(): Error!" << '\n'
                    << "Value " << val << " outside of recommended range " << '\n'
                    << min << (lower_open ? " (open) - " : " (closed) - ") << max
                    << (upper_open ? " (open)" : " (closed)") << '\n'
                    << GWARNING;
        }
        else {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In checkValueRange<int_type>(): Error!" << '\n'
                << "Value " << val << " outside of allowed range " << '\n'
                << min << (lower_open ? " (open) - " : " (closed) - ") << max
                << (upper_open ? " (open)" : " (closed)") << '\n'
            );
        }
    }

    return val;
}

/******************************************************************************/
/**
 * @brief Finds the minimum and maximum component in a vector of undefined types.
 *
 * This function requires that x_type_undet can be compared using the usual operators.
 *
 * @tparam x_type_undet The element type of the vector (must be comparable)
 * @param ext_dat The vector holding the data, for which extreme values should be calculated
 * @return A std::tuple {min, max} holding the extreme values
 */
template <typename x_type_undet>
auto getMinMax(const std::vector<x_type_undet> &ext_dat) {
    if(ext_dat.size() < 2uz) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GBasePlotter::getMinMax(1D): Error!" << '\n'
            << "Got vector of invalid size " << ext_dat.size() << '\n'
        );
    }

    auto [min_it, max_it] = std::minmax_element(ext_dat.begin(), ext_dat.end());
    return std::tuple<x_type_undet, x_type_undet>{*min_it, *max_it};
}

/******************************************************************************/
/**
 * @brief Find the per-component minimum and maximum in a vector of tuples of undefined types.
 *
 * One variadic implementation serves every tuple arity (the former separate 2D/3D/4D overloads
 * were scaled copy-paste of the same per-component min/max loop). Each component type must be
 * comparable with the usual operators; the comparison shape (a plain `<`/`>` pair seeded from the
 * first entry) is preserved exactly, so NaN handling is unchanged.
 *
 * @tparam tuple_types The tuple's component types (any arity >= 1)
 * @param ext_dat The vector holding the data, for which extreme values should be calculated
 * @return A std::tuple {min_0, max_0, min_1, max_1, ...} holding the per-component extreme values
 */
template <typename... tuple_types>
auto getMinMax(const std::vector<std::tuple<tuple_types...>> &ext_dat) {
    // Do some error checking
    if(ext_dat.size() < 2uz) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GBasePlotter::getMinMax(" << sizeof...(tuple_types) << "D): Error!" << '\n'
            << "Got vector of invalid size " << ext_dat.size() << '\n'
        );
    }

    // Seed every {min, max} pair from the first entry: {min_0, max_0, min_1, max_1, ...}
    auto result = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::tuple_cat([&] {
            const auto &v = std::get<Is>(ext_dat.at(0));
            return std::tuple<tuple_types, tuple_types>{v, v};
        }()...);
    }(std::index_sequence_for<tuple_types...>{});

    for(std::size_t i = 1; i < ext_dat.size(); i++) {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                [&] {
                    const auto &v = std::get<Is>(ext_dat.at(i));
                    auto &mn = std::get<2 * Is>(result);
                    auto &mx = std::get<2 * Is + 1>(result);
                    if(v < mn) {
                        mn = v;
                    }
                    if(v > mx) {
                        mx = v;
                    }
                }(),
                ...
            );
        }(std::index_sequence_for<tuple_types...>{});
    }

    return result;
}

/******************************************************************************/
/**
 * @brief Calculates the mean value from a std::vector of (floating point) values.
 *
 * @tparam T The element type; must support addition and division by a scalar
 * @param par_vec The vector of values for which the mean should be calculated (must be non-empty in DEBUG)
 * @return The arithmetic mean of the values stored in par_vec
 */
template <typename T>
T GMean(const std::vector<T> &par_vec) {
#ifdef DEBUG
    if(par_vec.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In T GMean(const std::vector<T>&): Error!" << '\n'
            << "par_vec has size 0" << '\n'
        );
    }
#endif /* DEBUG */

    T mean = T(0);
    for(const auto &v : par_vec) {
        mean += v;
    }
    return mean / static_cast<T>(par_vec.size());
}

/******************************************************************************/
/**
 * @brief Calculates the mean and standard deviation for a std::vector of (floating point) values.
 *
 * Uses the sample standard deviation (division by n-1). For a single-element vector the
 * standard deviation is reported as 0.
 *
 * @tparam T The element type; must support the usual arithmetic operators and std::sqrt
 * @param par_vec The vector of values for which the standard deviation should be calculated
 * @return A std::tuple {mean, sigma} holding the mean value and the standard deviation of par_vec
 */
template <typename T>
auto GStandardDeviation(const std::vector<T> &par_vec) {
    // GMean will throw in DEBUG mode if par_vec is empty
    T mean = GMean(par_vec);
    T sigma = T(0);

    // It is easy if the size is 1
    if(par_vec.size() == 1) {
        return std::tuple<T, T>{par_vec.at(0), T(0)};
    }

    for(const auto &v : par_vec) {
        const auto d = v - mean;
        sigma += d * d;
    }
    sigma /= T(par_vec.size() - 1);
    sigma = std::sqrt(sigma);

    return std::tuple<T, T>{mean, sigma};
}

/******************************************************************************/
/**
 * @brief Compile-time integer power: B^E.
 *
 * Replaces the old PowSmallPosInt struct template metaprogramming.
 *
 * @tparam B The base of the power
 * @tparam E The exponent of the power
 * @return The value of B raised to the power E, computed at compile time
 */
template <std::size_t B, std::size_t E>
constexpr std::size_t PowSmallPosInt() {
    if constexpr(E == 0) {
        return 1uz;
    }
    else if constexpr(E == 1) {
        return B;
    }
    else {
        return B * PowSmallPosInt<B, E - 1>();
    }
}

/******************************************************************************/
/**
 * @brief Subtracts the second vector from the first, element by element, in place.
 *
 * Note that we assume here that T understands operator- . After this function has been
 * called, @p a will have changed. In DEBUG mode a size mismatch throws.
 *
 * @tparam T The element type; must support binary subtraction
 * @param a The vector from whose elements numbers will be subtracted (modified in place)
 * @param b The vector whose elements will be subtracted from the elements of a
 */
template <typename T>
void subtractVec(std::vector<T> &a, const std::vector<T> &b) {
#ifdef DEBUG
    if(a.size() != b.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In subtractVec(std::vector<T>, const std::vector<T>&): Error!" << '\n'
            << "Found invalid sizes: " << a.size() << " / " << b.size() << '\n'
        );
    }
#endif /* DEBUG */

    std::transform(a.begin(), a.end(), b.begin(), a.begin(), [](const T &x, const T &y) {
        return x - y;
    });
}

/******************************************************************************/
/**
 * @brief Adds the second vector to the first, element by element, in place.
 *
 * Note that we assume here that T understands operator+ . After this function has been
 * called, @p a will have changed. In DEBUG mode a size mismatch throws.
 *
 * @tparam T The element type; must support binary addition
 * @param a The vector to whose elements numbers will be added (modified in place)
 * @param b The vector whose elements will be added to the elements of a
 */
template <typename T>
void addVec(std::vector<T> &a, const std::vector<T> &b) {
#ifdef DEBUG
    if(a.size() != b.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In addVec(std::vector<T>, const std::vector<T>&): Error!" << '\n'
            << "Found invalid sizes: " << a.size() << " / " << b.size() << '\n'
        );
    }
#endif /* DEBUG */

    std::transform(a.begin(), a.end(), b.begin(), a.begin(), [](const T &x, const T &y) {
        return x + y;
    });
}

/******************************************************************************/
/**
 * @brief Multiplies each element of a std::vector<> by a constant, in place.
 *
 * Note that we assume here that T understands operator*= . After this function has been
 * called, @p a will have changed.
 *
 * @tparam T The element type; must support operator*=
 * @param a The vector whose elements will be multiplied by c (modified in place)
 * @param c The constant which will be multiplied with each position of a
 */
template <typename T>
void multVecConst(std::vector<T> &a, const T &c) {
    for(auto &v : a) {
        v *= c;
    }
}

/******************************************************************************/
/**
 * @brief Assigns a constant value to each position of the vector.
 *
 * @tparam T The element type; must be assignable from c
 * @param a The vector to whose elements c will be assigned (modified in place)
 * @param c The constant which will be assigned to each position of a
 */
template <typename T>
void assignVecConst(std::vector<T> &a, const T &c) {
    std::fill(a.begin(), a.end(), c);
}

/******************************************************************************/
/**
 * @brief Sums up the x- and y-components individually of a vector of 2d-tuples.
 *
 * @tparam fp_type The floating point type of the tuple components
 * @param data_points The vector of (x, y) tuples to be summed component-wise
 * @return A std::tuple {sum_x, sum_y} holding the per-component sums
 */
template <typename fp_type>
std::tuple<fp_type, fp_type>
sumTupleVec(const std::vector<std::tuple<fp_type, fp_type>> &data_points) {
    std::tuple<fp_type, fp_type> result{fp_type(0.), fp_type(0.)};
    for(const auto &p : data_points) {
        std::get<0>(result) += std::get<0>(p);
        std::get<1>(result) += std::get<1>(p);
    }
    return result;
}

/******************************************************************************/
/**
 * @brief Sums up the squares of the x- and y-components individually of a vector of 2d-tuples.
 *
 * @tparam fp_type The floating point type of the tuple components
 * @param data_points The vector of (x, y) tuples whose squared components are summed
 * @return A std::tuple {sum_x^2, sum_y^2} holding the per-component sums of squares
 */
template <typename fp_type>
std::tuple<fp_type, fp_type>
squareSumTupleVec(const std::vector<std::tuple<fp_type, fp_type>> &data_points) {
    std::tuple<fp_type, fp_type> result{fp_type(0.), fp_type(0.)};
    for(const auto &p : data_points) {
        const auto x = std::get<0>(p);
        const auto y = std::get<1>(p);
        std::get<0>(result) += x * x;
        std::get<1>(result) += y * y;
    }
    return result;
}

/******************************************************************************/
/**
 * @brief Sums up the products of the x- and y-components of a vector of 2d-tuples.
 *
 * @tparam fp_type The floating point type of the tuple components
 * @param data_points The vector of (x, y) tuples; each x*y product is accumulated
 * @return The sum over all data points of x*y
 */
template <std::floating_point fp_type>
fp_type productSumTupleVec(const std::vector<std::tuple<fp_type, fp_type>> &data_points) {
    fp_type result = fp_type(0.);
    for(const auto &p : data_points) {
        result += std::get<0>(p) * std::get<1>(p);
    }
    return result;
}

/******************************************************************************/
/**
 * @brief Calculates the "square deviation" of a set of floating point tuples from a line a + b*x.
 *
 * @tparam fp_type The floating point type of the data and line parameters
 * @param data_points A vector of bi-tuples with (x, y) data points
 * @param a The offset (intercept) of the line
 * @param b The slope of the line
 * @return The sum of squared residuals of the data points from the line
 */
template <std::floating_point fp_type>
fp_type squareDeviation(
    const std::vector<std::tuple<fp_type, fp_type>> &data_points,
    const fp_type &a,
    const fp_type &b
) {
    fp_type result = fp_type(0);
    for(const auto &p : data_points) {
        const auto d = std::get<1>(p) - a - (b * std::get<0>(p));
        result += d * d;
    }
    return result;
}

/******************************************************************************/
/**
 * @brief Calculates the parameters a and b of a regression line, plus their errors.
 *
 * The line is defined by L(x) = a + b*x. An empty input yields an all-zero result tuple.
 *
 * @tparam fp_type The floating point type of the data and the computed parameters
 * @param data_points A vector of (x, y) data points to which the line's parameters should fit
 * @return A std::tuple {a, error_a, b, error_b} of intercept, slope and their respective errors
 */
template <typename fp_type>
auto getRegressionParameters(const std::vector<std::tuple<fp_type, fp_type>> &data_points) {
    if(data_points.empty()) {
        return std::tuple<fp_type, fp_type, fp_type, fp_type>{
            fp_type(0.),
            fp_type(0.),
            fp_type(0.),
            fp_type(0.)
        };
    }

    fp_type a = fp_type(0);
    fp_type b = fp_type(0);
    fp_type n = fp_type(data_points.size());

    std::tuple<fp_type, fp_type> sum_xy = sumTupleVec(data_points);
    fp_type sum_x = std::get<0>(sum_xy);
    fp_type sum_y = std::get<1>(sum_xy);

    std::tuple<fp_type, fp_type> sq_sum_xy = squareSumTupleVec(data_points);
    fp_type sq_sum_x = std::get<0>(sq_sum_xy);

    fp_type prod_sum_xy = productSumTupleVec(data_points);

    const fp_type denom = (n * sq_sum_x) - (sum_x * sum_x);
    a = (sum_y * sq_sum_x - sum_x * prod_sum_xy) / denom;
    b = (n * prod_sum_xy - sum_x * sum_y) / denom;

    fp_type dev = squareDeviation(data_points, a, b);

    fp_type sigma_a = std::sqrt(dev / (n - fp_type(2.))) * std::sqrt(sq_sum_x / denom);
    fp_type sigma_b = std::sqrt(dev / (n - fp_type(2.))) * std::sqrt(n / denom);

    return std::tuple<fp_type, fp_type, fp_type, fp_type>{a, sigma_a, b, sigma_b};
}

/******************************************************************************/
/**
 * @brief Calculates the value and error of f = s/p for two independent measured quantities.
 *
 * Each input is a tuple {sleep_time, sleep_time_error, value, value_error}; s and p must
 * carry the same sleep_time, and p's value must be non-zero (otherwise this throws).
 * The error is propagated assuming s and p are independent.
 *
 * @tparam fp_type The floating point type of the tuple components
 * @param s The numerator measurement as {sleep_time, sleep_time_error, value, value_error}
 * @param p The denominator measurement as {sleep_time, sleep_time_error, value, value_error}
 * @return A std::tuple {sleep_time, 0, s/p, error_on_s/p}
 */
template <typename fp_type>
auto getRatioError(
    const std::tuple<fp_type, fp_type, fp_type, fp_type> &s,
    const std::tuple<fp_type, fp_type, fp_type, fp_type> &p
) {
    // p may not be 0
    if(0. == std::get<2>(p)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace()) << "In getRatioError(): Error!" << '\n'
                                                     << "Attempted division by 0." << '\n'
        );
    }

    fp_type sleep_time = std::get<0>(s);

    // Check that the sleep-times for s and p are the same
    if(sleep_time != std::get<0>(p)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In getRatioError(): Error!" << '\n'
            << "Sleep times differ: " << sleep_time << " / " << std::get<0>(p) << '\n'
        );
    }

    const fp_type s_val = std::get<2>(s);
    const fp_type s_err = std::get<3>(s);
    const fp_type p_val = std::get<2>(p);
    const fp_type p_err = std::get<3>(p);

    const fp_type s_err_term = s_err / p_val;
    const fp_type p_err_term = s_val * p_err / (p_val * p_val);

    return std::tuple<fp_type, fp_type, fp_type, fp_type>{
        sleep_time,
        fp_type(0.),
        s_val / p_val,
        std::sqrt((s_err_term * s_err_term) + (p_err_term * p_err_term))
    };
}

/******************************************************************************/
/**
 * @brief Calculates value and error of f = s/p for matched vectors of independent measurements.
 *
 * Applies getRatioError() element-wise to corresponding entries of @p sn and @p pn.
 * Both vectors must have the same size, otherwise this throws.
 *
 * @tparam fp_type The floating point type of the tuple components
 * @param sn The vector of numerator measurements {sleep_time, sleep_time_error, value, value_error}
 * @param pn The vector of denominator measurements {sleep_time, sleep_time_error, value, value_error}
 * @return A vector of {sleep_time, 0, s/p, error_on_s/p} tuples, one per input pair
 */
template <typename fp_type>
std::vector<std::tuple<fp_type, fp_type, fp_type, fp_type>> getRatioErrors(
    const std::vector<std::tuple<fp_type, fp_type, fp_type, fp_type>> &sn,
    const std::vector<std::tuple<fp_type, fp_type, fp_type, fp_type>> &pn
) {
    // Check that both vectors have the same size, otherwise complain
    if(sn.size() != pn.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In getRatioErrors(): Error!" << '\n'
            << "Vectors have invalid sizes: " << sn.size() << " / " << pn.size() << '\n'
        );
    }

    std::vector<std::tuple<fp_type, fp_type, fp_type, fp_type>> spn;
    spn.reserve(sn.size());
    for(auto const &[s, p] : std::views::zip(sn, pn)) {
        spn.push_back(getRatioError(s, p));
    }

    return spn;
}

/******************************************************************************/
/**
 * @brief Checks whether a floating point value is "close" to a target value within a margin.
 *
 * @tparam fp_type The floating point type of the values
 * @param val The value to be tested
 * @param target The reference value to compare against (default 0)
 * @param margin The maximum permitted absolute difference for the values to count as close (default 0.00001)
 * @return true if |val - target| <= margin, false otherwise
 */
template <std::floating_point fp_type>
bool isClose(
    fp_type val,
    fp_type target = 0,
    fp_type margin = static_cast<fp_type>(0.00001)
) {
    return (std::abs(val - target) <= margin);
}

/******************************************************************************/
/**
 * @brief Rational (algebraic) sigmoid — the Gjl-softsign function.
 *
 * See http://en.wikipedia.org/wiki/File:Gjl-t%28x%29.svg .
 *
 * NOT the logistic sigmoid (1/(1+e^-x)). This is a softsign:
 *   f(var) = barrier * var / (steepness + |var|)
 * which maps ℝ → (-barrier, +barrier) antisymmetrically and approaches its
 * asymptotes polynomially (not exponentially). Uses long double internally
 * for precision near the barrier. Precondition: steepness > 0 (checked in DEBUG).
 *
 * @tparam fp_type The floating point type of the argument and result
 * @param var       Input value
 * @param barrier   Asymptotic limit; output stays strictly within (-barrier, +barrier)
 * @param steepness Controls convergence speed; larger → slower approach to barrier (must be > 0)
 * @return The softsign value, strictly inside (-barrier, +barrier)
 */
template <std::floating_point fp_type>
fp_type grational_sigmoid(fp_type var, fp_type barrier, fp_type steepness) {
#ifdef DEBUG
    if(steepness <= fp_type(0)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "grational_sigmoid(): steepness must be > 0, got " << steepness << '\n'
        );
    }
#endif
    const auto lvar = static_cast<long double>(var);
    const auto lbarrier = static_cast<long double>(barrier);
    const auto lsteepness = static_cast<long double>(steepness);
    return static_cast<fp_type>(lbarrier * lvar / (lsteepness + std::abs(lvar)));
}

/******************************************************************************/

} /* namespace Gem::Common */
