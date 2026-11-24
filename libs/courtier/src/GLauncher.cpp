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

#include "courtier/GLauncherT.hpp"

// Standard header files go here
#include <string>
#include <system_error>

// Boost header files go here
// Boost.DLL with std::filesystem (as common's loader), for the location of the running program
#define BOOST_DLL_USE_STD_FS 1 // NOLINT(cppcoreguidelines-macro-usage): Boost.DLL reads a macro
#include <boost/dll/runtime_symbol_info.hpp>

// Geneva header files go here
#include "common/concurrency/GLoadOnceCellT.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The first item type claimed in this process stays its item type. The record is defined here, in
 * libgemfony-courtier, so that a program and every library and module it loads share it.
 */
void claimItemType(const std::type_info &item_type) {
    // Never destroyed: a launcher may be built while statics are torn down elsewhere
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    static auto *record = new Gem::Common::Concurrency::GLoadOnceCellT<std::string>();
    const std::string &claimed =
        record->getOrCompute([&] { return std::string(item_type.name()); });
    if(claimed != item_type.name()) {
        throw gem_exception(
            g_error_streamer() << "In Gem::Courtier::claimItemType(): Error!" << '\n'
                               << "A launcher for the item type "
                               << boost::core::demangle(item_type.name())
                               << " was built in a process whose items are of the type "
                               << boost::core::demangle(claimed.c_str()) << "." << '\n'
                               << "A process distributes one kind of item, through one consumer."
                               << '\n'
        );
    }
}

/******************************************************************************/
/**
 * A directory that cannot be read holds no modules.
 */
std::vector<std::string> GConsumerModules::available() const {
    std::vector<std::string> result;
    std::error_code ec;
    for(const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        const std::string name = entry.path().filename().string();
        if(name.size() > prefix.size() + suffix.size() && name.starts_with(prefix) &&
           name.ends_with(suffix)) {
            result.push_back(
                name.substr(prefix.size(), name.size() - prefix.size() - suffix.size())
            );
        }
    }
    std::ranges::sort(result);
    return result;
}

/******************************************************************************/
/**
 * The directory of the running program, as the operating system reports it.
 */
GConsumerModules GConsumerModules::besideProgram(std::string prefix) {
    return GConsumerModules{
        .directory = boost::dll::program_location().parent_path(),
        .prefix = std::move(prefix)
    };
}

/******************************************************************************/

} /* namespace Gem::Courtier */
