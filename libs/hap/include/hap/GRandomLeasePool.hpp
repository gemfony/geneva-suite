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
#include <memory>
#include <utility>

// Geneva headers go here
#include "common/concurrency/GMPMCQueueT.hpp"
#include "hap/GRandomBase.hpp"
#include "hap/GRandomT.hpp"

namespace Gem::Hap {

/******************************************************************************/
/**
 * @brief A process-wide pool of reusable RNG proxies, leased for one unit of work at a time.
 *
 * This replaces the former per-individual random engine. An optimization candidate is pure data and
 * owns no RNG state; whenever an @e external operation needs randomness on that candidate (adaption,
 * random initialization, cross-over position) it leases a proxy from this pool for the duration of the
 * operation and returns it automatically (RAII). Consequently the number of live proxies is bounded by
 * the @b peak concurrency (O(worker threads)), not by the population size (which was O(population) when
 * every candidate carried its own proxy). Under the @c QUEUE source a proxy holds an 80&nbsp;KB package,
 * so this is also the difference between a handful of packages and one per candidate.
 *
 * @c acquire() hands out a free proxy, or constructs a fresh one if none is free -- so it @b never
 * blocks. The returned @c Lease returns the proxy to the pool on destruction. A returned proxy keeps
 * its (partially consumed) state, so a burst of adaptions amortizes package usage across the O(threads)
 * proxies rather than pulling a new package per candidate.
 *
 * Thread-safety: the free list is a shared MPMC queue, so @c acquire()/release are safe to call from
 * many worker threads at once (e.g. the EA's parallel @c adaptChildren_). Each leased proxy is used by
 * exactly one thread for the lifetime of its lease, which is what a @c GRandomT proxy requires.
 *
 * Lifetime: the process-global instance (@c randomLeasePool()) is a function-local static, first
 * constructed when a run first needs randomness -- i.e. after the shared random factory is already
 * online -- and therefore destroyed @e before the factory's process-lifetime guard at shutdown, so a
 * proxy returning its package during pool teardown always finds the factory alive.
 */
class GRandomLeasePool {
public:
    /**
     * @brief An RAII handle to a leased proxy; returns it to the pool on destruction.
     */
    class Lease {
    public:
        Lease(GRandomLeasePool &pool, std::unique_ptr<GRandom> proxy) noexcept
          : pool_(&pool)
          , proxy_(std::move(proxy)) { /* nothing */ }

        Lease(Lease &&other) noexcept
          : pool_(other.pool_)
          , proxy_(std::move(other.proxy_)) { other.pool_ = nullptr; }

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease &operator=(Lease &&) = delete;

        ~Lease() {
            if(pool_ != nullptr && proxy_) { pool_->release(std::move(proxy_)); }
        }

        /** @brief Access the leased proxy through the uniform GRandomBase seam. */
        [[nodiscard]] GRandomBase &operator*() const noexcept { return *proxy_; }
        [[nodiscard]] GRandomBase *operator->() const noexcept { return proxy_.get(); }

    private:
        GRandomLeasePool        *pool_;  ///< the owning pool (null once moved-from)
        std::unique_ptr<GRandom> proxy_; ///< the leased proxy
    };

    GRandomLeasePool() = default;
    GRandomLeasePool(const GRandomLeasePool &) = delete;
    GRandomLeasePool &operator=(const GRandomLeasePool &) = delete;
    GRandomLeasePool(GRandomLeasePool &&) = delete;
    GRandomLeasePool &operator=(GRandomLeasePool &&) = delete;
    ~GRandomLeasePool() = default;

    /**
     * @brief Leases a proxy: a recycled one if free, otherwise a freshly constructed one (never blocks).
     * @return A Lease that returns the proxy to the pool when it goes out of scope
     */
    [[nodiscard]] Lease acquire() {
        if(auto recycled = free_.try_pop()) {
            return Lease(*this, std::move(*recycled));
        }
        return Lease(*this, std::make_unique<GRandom>());
    }

private:
    /** @brief Returns a proxy to the free list for reuse (called by ~Lease). */
    void release(std::unique_ptr<GRandom> proxy) {
        static_cast<void>(free_.push(std::move(proxy))); // unbounded queue: push never fails
    }

    /// The free list of recyclable proxies (unbounded -> release never drops a proxy).
    Gem::Common::Concurrency::GMPMCQueueT<std::unique_ptr<GRandom>, 0> free_;
};

/******************************************************************************/
/**
 * @brief The process-global RNG lease pool (see GRandomLeasePool).
 * @return A reference to the single process-wide lease pool
 */
inline GRandomLeasePool &randomLeasePool() {
    static GRandomLeasePool pool;
    return pool;
}

/******************************************************************************/

} /* namespace Gem::Hap */
