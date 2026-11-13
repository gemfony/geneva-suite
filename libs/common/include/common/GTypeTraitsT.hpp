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
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

// Boost headers go here

// Geneva headers go here

namespace Gem::Common {

/******************************************************************************/
/**
 * A concept for arithmetic types (integral or floating-point). The checked
 * numeric parameter classes in geneva/par constrain their template parameter
 * with it (alongside the standard std::floating_point / std::signed_integral
 * concepts), replacing the former in-body std::is_arithmetic_v static_assert
 * guards.
 *
 * @tparam T The type checked for being integral or floating-point
 */
template <typename T>
concept arithmetic = std::integral<T> || std::floating_point<T>;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
// This simple class is used to simplify detection of classes that have the common
// interface of Gemfony libraries.
class gemfony_common_interface_indicator {};

/**
 * A type trait helping to check whether a class has the Gemfony Scientific library
 * interface. The simple convention is that the base class of a hierarchy must
 * (we recommend) inherit from gemfony_common_interface_indicator.
 *
 * @tparam T The type checked for carrying the Gemfony common interface
 */
template <typename T>
struct has_gemfony_common_interface {
    static constexpr bool value = std::is_base_of_v<gemfony_common_interface_indicator, T>;
};

/**
 * The C++20 concept form of has_gemfony_common_interface, for use in requires
 * clauses. T satisfies it iff it carries the Gemfony common interface, i.e. it
 * (indirectly) derives from gemfony_common_interface_indicator. Equivalent to
 * has_gemfony_common_interface<T>::value but more ergonomic and giving clearer
 * diagnostics at the constraint site. (is_base_of, not std::derived_from, since
 * the indicator is inherited privately and is therefore not an accessible base.)
 *
 * @tparam T The type checked for carrying the Gemfony common interface
 */
template <typename T>
concept gemfony_common_interface =
    std::is_base_of_v<gemfony_common_interface_indicator, T>;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A type trait helping to check whether a class has a compare function.
 *
 * Modernized from pre-C++11 SFINAE to a C++20 requires-expression.
 * The @c ::value member is preserved for backward compatibility.
 *
 * @tparam T The type checked for a compare member
 */
template <typename T>
struct has_compare_member {
    static constexpr bool value = requires { &T::compare; };
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A type trait helping to check whether a class has a clone function.
 *
 * Modernized from pre-C++11 SFINAE to a C++20 requires-expression.
 * The @c ::value member is preserved for backward compatibility.
 *
 * @tparam T The type checked for a clone member
 */
template <typename T>
struct has_clone_member {
    static constexpr bool value = requires { &T::clone; };
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A type trait helping to check whether a class has a load function.
 *
 * Modernized from pre-C++11 SFINAE to a C++20 requires-expression.
 * The @c ::value member is preserved for backward compatibility.
 *
 * @tparam T The type checked for a load member
 */
template <typename T>
struct has_load_member {
    static constexpr bool value = requires { &T::load; };
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common */
