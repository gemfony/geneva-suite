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

#include "common/GGlobalDefines.hpp"

#include <cstdint>
#include <memory>

namespace Gem::Hap {
class GRandomBase;
} // namespace Gem::Hap

namespace Gem::Geneva::Genome {
class GGenome;
} // namespace Gem::Geneva::Genome

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief A pluggable, process-global fault injector consulted during an individual's process().
 *
 * This is the data-oriented successor of the retired per-individual "random crash" debugging facility:
 * instead of every individual carrying a crash probability, a single injector is registered once (like a
 * pluggable optimization monitor) and consulted at evaluation time. It lets tests and diagnostics
 * deterministically -- or probabilistically, via the individual's own RNG -- simulate evaluation
 * failures, to exercise the consumer's / optimization algorithm's error-handling and recovery paths.
 *
 * The facility is geneva-side and carries NO per-individual state and NO courtier dependency. When no
 * injector is registered (the default) the consultation is a single null-pointer check, so production
 * runs pay essentially nothing.
 *
 * A returned THROW surfaces as an EXCEPTION_CAUGHT processing status (as if evaluate() threw);
 * a returned FLAG_ERROR surfaces as an ERROR_FLAGGED status (as if the individual flagged a user error).
 * Both are handled by process() exactly like a genuine evaluation failure. The transport-level
 * "worker never returns" fault is a separate, consumer-side concern and is NOT modelled here.
 */
class GFaultInjector {
public:
    /** @brief The kind of fault to inject for a given evaluation. */
    enum class Fault : std::uint8_t {
        NONE = 0,       ///< Inject nothing; the evaluation proceeds normally
        THROW = 1,      ///< Abort the evaluation with an exception (-> EXCEPTION_CAUGHT)
        FLAG_ERROR = 2  ///< Abort the evaluation with a flagged error (-> ERROR_FLAGGED)
    };

    /** @brief The virtual destructor. */
    virtual ~GFaultInjector() = default;

    /**
     * @brief Decides which fault (if any) to inject for the given individual's imminent evaluation.
     *
     * Called from GGenome::process() before the objective is evaluated. Implementations may
     * inspect the individual and draw from its RNG stream to make a (probabilistic) decision. Must not
     * mutate the individual's parameters.
     *
     * @param item The individual about to be evaluated
     * @param gr The individual's own random-number stream (for probabilistic decisions)
     * @return The fault to inject (Fault::NONE for no fault)
     */
    virtual Fault evaluate(const Genome::GGenome &item, Gem::Hap::GRandomBase &gr) = 0;
};

/******************************************************************************/
/**
 * @brief The process-global holder for the active GFaultInjector (geneva-side, no courtier dependency).
 *
 * Null unless a test / diagnostic registers an injector (directly, or via Go2::registerFaultInjector()).
 * get() returns a raw, non-owning pointer so the per-evaluation consultation on the hot path is a plain
 * pointer read with no reference-count churn. The injector is expected to be registered before an
 * optimization run starts and cleared afterwards, so concurrent evaluation threads only ever read it.
 */
class GFaultInjectorRegistry {
public:
    /** @brief Registers (or replaces) the process-global injector. @param injector The injector to hold */
    static void set(std::unique_ptr<GFaultInjector> injector);
    /** @brief @return A non-owning pointer to the active injector, or nullptr if none is registered */
    static GFaultInjector *get() noexcept;
    /** @brief Removes any registered injector (restores the zero-cost default). */
    static void clear() noexcept;
};

/******************************************************************************/

} // namespace Gem::Geneva
