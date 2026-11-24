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
// numeric_enum_values_v machinery in GCommonEnums.hpp. Only tribool's textual
// insertion operator needs an implementation of its own.

/******************************************************************************/
/**
 * Puts a Gem::Common::tribool into a stream, as its name (enum_names_v)
 *
 * @param o The output stream to write to
 * @param x The tribool value to write (True / False / Indeterminate)
 * @return The output stream, to allow chaining ("<invalid value N>" for a number cast to tribool
 * that is none of its values)
 */
std::ostream &operator<<(std::ostream &o, Gem::Common::tribool const &x) {
    o << enumName(x);
    return o;
}

/******************************************************************************/
/**
 * The modes for a help text, built from SERIALIZATIONMODES.
 *
 * @return "GEM_BINARY (3) or GEM_JSON (4)", for the modes there are
 */
std::string serializationModeChoices() {
    std::string text;
    for(std::size_t m = 0; m < SERIALIZATIONMODES.size(); ++m) {
        if(m > 0) {
            text += (m + 1 == SERIALIZATIONMODES.size()) ? " or " : ", ";
        }
        text += enumName(SERIALIZATIONMODES.at(m)) + " (" +
                std::to_string(std::to_underlying(SERIALIZATIONMODES.at(m))) + ")";
    }
    return text;
}

/******************************************************************************/

} /* namespace Gem::Common */
