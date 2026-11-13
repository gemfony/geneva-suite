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

// Standard header files go here
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace Gem::Weft {

/******************************************************************************/
/**
 * @brief The exception type thrown by the Weft serialization engine.
 *
 * Weft is a self-contained, dependency-free foundation library (the whole point
 * of the split is that it can be reused in other projects), so it carries its
 * own tiny error facility rather than reaching for Gem::Common's
 * @c geneva_exception / @c g_error_streamer / @c GLogger. A @c weft_exception is a
 * plain @c std::runtime_error: the caller catches it and decides whether/how to
 * log — Weft never logs on its own.
 */
class weft_exception : public std::runtime_error {
public:
    /** @brief Constructs the exception from a ready-made diagnostic string. */
    explicit weft_exception(const std::string &what_arg)
      : std::runtime_error(what_arg) {}
    /** @brief Constructs the exception from a C-string diagnostic. */
    explicit weft_exception(const char *what_arg)
      : std::runtime_error(what_arg) {}
};

/******************************************************************************/
/**
 * @brief A minimal ostream-style message builder — the Weft analogue of
 * Gem::Common's @c g_error_streamer.
 *
 * It accumulates a diagnostic through @c operator<< and converts to a
 * @c std::string, so a throw reads exactly like the Gem::Common idiom it replaces:
 * @code
 *   throw weft_exception(weft_error_streamer() << "In foo(): bad tag \"" << t << "\"\n");
 * @endcode
 * Unlike @c g_error_streamer it neither logs nor stamps a source location — Weft
 * has no logger and stays dependency-free; the message text carries the context.
 */
class weft_error_streamer {
public:
    weft_error_streamer() = default;

    /** @brief Streams an arbitrary value into the accumulating diagnostic. */
    template <typename T>
    weft_error_streamer &operator<<(const T &value) {
        oss_ << value;
        return *this;
    }

    /** @brief @return The accumulated diagnostic as a string. */
    [[nodiscard]] std::string str() const { return oss_.str(); }
    /** @brief Implicit conversion to string, so a streamer seeds a weft_exception directly. */
    operator std::string() const { return oss_.str(); } // NOLINT(google-explicit-constructor)

private:
    std::ostringstream oss_;
};

/******************************************************************************/

} // namespace Gem::Weft
