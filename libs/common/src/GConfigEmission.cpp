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

#include "common/GConfigEmission.hpp"

// Standard library headers used directly in this translation unit
#include <cstdlib>
#include <string_view>

// Other Geneva headers whose symbols are used directly
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief Reports whether the configuration-emission switch appears in the command line.
 *
 * @param argc The argument count as passed to main()
 * @param argv The argument vector as passed to main()
 * @return true if the configuration-emission switch is present, false otherwise
 */
bool configEmissionRequested(int argc, char **argv) {
    for(int i = 1; i < argc; ++i) {
        if(std::string_view(argv[i]) == CONFIG_EMISSION_FLAG) {
            return true;
        }
    }
    return false;
}

/******************************************************************************/
/**
 * @brief Enters configuration-emission mode by enabling GParserBuilder update-in-place globally.
 *
 * When the environment variable named by CONFIG_REFERENCE_ENV is set, the emitted configs are additionally
 * made byte-stable (the header's creation timestamp is suppressed). This is how the config-reference build
 * target generates the single, version-controlled reference tree without a churning timestamp; ordinary
 * --update-configs runs (which materialize into the build/install tree, not source) keep the timestamp.
 */
void beginConfigEmission() {
    GParserBuilder::setUpdateInPlace(true);
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- read-only getenv during single-threaded startup
    if(std::getenv(CONFIG_REFERENCE_ENV.data()) != nullptr) {
        GParserBuilder::setEmitTimestamp(false);
    }
}

/******************************************************************************/
/**
 * @brief Concludes a configuration-emission run: logs completion and exits the process cleanly.
 */
void finishConfigEmission() {
    glogger << "Configuration emission complete; configuration files were refreshed in place." << '\n'
            << GLOGGING;
    // NOLINTNEXTLINE(concurrency-mt-unsafe) -- deliberate: a config-emission run ends the process here
    std::exit(0);
}

/******************************************************************************/

} /* namespace Gem::Common */
