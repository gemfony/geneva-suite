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

#include "common/GCommonEnums.hpp"
#include <ostream>
#include <string>
#include <utility>

namespace Gem::Common {

/******************************************************************************/
// The numeric enum stream operators (sortOrder, dimensions, logType,
// serializationMode, expectation) are supplied by the shared
// numeric_enum_io_v machinery in GCommonEnums.hpp. Only tribool's textual
// insertion operator needs a hand-written implementation.

/******************************************************************************/
/**
 * Puts a Gem::Common::tribool into a stream
 *
 * @param o The output stream to write to
 * @param x The tribool value to write (True / False / Indeterminate)
 * @return The output stream, to allow chaining
 */
std::ostream &operator<<(std::ostream &o, Gem::Common::tribool const &x) {
    switch(x) {
    case Gem::Common::tribool::True:          o << "True";          break;
    case Gem::Common::tribool::False:         o << "False";         break;
    case Gem::Common::tribool::Indeterminate: o << "Indeterminate"; break;
    default:
        // Guard against corrupted-stream deserialisation (>> casts any
        // ENUMBASETYPE to tribool without validation). Without this arm
        // the stream would be left untouched, silently producing empty
        // output instead of a diagnostic.
        o << "tribool::?(" << std::to_underlying(x) << ")";
        break;
    }
    return o;
}

/******************************************************************************/
/**
 * Converts a serializationMode to a string representation for debugging purposes
 *
 * @param ser_mod The serialization mode to convert
 * @return A string representation of the mode ("TEXT", "XML", "BINARY", or "unknown")
 */
std::string serModeToString(Gem::Common::serializationMode ser_mod) {
    switch(ser_mod) {
    case Gem::Common::serializationMode::GEM_BINARY:
        return "GEM_BINARY";
    case Gem::Common::serializationMode::GEM_JSON:
        return "GEM_JSON";
    default:
        return "unknown";
    }
}

/******************************************************************************/

} /* namespace Gem::Common */
