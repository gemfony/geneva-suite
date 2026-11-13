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
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * @brief A thread-safe store of epoch-stamped entries with TTL + capacity eviction, presenting two
 * faces over one shared epoch clock and bound (cap, ttl_rounds), under a single internal lock.
 *
 * @li a @b keyed face -- @ref retain / @ref takeRetained / @ref dropRetained -- a map of values kept by
 *     key, each stamped with the epoch at which it was retained;
 * @li a @b FIFO face -- @ref park / @ref drainParked -- a queue of values in arrival order, each stamped
 *     with the epoch at which it was parked.
 *
 * Both faces age against one monotonic @e epoch counter advanced by @ref advanceEpochAndEvict (call it
 * once per logical round): an entry older than @c ttl_rounds epochs is evicted, and each face is bounded
 * to @c cap entries (the oldest by epoch evicted first -- FIFO front for the queue, min-epoch for the
 * map). @c cap @c == @c 0 disables retention (the parked face holds nothing).
 *
 * This was lifted from the courtier networked-consumer late-return facility, where a result that arrives
 * after its batch retired is @e parked for the algorithm to reap, and a clone of each un-returned
 * original is @e retained (keyed by correlation id) so a late payload-only return can still be applied.
 * That domain policy (graft-or-drop, drop accounting) stays in the consumer; this primitive owns only
 * the two aging containers and their eviction. All operations are individually atomic; callers that need
 * a compound action atomic with @e their own state (e.g. retiring a batch while retaining its originals)
 * simply invoke these methods while holding that outer lock -- the consistent lock order (outer then this
 * store's lock, never the reverse) keeps that safe.
 *
 * @tparam Key   The key type of the keyed face
 * @tparam Value The stored value type (may be move-only, e.g. a unique_ptr)
 * @tparam Hash  Hash for the keyed face (defaults to std::hash<Key>)
 */
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class GAgingStoreT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    GAgingStoreT() = default;
    GAgingStoreT(const GAgingStoreT &) = delete;
    GAgingStoreT &operator=(const GAgingStoreT &) = delete;

    /** @brief Sets the capacity and TTL bound shared by both faces.
     *  @param cap Maximum entries held per face; 0 disables the parked (FIFO) face's retention.
     *  @param ttl_rounds An entry is evicted this many epoch advances after it was stored. */
    void configure(std::size_t cap, std::uint64_t ttl_rounds) {
        std::scoped_lock const lk(mtx_);
        cap_ = cap;
        ttl_rounds_ = ttl_rounds;
    }

    /** @brief @return true iff retention is enabled (cap > 0). */
    [[nodiscard]] bool buffering() const {
        std::scoped_lock const lk(mtx_);
        return cap_ > 0;
    }

    // ---- keyed face ----------------------------------------------------------

    /** @brief Retains @p v under @p k, stamped with the current epoch (overwrites any existing entry).
     *  @param k The key. @param v The value to retain (moved in). */
    void retain(const Key &k, Value v) {
        std::scoped_lock const lk(mtx_);
        retained_.insert_or_assign(k, std::make_pair(epoch_, std::move(v)));
    }

    /** @brief Atomically removes and returns the value retained under @p k, if any.
     *  @param k The key. @return The value (moved out) if present, else std::nullopt. */
    std::optional<Value> takeRetained(const Key &k) {
        std::scoped_lock const lk(mtx_);
        auto it = retained_.find(k);
        if(it == retained_.end()) {
            return std::nullopt;
        }
        std::optional<Value> out(std::move(it->second.second));
        retained_.erase(it);
        return out;
    }

    /** @brief Drops the entry retained under @p k, if present.
     *  @param k The key. */
    void dropRetained(const Key &k) {
        std::scoped_lock const lk(mtx_);
        retained_.erase(k);
    }

    /** @brief @return The number of entries currently retained in the keyed face. */
    [[nodiscard]] std::size_t retainedSize() const {
        std::scoped_lock const lk(mtx_);
        return retained_.size();
    }

    // ---- FIFO face -----------------------------------------------------------

    /** @brief Parks @p v at the back of the FIFO, stamped with the current epoch, then enforces the TTL
     *  and capacity bounds (oldest-first).
     *  @param v The value to park (moved in).
     *  @return The number of parked entries evicted by this call's bound enforcement. */
    std::uint64_t park(Value v) {
        std::scoped_lock const lk(mtx_);
        parked_.emplace_back(epoch_, std::move(v));
        return evictParked_locked();
    }

    /** @brief Drains the entire FIFO, transferring ownership of every parked value to the caller in
     *  arrival (FIFO) order; the face is left empty.
     *  @return The parked values in arrival order. */
    std::vector<Value> drainParked() {
        std::scoped_lock const lk(mtx_);
        std::vector<Value> out;
        out.reserve(parked_.size());
        for(auto &entry : parked_) {
            out.push_back(std::move(entry.second));
        }
        parked_.clear();
        return out;
    }

    /** @brief @return The number of values currently parked in the FIFO face. */
    [[nodiscard]] std::size_t parkedSize() const {
        std::scoped_lock const lk(mtx_);
        return parked_.size();
    }

    // ---- epoch / lifecycle ---------------------------------------------------

    /** @brief Advances the epoch by one round and evicts TTL/cap-expired entries from both faces.
     *  @return The number of @e parked (FIFO) entries evicted (keyed-face evictions are not counted --
     *          retiring a retained original is not a loss until a late return actually needs it). */
    std::uint64_t advanceEpochAndEvict() {
        std::scoped_lock const lk(mtx_);
        ++epoch_;
        const std::uint64_t evicted = evictParked_locked();
        evictRetained_locked();
        return evicted;
    }

    /** @brief @return The current epoch value (primarily for tests/diagnostics). */
    [[nodiscard]] std::uint64_t epoch() const {
        std::scoped_lock const lk(mtx_);
        return epoch_;
    }

    /** @brief Empties both faces (the epoch and the bound are left unchanged). */
    void clear() {
        std::scoped_lock lk(mtx_);
        parked_.clear();
        retained_.clear();
    }

private:
    /** @brief Evicts parked entries past the TTL horizon (front is oldest) then bounds the FIFO to cap
     *  (oldest first). @return The number of entries evicted. Caller holds mtx_. */
    std::uint64_t evictParked_locked() {
        std::uint64_t evicted = 0;
        while(not parked_.empty() && (epoch_ - parked_.front().first) >= ttl_rounds_) {
            parked_.pop_front();
            ++evicted;
        }
        while(parked_.size() > cap_) {
            parked_.pop_front();
            ++evicted;
        }
        return evicted;
    }

    /** @brief Evicts retained entries past the TTL horizon, then bounds the map to cap (lowest epoch
     *  first). Caller holds mtx_. */
    void evictRetained_locked() {
        for(auto it = retained_.begin(); it != retained_.end();) {
            if(epoch_ - it->second.first >= ttl_rounds_) {
                it = retained_.erase(it);
            }
            else {
                ++it;
            }
        }
        while(retained_.size() > cap_) {
            auto oldest = std::min_element(
                retained_.begin(), retained_.end(),
                [](const auto &a, const auto &b) { return a.second.first < b.second.first; });
            retained_.erase(oldest);
        }
    }

    mutable std::mutex mtx_;
    std::uint64_t epoch_ = 0;       ///< Monotonic round counter; both faces age against it
    std::size_t cap_ = 0;           ///< Per-face capacity bound (0 disables the parked face)
    std::uint64_t ttl_rounds_ = 8;  ///< An entry is evicted this many epochs after it was stored

    std::deque<std::pair<std::uint64_t, Value>>        parked_;   ///< (epoch, value) FIFO of parked values
    std::unordered_map<Key, std::pair<std::uint64_t, Value>, Hash> retained_; ///< key -> (epoch, value)
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
