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
#include <optional>
#include <ostream>
#include <string>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/record/GFileSink.hpp"

namespace Gem::Common::Record {

/******************************************************************************/
/**
 * Writes a stream as CSV (RFC 4180), into `<directory>/<stream name>.csv` (GFileSink): a header
 * line of the column names, then one line per row. Integers are written as such; a floating-point
 * value in the shortest form that reads back as the same value ("nan", "inf" and "-inf" for the
 * values that are not finite); a bool as "true" or "false"; a time stamp in ISO 8601, in UTC
 * ("2026-11-21T22:15:03.123456789Z"). A text, and a column name, is enclosed in double quotes when
 * it holds a comma, a double quote (written twice), a line break, or space at its start or end.
 * The default sink of a stream.
 */
class GCsvSink final : public GFileSink {
public:
    /** @brief The rows of a CSV file written for the given columns, as the text of their fields */
    [[nodiscard]] static std::optional<std::vector<std::vector<std::string>>>
    readRows(std::filesystem::path const &file, std::vector<GColumn> const &columns);

protected:
    /** @brief The extension */
    [[nodiscard]] std::string extension_() const override;
    /** @brief Writes the header line */
    void writeStart_(std::ostream &out, std::vector<GColumn> const &columns) override;
    /** @brief Writes one row as one line */
    void writeRow_(std::ostream &out, GRow const &row) override;
    /** @brief Whether the file's header line is that of the columns */
    [[nodiscard]] bool holdsColumns_(
        std::filesystem::path const &existing,
        std::vector<GColumn> const &columns
    ) const override;
    /** @brief A CSV file begins with its header line @return 1 */
    [[nodiscard]] std::size_t startLines_() const override;
    /** @brief The value of one column of a CSV row, as a number */
    [[nodiscard]] std::optional<double>
    numberIn_(std::string const &line, std::size_t column) const override;
};

/******************************************************************************/

} /* namespace Gem::Common::Record */
