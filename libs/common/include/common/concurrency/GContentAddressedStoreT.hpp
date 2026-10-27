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
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Boost headers go here

// Geneva headers go here

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * A thread-safe, content-addressed store of opaque blobs, keyed by a content id, with an optional
 * LRU capacity bound and optional per-peer "who already holds what" ack tracking. It is the generic
 * heart of the wire layout send-once machinery (a content-addressed dedup cache shared by a server and
 * its peers), but carries no transport or genome knowledge and is reusable for any content-addressed
 * intern-and-reference pattern.
 *
 * Two collaborating roles, one store:
 *
 *  - BLOB STORE: @c id -> blob. Both ends of a link cache what they have seen, so a later id-only
 *    reference resolves locally (a holder) or can be answered on a cache-miss fetch (the source).
 *  - PER-PEER ACK TRACKING: for each peer, the set of ids that peer is known to already hold, so a
 *    given blob is sent to a given peer exactly once and referenced by id thereafter. A peer's set is
 *    dropped when its session ends (@c forgetPeer) -- on reconnect it starts empty and the miss ->
 *    fetch fallback transparently re-sends what is needed.
 *
 * An optional capacity bound (LRU over the blob store) keeps a long run with many distinct ids from
 * growing without limit. Eviction never breaks correctness because a missing blob is re-fetched on
 * demand -- crucially, evicting a blob also drops that id from EVERY peer's ack set, so the source can
 * never keep referencing an id it can no longer answer (which would deadlock the holder).
 *
 * @tparam Id   The content id (the content-derived key). Needs equality and a hash (see @p Hash).
 * @tparam Blob The stored value type (default @c std::string -- an opaque serialized blob).
 * @tparam Peer The peer/session identifier type for ack tracking (default @c std::uint64_t).
 * @tparam Hash The hasher for @p Id (default @c std::hash<Id>; supply a custom one for array-like ids).
 */
template <typename Id, typename Blob = std::string, typename Peer = std::uint64_t, typename Hash = std::hash<Id>>
class GContentAddressedStoreT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    GContentAddressedStoreT() = default;

    GContentAddressedStoreT(const GContentAddressedStoreT &) = delete;
    GContentAddressedStoreT &operator=(const GContentAddressedStoreT &) = delete;

    /***************************************************************************/
    // Content-addressed blob store.

    /** @brief Whether a blob for the given id is cached.
     *  @param id The content id to look up. @return true iff the blob is present. */
    bool has(const Id &id) const {
        std::scoped_lock const lk(mtx_);
        return blobs_.contains(id);
    }

    /** @brief Copies out the cached blob for an id, if present (and marks it most-recently-used).
     *  @param id The content id to look up.
     *  @param out Receives the blob on a hit (left unchanged on a miss).
     *  @return true on a hit (out written), false on a miss. */
    bool tryGet(const Id &id, Blob &out) const {
        std::scoped_lock const lk(mtx_);
        auto it = blobs_.find(id);
        if(it == blobs_.end()) {
            return false;
        }
        touch_locked(it->second.lru_pos);
        out = it->second.blob;
        return true;
    }

    /** @brief Caches a blob under its id (idempotent; refreshes recency on a repeat). Enforces the
     *  capacity bound afterwards.
     *  @param id The content id. @param blob The blob to cache. */
    void put(const Id &id, Blob blob) {
        std::scoped_lock const lk(mtx_);
        auto it = blobs_.find(id);
        if(it != blobs_.end()) {
            it->second.blob = std::move(blob);
            touch_locked(it->second.lru_pos);
            return;
        }
        lru_.push_front(id);
        blobs_.emplace(id, Entry{.blob = std::move(blob), .lru_pos = lru_.begin()});
        evict_locked();
    }

    /***************************************************************************/
    // Per-peer ack tracking.

    /** @brief Whether a peer is known to already hold a given id.
     *  @param peer The peer (session) to query. @param id The content id.
     *  @return true iff the peer was previously recorded as holding this id. */
    bool peerHas(const Peer &peer, const Id &id) const {
        std::scoped_lock const lk(mtx_);
        auto it = peer_acked_.find(peer);
        return it != peer_acked_.end() && it->second.contains(id);
    }

    /** @brief Records that a peer now holds a given id (so it is referenced by id thereafter).
     *  @param peer The peer (session). @param id The content id the peer now holds. */
    void markPeerHas(const Peer &peer, const Id &id) {
        std::scoped_lock const lk(mtx_);
        peer_acked_[peer].insert(id);
    }

    /** @brief Drops all per-peer ack state for a peer (its session ended / it reconnected). The blob
     *  store is left intact (other peers may still need it, and it answers future fetches).
     *  @param peer The peer whose ack state is forgotten. */
    void forgetPeer(const Peer &peer) {
        std::scoped_lock const lk(mtx_);
        peer_acked_.erase(peer);
    }

    /***************************************************************************/
    // Capacity / introspection.

    /** @brief Sets the maximum number of blobs retained (LRU eviction beyond it); 0 == unbounded.
     *  @param max_items The capacity bound (0 disables eviction). */
    void setCapacity(std::size_t max_items) {
        std::scoped_lock const lk(mtx_);
        capacity_ = max_items;
        evict_locked();
    }

    /** @brief @return The number of blobs currently cached. */
    std::size_t size() const {
        std::scoped_lock const lk(mtx_);
        return blobs_.size();
    }

    /** @brief @return The number of peers with recorded ack state. */
    std::size_t trackedPeers() const {
        std::scoped_lock const lk(mtx_);
        return peer_acked_.size();
    }

    /** @brief Clears the entire store (blob store + all per-peer ack state). */
    void clear() {
        std::scoped_lock const lk(mtx_);
        blobs_.clear();
        lru_.clear();
        peer_acked_.clear();
    }

private:
    /***************************************************************************/
    /** @brief One cached blob plus its position in the LRU recency list. @c lru_pos is mutable because
     *  a read (@c tryGet, a const op) refreshes recency. */
    struct Entry {
        Blob blob;                                          ///< the cached blob
        mutable typename std::list<Id>::iterator lru_pos;   ///< this id's node in lru_ (front == most recent)
    };

    /** @brief Moves an id to the front of the recency list (most-recently-used). Caller holds mtx_.
     *  @param pos The id's current node in lru_ (updated in place to the new front node). */
    void touch_locked(typename std::list<Id>::iterator &pos) const {
        lru_.splice(lru_.begin(), lru_, pos);
        pos = lru_.begin();
    }

    /** @brief Evicts least-recently-used blobs until the capacity bound is met. Evicting a blob also
     *  drops that id from EVERY peer's ack set: otherwise the source would keep referencing the evicted
     *  blob by id to a peer (still believing the peer holds it) while no longer being able to answer
     *  that peer's cache-miss fetch -- a deadlock. Clearing the acks forces the next send of that blob
     *  to that peer to re-inline it in full (which re-populates the store). Caller holds mtx_. */
    void evict_locked() {
        if(capacity_ == 0) {
            return;
        }
        while(blobs_.size() > capacity_ && not lru_.empty()) {
            const Id victim = lru_.back();
            lru_.pop_back();
            blobs_.erase(victim);
            for(auto &kv : peer_acked_) {
                kv.second.erase(victim);
            }
        }
    }

    mutable std::mutex mtx_;
    std::unordered_map<Id, Entry, Hash> blobs_;                          ///< id -> (blob, lru node)
    mutable std::list<Id> lru_;                                          ///< recency list (front == MRU)
    std::unordered_map<Peer, std::unordered_set<Id, Hash>> peer_acked_;  ///< per-peer set of ids the peer holds
    std::size_t capacity_ = 0;                                           ///< max blobs retained (0 == unbounded)
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
