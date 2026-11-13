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

/**
 * @file
 * @brief Overlays a per-directory config-overrides.json onto materialized configuration files.
 *
 * A build materializes a binary's configuration files from the code's registered defaults (via
 * --update-configs); this tool then applies that directory's intentional non-default values -- the only
 * value-carrying configuration artifacts the source tree keeps. It is the build-time invocation of
 * Gem::Common::applyConfigOverrides().
 *
 * Usage:  GConfigOverlay <config-dir> <config-overrides.json>
 *
 * The override document maps a configuration file name to an object of that file's non-default
 * key/value pairs, e.g.
 *   { "Go2.json": { "consumer": "gpu" }, "GGPUConsumer.json": { "kernel_path": "./k.cu" } }
 * For each entry the named file in <config-dir> is loaded, the overrides are overlaid onto it (each named
 * parameter's value replaced, its code-owned default/comment left intact), and it is written back.
 *
 * A target file that is not present in this build is skipped with a note: a directory's fragment may list
 * a build-variant configuration (e.g. the float and double GPU-consumer configs) of which only one is
 * materialized for a given build. Genuine staleness within a file that IS present -- an override that
 * names a parameter the configuration does not contain, or any other structural mismatch -- remains a hard
 * error (non-zero exit), so a stale override still fails the build.
 */

#include <filesystem>
#include <iostream>
#include <string>

#include <boost/json.hpp>

#include "common/GExceptions.hpp"
#include "common/GJsonIO.hpp"

int main(int argc, char **argv) {
    if(argc != 3) {
        std::cerr << "Usage: " << (argc > 0 ? argv[0] : "GConfigOverlay")
                  << " <config-dir> <config-overrides.json>\n";
        return 2;
    }

    std::filesystem::path const config_dir{argv[1]};
    std::filesystem::path const overrides_file{argv[2]};

    try {
        boost::json::value const overrides = Gem::Common::parseJsonFile(overrides_file);
        if(not overrides.is_object()) {
            std::cerr << "GConfigOverlay: " << overrides_file.string()
                      << " must be a JSON object mapping config file names to their overrides.\n";
            return 1;
        }

        for(auto const &entry : overrides.get_object()) {
            std::filesystem::path const target = config_dir / std::string(entry.key());
            // A build-variant config named in the fragment may not be materialized for this build; skip it.
            if(not std::filesystem::exists(target)) {
                std::cout << "GConfigOverlay: skipping " << target.string()
                          << " (not materialized in this build)\n";
                continue;
            }
            boost::json::value config = Gem::Common::parseJsonFile(target);
            Gem::Common::applyConfigOverrides(config, entry.value());
            Gem::Common::writeJsonFile(target, config);
            std::cout << "GConfigOverlay: applied overrides to " << target.string() << '\n';
        }
    }
    catch(std::exception const &e) {
        std::cerr << "GConfigOverlay: error while overlaying " << overrides_file.string() << ":\n"
                  << e.what() << '\n';
        return 1;
    }

    return 0;
}
