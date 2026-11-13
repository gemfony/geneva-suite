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
#include <string>

// Boost headers go here

// Geneva headers go here

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief This class allows to specify default values for specific types
 * through specializations.
 *
 * @tparam T The type for which a default value should be provided
 */
template <typename T>
struct GDefaultValueT {
    /**
     * @brief Returns the default value for type T
     *
     * @return A value-initialized T (T(0) for the generic case)
     */
    static T value() {
        return T(0);
    }
};

/******************************************************************************/
/**
 * @brief Specialization for T == bool
 */
template <>
struct GDefaultValueT<bool> {
    /**
     * @brief Returns the default value for bool
     *
     * @return The boolean default value (true)
     */
    static bool value() {
        return true;
    }
};

/******************************************************************************/
/**
 * @brief Specialization for T == std::string
 */
template <>
struct GDefaultValueT<std::string> {
    /**
     * @brief Returns the default value for std::string
     *
     * @return An empty std::string
     */
    static std::string value() {
        return {};
    }
};
/******************************************************************************/

} /* namespace Gem::Common */
