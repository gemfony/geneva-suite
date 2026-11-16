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
#include <cstddef>
#include <string>
#include <string_view>

// Geneva header files go here
#include "common/GCommonEnums.hpp"     // serializationMode
#include "courtier/GCourtierEnums.hpp" // the consumer defaults

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The configuration of the thread-pool consumer GStdThreadConsumerT (mnemonic "stc").
 *
 * A plain aggregate, to be copied freely. It includes no network library, so a program can name the
 * consumer by its configuration without seeing the consumer's header.
 */
struct GStdThreadConsumerConfig {
    /// The mnemonic of the consumer family this struct configures, as chosen with --consumer
    static constexpr std::string_view MNEMONIC = "stc";

    /// The number of worker threads; 0: as many as the machine has hardware threads
    unsigned int nThreads = 0;
};

/******************************************************************************/

} /* namespace Gem::Courtier */
