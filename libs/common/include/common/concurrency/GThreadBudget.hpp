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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/GCommonHelperFunctions.hpp" // getNHardwareThreads
#include "common/GLogger.hpp"

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * Whether a pool can correctly run with fewer threads than it asked for.
 * Elastic pools (adaption/evaluation parallelism, io threads, RNG producers) are
 * correct at any worker count; Fixed pools (e.g. a client compute pool sized to
 * its prefetch depth) need exactly what they requested to make progress.
 */
enum class ThreadElasticity : std::uint8_t { Fixed, Elastic };

/******************************************************************************/
/**
 * The process-wide thread budget: a registry that knows how many threads the
 * process has collectively reserved and compares that to the hardware ceiling.
 * Geneva creates threads from many independent, mutually-unaware sources (the
 * OA organizational pool, the consumers' worker/io pools, the meta-EA
 * orchestration pool, Hap's RNG producers, the clients' compute pools); this is
 * the one place that sums them.
 *
 * The budget does ACCOUNTING AND ADVICE, not scheduling: reserve() never blocks,
 * never throws and never refuses a request. Every reservation is granted in
 * full and the budget's only behavioural surface is a rate-limited warning,
 * naming the largest reservations, when the reserved total crosses a multiple
 * of the hardware ceiling (previously oversubscription was silent). The
 * recorded ThreadElasticity is advisory metadata: it documents whether a pool
 * could correctly run smaller than requested, without affecting the grant.
 *
 * Thread-safe; all operations are short mutex-protected critical sections taken
 * at pool construction/teardown, never on a hot path.
 */
class GThreadBudget {
public:
    /***************************************************************************/
    /**
     * An RAII handle for one reservation: holds the reserved thread count for
     * its lifetime and returns it to the budget on destruction (or on an early
     * release()). Move-only; a default-constructed handle is empty. A
     * reservation must outlive the threads it accounts for -- hold it as a
     * member released after the workers are joined.
     */
    class Reservation {
        friend class GThreadBudget;

    public:
        /** @brief Default constructor: an empty (no-op) reservation */
        Reservation() = default;

        /** @brief Move constructor: takes over the other handle's reservation
         *  @param other The handle to take the reservation from (left empty) */
        Reservation(Reservation &&other) noexcept
          : budget_(std::exchange(other.budget_, nullptr))
          , key_(std::exchange(other.key_, 0))
          , granted_(std::exchange(other.granted_, 0)) { /* nothing */ }

        /** @brief Move assignment: releases any held reservation, then takes over the other's
         *  @param other The handle to take the reservation from (left empty)
         *  @return A reference to this handle */
        Reservation &operator=(Reservation &&other) noexcept {
            if(this != &other) {
                release();
                budget_ = std::exchange(other.budget_, nullptr);
                key_ = std::exchange(other.key_, 0);
                granted_ = std::exchange(other.granted_, 0);
            }
            return *this;
        }

        /** @brief The destructor returns the reservation to the budget */
        ~Reservation() { release(); }

        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;

        /** @brief The number of threads the pool should actually start
         *  @return The granted thread count (0 only for an empty handle) */
        [[nodiscard]] unsigned int granted() const noexcept { return granted_; }

        /** @brief Returns the reservation to the budget early; idempotent */
        void release() noexcept {
            if(budget_ != nullptr) {
                budget_->release_(key_);
                budget_ = nullptr;
                granted_ = 0;
            }
        }

    private:
        /** @brief Constructs a live handle (used by GThreadBudget::reserve())
         *  @param budget The owning budget
         *  @param key The registry key of this reservation
         *  @param granted The granted thread count */
        Reservation(GThreadBudget *budget, std::size_t key, unsigned int granted)
          : budget_(budget)
          , key_(key)
          , granted_(granted) { /* nothing */ }

        GThreadBudget *budget_ = nullptr; ///< The owning budget (nullptr == empty handle)
        std::size_t key_ = 0;             ///< This reservation's registry key
        unsigned int granted_ = 0;        ///< The granted thread count
    };

    /***************************************************************************/
    /**
     * @brief Reserves threads for a named source
     *
     * Advisory: never blocks, never throws, never refuses. The grant always
     * equals max(1, desired) regardless of elasticity -- a pool always gets
     * at least one worker. Crossing the warning threshold (ceiling x factor)
     * emits one rate-limited warning naming the largest reservations.
     *
     * @param source A short name identifying the reserving component (e.g. "oa:tp")
     * @param desired The number of threads the pool wants to start
     * @param elasticity Whether the pool could correctly run smaller (recorded for diagnostics)
     * @return An RAII handle holding the reservation; the pool starts granted() threads
     */
    [[nodiscard]] Reservation
    reserve(std::string_view source, unsigned int desired, ThreadElasticity elasticity) {
        const unsigned int granted = std::max(1u, desired);

        std::size_t key = 0;
        bool warn = false;
        unsigned int total = 0;
        std::vector<std::pair<std::string, unsigned int>> top;
        {
            std::scoped_lock const lk(mutex_);
            key = next_key_++;
            entries_.emplace(key, entry{std::string(source), granted, elasticity});
            reserved_ += granted;
            total = reserved_;

            const unsigned int threshold = warnThreshold_();
            if(reserved_ > threshold && not warned_ && n_warnings_ < MAX_WARNINGS) {
                warned_ = true;
                ++n_warnings_;
                warn = true;
                top = topReservations_(3);
            }
        }

        if(warn) {
            std::string top_txt;
            for(const auto &[name, count] : top) {
                top_txt += "  " + name + ": " + std::to_string(count) + '\n';
            }
            glogger << "In GThreadBudget::reserve(): the process-wide reserved thread count ("
                    << total << ") now exceeds " << OVERSUBSCRIPTION_FACTOR
                    << "x the hardware ceiling (" << ceiling() << ")." << '\n'
                    << "The largest reservations are:" << '\n'
                    << top_txt
                    << "Collectively these pools oversubscribe the cores and every pool slows down."
                    << '\n'
                    << GWARNING;
        }

        return Reservation{this, key, granted};
    }

    /***************************************************************************/
    /** @brief The current sum of reserved threads across all live reservations
     *  @return The reserved total */
    [[nodiscard]] unsigned int reserved() const noexcept {
        std::scoped_lock const lk(mutex_);
        return reserved_;
    }

    /** @brief The soft ceiling the budget compares against
     *  @return The number of hardware threads */
    [[nodiscard]] unsigned int ceiling() const noexcept { return getNHardwareThreads(); }

    /** @brief Whether the reserved total exceeds the hardware ceiling
     *  @return true if more threads are reserved than the hardware provides */
    [[nodiscard]] bool oversubscribed() const noexcept { return reserved() > ceiling(); }

    /***************************************************************************/
    /// The warning fires when the reserved total exceeds ceiling() times this factor. A factor
    /// above 1 is deliberate: a compute pool and an io pool legitimately overlap (io threads
    /// mostly block), so ~2x the core count is normal -- the warning should catch pathology,
    /// not the default configuration (~1.3x with the fixed organizational-pool default).
    static constexpr double OVERSUBSCRIPTION_FACTOR = 2.0;

private:
    /***************************************************************************/
    /** @brief One live reservation's bookkeeping record */
    struct entry {
        std::string source;          ///< The reserving component's name
        unsigned int count = 0;      ///< The granted thread count
        ThreadElasticity elasticity; ///< Whether the pool could run smaller (recorded for diagnostics)
    };

    /** @brief Returns a reservation to the budget (called by Reservation only)
     *  @param key The registry key handed out by reserve() */
    void release_(std::size_t key) noexcept {
        std::scoped_lock const lk(mutex_);
        if(const auto it = entries_.find(key); it != entries_.end()) {
            reserved_ -= it->second.count;
            entries_.erase(it);
        }
        // Re-arm the warning once the total has dropped back below the threshold.
        if(reserved_ <= warnThreshold_()) {
            warned_ = false;
        }
    }

    /** @brief The reserved total above which the warning fires (mutex_ must be held or irrelevant)
     *  @return ceiling() x OVERSUBSCRIPTION_FACTOR, as an integer count */
    [[nodiscard]] unsigned int warnThreshold_() const noexcept {
        return static_cast<unsigned int>(static_cast<double>(ceiling()) * OVERSUBSCRIPTION_FACTOR);
    }

    /** @brief The n largest live reservations, aggregated by source name (mutex_ must be held)
     *  @param n The maximum number of (source, count) pairs to return
     *  @return The aggregated reservations, largest first */
    [[nodiscard]] std::vector<std::pair<std::string, unsigned int>>
    topReservations_(std::size_t n) const {
        std::map<std::string, unsigned int> by_source;
        for(const auto &[key, e] : entries_) {
            by_source[e.source] += e.count;
        }
        std::vector<std::pair<std::string, unsigned int>> sorted(by_source.begin(), by_source.end());
        std::ranges::sort(sorted, [](const auto &a, const auto &b) { return a.second > b.second; });
        if(sorted.size() > n) {
            sorted.resize(n);
        }
        return sorted;
    }

    /***************************************************************************/
    /// The warning re-arms when the total drops back below the threshold, but fires at most
    /// this often per process -- a run whose pool churn oscillates around the threshold (e.g. a
    /// meta-optimization constructing sub-pools per generation) must not spam the log.
    static constexpr unsigned int MAX_WARNINGS = 3;

    mutable std::mutex mutex_;               ///< Guards all bookkeeping below
    std::map<std::size_t, entry> entries_;   ///< Live reservations by key
    std::size_t next_key_ = 1;               ///< The next registry key to hand out
    unsigned int reserved_ = 0;              ///< Current sum of granted thread counts
    bool warned_ = false;                    ///< Whether the warning has fired for the current excursion
    unsigned int n_warnings_ = 0;            ///< How often the warning has fired in this process
};

/******************************************************************************/
/**
 * @brief The process-global thread budget
 *
 * First constructed when the first pool reserves -- after main() has begun; no
 * before-main construction. Deliberately NEVER destroyed (reclaimed only by the
 * OS at process exit): reservations are held by singletons with static-storage
 * lifetime (the random factory's producers, a staged source's refill ring), and
 * their release during static destruction must find a live budget regardless of
 * destruction order -- the same reasoning that makes the logger never-destroy.
 *
 * @return A reference to the one process-wide budget
 */
inline GThreadBudget &threadBudget() {
    static GThreadBudget *budget = new GThreadBudget(); // NOLINT(cppcoreguidelines-owning-memory) -- deliberate never-destroy
    return *budget;
}

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
