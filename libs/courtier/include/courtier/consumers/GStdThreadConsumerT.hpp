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
#include <atomic>
#include <cstddef>
#include <memory>
#include <span>
#include <thread>
#include <vector>

// Geneva headers (reused from the common library)
#include "common/concurrency/GThreadPool.hpp"
#include "courtier/GBaseConsumerT.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * A local, multi-threaded consumer. Each round's items are evaluated concurrently on a
 * Gem::Common::Concurrency::GThreadPool. A local evaluation never goes MISSING -- it either succeeds
 * (PROCESSED) or, when the user's evaluate() throws, is funnelled by
 * GProcessingContainerT::process() into the item's EXCEPTION_CAUGHT status (the throw is caught
 * here so it never escapes the worker thread). Reconciliation against the policy is inherited
 * from GBaseConsumerT.
 *
 * @tparam processable_type The work-item type evaluated by the worker threads.
 */
template <typename processable_type>
class GStdThreadConsumerT final : public GBaseConsumerT<processable_type> {
public:
    using item_ptr = typename GBaseConsumerT<processable_type>::item_ptr;

    /***************************************************************************/
    /** @brief Initialization with the number of worker threads (0 == hardware concurrency).
     *  @param n_threads Number of worker threads in the pool; 0 means use the hardware concurrency. */
    explicit GStdThreadConsumerT(unsigned int n_threads = 0)
        : pool_(
              "consumer:stc",
              n_threads == 0 ? default_threads() : n_threads,
              Gem::Common::Concurrency::ThreadElasticity::Elastic
          )
    {
        instances_constructed().fetch_add(1, std::memory_order_relaxed);
    }

    ~GStdThreadConsumerT() override = default;

    /***************************************************************************/
    /** @brief Process-wide count of how many thread-pool consumers (of this work-item type) have ever
     *  been constructed. Each instance owns its own GThreadPool, so this also counts the worker pools
     *  built. The single-shared-consumer goal is that every un-injected algorithm needing this KIND
     *  converges on ONE shared instance, so a whole process (even a nested EA-in-EA) builds exactly one;
     *  the count is the observable for that invariant. Lightweight (one relaxed atomic add per ctor).
     *  @return A reference to the process-wide construction counter. */
    static std::atomic<std::size_t> &instances_constructed() {
        static std::atomic<std::size_t> counter{0};
        return counter;
    }

protected:
    /***************************************************************************/
    /**
     * @brief Evaluates all items of one round concurrently. process() sets PROCESSED on success and
     * EXCEPTION_CAUGHT on a caught processing exception (which it also re-throws -- swallowed here).
     *
     * Delegates the fork/join to GThreadPool::blocking_for_each, which waits on a PER-BATCH latch rather
     * than GThreadPool::wait() (a global drain barrier): the pool is shared, so several algorithms can
     * submit concurrently (the fan-in case -- e.g. a meta-optimization over a population of inner
     * algorithms), and each must wait for ONLY its own items, not the whole pool.
     *
     * @param items The work items of one round; each is evaluated on the shared pool and the call blocks until all have finished
     */
    void dispatch_(std::span<item_ptr> items) override {
        // Run the batch across the shared pool and block until it finishes. Only DO_PROCESS slots are
        // evaluated (null/already-resolved slots are no-ops); an item that throws records its own failure
        // status (EXCEPTION_CAUGHT), which blocking_for_each swallows so it never escapes the worker.
        pool_.blocking_for_each(items, [](item_ptr &it) {
            if(it && it->getProcessingStatus() == Gem::Courtier::processingStatus::DO_PROCESS) {
                it->process();
            }
        });
    }

private:
    /** @brief Default number of worker threads when none was requested.
     *  @return The hardware concurrency, or 1 if it cannot be determined. */
    static unsigned int default_threads() {
        const unsigned int hc = std::thread::hardware_concurrency();
        return hc == 0 ? 1u : hc;
    }

    Gem::Common::Concurrency::GThreadPool pool_;
};

/******************************************************************************/

} /* namespace Gem::Courtier */
