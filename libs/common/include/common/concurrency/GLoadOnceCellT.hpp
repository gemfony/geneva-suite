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

// Standard header files go here
#include <atomic>
#include <mutex>
#include <utility>

// Geneva headers go here
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

namespace Gem::Common::Concurrency {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A fill/compute-once-then-immutable cell: one payload written exactly once, thereafter read
 * lock-free by any number of concurrent readers.
 *
 * This is the single home for the "single writer, many lock-free readers" idiom that recurred three times
 * across the tree (the load-once problem store, the lazily-cached genome-layout content id, and the
 * build-once shared genome in the flat-individual factory) -- each of which hand-rolled its own
 * @c std::once_flag / @c std::atomic / @c mutex combination. Concentrating the mechanism here keeps those
 * sites Inv-2 compliant (concurrency primitives come only from @c common/concurrency/) and gives every
 * "immutable-after-load constant" one properly-homed facility.
 *
 * Two access flavours share the same one-time fill (guarded by a single mutex, so concurrent first calls
 * are serialised and only one producer wins):
 *
 *  - @b externally-filled -- call @c ensureLoaded(fill) to populate the cell from a data source (a file, a
 *    parsed configuration), then read it on a hot path with @c get() (a plain acquire-load of the ready flag
 *    plus a reference to the immutable payload -- no locking). @c get() throws if read before load, so a
 *    read-before-fill programming error is caught rather than returning a default-constructed value.
 *  - @b lazily-self-computed -- call @c getOrCompute(compute) to derive the payload from the surrounding
 *    object on first read and return the cached value on every subsequent read. This variant cannot be read
 *    before it is ready (the compute runs inline on first access), so it needs no throwing path.
 *
 * The ready flag is stored with @c release inside the one-time fill and acquire-loaded by @c loaded() /
 * @c get(), so a reader that observes @c loaded() @c == @c true is guaranteed to see the fully-written
 * payload -- and observing it lets a reader skip the mutex entirely. The cell is non-copyable and
 * non-movable (it owns a mutex); an owner that is itself copyable simply leaves the cell cold in its copy
 * (the copy recomputes / reloads on first use).
 *
 * @par Re-keying: reset()
 * A cell whose owner can be *re-keyed* -- assigned a new configuration path, loaded with a different
 * structure -- must be able to drop a payload that belonged to the previous key, or it would keep
 * answering for the object the owner no longer is. @c reset() does that, and is deliberately a
 * **single-threaded lifecycle operation** (assignment, deserialize-into, re-keying), never a concurrent
 * one: it invalidates references handed out by earlier @c get() / @c getOrCompute() calls, in the same
 * way @c GContentAddressedStoreT::clear() does. The fill path itself stays load-once between resets.
 *
 * @tparam T The (default-constructible) payload type held by the cell
 */
template <typename T>
class GLoadOnceCellT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /***************************************************************************/
    /** @brief The default constructor: an empty, not-yet-loaded cell. */
    GLoadOnceCellT() = default;
    /** @brief The destructor. */
    ~GLoadOnceCellT() = default;

    /** @brief The cell owns a mutex and is therefore non-copyable. */
    GLoadOnceCellT(const GLoadOnceCellT &) = delete;
    /** @brief The cell owns a mutex and is therefore non-copy-assignable. */
    GLoadOnceCellT &operator=(const GLoadOnceCellT &) = delete;

    /***************************************************************************/
    /**
     * @brief Returns the payload, computing it from @p compute on the first call and returning the cached
     * value thereafter (the lazily-self-computed flavour).
     *
     * The producer is invoked at most once per fill cycle (i.e. until the next @c reset()): concurrent
     * first calls are serialised by the fill mutex and only one wins; every caller (first or later) sees
     * the same fully-written payload before the reference is returned. A producer that **throws** does
     * not consume the cycle -- the cell stays unloaded and the next call retries, which is what a caller
     * filling from a failure-prone source (a file, a parse) relies on.
     *
     * @tparam ComputeFn A callable returning a @c T (the derived payload)
     * @param compute The producer invoked (once) to compute the payload
     * @return A const reference to the cached payload
     */
    template <typename ComputeFn>
    const T &getOrCompute(ComputeFn &&compute) {
        // Fast path: an acquire-load that sees the ready flag also sees the fully-written payload,
        // so a loaded cell is read without touching the mutex at all.
        if(this->loaded()) {
            return data_;
        }
        std::scoped_lock const fill_lock(fill_mutex_);
        if(not loaded_.load(std::memory_order_relaxed)) { // re-check under the lock
            data_ = std::forward<ComputeFn>(compute)();
            loaded_.store(true, std::memory_order_release);
        }
        return data_;
    }

    /***************************************************************************/
    /**
     * @brief Drops the payload, so the next @c getOrCompute() / @c ensureLoaded() fills the cell afresh.
     *
     * This is the re-keying operation described in the class documentation: a
     * **single-threaded lifecycle** call made when the owning object's identity changed (assignment,
     * deserialize-into, a new configuration path), never a concurrent one. References previously
     * returned by @c get() / @c getOrCompute() are invalidated.
     */
    void reset() {
        std::scoped_lock const fill_lock(fill_mutex_);
        loaded_.store(false, std::memory_order_release);
        data_ = T{};
    }

    /***************************************************************************/
    /**
     * @brief Fills the cell exactly once, on the first call, by invoking @p fill; later calls are no-ops
     * (the externally-filled flavour, read afterwards via @c get()).
     *
     * @tparam FillFn A callable returning a @c T (the constructed payload)
     * @param fill The producer invoked (once) to build the payload, e.g. a lambda that reads a data file
     */
    template <typename FillFn>
    void ensureLoaded(FillFn &&fill) {
        (void)this->getOrCompute(std::forward<FillFn>(fill));
    }

    /***************************************************************************/
    /** @brief @return true once the cell has been filled (acquire-load of the ready flag) */
    [[nodiscard]] bool loaded() const noexcept { return loaded_.load(std::memory_order_acquire); }

    /***************************************************************************/
    /**
     * @brief Reads the immutable payload (lock-free once loaded).
     *
     * @return A const reference to the stored payload
     * @throws geneva_exception if the cell has not been loaded yet (a programming error: fill the cell via
     *         @c ensureLoaded() before any reader calls @c get())
     */
    [[nodiscard]] const T &get() const {
        if(not this->loaded()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GLoadOnceCellT::get(): Error!" << '\n'
                << "The cell was read before it was loaded. Fill it once (via ensureLoaded())" << '\n'
                << "before any reader calls get()." << '\n'
            );
        }
        return data_;
    }

private:
    /***************************************************************************/
    // Data

    T data_{};                         ///< The immutable-between-resets payload
    mutable std::mutex fill_mutex_;    ///< Serialises the fill (and reset) -- never taken on a loaded read
    std::atomic<bool> loaded_{false};  ///< Ready flag; readers acquire-load it before touching data_
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
