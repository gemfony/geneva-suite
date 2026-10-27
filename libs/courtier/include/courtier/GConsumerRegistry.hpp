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

// Standard headers
#include <functional>
#include <memory>
#include <mutex>

// Geneva headers
#include "common/GSingletonT.hpp"
#include "courtier/GBaseConsumerT.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * The process-global holder for the SINGLE consumer a server binary uses -- the one work endpoint every
 * optimization algorithm submits through. An algorithm is transport-agnostic: it never builds or selects
 * a consumer, it just asks the registry for the one consumer and calls processBatch(), with the
 * guarantee of an intact, fully-evaluated population on return -- never knowing whether evaluation
 * happened locally, on a GPU, or on a remote client. The consumer is established once: by Go2 (from the
 * chosen mnemonic / command-line default), or manually by the user (setConsumer); a standalone algorithm
 * with none set lazily builds a default local thread-pool consumer (ensureConsumer). There is never more
 * than one consumer per process (a meta-optimization runs its sub-optimizations on a separate
 * orchestration thread pool that is NOT a consumer -- see GMetaEvolutionaryAlgorithm).
 *
 * The single instance is handed out via GSingletonT (it outlives ordinary statics and is reachable
 * concurrently). All access is mutex-guarded, so concurrent submitters (e.g. the sub-optimizations of a
 * meta-optimization, run in parallel) race-freely converge on the one consumer. It is CLEARABLE (clear)
 * so unit tests can start from a known-empty state.
 *
 * @tparam processable_type The work-item type the consumer handles
 */
template <typename processable_type>
class GConsumerRegistryT {
public:
    using consumer_t = GBaseConsumerT<processable_type>;
    using consumer_ptr = std::shared_ptr<consumer_t>;

    GConsumerRegistryT() = default;

    /***************************************************************************/
    /** @brief The process-global registry instance for this work-item type.
     *  @return A reference to the single per-process GConsumerRegistryT<processable_type> */
    static GConsumerRegistryT &instance() {
        return *Gem::Common::GSingletonT<GConsumerRegistryT>::instance();
    }

    /***************************************************************************/
    /** @brief The process's single consumer, or nullptr if none has been established yet.
     *  @return The registered consumer, or nullptr */
    consumer_ptr consumer() {
        const std::scoped_lock lk(mtx_);
        return consumer_;
    }

    /***************************************************************************/
    /**
     * @brief Returns the process consumer, building and registering one via @p factory if none exists
     * yet. Idempotent under concurrency: only the first caller runs the factory; later callers (and
     * concurrent ones) get the same consumer, so a process has at most one. The factory runs under the
     * lock (consumer construction is cheap and one-shot).
     *
     * @param factory Builds a ready consumer (clone function set) when none is present
     * @return The process consumer (the pre-existing one, or the freshly built+registered one)
     */
    consumer_ptr ensureConsumer(const std::function<consumer_ptr()> &factory) {
        const std::scoped_lock lk(mtx_);
        if(consumer_) {
            return consumer_;
        }
        consumer_ = factory();
        return consumer_;
    }

    /***************************************************************************/
    /** @brief Registers (or replaces) the process consumer -- e.g. the one Go2 builds, or a custom one
     *  (a GPU consumer) the user supplies.
     *  @param c The consumer to register as the process's single consumer */
    void setConsumer(consumer_ptr c) {
        const std::scoped_lock lk(mtx_);
        consumer_ = std::move(c);
    }

    /***************************************************************************/
    /** @brief Drops the registered consumer, returning the holder to its empty state. Primarily for
     *  tests (start each from a known-empty state) and explicit teardown. */
    void clear() {
        const std::scoped_lock lk(mtx_);
        consumer_.reset();
    }

private:
    std::mutex mtx_;        ///< Guards the single consumer
    consumer_ptr consumer_; ///< The process's single consumer (null until established)
};

/******************************************************************************/

} /* namespace Gem::Courtier */
