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
#include <string_view>

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief The commit of the Geneva sources this library was built from: the 40 hexadecimal digits
 * of the git commit id, followed by "-dirty" when tracked files differed from that commit.
 *
 * A server of the networked consumers names it in the HELLO it greets each client with, and a
 * client built from another commit ends (courtier/GWireCodec.hpp). The build takes it at configure
 * time and again at every build, from git or from the file GENEVA_COMMIT at the top of the sources
 * (CMakeModules/GenevaCommit.cmake says how; a tree without either does not configure). The
 * definition is a source the build writes into the build directory.
 *
 * @return The commit id; the text lives as long as the program
 */
[[nodiscard]] std::string_view genevaCommit() noexcept;

/******************************************************************************/

} /* namespace Gem::Common */
