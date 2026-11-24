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

/*
 * The configuration of the MPI cascade (GMPICascade.hpp): how many processes a group has, how the
 * processes of the base communicator are laid out, and how long a wait of the cascade pauses
 * between two looks. This header includes no MPI header: a program reads and writes the
 * configuration file, and a test checks it, without MPI.
 */

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard headers go here
#include <chrono>
#include <cstdint>
#include <string>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierEnums.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * How the processes of a cascade's base communicator are laid out. The layout follows from how the
 * program is started (the consumer it is started for), so it is set by the program and is not a
 * key of the configuration file.
 */
enum class GMPICascadeLayout : Gem::Common::ENUMBASETYPE {
    SUBMITTER_AND_GROUPS =
        0,          ///< base rank 0 submits through courtier's MPI protocol; groups follow
    GROUPS_ONLY = 1 ///< every rank belongs to a group; the leads reach their work otherwise
};

/******************************************************************************/
/**
 * @brief Refuses poll pauses that a polled wait cannot use: it starts at @p pause_min and doubles
 * the pause up to @p pause_max, so both must be positive and in order. The one statement of this
 * rule; GMPICascadeConfig::check() and the polled helpers (GMPIPolledCalls.hpp) use it.
 *
 * @param pause_min The first pause after a look that found nothing
 * @param pause_max The longest pause
 * @throws gem_exception unless 0 < pause_min <= pause_max; the text names both values
 */
inline void
checkPollPauses(std::chrono::microseconds pause_min, std::chrono::microseconds pause_max) {
    if(pause_min.count() <= 0 || pause_max < pause_min) {
        throw gem_exception(
            g_error_streamer()
            << "In checkPollPauses(): Error!" << '\n'
            << "The poll pauses must satisfy 0 < pause_min <= pause_max, but pause_min is "
            << pause_min.count() << " us and pause_max is " << pause_max.count() << " us" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * @brief The configuration of a cascade (GMPICascade). An aggregate, as the consumers'
 * configuration structs: a program may fill it itself or read the group size and the poll
 * pauses from the cascade's configuration file with load().
 *
 * Only rank 0 of the base communicator provides it; the cascade broadcasts rank 0's values to every
 * rank and checks them there (check()), so that every rank refuses a bad configuration alike.
 */
struct GMPICascadeConfig {
    /// The processes of one group, the lead included; the key sub_client_group_size of the file
    std::uint16_t groupSize = GMPICASCADEDEFAULTGROUPSIZE;
    /// How the base communicator is laid out; set by the program, no key
    GMPICascadeLayout layout = GMPICascadeLayout::SUBMITTER_AND_GROUPS;
    /// The first pause of a polled wait of the cascade after a look that found nothing; the key
    /// sub_client_poll_pause_min_us of the file
    std::chrono::microseconds pauseMin = GMPIPEERPOLLPAUSEMIN;
    /// The longest pause of a polled wait of the cascade; the key sub_client_poll_pause_max_us
    std::chrono::microseconds pauseMax = GMPIPEERPOLLPAUSEMAX;

    /**
     * @brief Reads the group size and the poll pauses from @p config_file (keys
     * "sub_client_group_size", "sub_client_poll_pause_min_us", "sub_client_poll_pause_max_us"),
     * writing the file with the current values as its defaults if it is absent; under
     * --update-configs the file is refreshed in place. The values are not checked here: the
     * cascade checks the configuration it received from rank 0 (check()), whoever filled it.
     *
     * @param config_file The path of the configuration file (usually GMPICASCADECONFIGFILE)
     * @throws gem_exception if the file cannot be read or parsed: there is no silent default
     */
    void load(const std::string &config_file) {
        Gem::Common::GParserBuilder gpb;
        gpb.registerFileParameter<std::uint16_t>(
            "sub_client_group_size",
            groupSize,
            groupSize,
            Gem::Common::VAR_IS_ESSENTIAL,
            "The number of processes of one group of the MPI cascade, the lead included; each "
            "group "
            "evaluates one item at a time"
        );
        std::int64_t pause_min_us = pauseMin.count();
        std::int64_t pause_max_us = pauseMax.count();
        gpb.registerFileParameter<std::int64_t>(
            "sub_client_poll_pause_min_us",
            pause_min_us,
            pause_min_us,
            Gem::Common::VAR_IS_ESSENTIAL,
            "The shortest pause in microseconds between two looks of a waiting process of the MPI "
            "cascade (a member waiting for its lead, a lead waiting for its members); the pause "
            "doubles up to sub_client_poll_pause_max_us while nothing comes. Greater than 0"
        );
        gpb.registerFileParameter<std::int64_t>(
            "sub_client_poll_pause_max_us",
            pause_max_us,
            pause_max_us,
            Gem::Common::VAR_IS_ESSENTIAL,
            "The longest pause in microseconds between two looks of a waiting process of the MPI "
            "cascade; at least sub_client_poll_pause_min_us"
        );
        gpb.parseConfigFile(config_file);
        pauseMin = std::chrono::microseconds(pause_min_us);
        pauseMax = std::chrono::microseconds(pause_max_us);
    }

    /**
     * @brief Refuses a configuration a cascade cannot use: a group size of 0, a layout that is
     * not one of the two, or poll pauses that break checkPollPauses()'s rule.
     *
     * @throws gem_exception naming what is wrong
     */
    void check() const {
        if(groupSize == 0) {
            throw gem_exception(
                g_error_streamer()
                << "In GMPICascadeConfig::check(): Error!" << '\n'
                << "The group size of the MPI cascade (sub_client_group_size) is 0; a group has "
                << "at least one process, its lead" << '\n'
            );
        }
        if(layout != GMPICascadeLayout::SUBMITTER_AND_GROUPS &&
           layout != GMPICascadeLayout::GROUPS_ONLY) {
            throw gem_exception(
                g_error_streamer() << "In GMPICascadeConfig::check(): Error!" << '\n'
                                   << "The layout of the MPI cascade has the unknown value "
                                   << static_cast<Gem::Common::ENUMBASETYPE>(layout) << '\n'
            );
        }
        checkPollPauses(pauseMin, pauseMax);
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier */
