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
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// Boost headers go here

// Geneva headers go here

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * A thread-safe key -> value store: a mutex-guarded ordered map with the usual lookup / insert / erase /
 * snapshot operations, each performed under a single lock. It is the shared building block for the many
 * "occasional access, must be thread-safe" maps across Geneva (e.g. global option stores), replacing
 * hand-rolled @c std::map + @c std::mutex pairs at the call site.
 *
 * The backing container is an ORDERED @c std::map, so @c keys() / @c values() / @c snapshot() return in
 * key order -- deterministic and stable, which callers (and their tests) rely on. Like the maps it
 * replaces, it assumes occasional access and is not tuned for high-frequency querying (a single coarse
 * lock); reach for a different primitive if you need a hot path.
 *
 * @tparam Key   The key type (must be ordered -- usable as a @c std::map key).
 * @tparam Value The mapped value type.
 */
template <typename Key, typename Value>
class GThreadSafeKeyedStoreT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    using key_type = Key;
    using mapped_type = Value;

    GThreadSafeKeyedStoreT() = default;

    GThreadSafeKeyedStoreT(const GThreadSafeKeyedStoreT &) = delete;
    GThreadSafeKeyedStoreT &operator=(const GThreadSafeKeyedStoreT &) = delete;

    /***************************************************************************/

    /** @brief Retrieves a value into an out-parameter.
     *  @param key The key to look up. @param out Receives the value on a hit (untouched on a miss).
     *  @return true on a hit (out written), false on a miss. */
    bool get(const Key &key, Value &out) const {
        std::scoped_lock const guard(mutex_);
        if(auto it = kvp_.find(key); it != kvp_.end()) {
            out = it->second;
            return true;
        }
        return false;
    }

    /** @brief Retrieves a value as an optional.
     *  @param key The key to look up. @return The value, or std::nullopt on a miss. */
    std::optional<Value> get(const Key &key) const {
        std::scoped_lock const guard(mutex_);
        if(auto it = kvp_.find(key); it != kvp_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /** @brief Inserts a new entry or overwrites an existing one.
     *  @param key The key. @param value The value to store. */
    void set(const Key &key, Value value) {
        std::scoped_lock const guard(mutex_);
        kvp_[key] = std::move(value);
    }

    /** @brief Inserts an entry only if the key is not already present.
     *  @param key The key. @param value The value to store.
     *  @return true if inserted, false if the key already existed (left unchanged). */
    bool setOnce(const Key &key, Value value) {
        std::scoped_lock const guard(mutex_);
        if(kvp_.contains(key)) {
            return false;
        }
        kvp_[key] = std::move(value);
        return true;
    }

    /** @brief Removes an entry if present.
     *  @param key The key to remove. @return true if an entry was removed, false if it was absent. */
    bool remove(const Key &key) {
        std::scoped_lock const guard(mutex_);
        return kvp_.erase(key) != 0;
    }

    /** @brief @param key The key to check. @return true iff an entry for the key is present. */
    bool contains(const Key &key) const {
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
    std::vector<Key> keys() const {
        std::scoped_lock const guard(mutex_);
        std::vector<Key> result;
        result.reserve(kvp_.size());
        for(auto const &[k, _] : kvp_) {
            result.push_back(k);
        }
        return result;
    }

    /** @brief @return A snapshot of all values, in key order (atomic w.r.t. concurrent mutation). */
    std::vector<Value> values() const {
        std::scoped_lock const guard(mutex_);
        std::vector<Value> result;
        result.reserve(kvp_.size());
        for(auto const &[_, v] : kvp_) {
            result.push_back(v);
        }
        return result;
    }

    /** @brief Removes all entries. */
    void clear() {
        std::scoped_lock const guard(mutex_);
        kvp_.clear();
    }

private:
    std::map<Key, Value> kvp_{};
    mutable std::mutex mutex_; ///< guards every operation
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
