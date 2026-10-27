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
#include <cstdint>
#include <vector>

// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named
#include "common/GLogger.hpp"
#include "courtier/GWireProtocolT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GIndividualProcessingResult.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
/**
 * What a message MEANS in Geneva's use of the courtier transport.
 *
 * Courtier owns the frame vocabulary (GFrameKind) and never learns these: it carries the tag as an
 * opaque number and hands it back to the seam below. Geneva alone decides what each one implies.
 *
 * The values are part of the wire format and are therefore stable.
 */
//! [courtier-geneva-commands#1]
enum class geneva_command : std::uint32_t {
    evaluate = 0,               ///< server -> worker: evaluate this individual
    evaluation = 1,             ///< worker -> server: the computed results of an unmodified individual
    evaluated_and_modified = 2, ///< worker -> server: the whole individual, whose genome the worker changed
    terminate = 3               ///< server -> worker: stop working
};
//! [courtier-geneva-commands#1]

/******************************************************************************/
/**
 * The payload of an `evaluation` message: exactly the two OUTPUTS an evaluation produces.
 *
 * A worker that did not change the genome has nothing to return but these -- the result store and the
 * feasibility level. Everything else about the individual (its parameters, its structural layout, its
 * pre-/post-processors, the shared problem policy, the algorithm-owned scratch) the server already holds
 * and keeps. Shipping the individual back to carry two small arrays was what the old degenerate
 * "individual with its genome omitted" encoding did; this says the same thing in the size it deserves,
 * and -- unlike that encoding -- its size does not grow with whatever data a derived individual happens
 * to carry.
 *
 * Courtier never reads this: it is the library payload of a RETURN frame, relayed as opaque content.
 */
//! [courtier-geneva-commands#2]
struct GEvaluationResults {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /** @brief (De)serialises the results and the validity level.
     *  @tparam Archive The GArchive codec type
     *  @param ar The archive to read from / write to
     *  @param version The (unused) serialization version number */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named(ar, "results", results);
        Gem::Common::archive_named(ar, "validity_level", validity_level);
    }
    ///////////////////////////////////////////////////////////////////////

    std::vector<individual_processing_result> results; ///< one {raw, transformed, set} per criterion
    double validity_level = 0.;                        ///< the computed feasibility level (<= 1 == feasible)
};
//! [courtier-geneva-commands#2]

/******************************************************************************/

} /* namespace Gem::Geneva::Genome */

namespace Gem::Courtier {

/******************************************************************************/
/**
 * Geneva's answers to the three questions courtier cannot answer about a message.
 *
 * This specialization is defined in a header that GGenome.hpp itself includes, so no translation unit
 * can instantiate a courtier transport for a Geneva individual without it -- the primary template's
 * degenerate protocol can never be picked up by accident.
 */
template <>
struct GWireProtocolT<Gem::Geneva::Genome::GGenome> {
    /** @brief An ordinary return carries only what an evaluation produced. */
    using payload_type = Gem::Geneva::Genome::GEvaluationResults;

    /** @brief @return The tag on a dispatched work item: evaluate it. */
    static std::uint32_t workTag() {
        return static_cast<std::uint32_t>(Gem::Geneva::Genome::geneva_command::evaluate);
    }

    /** @brief @return The tag on a shutdown frame: stop working. */
    static std::uint32_t shutdownTag() {
        return static_cast<std::uint32_t>(Gem::Geneva::Genome::geneva_command::terminate);
    }

    /**
     * @brief Worker side: chooses the message a processed individual comes back as.
     *
     * An evaluation that left the genome alone -- the overwhelmingly common case -- answers with an
     * `evaluation`: the two computed outputs, nothing else. A worker that MODIFIED the individual (a
     * nested / tiered optimization that found a better point) has to send the whole thing back, and says
     * so with `evaluated_and_modified`. The choice is a message type, not a flag inside a payload.
     *
     * @param item The processed individual
     * @param tag Receives the geneva_command describing the return
     * @param payload Receives the computed results, for an `evaluation`
     * @return true if the whole individual travels back, false if @p payload replaces it
     */
    static bool buildReturn(
        const Gem::Geneva::Genome::GGenome &item,
        std::uint32_t &tag,
        payload_type &payload
    ) {
        tag = static_cast<std::uint32_t>(Gem::Geneva::Genome::geneva_command::evaluation);
        payload.results = item.getStoredResults();
        payload.validity_level = item.getValidityLevel();
        return false;
    }

    /**
     * @brief Server side: writes a returned message onto the live population element it belongs to.
     *
     * The element is reconciled IN PLACE -- its heap address is load-bearing (a concurrent per-individual
     * prefetch may hold a snapshot of population addresses across the submission), so nothing is swapped
     * out. The OA-owned scratch is never re-copied: it is stripped on the wire and the element's own copy
     * is the live one.
     *
     * @param slot The live population element the return belongs to
     * @param tag The geneva_command the worker chose
     * @param payload The computed results, for an `evaluation`
     * @param item The returned individual, for an `evaluated_and_modified`
     */
    static void applyReturn(
        Gem::Geneva::Genome::GGenome &slot,
        std::uint32_t tag,
        const payload_type *payload,
        const Gem::Geneva::Genome::GGenome *item
    ) {
        using Gem::Geneva::Genome::geneva_command;
        switch(static_cast<geneva_command>(tag)) {
        case geneva_command::evaluation:
            if(payload != nullptr) {
                // The genome, the layout and the algorithm-owned scratch stay the element's own: the
                // worker changed none of them, and the server has held them correctly all along.
                slot.setStoredResults(payload->results);
                slot.setValidityLevel(payload->validity_level);
            }
            break;

        case geneva_command::evaluated_and_modified:
            if(item != nullptr) {
                slot.absorbGenomeFrom(*item); // the worker changed the genome -> take it
                slot.absorbResultsFrom(*item);
            }
            break;

        default:
            glogger << "In GWireProtocolT<GGenome>::applyReturn():" << '\n'
                    << "Got a return carrying the unexpected geneva command " << tag << '\n'
                    << "The return is ignored; the slot keeps its previous state." << '\n'
                    << GWARNING;
            break;
        }
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier */
