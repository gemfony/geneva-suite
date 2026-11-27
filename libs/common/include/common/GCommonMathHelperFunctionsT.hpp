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
#include <typeinfo>
#include <utility>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GChecks.hpp"
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GTypeTraitsT.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * The rules shared by the three range checks below -- checkRangeCompliance(), checkValueRange()
 * and enforceRangeConstraint() -- stated once.
 */
namespace detail {

/**
 * @brief The prefix of a range message naming the caller: "[caller] ", or nothing for "" or "empty".
 *
 * @param caller The name of the calling context
 * @return The prefix
 */
inline std::string rangeCaller(const std::string &caller) {
    return (caller.empty() || caller == "empty") ? std::string() : "[" + caller + "] ";
}

/**
 * @brief Checks whether a value lies in a range; the one body of the range checks.
 *
 * The boundaries must be ordered, lower <= upper; a NaN boundary orders with nothing and is
 * refused as well. A value lies in the range if it is above lower (or equal to it, unless the lower
 * boundary is open) and below upper (or equal to it, unless the upper boundary is open). NaN lies in
 * no range: every comparison with it is false. Infinities are ordinary values.
 *
 * @tparam num_type The arithmetic type of the value and boundaries
 * @param val The value to be checked
 * @param lower The lower boundary
 * @param upper The upper boundary
 * @param lower_open If true, the lower boundary itself lies outside of the range
 * @param upper_open If true, the upper boundary itself lies outside of the range
 * @param caller The name of the calling context, for the error message ("" or "empty": none)
 * @return true if val lies in the range
 * @throw gem_exception if lower <= upper does not hold
 */
template <arithmetic num_type>
bool inRange(
    num_type val,
    num_type lower,
    num_type upper,
    bool lower_open,
    bool upper_open,
    const std::string &caller
) {
    checkThat(
        lower <= upper,
        rangeCaller(caller),
        "Lower boundary > upper boundary: ",
        lower,
        " / ",
        upper
    );
    const bool above_lower = lower_open ? val > lower : val >= lower;
    const bool below_upper = upper_open ? val < upper : val <= upper;
    return above_lower && below_upper;
}

} /* namespace detail */

/******************************************************************************/
/**
 * @brief Enforces a value inside of a given range (both boundaries inclusive).
 *
 * Note that the value of @p val may change: if it falls outside [lower, upper]
 * it is clamped to the nearest boundary. A NaN has no nearest boundary and is refused.
 *
 * @tparam fp_type The floating point type of the value and boundaries
 * @param val The value to be constrained; modified in place if out of range
 * @param lower The lower (inclusive) boundary of the allowed range
 * @param upper The upper (inclusive) boundary of the allowed range
 * @param caller Optional name of the calling context, used in log/error messages ("empty" suppresses the prefix)
 * @param verbose If true, emits a warning whenever the value is clamped
 * @return The (possibly clamped) value
 * @throw gem_exception if val is NaN or lower <= upper does not hold
 */
template <std::floating_point fp_type>
fp_type enforceRangeConstraint(
    fp_type &val,
    const fp_type &lower,
    const fp_type &upper,
    const std::string &caller = "empty",
    bool verbose = false
) {
    if(detail::inRange(val, lower, upper, false, false, caller)) {
        return val;
    }

    checkThat(not std::isnan(val), detail::rangeCaller(caller), "value NaN lies in no range");
    const bool below = val < lower;
    const fp_type bound = below ? lower : upper;
    if(verbose) {
        glogger << detail::rangeCaller(caller) << "value " << val
                << (below ? " < lower boundary " : " > upper boundary ") << bound << '\n'
                << "Will be adapted to " << bound << '\n'
                << GWARNING;
    }
    val = bound;
    return val;
}

/******************************************************************************/
/**
 * @brief Checks that a given value is inside of a given set of boundaries (both inclusive).
 *
 * NaN lies in no range.
 *
 * @tparam num_type The arithmetic type of the value and boundaries
 * @param val The value to be checked
 * @param lower The lower (inclusive) boundary of the allowed range
 * @param upper The upper (inclusive) boundary of the allowed range
 * @param caller Optional name of the calling context, used in error messages ("empty" suppresses the prefix)
 * @return true if lower <= val <= upper, false otherwise
 * @throw gem_exception if lower <= upper does not hold
 */
template <arithmetic num_type>
bool checkRangeCompliance(
    const num_type &val,
    const num_type &lower,
    const num_type &upper,
    const std::string &caller = "empty"
) {
    return detail::inRange(val, lower, upper, false, false, caller);
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
 * @brief Checks whether a value is strictly better than another under a sort order.
 *
 * NaN is worse than every number, and two NaN are equal (neither is better than the other), so
 * the relation is a strict weak order for all doubles and can drive a sort.
 *
 * @param new_val The value tested for being the better one
 * @param old_val The value it is compared against
 * @param sort_order LOWERISBETTER: the smaller number is better; HIGHERISBETTER: the larger one
 * @return true if new_val is strictly better than old_val under sort_order
 */
inline bool isBetter(double new_val, double old_val, Gem::Common::sortOrder sort_order) {
    if(std::isnan(new_val)) {
        return false;
    }
    if(std::isnan(old_val)) {
        return true;
    }
    return sort_order == Gem::Common::sortOrder::HIGHERISBETTER ? new_val > old_val
                                                                : new_val < old_val;
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
 * @brief Checks that a value is contained in a given range, warning or throwing otherwise.
 *
 * NaN lies in no range and is refused also when only a warning is asked for: it is never returned
 * as a valid value.
 *
 * @tparam num_type The arithmetic type of the value and boundaries
 * @param val The value to be checked for containment
 * @param min The lower boundary (included unless lower_open)
 * @param max The upper boundary (included unless upper_open)
 * @param lower_open If true the lower boundary is exclusive (val must be strictly greater); default closed
 * @param upper_open If true the upper boundary is exclusive (val must be strictly smaller); default closed
 * @param warn_only If true, an out-of-range value only triggers a warning; otherwise it throws
 * @param var_name Optional variable name to include in the warning/error message
 * @return The value being checked (unchanged)
 * @throw gem_exception if val lies outside of the range (and warn_only is false, or val is NaN), or
 *        if min <= max does not hold
 */
template <arithmetic num_type>
num_type checkValueRange(
    num_type val,
    num_type min,
    num_type max,
    bool lower_open = false,
    bool upper_open = false,
    bool warn_only = false,
    const std::string &var_name = std::string()
) {
    if(detail::inRange(val, min, max, lower_open, upper_open, var_name)) {
        return val;
    }

    std::ostringstream range;
    range << min << (lower_open ? " (open) - " : " (closed) - ") << max
          << (upper_open ? " (open)" : " (closed)");
    const std::string name = var_name.empty() ? std::string() : " of variable " + var_name;

    checkThat(
        warn_only && not std::isnan(val),
        "Value ",
        val,
        name,
        " outside of allowed range\n",
        range.str()
    );
    glogger << "Value " << val << name << " outside of recommended range\n"
            << range.str() << '\n'
            << GWARNING;
    return val;
}

/******************************************************************************/
/**
 * @brief Finds the minimum and maximum component in a vector of undefined types.
 *
 * This function requires that x_type can be compared using the usual operators.
 *
 * @tparam x_type The element type of the vector (must be comparable)
 * @param ext_dat The vector holding the data, for which extreme values should be calculated
 * @return A std::tuple {min, max} holding the extreme values
 */
template <typename x_type>
auto getMinMax(const std::vector<x_type> &ext_dat) {
    if(ext_dat.size() < 2uz) {
        throw gem_exception(
            g_error_streamer() << "Got vector of invalid size " << ext_dat.size() << '\n'
        );
    }

    auto [min_it, max_it] = std::minmax_element(ext_dat.begin(), ext_dat.end());
    return std::tuple<x_type, x_type>{*min_it, *max_it};
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
 * @tparam tuple_element_type The tuple's component types (any arity >= 1)
 * @param ext_dat The vector holding the data, for which extreme values should be calculated
 * @return A std::tuple {min_0, max_0, min_1, max_1, ...} holding the per-component extreme values
 */
template <typename... tuple_element_type>
auto getMinMax(const std::vector<std::tuple<tuple_element_type...>> &ext_dat) {
    // Do some error checking
    if(ext_dat.size() < 2uz) {
        throw gem_exception(
            g_error_streamer() << "Got vector of invalid size " << ext_dat.size() << '\n'
        );
    }

    // Seed every {min, max} pair from the first entry: {min_0, max_0, min_1, max_1, ...}
    auto result = [&]<std::size_t... INDICES>(std::index_sequence<INDICES...>) {
        return std::tuple_cat([&] {
            const auto &v = std::get<INDICES>(ext_dat.at(0));
            return std::tuple<tuple_element_type, tuple_element_type>{v, v};
        }()...);
    }(std::index_sequence_for<tuple_element_type...>{});

    for(std::size_t i = 1; i < ext_dat.size(); i++) {
        [&]<std::size_t... INDICES>(std::index_sequence<INDICES...>) {
            (
                [&] {
                    const auto &v = std::get<INDICES>(ext_dat.at(i));
                    auto &mn = std::get<2 * INDICES>(result);
                    auto &mx = std::get<2 * INDICES + 1>(result);
                    if(v < mn) {
                        mn = v;
                    }
                    if(v > mx) {
                        mx = v;
                    }
                }(),
                ...);
        }(std::index_sequence_for<tuple_element_type...>{});
    }

    return result;
}

/******************************************************************************/
/**
 * @brief Calculates the mean value from a std::vector of (floating point) values.
 *
 * @tparam T The element type; must support addition and division by a scalar
 * @param par_vec The vector of values for which the mean should be calculated (must be non-empty)
 * @return The arithmetic mean of the values stored in par_vec
 */
template <typename T>
T GMean(const std::vector<T> &par_vec) {
    checkThat(not par_vec.empty(), "par_vec has size 0");

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
    // GMean throws if par_vec is empty
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
 * called, @p a will have changed. A size mismatch throws.
 *
 * @tparam T The element type; must support binary subtraction
 * @param a The vector from whose elements numbers will be subtracted (modified in place)
 * @param b The vector whose elements will be subtracted from the elements of a
 */
template <typename T>
void subtractVec(std::vector<T> &a, const std::vector<T> &b) {
    checkThat(a.size() == b.size(), "Found invalid sizes: ", a.size(), " / ", b.size());

    std::transform(a.begin(), a.end(), b.begin(), a.begin(), [](const T &x, const T &y) {
        return x - y;
    });
}

/******************************************************************************/
/**
 * @brief Adds the second vector to the first, element by element, in place.
 *
 * Note that we assume here that T understands operator+ . After this function has been
 * called, @p a will have changed. A size mismatch throws.
 *
 * @tparam T The element type; must support binary addition
 * @param a The vector to whose elements numbers will be added (modified in place)
 * @param b The vector whose elements will be added to the elements of a
 */
template <typename T>
void addVec(std::vector<T> &a, const std::vector<T> &b) {
    checkThat(a.size() == b.size(), "Found invalid sizes: ", a.size(), " / ", b.size());

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
 * The line is defined by L(x) = a + b*x. An empty input yields an all-zero result tuple. The errors
 * need at least three points: for two they are not finite (a line through two points has no
 * deviation to estimate them from).
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
        throw gem_exception(g_error_streamer() << "Attempted division by 0." << '\n');
    }

    fp_type sleep_time = std::get<0>(s);

    // Check that the sleep-times for s and p are the same
    if(sleep_time != std::get<0>(p)) {
        throw gem_exception(
            g_error_streamer() << "Sleep times differ: " << sleep_time << " / " << std::get<0>(p)
                               << '\n'
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
        throw gem_exception(
            g_error_streamer() << "Vectors have invalid sizes: " << sn.size() << " / " << pn.size()
                               << '\n'
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
bool isClose(fp_type val, fp_type target = 0, fp_type margin = static_cast<fp_type>(0.00001)) {
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
 * for precision near the barrier. Precondition: steepness > 0 (checked).
 *
 * @tparam fp_type The floating point type of the argument and result
 * @param var       Input value
 * @param barrier   Asymptotic limit; output stays strictly within (-barrier, +barrier)
 * @param steepness Controls convergence speed; larger → slower approach to barrier (must be > 0)
 * @return The softsign value, strictly inside (-barrier, +barrier)
 */
template <std::floating_point fp_type>
fp_type grational_sigmoid(fp_type var, fp_type barrier, fp_type steepness) {
    checkThat(steepness > fp_type(0), "steepness must be > 0, got ", steepness);
    const auto lvar = static_cast<long double>(var);
    const auto lbarrier = static_cast<long double>(barrier);
    const auto lsteepness = static_cast<long double>(steepness);
    return static_cast<fp_type>(lbarrier * lvar / (lsteepness + std::abs(lvar)));
}

/******************************************************************************/

} /* namespace Gem::Common */
