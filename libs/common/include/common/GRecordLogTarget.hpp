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
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GCommonEnums.hpp"
#include "common/GLogger.hpp"
#include "common/record/GSink.hpp"
#include "common/record/GStream.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * A log target that records every message as a row of a stream of the recorder
 * (Record::recorder()), so that the log is data that other tools read and follows the recorder's
 * output rule: `<record directory>/<stream name>.<extension>`, an existing file backed up. A
 * program adds it like any target:
 *
 * @code
 * glogger.addLogTarget(std::make_shared<Gem::Common::GRecordLogTarget>());
 * glogger.addLogTarget(std::make_shared<Gem::Common::GRecordLogTarget>(
 *     "run", std::vector<std::shared_ptr<Gem::Common::Record::GSink>>{
 *                std::make_shared<Gem::Common::Record::GTextSink>()})); // records/run.log, plain text
 * @endcode
 *
 * The stream has the columns `time` (the wall clock), `severity` (the kind of message as text:
 * "LOGGING", "WARNING", "EXIT"), `source` (the extension of a message logged with a source, else
 * empty) and `text` (the message as it was logged). Its sinks are the ones given, a CSV file if
 * none is; Record::GRecorder::addSink() adds more. The stream is declared at the first message, in
 * the recorder's directory of that moment, so a sink added after the target was built and before
 * the first message joins it.
 *
 * The target holds no lock of its own: GLogger calls it under its mutex, one message at a time,
 * and the stream is thread-safe. A warning the stream raises while it writes (it backs up an
 * existing file) reaches GLogger from inside the target and goes to std::cerr (GLogger). A
 * message the stream does not take -- once Record::GRecorder::closeAll() has closed it, or when
 * a sink fails or the name is refused -- is written to std::cerr, so that logging never brings a
 * program down.
 */
class GRecordLogTarget : public GBaseLogTarget {
public:
    /** @brief A target that records into the stream of the given name, with the given sinks */
    explicit GRecordLogTarget(
        std::string stream_name = "log",
        std::vector<std::shared_ptr<Record::GSink>> sinks = {}
    );

    /** @brief Records a message without a severity, as logType::LOGGING */
    void log(std::string const &message) const override;
    /** @brief Records a message and its source without a severity, as logType::LOGGING */
    void logWithSource(std::string const &message, std::string const &extension) const override;
    /** @brief Records a message of a given severity */
    void log(std::string const &message, logType severity) const override;
    /** @brief Records a message of a given severity, with its source */
    void logWithSource(
        std::string const &message,
        std::string const &extension,
        logType severity
    ) const override;

private:
    /** @brief Appends one message as a row; declares the stream at the first */
    void record_(std::string const &message, std::string const &source, logType severity) const;

    std::string stream_name_;                           ///< The name of the stream
    std::vector<std::shared_ptr<Record::GSink>> sinks_; ///< Its sinks; none: a CSV file
    mutable std::optional<Record::GStream> stream_;     ///< Declared at the first message
};

/******************************************************************************/

} /* namespace Gem::Common */
