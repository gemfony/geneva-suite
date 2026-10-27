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

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "courtier/GWireSerializationContext.hpp"

using namespace Gem::Courtier;

namespace {
GWireBlobId id(std::uint64_t hi, std::uint64_t lo) { return GWireBlobId{hi, lo}; }
} // namespace

/******************************************************************************/
TEST_CASE("GWireBlobRegistry: blob store put/has/get", "[wire][blob]") {
    GWireBlobRegistry reg;
    const auto a = id(1, 2);
    const auto b = id(3, 4);

    CHECK_FALSE(reg.has(a));
    std::string out;
    CHECK_FALSE(reg.tryGet(a, out));

    reg.put(a, "blob-A");
    CHECK(reg.has(a));
    CHECK(reg.tryGet(a, out));
    CHECK(out == "blob-A");
    CHECK(reg.size() == 1);

    // A second id is distinct; an unrelated id stays a miss.
    reg.put(b, "blob-B");
    CHECK(reg.size() == 2);
    CHECK(reg.tryGet(b, out));
    CHECK(out == "blob-B");
    CHECK_FALSE(reg.has(id(9, 9)));

    // put() of an existing id refreshes the blob, not the count.
    reg.put(a, "blob-A2");
    CHECK(reg.size() == 2);
    CHECK(reg.tryGet(a, out));
    CHECK(out == "blob-A2");
}

/******************************************************************************/
TEST_CASE("GWireBlobRegistry: per-peer ack tracking", "[wire][blob]") {
    GWireBlobRegistry reg;
    const auto x = id(10, 0);
    const auto y = id(20, 0);
    const GWirePeerId p1 = 100;
    const GWirePeerId p2 = 200;

    CHECK_FALSE(reg.peerHasBlob(p1, x));

    reg.markPeerHasBlob(p1, x);
    CHECK(reg.peerHasBlob(p1, x));
    CHECK_FALSE(reg.peerHasBlob(p1, y)); // a different blob for the same peer
    CHECK_FALSE(reg.peerHasBlob(p2, x)); // the same blob for a different peer
    CHECK(reg.trackedPeers() == 1);

    reg.markPeerHasBlob(p2, x);
    CHECK(reg.peerHasBlob(p2, x));
    CHECK(reg.trackedPeers() == 2);

    // Forgetting a peer (session end / reconnect) clears only that peer's acks; others are untouched.
    reg.forgetPeer(p1);
    CHECK_FALSE(reg.peerHasBlob(p1, x));
    CHECK(reg.peerHasBlob(p2, x));
    CHECK(reg.trackedPeers() == 1);
}

/******************************************************************************/
TEST_CASE("GWireBlobRegistry: LRU eviction respects the capacity bound", "[wire][blob]") {
    GWireBlobRegistry reg;
    reg.setCapacity(2);

    reg.put(id(1, 0), "one");
    reg.put(id(2, 0), "two");
    CHECK(reg.size() == 2);

    // Touch id(1) so it becomes most-recently-used; inserting a third must then evict id(2).
    std::string out;
    CHECK(reg.tryGet(id(1, 0), out));
    reg.put(id(3, 0), "three");
    CHECK(reg.size() == 2);
    CHECK(reg.has(id(1, 0)));        // recently used -> retained
    CHECK_FALSE(reg.has(id(2, 0)));  // least recently used -> evicted
    CHECK(reg.has(id(3, 0)));        // just inserted -> retained

    // Eviction does not break correctness: a re-put restores the blob.
    reg.put(id(2, 0), "two-again");
    CHECK(reg.has(id(2, 0)));
    CHECK(reg.size() == 2); // and the bound still holds (id(1) or id(3) evicted)
}

/******************************************************************************/
TEST_CASE("GWireBlobRegistry: evicting a blob also clears its per-peer acks", "[wire][blob]") {
    // Correctness coupling: a server must never keep referencing an evicted blob by id to a peer it
    // can no longer answer a fetch for. Evicting a blob therefore clears that id from every peer's ack
    // set, so the next send re-inlines the blob in full.
    GWireBlobRegistry reg;
    reg.setCapacity(2);
    const GWirePeerId p = 7;

    reg.put(id(1, 0), "L1");
    reg.markPeerHasBlob(p, id(1, 0));
    reg.put(id(2, 0), "L2");
    reg.markPeerHasBlob(p, id(2, 0));
    REQUIRE(reg.peerHasBlob(p, id(1, 0)));
    REQUIRE(reg.peerHasBlob(p, id(2, 0)));

    // Inserting a third blob evicts the least-recently-used (id 1) -- and with it, peer p's ack for id 1.
    reg.put(id(3, 0), "L3");
    CHECK_FALSE(reg.has(id(1, 0)));
    CHECK_FALSE(reg.peerHasBlob(p, id(1, 0))); // ack cleared -> the server will re-inline id 1
    CHECK(reg.peerHasBlob(p, id(2, 0)));       // id 2 still present -> ack retained
}

/******************************************************************************/
TEST_CASE("GWireSerializationScope: thread-local install / restore / nesting", "[wire][blob]") {
    CHECK(GWireSerializationScope::current() == nullptr);

    GWireSerializationContext outer;
    outer.enabled = true;
    outer.peer = 7;
    {
        GWireSerializationScope const s_outer(&outer);
        CHECK(GWireSerializationScope::current() == &outer);

        GWireSerializationContext inner;
        inner.peer = 42;
        {
            GWireSerializationScope const s_inner(&inner);
            CHECK(GWireSerializationScope::current() == &inner);

            // A nested "no context" scope (e.g. a self-contained fetch round trip mid-decode).
            {
                GWireSerializationScope const s_none(nullptr);
                CHECK(GWireSerializationScope::current() == nullptr);
            }
            CHECK(GWireSerializationScope::current() == &inner); // restored
        }
        CHECK(GWireSerializationScope::current() == &outer); // restored
    }
    CHECK(GWireSerializationScope::current() == nullptr); // restored to "none"
}
