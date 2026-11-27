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
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

// Boost headers go here
#include <boost/core/demangle.hpp>

// Geneva headers go here
#include "common/GChecks.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GTypeTraitsT.hpp"

namespace Gem::Common {

/******************************************************************************/
// Helper for narrow: maps an enum to its underlying integer; otherwise
// passes the type through unchanged. std::conditional_t cannot be used
// directly because it eagerly evaluates both branches and
// std::underlying_type<int> is ill-formed.
namespace detail {
template <typename T, bool = std::is_enum_v<T>>
struct enum_or_self {
    using type = T;
};
template <typename T>
struct enum_or_self<T, true> {
    using type = std::underlying_type_t<T>;
};
template <typename T>
using enum_or_self_t = typename enum_or_self<T>::type;
} // namespace detail

/******************************************************************************/
/**
 * Checked numeric/enum cast. Returns the converted value, or throws
 * std::overflow_error if a narrowing conversion would change it. Range
 * validation always happens *before* the value-changing static_cast, so an
 * out-of-range input never reaches a cast whose result would be undefined
 * behaviour (e.g. float->int or float->float out of range).
 *
 * @note Naming and semantics follow the C++ Core Guidelines / GSL: this is the
 * *checked* narrowing cast, i.e. the counterpart of `gsl::narrow<T>(x)` (which
 * throws when the round-tripped value differs). It is deliberately NOT named
 * `narrow_cast`: in the GSL `gsl::narrow_cast` is merely an *unchecked*
 * `static_cast` marker, so that name would advertise the opposite of what this
 * does. We keep our own implementation rather than depend on the GSL -- it
 * additionally supports enum source/target types, throws std::overflow_error
 * (instead of introducing a gsl::narrowing_error type), and avoids pulling the
 * Microsoft.GSL library in as an extra Geneva dependency. It also supersedes
 * the previously used boost::numeric_cast.
 *
 * What is checked, by target type:
 *   - integer / enum target: rejected unless the value fits (round-trip and
 *     sign for integer<->integer, an exact boundary check for float->integer,
 *     NaN always). Enum
 *     source/target types are handled via their underlying integer type.
 *     Float->integer truncates toward zero (no fractional-part check) but is
 *     rejected if outside the target integer range.
 *   - floating target from a *wider* floating type: a finite value whose
 *     magnitude exceeds the destination range is rejected (it would overflow
 *     to +/-inf). inf/NaN pass through unchanged; precision loss is inherent
 *     and is NOT treated as an error.
 *   - floating target from an integer/enum or a not-wider floating type:
 *     always in range, hence unchecked (may lose precision).
 *
 * @tparam to_type The target arithmetic or enum type the value is converted to
 * @tparam from_type The source arithmetic or enum type of the value (deduced)
 * @param value The value to be converted
 * @return The value converted to to_type
 * @throws std::overflow_error naming the value and both types if the narrowing conversion would
 *         change the value
 */
template <typename to_type, typename from_type>
to_type narrow(from_type value) {
    static_assert(
        (std::is_arithmetic_v<to_type> || std::is_enum_v<to_type>) &&
            (std::is_arithmetic_v<from_type> || std::is_enum_v<from_type>),
        "narrow requires arithmetic or enum types"
    );

    // For enum source/target types fall back to their underlying integer
    // representation so the same range checks apply. Without this, casts
    // like `narrow<gColor>(some_int)` silently degenerated to plain
    // static_cast<gColor>(...) and dropped all bounds checking.
    using ToCheck = detail::enum_or_self_t<to_type>;
    using FromCheck = detail::enum_or_self_t<from_type>;

    const auto check_value = static_cast<FromCheck>(value);
    [[maybe_unused]] auto refuse = [&check_value]() {
        return std::overflow_error(
            std::format(
                "narrow: the value {} of type {} does not fit into {}",
                +check_value,
                boost::core::demangle(typeid(from_type).name()),
                boost::core::demangle(typeid(to_type).name())
            )
        );
    };

    if constexpr(std::is_integral_v<ToCheck> && std::is_integral_v<FromCheck>) {
        // Integer-to-integer (and enum-via-underlying): a round-trip on the
        // underlying integer types catches narrowing in both directions. It is
        // done purely on the integers (never forming a To-typed result first),
        // so no out-of-range enum value is ever materialised; integral
        // conversions are well-defined, so this is never UB. Between a signed
        // and an unsigned type of the same width (or to a wider unsigned type)
        // the round trip gives back the original value, so a change of sign is
        // checked by itself.
        const auto converted = static_cast<ToCheck>(check_value);
        bool sign_changed = false;
        if constexpr(std::is_signed_v<FromCheck> && not std::is_signed_v<ToCheck>) {
            sign_changed = check_value < FromCheck{0};
        }
        else if constexpr(not std::is_signed_v<FromCheck> && std::is_signed_v<ToCheck>) {
            sign_changed = converted < ToCheck{0};
        }
        if(sign_changed || static_cast<FromCheck>(converted) != check_value) {
            throw refuse();
        }
        return static_cast<to_type>(value);
    }
    else if constexpr(std::is_integral_v<ToCheck> && std::is_floating_point_v<FromCheck>) {
        // Float-to-integer: validate the source range BEFORE the float->int
        // static_cast, which is undefined behaviour for out-of-range inputs.
        // The obvious `value > ToCheck::max()` check is unsafe at the int64
        // extreme: int64_t::max() == 2^63 - 1 is NOT exactly representable in
        // double; static_cast<double>(int64_max) rounds UP to 2^63, so a value
        // equal to 2^63 would pass and static_cast<int64_t>(2^63) is UB.
        //
        // Detect whether the max-as-float round-tripped exactly by checking
        // whether the float distance between adjacent integer endpoints is
        // exactly 1: it is when no rounding occurred (e.g. int32 -> double)
        // and < 1 when adjacent integers collapsed onto the same float
        // (int64 -> double). When inexact, reject `==` at the boundary too.
        constexpr ToCheck to_max = std::numeric_limits<ToCheck>::max();
        constexpr ToCheck to_min = std::numeric_limits<ToCheck>::min();
        constexpr auto max_as_from = static_cast<FromCheck>(to_max);
        constexpr auto max_minus_one_as_from = static_cast<FromCheck>(to_max - 1);
        constexpr auto min_as_from = static_cast<FromCheck>(to_min);
        constexpr bool max_is_exact = (max_as_from - max_minus_one_as_from) == FromCheck{1};

        if(std::isnan(check_value) || check_value < min_as_from) { // NaN fails every comparison
            throw refuse();
        }
        if constexpr(max_is_exact) {
            // ToCheck::max() round-trips exactly, so equality is legitimate.
            if(check_value > max_as_from) {
                throw refuse();
            }
        }
        else {
            // Rounding pushed max_as_from above ToCheck::max(): equality
            // would convert to UB territory, so reject it as well.
            if(check_value >= max_as_from) {
                throw refuse();
            }
        }
        return static_cast<to_type>(value); // provably in range now
    }
    else if constexpr(std::is_floating_point_v<ToCheck> && std::is_floating_point_v<FromCheck>) {
        // Floating-to-floating. Only a *narrowing* conversion (destination
        // range strictly smaller than the source) can overflow. Reject a
        // finite value whose magnitude exceeds the destination range BEFORE the
        // cast, since an out-of-range floating conversion is undefined
        // behaviour. inf/NaN pass through unchanged; precision loss is inherent
        // and is NOT treated as an error.
        if constexpr(std::numeric_limits<ToCheck>::max() < std::numeric_limits<FromCheck>::max()) {
            if(std::isfinite(check_value)) {
                // Widening the (smaller) destination max into FromCheck is
                // exact, so this comparison is well-defined.
                constexpr auto to_max = static_cast<FromCheck>(std::numeric_limits<ToCheck>::max());
                if(check_value > to_max || check_value < -to_max) {
                    throw refuse();
                }
            }
        }
        return static_cast<to_type>(value);
    }
    else {
        // Integer/enum -> floating: always in range (may lose precision, which
        // is inherent and not treated as an error).
        return static_cast<to_type>(value);
    }
}

/******************************************************************************/
/**
 * @brief Generates a UUID v4 string (e.g. "550e8400-e29b-41d4-a716-446655440000").
 *
 * The 122 random bits come from randomBits64(), whose engines are seeded with full entropy.
 *
 * @return A newly generated, hyphen-formatted version-4 UUID string
 */
inline std::string generate_uuid_v4() {
    std::uint64_t hi = randomBits64();
    std::uint64_t lo = randomBits64();
    // Set version 4 (nibble at bits 15-12 of `hi`, i.e. the 3rd group nibble)
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // Set variant 10xx (top 2 bits of lo's most-significant byte)
    lo = (lo & 0xBFFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    // Lowercase, zero-padded hex in the canonical 8-4-4-4-12 grouping (identical bytes to the
    // former std::hex/std::setfill/std::setw manipulator chain).
    return std::format(
        "{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
        static_cast<std::uint32_t>(hi >> 32),
        static_cast<std::uint32_t>((hi >> 16) & 0xFFFFU),
        static_cast<std::uint32_t>(hi & 0xFFFFU),
        static_cast<std::uint32_t>(lo >> 48),
        lo & 0x0000FFFFFFFFFFFFULL
    );
}

/******************************************************************************/
/**
 * @brief Reads one value of type T from the whole of a text; the one strict reader of the
 * collection, for every text a user or a file supplies.
 *
 * Surrounding white space is ignored. Anything else that is not part of one value of T makes the
 * result empty: text where a number belongs, a remainder ("12abc", the ".5" of "2.5" for an
 * integer), a negative number for an unsigned type (a stream would wrap "-1" around), a number
 * outside the range of T ("1e400", "300" for std::uint8_t) and an empty text. Numbers are read with
 * std::from_chars, independent of any locale, so the text of to_string() reads back, "inf" and
 * "nan" included; a leading '+' is accepted. An 8-bit integer (std::int8_t, std::uint8_t) is read
 * as a number, a char as a character. A bool is "true", "false", "1" or "0". A std::string is the
 * text unchanged. Every other type is read through its stream extraction operator, in the classic
 * "C" locale.
 *
 * @tparam T The type to read (arithmetic, std::string, or a type with a stream extraction operator)
 * @param text The text to read
 * @return The value, or nothing if @p text is not one value of T
 */
template <typename T>
std::optional<T> parseWhole(std::string_view text) {
    if constexpr(std::is_same_v<T, std::string>) {
        return std::string(text);
    }
    else {
        const std::string trimmed = trimWhitespace(text);
        if constexpr(std::is_same_v<T, bool>) {
            if(trimmed == "true" || trimmed == "1") {
                return true;
            }
            if(trimmed == "false" || trimmed == "0") {
                return false;
            }
            return std::nullopt;
        }
        else if constexpr(std::is_arithmetic_v<T> && not std::is_same_v<T, char>) {
            std::string_view number{trimmed};
            if(number.starts_with('+') && not number.substr(1).starts_with('-')) {
                number.remove_prefix(1); // std::from_chars knows no '+'
            }
            T value{};
            const char *const last = number.data() + number.size();
            const auto [end, ec] = std::from_chars(number.data(), last, value);
            if(ec != std::errc{} || end != last) {
                return std::nullopt;
            }
            return value;
        }
        else {
            T value{};
            std::istringstream iss(trimmed);
            iss.imbue(std::locale::classic());
            iss >> value;
            if(iss.fail() || not(iss >> std::ws).eof()) {
                return std::nullopt;
            }
            return value;
        }
    }
}

/******************************************************************************/
/**
 * @brief Converts a text to T with parseWhole(); it replaced boost::lexical_cast and, like that,
 * refuses a text that is not one value of T.
 *
 * @tparam T The type to read (see parseWhole())
 * @param s The text to convert
 * @return The value read from the whole of @p s
 * @throws gem_exception naming the text and the type if @p s is not one value of T
 */
template <typename T>
T from_string(const std::string &s) {
    std::optional<T> value = parseWhole<T>(s);
    checkThat(
        value.has_value(),
        "the text \"",
        s,
        "\" is not a value of type ",
        boost::core::demangle(typeid(T).name())
    );
    return *std::move(value);
}

/******************************************************************************/
/**
 * @brief Reads a given environment variable and converts it to a target type.
 *
 * The value is trimmed of surrounding whitespace and read with parseWhole(). Access to std::getenv
 * is serialised with a local mutex.
 *
 * @tparam target_type The type the environment variable's value is converted to (see parseWhole())
 * @param var The name of the environment variable to be read
 * @return The converted environment variable, or an empty optional if it is unset
 * @throws gem_exception naming the variable if its value is not one value of target_type
 */
template <typename target_type>
std::optional<target_type> environmentVariableAs(std::string const &var) {
    std::string result_str;

    {
        // std::getenv is not thread-safe; serialise access with a local mutex.
        static std::mutex read_env_mutex;
        std::scoped_lock const lk(read_env_mutex);

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
        char *env_ptr = 0;
        size_t sz = 0;
        if(0 == _dupenv_s(&env_ptr, &sz, var.c_str()) && nullptr != env_ptr) {
            result_str = std::string(env_ptr);
            free(env_ptr);
        }
        else {
            return {};
        }
#else
        // NOLINTNEXTLINE(concurrency-mt-unsafe) -- called under lock; Geneva never calls putenv/setenv from threads
        const char *env_ptr = std::getenv(var.c_str());
        if(env_ptr) {
            result_str = std::string(env_ptr);
        }
        else {
            return {};
        }
#endif
    } // releases the lock

    std::optional<target_type> value = parseWhole<target_type>(trimWhitespace(result_str));
    checkThat(
        value.has_value(),
        "the environment variable ",
        var,
        " has the value \"",
        result_str,
        "\", which is not a value of type ",
        boost::core::demangle(typeid(target_type).name())
    );
    return value;
}

/******************************************************************************/
/**
 * @brief Throws if two raw pointers alias the same object.
 *
 * No-op for nullptr p1.
 *
 * @tparam T The pointee type
 * @param p1 The first pointer (the check is skipped when this is nullptr)
 * @param p2 The second pointer compared against p1 for aliasing
 */
template <typename T>
void ptrDifferenceCheck(const T *p1, const T *p2) {
    checkThat(nullptr == p1 || p1 != p2, "p1 and p2 point to the same object");
}

/******************************************************************************/
/**
 * @brief Shared-pointer overload: throws if both non-null shared pointers alias
 * the same object.
 *
 * @tparam T The pointee type
 * @param p1 The first shared pointer (the check is skipped when this is empty)
 * @param p2 The second shared pointer compared against p1 for aliasing
 */
template <typename T>
void ptrDifferenceCheck(const std::shared_ptr<T> &p1, const std::shared_ptr<T> &p2) {
    checkThat(not p1 || p1.get() != p2.get(), "smart pointers p1 and p2 point to the same object");
}

/******************************************************************************/
/**
 * Converts a raw base pointer to target_type*. The `requires` clause restricts
 * the template to *downcasts* (target_type must derive from base_type). The
 * conversion is a static_cast; without NDEBUG a dynamic_cast checks it first
 * (GEM_DEBUG_CHECK: a dynamic_cast on every conversion would count in a release
 * build). Returns nullptr unchanged.
 *
 * @tparam base_type The source (base) pointer type
 * @tparam target_type The target (derived) pointer type; must derive from base_type
 * @param convert_ptr The base pointer to convert (nullptr is returned unchanged)
 * @return convert_ptr converted to const target_type*, or nullptr if the input was nullptr
 * @throws gem_exception without NDEBUG if the dynamic type is not target_type
 */
template <typename base_type, typename target_type>
    requires std::derived_from<target_type, base_type>
const target_type *g_ptr_conversion(const base_type *convert_ptr) {
    GEM_DEBUG_CHECK(
        nullptr == convert_ptr || nullptr != dynamic_cast<const target_type *>(convert_ptr),
        "invalid conversion from ",
        typeid(base_type).name(),
        " to ",
        typeid(target_type).name()
    );
    return static_cast<const target_type *>(convert_ptr);
}

/******************************************************************************/
/**
 * @brief Shared-pointer overload of g_ptr_conversion.
 *
 * @tparam base_type The source (base) pointee type
 * @tparam target_type The target (derived) pointee type; must derive from base_type
 * @param convert_ptr The base shared pointer to convert (empty is returned unchanged)
 * @return convert_ptr converted to std::shared_ptr<target_type>, or empty if the input was empty
 * @throws gem_exception without NDEBUG if the dynamic type is not target_type
 */
template <typename base_type, typename target_type>
    requires std::derived_from<target_type, base_type>
std::shared_ptr<target_type> g_ptr_conversion(const std::shared_ptr<base_type> &convert_ptr) {
    GEM_DEBUG_CHECK(
        not convert_ptr || nullptr != dynamic_cast<target_type *>(convert_ptr.get()),
        "invalid conversion from ",
        typeid(base_type).name(),
        " to ",
        typeid(target_type).name()
    );
    return std::static_pointer_cast<target_type>(convert_ptr);
}

/******************************************************************************/
/**
 * @brief Converts convert_ptr to target_type and checks it does not alias
 * compare_ptr.
 *
 * Only accessible when base_type is a base of target_type.
 *
 * @tparam base_type The source (base) pointee type
 * @tparam target_type The target (derived) pointee type; must derive from base_type
 * @param convert_ptr The base shared pointer to convert
 * @param compare_ptr The shared pointer that convert_ptr must not alias
 * @return convert_ptr converted to std::shared_ptr<target_type>
 * @throws gem_exception on aliasing, and without NDEBUG on a failed conversion
 */
template <typename base_type, typename target_type>
    requires std::derived_from<target_type, base_type>
std::shared_ptr<target_type> g_convert_and_compare(
    std::shared_ptr<base_type> convert_ptr,
    std::shared_ptr<target_type> compare_ptr
) {
    auto p = g_ptr_conversion<base_type, target_type>(convert_ptr);
    ptrDifferenceCheck(p, compare_ptr);
    return p;
}

/******************************************************************************/
/**
 * @brief Raw-pointer overload of g_convert_and_compare.
 *
 * @tparam base_type The source (base) pointee type
 * @tparam target_type The target (derived) pointee type; must derive from base_type
 * @param convert_ptr The base pointer to convert
 * @param compare_ptr The pointer that convert_ptr must not alias
 * @return convert_ptr converted to const target_type*
 * @throws gem_exception on aliasing, and without NDEBUG on a failed conversion
 */
template <typename base_type, typename target_type>
    requires std::derived_from<target_type, base_type>
const target_type *
g_convert_and_compare(const base_type *convert_ptr, const target_type *compare_ptr) {
    const auto *p = g_ptr_conversion<base_type, target_type>(convert_ptr);
    ptrDifferenceCheck(p, compare_ptr);
    return p;
}

/******************************************************************************/
/**
 * @brief Reference overload of g_convert_and_compare.
 *
 * @tparam base_type The source (base) referent type
 * @tparam target_type The target (derived) pointee type; must derive from base_type
 * @param convert_ref The base reference whose address is converted
 * @param compare_ptr The pointer that the converted address must not alias
 * @return The address of convert_ref converted to const target_type*
 * @throws gem_exception on aliasing, and without NDEBUG on a failed conversion
 */
template <typename base_type, typename target_type>
    requires std::derived_from<target_type, base_type>
const target_type *
g_convert_and_compare(const base_type &convert_ref, const target_type *compare_ptr) {
    const auto *p = g_ptr_conversion<base_type, target_type>(&convert_ref);
    ptrDifferenceCheck(p, compare_ptr);
    return p;
}

/******************************************************************************/
/**
 * @brief Returns a space-separated string representation of a std::vector.
 *
 * T must be streamable. A trailing space follows the last element.
 *
 * @tparam T The element type (must be streamable to an ostream)
 * @param vec The vector whose elements are stringified
 * @return A string holding each element separated and trailed by a single space
 */
template <typename T>
std::string vecToString(const std::vector<T> &vec) {
    std::ostringstream result;
    for(const auto &item : vec) {
        result << item << " ";
    }
    return result.str();
}

/******************************************************************************/
/**
 * @brief Deep-copies a shared_ptr to a cloneable/loadable object using clone()/load().
 *
 * Loads in place when the dynamic types match and deep-clones otherwise; an
 * empty source resets the target.
 *
 * @tparam T The pointee type (must satisfy gemfony_common_interface)
 * @param from The source shared pointer to copy from
 * @param to The destination shared pointer to copy into (modified in place)
 */
template <typename T>
    requires Gem::Common::gemfony_common_interface<T>
void copyCloneableSmartPointer(const std::shared_ptr<T> &from, std::shared_ptr<T> &to) {
    if(not from) {
        to.reset();
    }
    else if(not to or typeid(*to) != typeid(*from)) {
        // Either nothing to load into, or the existing target has a different
        // dynamic type than the source (e.g. polymorphic container slots whose
        // element types differ after a structural change). load() assumes a
        // matching concrete type, so in both cases deep-clone the source instead
        // of loading into a type-incompatible target.
        to = from->T::template clone<T>();
    }
    else {
        to->T::load(from);
    }
}

/******************************************************************************/
/**
 * @brief unique_ptr counterpart of copyCloneableSmartPointer(): deep-copies a single cloneable
 * object held in a unique_ptr.
 *
 * Loads in place when the dynamic types match (no allocation) and deep-clones
 * otherwise. No atomic reference counting is involved.
 *
 * @tparam T The pointee type (must satisfy gemfony_common_interface)
 * @param from The source unique pointer to copy from
 * @param to The destination unique pointer to copy into (modified in place)
 */
template <typename T>
    requires Gem::Common::gemfony_common_interface<T>
void copyCloneableSmartPointer(const std::unique_ptr<T> &from, std::unique_ptr<T> &to) {
    if(not from) {
        to.reset();
    }
    else if(not to or typeid(*to) != typeid(*from)) {
        to = from->template clone<T>();
    }
    else {
        to->T::load(*from);
    }
}

/******************************************************************************/
/**
 * @brief Wraps a uniquely-owned object in a NON-OWNING std::shared_ptr (a no-op deleter).
 *
 * For APIs that still take a const std::shared_ptr<T>& but only read through it. The unique_ptr
 * retains sole ownership; the returned shared_ptr must not outlive it. Lets unique_ptr-owned objects
 * be passed to such read-only shared_ptr APIs without changing those APIs or co-owning the object.
 *
 * @tparam T The pointee type
 * @param p The uniquely-owned object to wrap (ownership is NOT transferred)
 * @return A non-owning shared_ptr aliasing p.get() with a do-nothing deleter
 */
template <typename T>
std::shared_ptr<T> nonOwningShared(const std::unique_ptr<T> &p) {
    return std::shared_ptr<T>(p.get(), [](T *) { /* non-owning: do not delete */ });
}

/******************************************************************************/
/**
 * @brief Deep-copies a container of smart pointers (std::shared_ptr or std::unique_ptr) to
 * cloneable objects using clone()/load().
 *
 * Loads in place where both containers hold an element of the same dynamic type, clones
 * otherwise (copyCloneableSmartPointer()), and resizes the target to the size of the source; an
 * empty slot is copied as an empty slot.
 *
 * @tparam smart_ptr_type The element type of the containers: a std::shared_ptr or std::unique_ptr
 *         to a type that satisfies gemfony_common_interface
 * @tparam c_type The container template (e.g. std::vector) holding the smart pointers
 * @param from The source container to copy from
 * @param to The destination container to copy into (resized/modified in place)
 */
template <typename smart_ptr_type, template <typename, typename> class c_type>
    requires requires(const smart_ptr_type &from, smart_ptr_type &to) {
        copyCloneableSmartPointer(from, to); // a shared_ptr or unique_ptr of the common interface
    }
void copyCloneableSmartPointerContainer(
    const c_type<smart_ptr_type, std::allocator<smart_ptr_type>> &from,
    c_type<smart_ptr_type, std::allocator<smart_ptr_type>> &to
) {
    auto it_from = from.begin();
    for(auto it_to = to.begin(); it_from != from.end() && it_to != to.end(); ++it_from, ++it_to) {
        copyCloneableSmartPointer(*it_from, *it_to);
    }
    for(; it_from != from.end(); ++it_from) {
        copyCloneableSmartPointer(*it_from, to.emplace_back()); // an empty slot stays empty
    }
    to.resize(from.size()); // drops the surplus of a longer target
}

/******************************************************************************/
/**
 * @brief Deep-copies a container of cloneable objects (held by value) using load().
 *
 * Loads in place for the overlapping range and resizes the target container as
 * needed.
 *
 * @tparam T The element type (must satisfy gemfony_common_interface)
 * @tparam c_type The container template (e.g. std::vector) holding the objects
 * @param from The source container to copy from
 * @param to The destination container to copy into (resized/modified in place)
 */
template <typename T, template <typename, typename> class c_type>
    requires Gem::Common::gemfony_common_interface<T>
void copyCloneableObjectsContainer(
    const c_type<T, std::allocator<T>> &from,
    c_type<T, std::allocator<T>> &to
) {
    using iter_t = typename c_type<T, std::allocator<T>>::iterator;
    using const_iter_t = typename c_type<T, std::allocator<T>>::const_iterator;

    const std::size_t size_from = from.size();
    const std::size_t size_to = to.size();

    if(size_from == size_to) {
        auto it_from = from.begin();
        for(auto it_to = to.begin(); it_from != from.end(); ++it_from, ++it_to) {
            it_to->T::load(*it_from);
        }
    }
    else if(size_from > size_to) {
        auto it_from = from.begin();
        for(auto it_to = to.begin(); it_to != to.end(); ++it_from, ++it_to) {
            it_to->T::load(*it_from);
        }
        for(auto it = from.begin() + size_to; it != from.end(); ++it) {
            to.push_back(T(*it));
        }
    }
    else { // size_from < size_to
        auto it_from = from.begin();
        for(auto it_to = to.begin(); it_from != from.end(); ++it_from, ++it_to) {
            it_to->T::load(*it_from);
        }
        to.resize(size_from);
    }
}

/******************************************************************************/
/**
 * @brief Splits a string into a vector of split_type values using a single separator.
 *
 * @tparam split_type The type each fragment is converted to (see parseWhole())
 * @param raw The input string to split
 * @param sep The separator string fragments are split on
 * @return A vector of the converted fragments
 * @throws gem_exception if a fragment is not one value of split_type
 */
template <typename split_type>
std::vector<split_type> splitStringT(const std::string &raw, const char *sep) {
    std::vector<split_type> result;
    for(const auto &fragment : Gem::Common::splitString(raw, sep)) {
        result.push_back(Gem::Common::from_string<split_type>(fragment));
    }
    return result;
}

/******************************************************************************/
/**
 * @brief Splits a string into a vector of (split1_type, split2_type) tuples using
 * two different separators.
 *
 * A possible usage: "0/0 0/1 1/0" → tuples of ints (sep1 = " ", sep2 = "/").
 *
 * @tparam split1_type The type of the first tuple element
 * @tparam split2_type The type of the second tuple element
 * @param raw The input string to split
 * @param sep1 The outer separator splitting raw into fragments (must differ from sep2)
 * @param sep2 The inner separator splitting each fragment into its two sub-fields
 * @return A vector of (split1_type, split2_type) tuples
 * @throws gem_exception if sep1 and sep2 are identical, if a fragment lacks exactly two sub-fields,
 *         or if a sub-field is not one value of its type
 */
template <typename split1_type, typename split2_type>
std::vector<std::tuple<split1_type, split2_type>>
splitStringT(const std::string &raw, const char *sep1, const char *sep2) {
    checkThat(
        std::string(sep1) != std::string(sep2),
        "sep1 and sep2 are identical: \"",
        sep1,
        "\" / \"",
        sep2,
        "\""
    );

    std::vector<std::tuple<split1_type, split2_type>> result;
    for(const auto &fragment : Gem::Common::splitString(raw, sep1)) {
        const auto sub = Gem::Common::splitString(fragment, sep2);
        checkThat(2 == sub.size(), "fragment \"", fragment, "\" lacks two sub-fields");
        result.emplace_back(
            Gem::Common::from_string<split1_type>(sub[0]),
            Gem::Common::from_string<split2_type>(sub[1])
        );
    }
    return result;
}

/******************************************************************************/
/**
 * @brief Returns a reference to the value at key in m, preserving the map's const/ref qualification.
 *
 * A single forwarding template replaces the former mutable+const overload pair: @c std::forward_like
 * transfers the value category and constness of @p m onto the returned reference, so a mutable map
 * yields a mutable reference and a const map a const reference.
 *
 * @tparam map_type The map type (deduced; a const or non-const std::map<std::string, item_type>)
 * @param m The map to look up in
 * @param key The key whose mapped value is returned
 * @return A reference to the mapped value associated with key, const-qualified to match @p m
 * @throws gem_exception if the map is empty or the key is absent
 */
template <typename map_type>
decltype(auto) getMapItem(map_type &&m, const std::string &key) {
    checkThat(not m.empty(), "map is empty");
    auto it = m.find(key);
    checkThat(it != m.end(), "key \"", key, "\" not found");
    return std::forward_like<map_type>(it->second);
}

/******************************************************************************/
/**
 * @brief Adds an operator== to every object with a Gemfony-common interface.
 *
 * @tparam gemfony_common_type The compared type (must satisfy gemfony_common_interface)
 * @param x The left-hand operand
 * @param y The right-hand operand
 * @return true if the two objects compare equal, false otherwise
 */
template <class gemfony_common_type>
    requires Gem::Common::gemfony_common_interface<gemfony_common_type>
bool operator==(const gemfony_common_type &x, const gemfony_common_type &y) {
    try {
        x.compare(y, Gem::Common::expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
        return true;
    }
    catch(g_expectation_violation &) {
        return false;
    }
}

/******************************************************************************/
/**
 * @brief Adds an operator!= to every object with a Gemfony-common interface.
 *
 * @tparam gemfony_common_type The compared type (must satisfy gemfony_common_interface)
 * @param x The left-hand operand
 * @param y The right-hand operand
 * @return true if the two objects compare unequal, false otherwise
 */
template <class gemfony_common_type>
    requires Gem::Common::gemfony_common_interface<gemfony_common_type>
bool operator!=(const gemfony_common_type &x, const gemfony_common_type &y) {
    try {
        x.compare(y, Gem::Common::expectation::INEQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
        return true;
    }
    catch(g_expectation_violation &) {
        return false;
    }
}

/******************************************************************************/
/**
 * @brief Converts integral types (and int-convertible unscoped enums) to std::string.
 *
 * @tparam integral_type The integral or implicitly-int-convertible enum type
 * @param val The value to convert
 * @return The decimal string representation of val
 */
template <typename integral_type>
    requires (
        std::is_integral_v<integral_type> ||
        (std::is_enum_v<integral_type> && std::is_convertible_v<integral_type, int>)
    )
std::string to_string(integral_type val) {
    return std::to_string(val);
}

/******************************************************************************/
/**
 * @brief Converts floating-point values to the shortest std::string that reads back as the same
 * value (std::format, so 0.1 is "0.1", not "0.10000000000000001"), with a decimal point and no
 * digit grouping whatever global locale the program has set; parseWhole() reads it back, "inf" and
 * "nan" included.
 *
 * @tparam fp_type The floating-point type
 * @param val The value to convert
 * @return The shortest text that reads back as val
 */
template <std::floating_point fp_type>
std::string to_string(fp_type val) {
    return std::format("{}", val);
}

/******************************************************************************/
/**
 * @brief Converts a scoped enum (enum class) to std::string via uint32_t cast.
 *
 * @tparam enum_type The scoped enum type (not implicitly convertible to int)
 * @param val The enum value to convert
 * @return The decimal string representation of the enum's underlying value
 */
template <typename enum_type>
    requires (std::is_enum_v<enum_type> && !std::is_convertible_v<enum_type, int>)
std::string to_string(enum_type val) {
    return std::to_string(std::to_underlying(val));
}

/******************************************************************************/
/**
 * @brief Converts any remaining (non-enum, non-arithmetic) streamable type to std::string, in the
 * classic "C" locale (e.g. a std::chrono::duration as "2.5s", never "2,5s").
 *
 * @tparam default_type The streamable type to convert
 * @param val The value to convert
 * @return The string produced by streaming val into an ostringstream
 */
template <typename default_type>
    requires (!std::is_enum_v<default_type> && !std::is_arithmetic_v<default_type>)
std::string to_string(const default_type &val) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << val;
    return oss.str();
}

/******************************************************************************/

} /* namespace Gem::Common */
