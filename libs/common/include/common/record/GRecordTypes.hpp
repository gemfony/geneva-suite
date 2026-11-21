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
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

// Boost header files go here

// Geneva header files go here

/******************************************************************************/
/**
 * @namespace Gem::Common::Record
 * @brief The recording framework: a process-wide recorder (recorder()) hands out streams --
 * named tables with fixed, typed columns -- to which any part of a program appends rows; the
 * rows go to sinks (a CSV file, a JSON Lines file, a plot, ...). The framework knows rows of
 * named, typed values and where they go; what a tenant records and when is the tenant's business.
 */
namespace Gem::Common::Record {

/******************************************************************************/
/**
 * Whether a stream adds an automatic time column, named "time", as its first column: none; the
 * wall clock (a time stamp, std::chrono::system_clock); or the steady clock (the seconds since
 * the stream was declared, as a floating-point value).
 */
enum class withTime { // NOLINT(readability-identifier-naming) -- the names the maintainer chose
    none,             // NOLINT(readability-identifier-naming)
    wall,             // NOLINT(readability-identifier-naming)
    steady            // NOLINT(readability-identifier-naming)
};

/******************************************************************************/
/**
 * The type of a column. The order is that of the alternatives of GValue.
 */
enum class columnType {
    INTEGER,   ///< A signed integer (std::int64_t)
    UNSIGNED,  ///< An unsigned integer (std::uint64_t)
    FLOATING,  ///< A floating-point value (double)
    BOOLEAN,   ///< A bool
    TEXT,      ///< A string
    TIMESTAMP  ///< A point in time of the wall clock (GTimeStamp)
};

/** @brief A time stamp: a point in time of the wall clock */
using GTimeStamp = std::chrono::system_clock::time_point;

/** @brief One value of a row; the index of the alternative is its columnType */
using GValue = std::variant<std::int64_t, std::uint64_t, double, bool, std::string, GTimeStamp>;

/** @brief One row: one value per column, in column order */
using GRow = std::vector<GValue>;

/******************************************************************************/
/**
 * A column of a stream: its name and its type.
 */
struct GColumn {
    std::string name; ///< The name, e.g. the header of a CSV column
    columnType type;  ///< The type of its values

    /** @brief Two columns are equal if name and type are @return Whether they are */
    bool operator==(GColumn const &) const = default;
};

/******************************************************************************/
/**
 * @brief The type of a value
 * @param value The value
 * @return Its column type
 */
[[nodiscard]] inline columnType typeOf(GValue const &value) {
    return static_cast<columnType>(value.index());
}

/******************************************************************************/
/**
 * @brief The name of a column type, for messages
 * @param type The column type
 * @return "integer", "unsigned", "floating", "boolean", "text" or "timestamp"
 */
[[nodiscard]] std::string to_string(columnType type);

/******************************************************************************/
/**
 * @brief The column type a C++ type is recorded as: bool as BOOLEAN, a signed integer as INTEGER,
 * an unsigned one as UNSIGNED, a floating-point type as FLOATING, anything that converts to a
 * std::string_view as TEXT and a point in time of the system clock as TIMESTAMP
 * @tparam T The C++ type of a typed column (GStreamT)
 * @return Its column type
 */
template <typename T>
[[nodiscard]] consteval columnType columnTypeOf() {
    using U = std::remove_cvref_t<T>;
    if constexpr(std::is_same_v<U, bool>) {
        return columnType::BOOLEAN;
    }
    else if constexpr(std::is_integral_v<U> && std::is_signed_v<U>) {
        return columnType::INTEGER;
    }
    else if constexpr(std::is_integral_v<U>) {
        return columnType::UNSIGNED;
    }
    else if constexpr(std::is_floating_point_v<U>) {
        return columnType::FLOATING;
    }
    else if constexpr(std::is_convertible_v<U const &, std::string_view>) {
        return columnType::TEXT;
    }
    else if constexpr(std::is_convertible_v<U const &, GTimeStamp>) {
        return columnType::TIMESTAMP;
    }
    else {
        static_assert(false, "This type cannot be a column of a stream");
    }
}

/******************************************************************************/
/**
 * @brief Converts a value of a typed column into the value of a row
 * @tparam T The C++ type of the column
 * @param value The value
 * @return The value as the alternative of its column type
 */
template <typename T>
[[nodiscard]] GValue toValue(T const &value) {
    constexpr columnType TYPE = columnTypeOf<T>();
    if constexpr(TYPE == columnType::BOOLEAN) {
        return GValue(std::in_place_index<static_cast<std::size_t>(TYPE)>, value);
    }
    else if constexpr(TYPE == columnType::INTEGER) {
        return GValue(std::in_place_index<0>, static_cast<std::int64_t>(value));
    }
    else if constexpr(TYPE == columnType::UNSIGNED) {
        return GValue(std::in_place_index<1>, static_cast<std::uint64_t>(value));
    }
    else if constexpr(TYPE == columnType::FLOATING) {
        return GValue(std::in_place_index<2>, static_cast<double>(value));
    }
    else if constexpr(TYPE == columnType::TEXT) {
        return GValue(std::in_place_index<4>, std::string(std::string_view(value)));
    }
    else {
        return GValue(std::in_place_index<5>, GTimeStamp(value));
    }
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
