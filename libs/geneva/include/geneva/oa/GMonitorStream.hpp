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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Geneva headers go here
#include "common/record/GRecorder.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

class GOptimizationAlgorithmBase;

/******************************************************************************/
/**
 * The stream of a pluggable monitor (GBasePluggableOM) in the recorder (Gem::Common::Record), and
 * what every shipped monitor's rows share. A monitor holds one as a member:
 *
 * - **Transient.** It is no part of the monitor's configuration (Inv 4): a copy of it is empty,
 *   so a monitor's copy constructor can stay defaulted and a clone has no stream of its own; an
 *   assigned monitor keeps its own stream.
 * - **Declared at INFOINIT.** beginAlgorithm() says whether the monitor must declare (declare()):
 *   at the first INFOINIT, and after the stream was closed. In a Go2 chain the later algorithms
 *   find it open and record into the same stream. A run resumed from a checkpoint appends to the
 *   files (GStreamOptions::resume).
 * - **The first three columns.** Every row begins with "algorithm" (its name), "iteration" (the
 *   algorithm's own iteration) and "chain_iteration": the iteration counted through a Go2 chain,
 *   so that a plot over it does not jump back when the next algorithm starts at 0 (row()).
 */
class GMonitorStream {
public:
    /** @brief An empty holder */
    GMonitorStream() = default;
    /** @brief A copy is empty: the stream belongs to the object that declared it
     *  @param cp The holder copied from (its stream is not taken) */
    GMonitorStream([[maybe_unused]] GMonitorStream const &cp) { /* nothing */
    }
    /** @brief The target keeps its own stream @param cp The holder assigned from (nothing is
     *  taken) @return This holder */
    GMonitorStream &operator=([[maybe_unused]] GMonitorStream const &cp) {
        return *this;
    }
    /** @brief Takes over the stream of another holder */
    GMonitorStream(GMonitorStream &&) noexcept = default;
    /** @brief Closes this holder's stream and takes over that of another */
    GMonitorStream &operator=(GMonitorStream &&) noexcept = default;
    /** @brief Closes the stream (GStream) */
    ~GMonitorStream() = default;

    /** @brief At INFOINIT: whether the stream must be declared; else a chain goes on */
    [[nodiscard]] bool beginAlgorithm(GOptimizationAlgorithmBase const &goa);
    /** @brief Declares the stream: the three first columns, then the monitor's own */
    void declare(
        std::string const &name,
        std::vector<Gem::Common::Record::GColumn> columns,
        GOptimizationAlgorithmBase const &goa,
        std::vector<std::shared_ptr<Gem::Common::Record::GSink>> sinks = {}
    );
    /** @brief The start of a row: the values of the three first columns */
    [[nodiscard]] Gem::Common::Record::GRow row(GOptimizationAlgorithmBase const &goa);
    /** @brief Appends a row that begins with row() */
    void append(Gem::Common::Record::GRow row);
    /** @brief Whether the holder has a stream that is open */
    [[nodiscard]] bool isOpen() const;

private:
    Gem::Common::Record::GStream stream_; ///< The stream
    std::uint64_t chain_offset_ = 0;      ///< Added to the current algorithm's iteration
    std::uint64_t next_chain_iteration_ = 0; ///< The chain iteration after the last row
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
