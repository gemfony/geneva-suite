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
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/record/GFileSink.hpp"

namespace Gem::Common::Record {

/******************************************************************************/
/**
 * Writes a stream as JSON Lines, into `<directory>/<stream name>.jsonl` (GFileSink): one JSON
 * object per row, its members the columns in column order, so the types are kept: integers and
 * floating-point values as numbers (a floating-point value in the shortest form that reads back as
 * the same value; one that is not finite as null, since JSON has no such numbers), a bool as true
 * or false, a text as a string, a time stamp as a string in ISO 8601, in UTC.
 */
class GJsonLinesSink final : public GFileSink {
protected:
    /** @brief The extension @return ".jsonl" */
    [[nodiscard]] std::string extension_() const override;
    /** @brief A JSON Lines file begins with its first row: writes nothing @param out The file
     *  @param columns The columns */
    void writeStart_(std::ostream &out, std::vector<GColumn> const &columns) override;
    /** @brief Writes one row as one JSON object on one line @param out The file @param row The row */
    void writeRow_(std::ostream &out, GRow const &row) override;
    /** @brief Whether the members of the file's first object are the columns, in their order
     *  @param existing The file @param columns The columns @return Whether they are */
    [[nodiscard]] bool holdsColumns_(
        std::filesystem::path const &existing,
        std::vector<GColumn> const &columns
    ) const override;
};

/******************************************************************************/

} /* namespace Gem::Common::Record */
