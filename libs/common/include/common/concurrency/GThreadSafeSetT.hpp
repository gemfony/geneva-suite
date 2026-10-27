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
#include <mutex>
#include <set>
#include <vector>

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * @brief A small mutex-guarded ordered set of keys with an atomic @ref drain.
 *
 * It packages the recurring "a @c std::set guarded by its own @c std::mutex" idiom -- a membership set
 * mutated from one or more threads, occasionally emptied wholesale. The motivating use is a network
 * session's borrow set: it records the correlation ids the session currently has in flight (@ref insert
 * on hand-out, @ref erase on normal return) and, if the session dies still holding some, @ref drain
 * hands the leftovers back for the owner to reclaim. drain() swaps the contents out under the lock and
 * returns them, so the caller can act on each element @e without holding the lock (e.g. invoke a
 * requeue callback that may re-enter other machinery).
 *
 * @tparam Key The element type (must be usable as a std::set key)
 */
template <typename Key>
class GThreadSafeSetT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    GThreadSafeSetT() = default;
    GThreadSafeSetT(const GThreadSafeSetT &) = delete;
    GThreadSafeSetT &operator=(const GThreadSafeSetT &) = delete;

    /** @brief Inserts @p key. @param key The element to add. @return true iff it was newly inserted. */
    bool insert(const Key &key) {
        std::scoped_lock const lk(mtx_);
        return set_.insert(key).second;
    }

    /** @brief Removes @p key if present. @param key The element to drop. @return true iff it was present. */
    bool erase(const Key &key) {
        std::scoped_lock const lk(mtx_);
        return set_.erase(key) > 0;
    }

    /** @brief @param key The element to test. @return true iff @p key is currently a member. */
    [[nodiscard]] bool contains(const Key &key) const {
        std::scoped_lock const lk(mtx_);
        return set_.find(key) != set_.end();
    }

    /** @brief @return The current number of elements. */
    [[nodiscard]] std::size_t size() const {
        std::scoped_lock const lk(mtx_);
        return set_.size();
    }

    /** @brief @return true iff the set is currently empty. */
    [[nodiscard]] bool empty() const {
        std::scoped_lock const lk(mtx_);
        return set_.empty();
    }

    /** @brief Atomically removes and returns every element, leaving the set empty.
     *  @return The drained elements (ascending order). */
    std::vector<Key> drain() {
        std::set<Key> taken;
        {
            std::scoped_lock const lk(mtx_);
            taken.swap(set_);
        }
        return {taken.begin(), taken.end()};
    }

    /** @brief Empties the set. */
    void clear() {
        std::scoped_lock const lk(mtx_);
        set_.clear();
    }

private:
    mutable std::mutex mtx_;
    std::set<Key> set_;
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
