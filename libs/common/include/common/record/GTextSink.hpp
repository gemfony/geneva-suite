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
 * Writes a stream as plain text for people to read, into `<directory>/<stream name>.log`
 * (GFileSink): no header, one row per line, the values of its columns separated by a space, an
 * empty text left out. A time stamp is written in ISO 8601 to the second, in UTC
 * ("2026-11-22T14:56:03Z"); a number, a bool and a text as GCsvSink writes them, a text without
 * quotes. A text that ends with a line break does not add an empty line; one that holds line
 * breaks continues on the lines that follow. For the log (GRecordLogTarget) a line reads
 * `2026-11-22T14:56:03Z WARNING <message>`, with the source of the message, if it has one,
 * before the message.
 *
 * A text file has no header by which a resumed stream could recognize it as its own: it is
 * backed up and begun anew, as for a stream that does not resume.
 */
class GTextSink final : public GFileSink {
protected:
    /** @brief The extension */
    [[nodiscard]] std::string extension_() const override;
    /** @brief A text file begins with its first row: writes nothing */
    void writeStart_(std::ostream &out, std::vector<GColumn> const &columns) override;
    /** @brief Writes one row as one line of text */
    void writeRow_(std::ostream &out, GRow const &row) override;
    /** @brief A text file cannot be recognized as a stream's own @return false */
    [[nodiscard]] bool holdsColumns_(
        std::filesystem::path const &existing,
        std::vector<GColumn> const &columns
    ) const override;
    /** @brief A text file begins with its first row @return 0 */
    [[nodiscard]] std::size_t startLines_() const override;
    /** @brief A text file holds no numbers to cut it back by @return none */
    [[nodiscard]] std::optional<double>
    numberIn_(std::string const &line, std::size_t column) const override;
};

/******************************************************************************/

} /* namespace Gem::Common::Record */
