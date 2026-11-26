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
#include <concepts>
#include <source_location>

// Boost header files go here

// Geneva header files go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"

/******************************************************************************/
/**
 * @file
 * The two ways to check a condition in the code of the collection, so that a reader sees at once
 * that a line is a check and not program logic:
 *
 *     Gem::Common::checkThat(pos < size(), "position ", pos, " is outside of [0, ", size(), "[");
 *     GEM_DEBUG_CHECK(std::ranges::all_of(...), "an item is not processed");
 *
 * checkThat() is always on. It is for checks that cost little next to the work they guard: a
 * comparison, a size, a null test. GEM_DEBUG_CHECK() is on only in builds without NDEBUG (an
 * assert in the standard sense, but throwing like checkThat(), so that a test can observe it). It
 * is for checks whose cost would count in a release build: a loop over all elements, a dynamic_cast
 * or a comparison on a path run for every random number or every parameter read.
 *
 * A failed check throws gem_exception, built through g_error_streamer, so its message starts with
 * the time and the place of the check (file, line, function) and goes on with the message parts.
 */

namespace Gem::Common {

/******************************************************************************/
/**
 * The condition of a check, together with the place in the code that checks it.
 *
 * checkThat() takes its condition as this type so that the place is recorded where checkThat() is
 * called: the defaulted std::source_location is filled in where the bool is converted, i.e. in the
 * line of the call. Only a bool is accepted: a pointer, a number or a string literal (all of
 * which would silently turn into a bool) is refused at compile time, so a check spells out what it
 * tests (`ptr != nullptr`, `n > 0`).
 */
class GCheckCondition {
public:
    /**
     * @brief Records the value of the condition and the place of the check.
     *
     * @tparam bool_type Exactly bool
     * @param holds Whether the checked condition holds
     * @param place The place of the check (defaulted to the caller's location)
     */
    template <std::same_as<bool> bool_type>
    GCheckCondition( // NOLINT(google-explicit-constructor,hicpp-explicit-conversions) -- implicit on purpose: it records the caller's place
        bool_type holds,
        std::source_location const &place = std::source_location::current()
    ) noexcept
      : holds_(holds)
      , place_(place) { /* nothing */
    }

    /** @brief Whether the checked condition holds
     *  @return true if the condition holds */
    [[nodiscard]] bool holds() const noexcept {
        return holds_;
    }

    /** @brief The place of the check
     *  @return The source location of the call of checkThat() */
    [[nodiscard]] std::source_location const &place() const noexcept {
        return place_;
    }

private:
    bool holds_;                 ///< Whether the checked condition holds
    std::source_location place_; ///< The place of the check
};

/******************************************************************************/
/**
 * Checks a condition, in every build, and throws gem_exception if it does not hold.
 *
 * The message parts are streamed into the message one after the other, and only when the check
 * fails, so a check that holds costs one comparison; a line break ends the message. The time and
 * the place of the check are put in front of it (g_error_streamer).
 *
 * @tparam part_types The types of the message parts (anything that can be streamed)
 * @param condition The checked condition (a bool; the place of the call is recorded with it)
 * @param message_parts What went wrong, in one or more parts streamed one after the other
 * @throw gem_exception if the condition does not hold
 */
template <typename... part_types>
void checkThat(GCheckCondition const &condition, part_types const &...message_parts) {
    static_assert(sizeof...(part_types) > 0, "checkThat(): a check says what went wrong");
    if(not condition.holds()) [[unlikely]] {
        g_error_streamer message(condition.place());
        (message << ... << message_parts) << '\n';
        throw gem_exception(message);
    }
}

} /* namespace Gem::Common */

/******************************************************************************/
/**
 * GEM_DEBUG_CHECK(condition, message parts...) is checkThat(condition, message parts...) in a build
 * without NDEBUG, and nothing in a build with NDEBUG: there neither the condition nor the message
 * is evaluated, but both are still compiled (in the body of a lambda that is never called), so a
 * check cannot break the build of the other mode. Like assert(), it is an expression in both modes.
 *
 * A macro and not a function, because only a macro can leave the condition unevaluated, and the
 * condition is what costs (a loop over all elements, a dynamic_cast). The arguments are passed on
 * as they are, so a condition may contain commas.
 */
#ifdef NDEBUG
#define GEM_DEBUG_CHECK(...) static_cast<void>([&] { ::Gem::Common::checkThat(__VA_ARGS__); })
#else
#define GEM_DEBUG_CHECK(...) ::Gem::Common::checkThat(__VA_ARGS__)
#endif

/******************************************************************************/
