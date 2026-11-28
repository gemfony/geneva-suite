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

#include "common/GFileExchangeRun.hpp"

// Standard header files go here
#include <algorithm>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <system_error>

// Geneva header files go here
#include "common/GChecks.hpp"
#include "common/GCommonHelperFunctionsT.hpp"

namespace Gem::Common {

/******************************************************************************/
namespace {

/** @brief At most this many lines of the program's output go into output_tail */
constexpr std::size_t OUTPUT_TAIL_LINES = 20;
/** @brief At most this many bytes of the program's output go into output_tail */
constexpr std::streamoff OUTPUT_TAIL_BYTES = 4096;

/**
 * @brief The last lines of a file, bounded in lines and bytes
 *
 * @param p The file; a missing file gives an empty text
 * @return At most OUTPUT_TAIL_LINES lines of the last OUTPUT_TAIL_BYTES bytes
 */
std::string tailOf(std::filesystem::path const &p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if(not in) {
        return {};
    }
    std::streamoff const size = in.tellg();
    in.seekg(std::max<std::streamoff>(0, size - OUTPUT_TAIL_BYTES));
    std::string tail{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    // Count the lines from the end; a final newline ends the last line and starts none
    std::size_t end = tail.size();
    if(end > 0 and tail[end - 1] == '\n') {
        --end;
    }
    std::size_t begin = end;
    for(std::size_t lines = 0; begin > 0; --begin) {
        if(tail[begin - 1] == '\n' and ++lines == OUTPUT_TAIL_LINES) {
            break;
        }
    }
    return tail.substr(begin);
}

/**
 * @brief The first part of a relative path ("config" for "config/a.json")
 */
std::filesystem::path firstPartOf(std::filesystem::path const &p) {
    return p.empty() ? std::filesystem::path{} : *p.begin();
}

} // namespace

/******************************************************************************/
/**
 * Creates the directory parent/prefix-UUID, and the parent if it is missing.
 *
 * @param parent The directory in which the run directory is created
 * @param prefix The first part of the run directory's name
 * @throws gem_exception or std::filesystem::filesystem_error if the directory cannot be created
 */
GRunDirectory::GRunDirectory(std::filesystem::path const &parent, std::string const &prefix)
  : path_(std::filesystem::absolute(parent / (prefix + "-" + generate_uuid_v4()))) {
    std::filesystem::create_directories(parent);
    checkThat(
        std::filesystem::create_directory(path_),
        "In GRunDirectory: the run directory ",
        path_.string(),
        " exists already"
    );
}

/******************************************************************************/
/**
 * Removes the directory with all it holds, unless it is kept. A failure to remove it is ignored:
 * a destructor does not throw, and a directory left behind holds nothing a later run reads.
 */
GRunDirectory::~GRunDirectory() {
    if(not keep_) {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
}

/******************************************************************************/
/**
 * The absolute path of the directory.
 *
 * @return The path, valid as long as this object lives
 */
std::filesystem::path const &GRunDirectory::path() const {
    return path_;
}

/******************************************************************************/
/**
 * Leaves the directory in place when this object ends.
 */
void GRunDirectory::keep() {
    keep_ = true;
}

/******************************************************************************/
/**
 * Runs a program in a directory of its own with file exchange. The run directory is
 * parent/name-UUID, the name being the program's file name; it is removed at the end, also when an
 * exception leaves this function, unless the spec says keep. The program's output goes to
 * PROGRAM_OUTPUT_FILE in it.
 *
 * The result file is read only if the program exited with 0. A program that a time limit or a stop
 * request ended -- with end_on_stop_request also one that ended while a stop was requested -- gives
 * no result even if it wrote one: what it wrote may be incomplete. The caller tells an error from an
 * interruption by result.program.end.
 *
 * @param spec The program, its arguments, the files to exchange and the limits of the run
 * @param parent The directory in which the run directory is created
 * @return How the program ended, the result file's content, the tail of its output and where it ran
 * @throws gem_exception if an input path is absolute, an input cannot be written, or the program
 * cannot be run (see runExternalCommand()); std::filesystem::filesystem_error if a directory or
 * link cannot be created
 */
GFileExchangeResult
runWithFileExchange(GFileExchangeSpec const &spec, std::filesystem::path const &parent) {
    GRunDirectory directory(parent, spec.program.filename().string());
    if(spec.keep) {
        directory.keep();
    }
    auto const &dir = directory.path();

    // Directories the run writes into are its own, never links into link_from
    std::vector<std::filesystem::path> own_names{firstPartOf(spec.result)};
    for(auto const &[name, content] : spec.inputs) {
        checkThat(
            name.is_relative() and not name.empty(),
            "In runWithFileExchange(): an input path must be relative to the run directory: ",
            name.string()
        );
        own_names.push_back(firstPartOf(name));
    }

    if(not spec.link_from.empty()) {
        for(auto const &entry : std::filesystem::directory_iterator(spec.link_from)) {
            auto const name = entry.path().filename();
            if(std::ranges::find(own_names, name) != own_names.end()) {
                continue;
            }
            auto const target = std::filesystem::absolute(entry.path());
            if(entry.is_directory()) {
                std::filesystem::create_directory_symlink(target, dir / name);
            }
            else {
                std::filesystem::create_symlink(target, dir / name);
            }
        }
    }

    for(auto const &[name, content] : spec.inputs) {
        std::filesystem::create_directories((dir / name).parent_path());
        std::ofstream out(dir / name, std::ios::binary);
        out << content;
        out.close();
        checkThat(
            not out.fail(),
            "In runWithFileExchange(): cannot write the input ",
            (dir / name).string()
        );
    }

    std::vector<std::string> arguments = spec.arguments;
    for(auto &argument : arguments) {
        for(auto pos = argument.find(RUN_DIRECTORY); pos != std::string::npos;
            pos = argument.find(RUN_DIRECTORY, pos + dir.string().size())) {
            argument.replace(pos, RUN_DIRECTORY.size(), dir.string());
        }
    }

    GProgramLimits limits = spec.limits;
    if(spec.run_inside) {
        limits.working_directory = dir;
    }

    GFileExchangeResult result;
    result.directory = dir;
    result.program = runExternalCommand(spec.program, arguments, dir / PROGRAM_OUTPUT_FILE, limits);
    result.output_tail = tailOf(dir / PROGRAM_OUTPUT_FILE);
    if(result.program.end == ProgramEnd::EXITED and result.program.code == 0 and
       not spec.result.empty() and std::filesystem::exists(dir / spec.result)) {
        result.result = loadTextDataFromFile(dir / spec.result);
    }
    return result;
}

/******************************************************************************/

} /* namespace Gem::Common */
