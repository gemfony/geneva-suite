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
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Geneva headers go here
#include "common/GCommonHelperFunctions.hpp"

/******************************************************************************/
/**
 * @file
 * A run of an external program that exchanges files with its caller: the inputs are written into a
 * directory of the run's own, the program runs, its result file is read, and the directory is
 * removed. An interrupted run gives no result.
 */

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief A directory of its own for one run, parent/prefix-UUID; removed at its end unless kept
 *
 * The parent is created if it is missing. Several runs, also from several threads, get different
 * directories.
 */
class GRunDirectory {
public:
    /** @brief Creates the directory parent/prefix-UUID */
    GRunDirectory(std::filesystem::path const &parent, std::string const &prefix);
    /** @brief Removes the directory with all it holds, unless it is kept; never throws */
    ~GRunDirectory();

    GRunDirectory(GRunDirectory const &) = delete;
    GRunDirectory(GRunDirectory &&) = delete;
    GRunDirectory &operator=(GRunDirectory const &) = delete;
    GRunDirectory &operator=(GRunDirectory &&) = delete;

    /** @brief The absolute path of the directory */
    [[nodiscard]] std::filesystem::path const &path() const;
    /** @brief Leaves the directory in place at the end (debugging, a failed run to inspect) */
    void keep();

private:
    std::filesystem::path path_; ///< the absolute path of the directory
    bool keep_ = false;          ///< whether the destructor leaves the directory in place
};

/******************************************************************************/
/**
 * @brief In an argument of a GFileExchangeSpec, this text stands for the absolute path of the run
 * directory
 */
inline constexpr std::string_view RUN_DIRECTORY = "@RUN_DIRECTORY@";

/**
 * @brief The file in the run directory that receives the program's standard output and standard
 * error
 */
inline constexpr std::string_view PROGRAM_OUTPUT_FILE = "program-output.txt";

/******************************************************************************/
/**
 * @brief What runWithFileExchange() runs and which files it exchanges
 */
struct GFileExchangeSpec {
    /** @brief The program; see runExternalCommand() for how it is found */
    std::filesystem::path program;
    /** @brief The arguments; RUN_DIRECTORY in them is replaced by the run directory's path */
    std::vector<std::string> arguments;
    /** @brief Files written into the run directory before the start: relative path, content */
    std::vector<std::pair<std::filesystem::path, std::string>> inputs;
    /**
     * @brief Optional: every entry of this directory is linked into the run directory, except an
     * entry whose name is the first part of an input's or of the result's path (such a directory is
     * created in the run directory, so the run writes nothing into this one)
     */
    std::filesystem::path link_from;
    /** @brief The result file, relative to the run directory; read after a clean end */
    std::filesystem::path result;
    /** @brief Whether the program's working directory is the run directory (else the caller's) */
    bool run_inside = false;
    /** @brief The limits of the run; their working directory is replaced when run_inside is set */
    GProgramLimits limits;
    /** @brief Whether the run directory is left in place at the end */
    bool keep = false;
};

/******************************************************************************/
/**
 * @brief What a run with file exchange gave
 */
struct GFileExchangeResult {
    /** @brief How the program ended */
    GProgramResult program;
    /** @brief The content of the result file; empty unless the program EXITED with 0 and wrote it */
    std::optional<std::string> result;
    /** @brief The last lines of the program's output, for messages */
    std::string output_tail;
    /** @brief Where the run took place (still there only if kept) */
    std::filesystem::path directory;
};

/******************************************************************************/
/**
 * @brief Runs a program in a directory of its own with file exchange: writes the inputs, links the
 * entries of link_from, runs the program within its limits and reads the result
 */
GFileExchangeResult
runWithFileExchange(GFileExchangeSpec const &spec, std::filesystem::path const &parent);

/******************************************************************************/

} /* namespace Gem::Common */
