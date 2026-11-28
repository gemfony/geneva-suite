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
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/concurrency/GStoreCommon.hpp"

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * A thread-safe key -> value store: a mutex-guarded ordered map with the usual lookup / insert / erase /
 * snapshot operations, each performed under a single lock. It satisfies the KeyedStore concept
 * (GStoreCommon.hpp). It is the shared building block for the many
 * "occasional access, must be thread-safe" maps (e.g. global option stores), replacing
 * hand-rolled @c std::map + @c std::mutex pairs at the call site.
 *
 * The backing container is an ORDERED @c std::map, so @c keys() / @c values() / @c snapshot() return in
 * key order -- deterministic and stable, which callers (and their tests) rely on. Like the maps it
 * replaces, it assumes occasional access and is not tuned for high-frequency querying (a single coarse
 * lock); reach for a different primitive if you need a hot path.
 *
 * waitUntilEmpty() lets a thread wait, bounded, until the last entry has gone (a server waiting
 * for its sessions to sign off, say). remove() and clear() notify under the store's mutex, so the
 * waiter may return while the thread that removed the last entry still holds that mutex: the store
 * must outlive every thread that may still call remove() or clear() at that moment. A store that is
 * a member of the object whose threads remove from it, and that is destroyed after those threads
 * are joined, meets this; otherwise hold it by a shared pointer.
 *
 * @tparam ordered_key_type   The key type (must be ordered -- usable as a @c std::map key).
 * @tparam mapped_value_type The mapped value type.
 */
template <typename ordered_key_type, typename mapped_value_type>
class GThreadSafeKeyedStoreT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    using key_type = ordered_key_type;
    using mapped_type = mapped_value_type;

    GThreadSafeKeyedStoreT() = default;

    GThreadSafeKeyedStoreT(const GThreadSafeKeyedStoreT &) = delete;
    GThreadSafeKeyedStoreT &operator=(const GThreadSafeKeyedStoreT &) = delete;

    /***************************************************************************/

    /** @brief Retrieves a value into an out-parameter.
     *  @param key The key to look up. @param out Receives the value on a hit (untouched on a miss).
     *  @return true on a hit (out written), false on a miss. */
    bool get(const ordered_key_type &key, mapped_value_type &out) const {
        std::scoped_lock const guard(mutex_);
        if(auto it = kvp_.find(key); it != kvp_.end()) {
            out = it->second;
            return true;
        }
        return false;
    }

    /** @brief Retrieves a value as an optional.
     *  @param key The key to look up. @return The value, or std::nullopt on a miss. */
    std::optional<mapped_value_type> get(const ordered_key_type &key) const {
        std::scoped_lock const guard(mutex_);
        if(auto it = kvp_.find(key); it != kvp_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /** @brief Inserts a new entry or overwrites an existing one.
     *  @param key The key. @param value The value to store. */
    void set(const ordered_key_type &key, mapped_value_type value) {
        std::scoped_lock const guard(mutex_);
        kvp_.insert_or_assign(key, std::move(value));
    }

    /** @brief Inserts an entry only if the key is not already present.
     *  @param key The key. @param value The value to store.
     *  @return true if inserted, false if the key already existed (left unchanged). */
    bool setOnce(const ordered_key_type &key, mapped_value_type value) {
        std::scoped_lock const guard(mutex_);
        return kvp_.try_emplace(key, std::move(value)).second;
    }

    /** @brief Removes an entry if present.
     *  @param key The key to remove. @return true if an entry was removed, false if it was absent. */
    bool remove(const ordered_key_type &key) {
        std::scoped_lock const guard(mutex_);
        const bool removed = kvp_.erase(key) != 0;
        if(removed && kvp_.empty()) {
            emptied_.notify_all();
        }
        return removed;
    }

    /** @brief Removes an entry if present and hands its value over.
     *  @param key The key to remove. @return The value, or nothing if the key was absent. */
    std::optional<mapped_value_type> take(const ordered_key_type &key) {
        std::scoped_lock const guard(mutex_);
        auto it = kvp_.find(key);
        if(it == kvp_.end()) {
            return std::nullopt;
        }
        std::optional<mapped_value_type> value{std::move(it->second)};
        kvp_.erase(it);
        if(kvp_.empty()) {
            emptied_.notify_all();
        }
        return value;
    }

    /** @brief @param key The key to check. @return true iff an entry for the key is present. */
    bool contains(const ordered_key_type &key) const {
        std::scoped_lock const guard(mutex_);
        return kvp_.contains(key);
    }

    /** @brief @return The number of entries. */
    std::size_t size() const {
        std::scoped_lock const guard(mutex_);
        return kvp_.size();
    }

    /** @brief @return true iff the store holds no entries. */
    bool empty() const {
        std::scoped_lock const guard(mutex_);
        return kvp_.empty();
    }

    /** @brief @return A snapshot of all keys, in key order. */
    std::vector<ordered_key_type> keys() const {
        std::scoped_lock const guard(mutex_);
        std::vector<ordered_key_type> result;
        result.reserve(kvp_.size());
        for(auto const &[k, _] : kvp_) {
            result.push_back(k);
        }
        return result;
    }

    /** @brief @return A snapshot of all values, in key order (atomic w.r.t. concurrent mutation). */
    std::vector<mapped_value_type> values() const {
        std::scoped_lock const guard(mutex_);
        std::vector<mapped_value_type> result;
        result.reserve(kvp_.size());
        for(auto const &[_, v] : kvp_) {
            result.push_back(v);
        }
        return result;
    }

    /** @brief Removes all entries and hands them over, in key order, in one step (an entry added
     *  meanwhile is either among them or stays).
     *  @return The entries that were in the store */
    std::vector<std::pair<ordered_key_type, mapped_value_type>> drain() {
        std::scoped_lock const guard(mutex_);
        std::vector<std::pair<ordered_key_type, mapped_value_type>> result;
        result.reserve(kvp_.size());
        for(auto &[k, v] : kvp_) {
            result.emplace_back(k, std::move(v));
        }
        kvp_.clear();
        emptied_.notify_all();
        return result;
    }

    /** @brief Removes all entries. */
    void clear() {
        std::scoped_lock const guard(mutex_);
        kvp_.clear();
        emptied_.notify_all();
    }

    /** @brief Blocks until the store holds no entry, but not longer than @p limit. Returns at once
     *  if it is empty already. The store must outlive every thread that may still be inside
     *  remove() or clear() when this returns (see the class description).
     *  @param limit The longest time to wait.
     *  @return true if the store is empty, false if the limit passed first. */
    template <typename rep_type, typename period_type>
    bool waitUntilEmpty(const std::chrono::duration<rep_type, period_type> &limit) const {
        std::unique_lock guard(mutex_);
        return emptied_.wait_for(guard, limit, [this] { return kvp_.empty(); });
    }

private:
    std::map<ordered_key_type, mapped_value_type> kvp_{};
    mutable std::mutex mutex_; ///< guards every operation
    /// Notified under mutex_ when the last entry goes (remove(), clear())
    mutable std::condition_variable emptied_;
};

static_assert(KeyedStore<GThreadSafeKeyedStoreT<int, int>, int, int>);

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
