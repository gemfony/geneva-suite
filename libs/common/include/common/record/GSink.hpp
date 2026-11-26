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
#include <span>
#include <string>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/record/GRecordTypes.hpp"

namespace Gem::Common::Record {

/******************************************************************************/
/**
 * What a sink is told about its stream when it is opened.
 */
struct GStreamInfo {
    std::string name;                ///< The name of the stream; a file sink names its file after it
    std::vector<GColumn> columns;    ///< The columns, the automatic time column included
    std::filesystem::path directory; ///< The directory of the recorder when the stream was declared
    bool resume = false;             ///< The stream continues a run that was resumed (GStreamOptions)
    std::string resumeColumn; ///< With resume: the column by which a file is cut back (GStreamOptions)
    double resumeAfter = 0.;  ///< With resume: the last value of resumeColumn whose rows are kept
    std::filesystem::path csvFile; ///< The file of the stream's GCsvSink; empty if it has none
};

/******************************************************************************/
/**
 * Where the rows of a stream go. A stream calls its sinks under its own lock, one call at a time
 * and in this order: open() once, before the first rows; write() with the rows of each flush, in
 * the order they were appended; close() once, at the end of the stream -- only if open() was
 * called, and then also when open() threw (a sink must tolerate that). A sink therefore needs no lock of its own. It reports a failure by throwing; the stream
 * passes the exception on to the caller of append() or close().
 *
 * The framework ships GCsvSink and GJsonLinesSink. Another library or a program may add its own.
 */
class GSink {
public:
    /** @brief The default constructor */
    GSink() = default;
    /** @brief The destructor */
    virtual ~GSink() = default;

    GSink(GSink const &) = delete;
    GSink(GSink &&) = delete;
    GSink &operator=(GSink const &) = delete;
    GSink &operator=(GSink &&) = delete;

    /** @brief Prepares for the rows of a stream, before the first of them @param info The stream */
    virtual void open(GStreamInfo const &info) = 0;
    /** @brief Takes rows of the stream, in the order they were appended @param rows The rows */
    virtual void write(std::span<GRow const> rows) = 0;
    /** @brief Ends the stream: no row follows */
    virtual void close() = 0;
};

/******************************************************************************/

} /* namespace Gem::Common::Record */
