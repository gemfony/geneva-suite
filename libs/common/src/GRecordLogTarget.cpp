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

#include "common/GRecordLogTarget.hpp"

// Standard header files go here
#include <exception>
#include <iostream>
#include <string_view>
#include <utility>

// Boost header files go here

// Geneva header files go here
#include "common/record/GRecordTypes.hpp"
#include "common/record/GRecorder.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * A target that records into the stream of the given name. The stream is declared at the first
 * message.
 *
 * @param stream_name The name of the stream, also that of its files
 * @param sinks The sinks of the stream; none: a CSV file
 */
GRecordLogTarget::GRecordLogTarget(
    std::string stream_name,
    std::vector<std::shared_ptr<Record::GSink>> sinks
)
  : stream_name_(std::move(stream_name))
  , sinks_(std::move(sinks)) { /* nothing */
}

/******************************************************************************/
/**
 * Records a message logged without a severity, as logType::LOGGING.
 *
 * @param message The message
 */
void GRecordLogTarget::log(std::string const &message) const {
    this->record_(message, std::string(), logType::LOGGING);
}

/******************************************************************************/
/**
 * Records a message and its source, logged without a severity, as logType::LOGGING.
 *
 * @param message The message
 * @param extension The identifier of its source
 */
void GRecordLogTarget::logWithSource(
    std::string const &message,
    std::string const &extension
) const {
    this->record_(message, extension, logType::LOGGING);
}

/******************************************************************************/
/**
 * Records a message of a given severity.
 *
 * @param message The message
 * @param severity The kind of message
 */
void GRecordLogTarget::log(std::string const &message, logType severity) const {
    this->record_(message, std::string(), severity);
}

/******************************************************************************/
/**
 * Records a message of a given severity, with its source.
 *
 * @param message The message
 * @param extension The identifier of its source
 * @param severity The kind of message
 */
void GRecordLogTarget::logWithSource(
    std::string const &message,
    std::string const &extension,
    logType severity
) const {
    this->record_(message, extension, severity);
}

/******************************************************************************/
/**
 * Appends one message as a row of the stream, declaring the stream at the first message. A
 * declaration is tried once; a refused one leaves the handle empty. A message the stream does not
 * take goes to std::cerr, with the reason if there is one.
 *
 * @param message The message
 * @param source The identifier of its source, or empty
 * @param severity The kind of message
 */
void GRecordLogTarget::record_(
    std::string const &message,
    std::string const &source,
    logType severity
) const {
    std::string reason = "the stream is closed, or its name was refused";
    try {
        if(not stream_) {
            stream_.emplace();
            *stream_ = Record::recorder().stream(
                stream_name_,
                {Record::GColumn{.name = "severity", .type = Record::columnType::TEXT},
                 Record::GColumn{.name = "source", .type = Record::columnType::TEXT},
                 Record::GColumn{.name = "text", .type = Record::columnType::TEXT}},
                Record::withTime::wall,
                Record::GStreamOptions{.sinks = sinks_}
            );
        }
        if(stream_->isOpen()) {
            stream_->append(Record::GRow{enumName(severity), source, message});
            return;
        }
    }
    catch(std::exception const &e) {
        reason = e.what();
    }
    std::cerr << "[GRecordLogTarget] the stream \"" << stream_name_ << "\" did not take a message ("
              << reason << "); it follows here:" << '\n'
              << message;
}

/******************************************************************************/

} /* namespace Gem::Common */
