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
#include <utility>

// Boost headers go here

// Geneva headers go here
#include "common/concurrency/GStoreCommon.hpp"

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * A thread-safe, content-addressed store of opaque blobs, keyed by a content id, with an optional
 * LRU capacity bound and optional per-peer "who already holds what" ack tracking. It is the generic
 * heart of a send-once machinery (a content-addressed dedup cache shared by a server and
 * its peers), but carries no transport knowledge and is reusable for any content-addressed
 * intern-and-reference pattern.
 *
 * Two collaborating roles, one store:
 *
 *  - BLOB STORE: @c id -> blob. Both ends of a link cache what they have seen, so a later id-only
 *    reference resolves locally. This face satisfies the KeyedStore concept (GStoreCommon.hpp).
 *  - PER-PEER ACK TRACKING: for each peer, the set of ids that peer is known to already hold, so a
 *    given blob is sent to a given peer exactly once and referenced by id thereafter. A peer's set is
 *    dropped when its session ends (@c forgetPeer) -- on reconnect it starts empty and is sent every
 *    blob in full again.
 *
 * An optional capacity bound (LRU over the blob store) keeps a long run with many distinct ids from
 * growing without limit. Eviction never breaks correctness: evicting a blob also drops that id from
 * EVERY peer's ack set, so the next send of that blob to any peer inlines it in full again. A sender
 * uses internFor(), which caches, acknowledges and decides in one step under one lock: done one
 * after the other (contains, set, peerHas, markPeerHas), an eviction by another thread in between can
 * leave an acknowledgement for a blob the store no longer holds.
 *
 * @tparam id_type   The content id (the content-derived key). Needs equality and a hash (see @p hash_type).
 * @tparam blob_type The stored value type (default @c std::string -- an opaque serialized blob).
 * @tparam peer_type The peer/session identifier type for ack tracking (default @c std::uint64_t).
 * @tparam hash_type The hasher for @p id_type (default @c std::hash<id_type>; supply a custom one for array-like ids).
 */
template <
    typename id_type,
    typename blob_type = std::string,
    typename peer_type = std::uint64_t,
    typename hash_type = std::hash<id_type>>
class GContentAddressedStoreT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    GContentAddressedStoreT() = default;

    GContentAddressedStoreT(const GContentAddressedStoreT &) = delete;
    GContentAddressedStoreT &operator=(const GContentAddressedStoreT &) = delete;

    /***************************************************************************/
    // Content-addressed blob store.

    /** @brief Whether a blob for the given id is cached.
     *  @param id The content id to look up. @return true iff the blob is present. */
    bool contains(const id_type &id) const {
        std::scoped_lock const lk(mtx_);
        return blobs_.contains(id);
    }

    /** @brief Copies out the cached blob for an id, if present (and marks it most-recently-used).
     *  @param id The content id to look up.
     *  @param out Receives the blob on a hit (left unchanged on a miss).
     *  @return true on a hit (out written), false on a miss. */
    bool get(const id_type &id, blob_type &out) const {
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
    void set(const id_type &id, blob_type blob) {
        std::scoped_lock const lk(mtx_);
        auto it = blobs_.find(id);
        if(it != blobs_.end()) {
            it->second.blob = std::move(blob);
            touch_locked(it->second.lru_pos);
            return;
        }
        insert_locked(id, std::move(blob));
    }

    /** @brief Interns a blob for a peer in one step, under one lock (see the class comment): caches
     *  it if absent (refreshing its recency otherwise), records that @p peer holds it, and tells
     *  whether it must be sent in full.
     *  @param peer The peer (session) it is sent to. @param id The content id.
     *  @param make_blob Called only if the blob is absent, under the store's lock (so it must not
     *  call into the store), to build it.
     *  @return true if the peer did not hold the blob yet (send it in full); false: refer to it by id */
    template <typename make_blob_type>
    bool internFor(const peer_type &peer, const id_type &id, make_blob_type &&make_blob) {
        std::scoped_lock const lk(mtx_);
        if(auto it = blobs_.find(id); it != blobs_.end()) {
            touch_locked(it->second.lru_pos);
        }
        else {
            insert_locked(id, std::forward<make_blob_type>(make_blob)());
        }
        return peer_acked_[peer].insert(id).second;
    }

    /***************************************************************************/
    // Per-peer ack tracking.

    /** @brief Whether a peer is known to already hold a given id.
     *  @param peer The peer (session) to query. @param id The content id.
     *  @return true iff the peer was previously recorded as holding this id. */
    bool peerHas(const peer_type &peer, const id_type &id) const {
        std::scoped_lock const lk(mtx_);
        auto it = peer_acked_.find(peer);
        return it != peer_acked_.end() && it->second.contains(id);
    }

    /** @brief Records that a peer now holds a given id (so it is referenced by id thereafter), as it
     *  is told, whether or not the store holds the blob; a sender uses internFor() instead.
     *  @param peer The peer (session). @param id The content id the peer now holds. */
    void markPeerHas(const peer_type &peer, const id_type &id) {
        std::scoped_lock const lk(mtx_);
        peer_acked_[peer].insert(id);
    }

    /** @brief Drops all per-peer ack state for a peer (its session ended / it reconnected). The blob
     *  store is left intact (other peers may still need it, and the next send to this peer takes it
     *  from there).
     *  @param peer The peer whose ack state is forgotten. */
    void forgetPeer(const peer_type &peer) {
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

    /** @brief @return true iff no blob is cached (per-peer ack state is not counted). */
    bool empty() const {
        std::scoped_lock const lk(mtx_);
        return blobs_.empty();
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
     *  a read (@c get, a const op) refreshes recency. */
    struct Entry {
        blob_type blob; ///< the cached blob
        mutable typename std::list<id_type>::iterator
            lru_pos; ///< this id's node in lru_ (front == most recent)
    };

    /** @brief Moves an id to the front of the recency list (most-recently-used). Caller holds mtx_.
     *  @param pos The id's current node in lru_ (updated in place to the new front node). */
    void touch_locked(typename std::list<id_type>::iterator &pos) const {
        lru_.splice(lru_.begin(), lru_, pos);
        pos = lru_.begin();
    }

    /** @brief Caches a blob that is absent as the most recently used one, then enforces the capacity
     *  bound (which never evicts it: it is the newest). Caller holds mtx_.
     *  @param id The content id. @param blob The blob. */
    void insert_locked(const id_type &id, blob_type blob) {
        lru_.push_front(id);
        blobs_.emplace(id, Entry{.blob = std::move(blob), .lru_pos = lru_.begin()});
        evict_locked();
    }

    /** @brief Evicts least-recently-used blobs until the capacity bound is met. Evicting a blob also
     *  drops that id from EVERY peer's ack set, so that the source never keeps referencing by id a
     *  blob it no longer holds itself: the next send of that blob to that peer re-inlines it in full
     *  (which re-populates the store). Caller holds mtx_. */
    void evict_locked() {
        if(capacity_ == 0) {
            return;
        }
        while(blobs_.size() > capacity_ && not lru_.empty()) {
            const id_type victim = lru_.back();
            lru_.pop_back();
            blobs_.erase(victim);
            for(auto &kv : peer_acked_) {
                kv.second.erase(victim);
            }
        }
    }

    mutable std::mutex mtx_;
    std::unordered_map<id_type, Entry, hash_type> blobs_; ///< id -> (blob, lru node)
    mutable std::list<id_type> lru_;                      ///< recency list (front == MRU)
    std::unordered_map<peer_type, std::unordered_set<id_type, hash_type>>
        peer_acked_;           ///< per-peer set of ids the peer holds
    std::size_t capacity_ = 0; ///< max blobs retained (0 == unbounded)
};

static_assert(KeyedStore<GContentAddressedStoreT<int>, int, std::string>);

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
