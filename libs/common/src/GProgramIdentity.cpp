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

#include "common/GGenevaCommit.hpp"

// Standard headers go here
#include <string>
#include <string_view>
#include <utility>

// Geneva headers go here
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"

namespace Gem::Common {

namespace {

/******************************************************************************/
/**
 * The identity of the user's program. A function-local static, so that a static object of the
 * program (the stamp of GENEVA_STAMP_PROGRAM) can set it before main(), whatever the order of
 * static initialization. No lock: see setProgramIdentity().
 */
std::string &identity() {
    static std::string id;
    return id;
}

} /* namespace */

/******************************************************************************/
void setProgramIdentity(std::string identity_text) {
    std::string &id = identity();
    if(identity_text == id) {
        return;
    }
    if(not id.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, timeAndPlace())
            << "In Gem::Common::setProgramIdentity(): Error!" << '\n'
            << "The program's identity is \"" << id << "\" and cannot become \"" << identity_text
            << "\": a process has one identity, set once" << '\n'
        );
    }
    id = std::move(identity_text);
}

/******************************************************************************/
std::string_view programIdentity() noexcept {
    return identity();
}

/******************************************************************************/

} /* namespace Gem::Common */
