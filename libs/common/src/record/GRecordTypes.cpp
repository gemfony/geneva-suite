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

#include "common/record/GRecordTypes.hpp"

namespace Gem::Common::Record {

/******************************************************************************/
/**
 * The name of a column type, for messages.
 *
 * @param type The column type
 * @return "integer", "unsigned", "floating", "boolean", "text" or "timestamp"
 */
std::string to_string(columnType type) {
    switch(type) {
    case columnType::INTEGER:
        return "integer";
    case columnType::UNSIGNED:
        return "unsigned";
    case columnType::FLOATING:
        return "floating";
    case columnType::BOOLEAN:
        return "boolean";
    case columnType::TEXT:
        return "text";
    case columnType::TIMESTAMP:
        return "timestamp";
    }
    return "unknown";
}

/******************************************************************************/

} /* namespace Gem::Common::Record */
