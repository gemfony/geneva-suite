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

// Standard headers go here
#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Geneva headers go here
#include "common/GLogger.hpp"

/******************************************************************************/
/**
 * @file
 * A log target for the courtier tests that look at what a role logs.
 */

namespace Gem::Courtier::Tests {

/******************************************************************************/
/** @brief A log target that keeps what is logged, so that a test can look at it. */
class CapturedLog final : public Gem::Common::GBaseLogTarget {
public:
    using Gem::Common::GBaseLogTarget::log;
    using Gem::Common::GBaseLogTarget::logWithSource;
    /** @brief Keeps @p message */
    void log(std::string const &message) const override {
        const std::scoped_lock lock(mutex_);
        lines_.push_back(message);
    }
    /** @brief Keeps @p message, preceded by @p extension */
    void logWithSource(std::string const &message, std::string const &extension) const override {
        std::string line = extension;
        line.append(": ").append(message);
        this->log(line);
    }
    /** @return Whether a line logged so far contains all of @p parts */
    [[nodiscard]] bool hasLineWith(std::initializer_list<std::string_view> parts) const {
        return this->countLinesWith(parts) > 0;
    }
    /** @return How many lines logged so far contain all of @p parts */
    [[nodiscard]] std::size_t countLinesWith(std::initializer_list<std::string_view> parts) const {
        const std::scoped_lock lock(mutex_);
        return static_cast<std::size_t>(
            std::ranges::count_if(lines_, [parts](const std::string &line) {
                return std::ranges::all_of(parts, [&line](std::string_view part) {
                    return line.contains(part);
                });
            })
        );
    }

private:
    mutable std::mutex mutex_; ///< Guards lines_ (the logger may call from any thread)
    mutable std::vector<std::string> lines_; ///< What was logged, in order
};

/******************************************************************************/

} /* namespace Gem::Courtier::Tests */
