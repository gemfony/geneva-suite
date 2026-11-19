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

// Standard headers
#include <cstdint>
#include <string>

// Geneva headers
#include "common/GParserBuilder.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The two time limits of the networked consumers, read from a Geneva-style JSON config file
 * (GParserBuilder), which is created with the defaults if it is absent. Both are whole
 * milliseconds; 0 means "no limit", the default of both.
 *
 * By default a networked consumer sets no time limit at all: how long an evaluation takes is the
 * user's problem's business (milliseconds to weeks), so the consumer does not guess. A lost client
 * is noticed by its connection (both transports give signs of life), and what it held is handed out
 * again at once. A client that hangs inside an evaluation while its process still answers is NOT
 * noticed that way; whoever needs a bound for it sets @c reclaim_lease_ms.
 *
 * This is the server-side question "when is an item that has not come back handed out again, or
 * given up". It is distinct from the algorithm's GSubmissionPolicy, which decides what to do about
 * an item that was given up (resubmit, clone, fatal). The values are applied to every networked
 * consumer through GNetworkedConsumerT::applyTimeoutConfig(), which refuses a negative value.
 */
struct GNetworkedTimeoutConfig {
    /// An item in flight longer than this is handed out again (0: never)
    std::int64_t reclaim_lease_ms = 0;
    /// A batch without any result for longer than this, counted from its start or its last
    /// result, is given up: its open items go to the submission policy (0: never)
    std::int64_t give_up_window_ms = 0;

    /**
     * @brief Reads the configuration from @p configFile, writing the defaults if it is absent.
     * @param configFile Path to the Geneva-style JSON config file (created with defaults if missing)
     */
    void load(const std::string &configFile) {
        Gem::Common::GParserBuilder gpb;
        gpb.registerFileParameter<std::int64_t>(
            "reclaim_lease_ms",
            reclaim_lease_ms,
            reclaim_lease_ms,
            Gem::Common::VAR_IS_ESSENTIAL,
            "An item in flight longer than this (ms) is handed out to another client; 0 (the "
            "default): never. The one bound on a client that hangs while its process answers"
        );
        gpb.registerFileParameter<std::int64_t>(
            "give_up_window_ms",
            give_up_window_ms,
            give_up_window_ms,
            Gem::Common::VAR_IS_ESSENTIAL,
            "A batch without any result for longer than this (ms), counted from its start or its "
            "last result, is given up and its open items go to the submission policy; 0 (the "
            "default): never"
        );
        gpb.parseConfigFile(configFile);
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier */
