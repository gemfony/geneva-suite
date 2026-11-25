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

#include "geneva/oa/GMonitorStream.hpp"

// Standard header files go here
#include <utility>

// Geneva header files go here
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * Declares the stream in the recorder: "algorithm", "iteration" and "chain_iteration", then the
 * monitor's own columns. A run that was resumed from a checkpoint appends to the files of the
 * stream when their columns are the same (the algorithm keeps its resume state until after
 * INFOINIT), after cutting them back to the checkpoint: the rows whose chain_iteration is past the
 * checkpoint's -- the one before the resumed run's first, counted through the chain with the chain
 * offset the checkpoint holds (GOptimizationAlgorithmBase::getChainOffset()) -- are dropped
 * (GStreamOptions::resumeColumn).
 *
 * @param name The name of the stream
 * @param columns The monitor's own columns
 * @param goa The algorithm, for the resume of its run
 * @param sinks The sinks (none: one CSV file)
 */
void GMonitorStream::declare(
    std::string const &name,
    std::vector<Gem::Common::Record::GColumn> columns,
    GOptimizationAlgorithmBase const &goa,
    std::vector<std::shared_ptr<Gem::Common::Record::GSink>> sinks
) {
    using Gem::Common::Record::columnType;
    using Gem::Common::Record::GColumn;
    const bool resumed = goa.resumedFromCheckpoint();
    columns.insert(
        columns.begin(),
        {GColumn{.name = "algorithm", .type = columnType::TEXT},
         GColumn{.name = "iteration", .type = columnType::UNSIGNED},
         GColumn{.name = "chain_iteration", .type = columnType::UNSIGNED}}
    );
    stream_ = Gem::Common::Record::recorder().stream(
        name,
        std::move(columns),
        Gem::Common::Record::withTime::none,
        {.sinks = std::move(sinks),
         .resume = resumed,
         .resumeColumn = resumed ? "chain_iteration" : "",
         .resumeAfter = static_cast<double>(chainIteration(goa)) - 1.}
    );
}

/******************************************************************************/
/**
 * The start of a row of the algorithm's current iteration.
 *
 * @param goa The algorithm
 * @return Its name, its iteration and the iteration counted through the chain
 */
Gem::Common::Record::GRow GMonitorStream::row(GOptimizationAlgorithmBase const &goa) {
    return {goa.getAlgorithmName(), std::uint64_t{goa.getIteration()}, chainIteration(goa)};
}

/******************************************************************************/
/**
 * The algorithm's current iteration counted through a GOptimizer chain.
 *
 * @param goa The algorithm
 * @return Its iteration plus the iterations of the chain before it
 */
std::uint64_t GMonitorStream::chainIteration(GOptimizationAlgorithmBase const &goa) {
    return std::uint64_t{goa.getIteration()} + goa.getChainOffset();
}

/******************************************************************************/
/**
 * Appends a row.
 *
 * @param row The row, beginning with row()
 */
void GMonitorStream::append(Gem::Common::Record::GRow row) {
    stream_.append(std::move(row));
}

/******************************************************************************/
/**
 * Whether the holder has a stream that is open.
 *
 * @return Whether it has
 */
bool GMonitorStream::isOpen() const {
    return stream_.isOpen();
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
