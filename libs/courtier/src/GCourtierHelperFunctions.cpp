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

#include "courtier/GCourtierHelperFunctions.hpp"
#include "courtier/GCourtierEnums.hpp"
#include <string>


namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief Translate the processingStatus into a clear-text string
 *
 * @param ps The processingStatus to be translated into a std::string
 * @return A string representing the processing status (empty string for an unrecognized value)
 */
std::string psToStr(const processingStatus &ps) {
    switch(ps) {
        using enum Gem::Courtier::processingStatus;
    case UNPROCESSED:
        return "UNPROCESSED";

    case DO_PROCESS:
        return "DO_PROCESS";

    case PROCESSED:
        return "PROCESSED";

    case EXCEPTION_CAUGHT:
        return "EXCEPTION_CAUGHT";

    case ERROR_FLAGGED:
        return "ERROR_FLAGGED";
    }

    // Make the compiler happy
    return {};
}

/******************************************************************************/
/**
 * @brief Translates a protocol frame kind into a clear-text string
 *
 * @param fk The GFrameKind to be translated into a std::string
 * @return A string representing the frame kind (empty string for an unrecognized value)
 */
std::string fkToStr(const GFrameKind &fk) {
    switch(fk) {
        using enum Gem::Courtier::GFrameKind;
    case NONE:
        return "NONE";

    case PULL:
        return "PULL";

    case NO_WORK:
        return "NO_WORK";

    case WORK:
        return "WORK";

    case RETURN:
        return "RETURN";

    case SHUTDOWN:
        return "SHUTDOWN";

    case BLOB_REQUEST:
        return "BLOB_REQUEST";

    case BLOB_REPLY:
        return "BLOB_REPLY";
    }

    // Make the compiler happy
    return {};
}

/******************************************************************************/

} /* namespace Gem::Courtier */
