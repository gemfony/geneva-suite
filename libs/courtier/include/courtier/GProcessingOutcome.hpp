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
#include <string>

// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named
#include "courtier/GCourtierEnums.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The processing LIFECYCLE a worker reports back for one dispatched slot -- courtier's own half of a
 * RETURN frame.
 *
 * Everything here is transport/scheduling state that courtier itself acts on: the status decides whether
 * the reconciliation loop sees a slot as processed, failed or missing; the error text is what a failure
 * reports; the timings feed the adaptive timeout; the correlation id routes the return to its slot. None
 * of it says anything about the PROBLEM being solved -- that lives in the frame's library tag and payload,
 * which courtier only relays.
 *
 * Carrying the lifecycle in the frame rather than inside a returned work item is what makes a
 * payload-only return possible at all: a library may answer with a small results record instead of a whole
 * work item, and the slot's status/timing/routing still arrive intact.
 */
struct GProcessingOutcome {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /**
     * @brief (De)serialises the outcome. Written only for RETURN frames (see GCommandContainerT).
     * @tparam Archive The GArchive codec type
     * @param ar The archive to read from / write to
     * @param version The (unused) serialization version number
     */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        archive_named(ar, "status", status);
        archive_named(ar, "error_descriptions", error_descriptions);
        archive_named(ar, "pre_processing_time", pre_processing_time);
        archive_named(ar, "processing_time", processing_time);
        archive_named(ar, "post_processing_time", post_processing_time);
        archive_named(ar, "correlation_id", correlation_id);
        archive_named(ar, "submission_uuid_hi", submission_uuid[0]);
        archive_named(ar, "submission_uuid_lo", submission_uuid[1]);
    }
    ///////////////////////////////////////////////////////////////////////

    processingStatus status = processingStatus::UNPROCESSED; ///< How the evaluation ended
    std::string error_descriptions;                          ///< Accumulated error text (empty on success)
    double pre_processing_time = 0.;                         ///< Pre-processing duration (seconds)
    double processing_time = 0.;                             ///< Processing duration (seconds)
    double post_processing_time = 0.;                        ///< Post-processing duration (seconds)
    CORRELATION_ID_TYPE correlation_id = 0;                  ///< (batch, slot) routing token of the slot
    SUBMISSION_UUID_TYPE submission_uuid{0, 0};              ///< The item's stable lineage id (informational)
};

/******************************************************************************/

} /* namespace Gem::Courtier */
