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
#include <cstdint>
#include <string>

// Boost headers go here

// Geneva headers go here

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief Maps a C++ type to a human-readable type name string.
 *
 * The primary template returns "unknown"; explicit specializations below
 * provide proper names for the common arithmetic and string types. Useful
 * e.g. for debugging output.
 *
 * @tparam T The type whose name string is requested
 */
template <typename T>
struct GTypeToStringT {
    /**
     * @brief Returns the type name for the (unspecialized) type T.
     * @return The literal string "unknown"
     */
    static std::string value() {
        return {"unknown"};
    }
};

/******************************************************************************/
/**
 * @brief Specialization for T == double
 */
template <>
struct GTypeToStringT<double> {
    /**
     * @brief Returns the type name for double.
     * @return The literal string "double"
     */
    static std::string value() {
        return {"double"};
    }
};

/******************************************************************************/
/**
 * @brief Specialization for T == float
 */
template <>
struct GTypeToStringT<float> {
    /**
     * @brief Returns the type name for float.
     * @return The literal string "float"
     */
    static std::string value() {
        return {"float"};
    }
};

/******************************************************************************/
/**
 * @brief Specialization for T == std::int32_t
 */
template <>
struct GTypeToStringT<std::int32_t> {
    /**
     * @brief Returns the type name for std::int32_t.
     * @return The literal string "int32_t"
     */
    static std::string value() {
        return {"int32_t"};
    }
};

/******************************************************************************/
/**
 * @brief Specialization for T == bool
 */
template <>
struct GTypeToStringT<bool> {
    /**
     * @brief Returns the type name for bool.
     * @return The literal string "bool"
     */
    static std::string value() {
        return {"bool"};
    }
};

/******************************************************************************/
/**
 * @brief Specialization for T == std::string
 */
template <>
struct GTypeToStringT<std::string> {
    /**
     * @brief Returns the type name for std::string.
     * @return The literal string "string"
     */
    static std::string value() {
        return {"string"};
    }
};

/******************************************************************************/

} // namespace Gem::Common
