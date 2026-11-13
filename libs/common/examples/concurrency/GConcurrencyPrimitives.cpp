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

/**
 * @file
 * A small, runnable tour of the Gem::Common::Concurrency facilities -- living documentation of the
 * intended call patterns, and an extra compile+exercise path. Each shared primitive gets a short section;
 * the program prints what it does and returns non-zero if any demonstrated invariant does not hold.
 */

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <print>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include "common/concurrency/GAgingStoreT.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "common/concurrency/GContentAddressedStoreT.hpp"
#include "common/concurrency/GSPSCStagingRingT.hpp"
#include "common/concurrency/GThreadSafeKeyedStoreT.hpp"
#include "common/concurrency/GThreadSafeSetT.hpp"

using namespace Gem::Common::Concurrency;

namespace {

/** @brief Demonstrates GContentAddressedStoreT: a content-addressed blob cache with LRU eviction and
 *  per-peer "who already holds what" tracking (the heart of the wire layout send-once machinery).
 *  @return true iff every demonstrated invariant held. */
bool demo_content_addressed_store() {
    std::cout << "== GContentAddressedStoreT ==\n";
    bool ok = true;

    // id -> opaque blob. Here ids are ints and blobs are strings.
    GContentAddressedStoreT<int, std::string, int> store;

    // Intern two blobs (recency, most- to least-recently-used: 2, 1). A peer is told it holds blob 1;
    // we resolve blob 2 by id. (We deliberately do NOT read blob 1 -- a read would refresh its recency.)
    store.put(1, "layout-A");
    store.put(2, "layout-B");
    store.markPeerHas(/*peer*/ 42, /*id*/ 1);
    std::string blob;
    ok = ok && store.tryGet(2, blob) && blob == "layout-B";
    ok = ok && store.peerHas(42, 1);
    std::cout << "  interned 2 blobs; peer 42 holds blob 1; tryGet(2) -> \"" << blob << "\"\n";

    // A capacity bound evicts the least-recently-used blob (id 1) -- and crucially clears that id from
    // every peer's ack set, so the source can never reference a blob it can no longer answer.
    store.setCapacity(1);
    ok = ok && !store.has(1) && !store.peerHas(42, 1) && store.has(2);
    std::cout << "  after setCapacity(1): has(1)=" << store.has(1)
              << ", peer 42 still holds blob 1? " << std::boolalpha << store.peerHas(42, 1)
              << " (LRU blob evicted, ack cleared)\n";

    std::cout << "  -> " << (ok ? "OK" : "FAILED") << "\n";
    return ok;
}

/** @brief Demonstrates GThreadSafeKeyedStoreT: a mutex-guarded ordered key->value store (the shared
 *  building block behind the global option stores). @return true iff every demonstrated invariant held. */
bool demo_thread_safe_keyed_store() {
    std::cout << "== GThreadSafeKeyedStoreT ==\n";
    bool ok = true;

    GThreadSafeKeyedStoreT<std::string, int> store;
    store.set("threads", 8);
    store.set("retries", 3);
    ok = ok && store.setOnce("threads", 99) == false; // setOnce refuses to overwrite
    ok = ok && store.get("threads").value_or(-1) == 8;

    int retries = -1;
    ok = ok && store.get("retries", retries) && retries == 3;

    // Snapshots come back in key order.
    const auto keys = store.keys();
    ok = ok && keys.size() == 2 && keys[0] == "retries" && keys[1] == "threads";
    std::println("  keys (in order): {}",
                 keys | std::views::join_with(' ') | std::ranges::to<std::string>());
    std::println("  threads={}, retries={}", store.get("threads").value_or(-1), retries);

    std::cout << "  -> " << (ok ? "OK" : "FAILED") << "\n";
    return ok;
}

/** @brief Demonstrates GCompletionLatchT: a per-batch "wait for N completions" latch. N workers each count
 *  down once; one waiter blocks until the batch is fully drained (the GStdThreadConsumerT round pattern).
 *  @return true iff every demonstrated invariant held. */
bool demo_completion_latch() {
    std::cout << "== GCompletionLatchT ==\n";
    bool ok = true;

    constexpr std::size_t kWorkers = 8;
    // Held via shared_ptr by every worker AND this waiter, so it outlives the last count_down().
    auto latch = std::make_shared<GCompletionLatchT>(kWorkers);
    std::atomic<std::size_t> done{0};

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for(std::size_t i = 0; i < kWorkers; ++i) {
        workers.emplace_back([latch, &done] {
            done.fetch_add(1, std::memory_order_relaxed); // stand-in for "process one work item"
            latch->count_down();
        });
    }

    latch->wait(); // unblocks only once all kWorkers have counted down
    ok = ok && done.load() == kWorkers && latch->remaining() == 0;
    std::cout << "  waited on " << kWorkers << " workers; completed=" << done.load()
              << ", remaining=" << latch->remaining() << "\n";

    for(auto &t : workers) {
        t.join();
    }

    std::cout << "  -> " << (ok ? "OK" : "FAILED") << "\n";
    return ok;
}

/** @brief Demonstrates GSPSCStagingRingT: a lock-free ring of bulk-filled pools refilled by one
 *  background producer ahead of a claim cursor (the Hap RNG prefetch substrate). Here a trivial fill
 *  paints a sentinel; readers claim chunks and must only ever see filled words.
 *  @return true iff every demonstrated invariant held. */
bool demo_spsc_staging_ring() {
    std::cout << "== GSPSCStagingRingT ==\n";
    bool ok = true;

    constexpr std::size_t kChunkWords = 8;
    constexpr std::uint64_t kSentinel = 0xC0FFEEULL;

    // The fill callback bulk-populates one pool's worth of words; it runs only on the producer thread.
    GSPSCStagingRingT<kChunkWords> ring([](std::uint64_t *dst, std::size_t n) {
        for(std::size_t i = 0; i < n; ++i) {
            dst[i] = kSentinel;
        }
    });

    // In-place read of a claimed chunk.
    const std::uint64_t *span = ring.claimSpan();
    for(std::size_t i = 0; i < kChunkWords; ++i) {
        ok = ok && span[i] == kSentinel;
    }

    // Copy-out read of the next chunk into a private buffer.
    std::uint64_t buf[kChunkWords] = {0};
    ring.copyChunkInto(buf);
    for(unsigned long long const w : buf) {
        ok = ok && w == kSentinel;
    }

    std::cout << "  claimed 2 chunks of " << kChunkWords << " words; all words == sentinel? "
              << std::boolalpha << ok << "\n";
    std::cout << "  -> " << (ok ? "OK" : "FAILED") << "\n";
    return ok;
}

/** @brief Demonstrates GAgingStoreT: an epoch-stamped store with a keyed face and a FIFO face, both
 *  aged by TTL + capacity (the courtier late-return facility: retain un-returned originals by id, park
 *  late arrivals for the algorithm to reap). @return true iff every demonstrated invariant held. */
bool demo_aging_store() {
    std::cout << "== GAgingStoreT ==\n";
    bool ok = true;

    GAgingStoreT<int, std::string> store;
    store.configure(/*cap*/ 4, /*ttl_rounds*/ 2);

    // Keyed face: retain a value by id, then take it back (removing it).
    store.retain(101, "original-for-101");
    auto taken = store.takeRetained(101);
    ok = ok && taken.has_value() && *taken == "original-for-101" && store.retainedSize() == 0;

    // FIFO face: park two late arrivals, then drain them in order.
    store.park("late-A");
    store.park("late-B");
    ok = ok && store.parkedSize() == 2;
    const auto drained = store.drainParked();
    ok = ok && drained.size() == 2 && drained[0] == "late-A" && drained[1] == "late-B";

    // Aging: a parked value falls out once it is older than ttl_rounds epochs.
    store.park("ages-out");
    const std::uint64_t e1 = store.advanceEpochAndEvict(); // epoch 1: still young
    const std::uint64_t e2 = store.advanceEpochAndEvict(); // epoch 2: evicted (age == ttl)
    ok = ok && e1 == 0 && e2 == 1 && store.parkedSize() == 0;

    std::cout << "  retained+took by key; parked+drained 2 in order; one aged out after ttl=2\n";
    std::cout << "  -> " << (ok ? "OK" : "FAILED") << "\n";
    return ok;
}

/** @brief Demonstrates GThreadSafeSetT: a mutex-guarded membership set with an atomic drain (a network
 *  session's in-flight borrow set -- insert on hand-out, erase on return, drain the leftovers if the
 *  session dies). @return true iff every demonstrated invariant held. */
bool demo_thread_safe_set() {
    std::cout << "== GThreadSafeSetT ==\n";
    bool ok = true;

    GThreadSafeSetT<std::uint64_t> in_flight;
    in_flight.insert(1001);
    in_flight.insert(1002);
    in_flight.insert(1003);
    in_flight.erase(1002);        // 1002 was returned normally
    ok = ok && in_flight.size() == 2 && in_flight.contains(1001) && not in_flight.contains(1002);

    // The session dies: drain reclaims everything still outstanding, atomically emptying the set.
    const auto leftover = in_flight.drain();
    ok = ok && leftover.size() == 2 && leftover[0] == 1001 && leftover[1] == 1003 && in_flight.empty();

    std::cout << "  3 handed out, 1 returned, 2 reclaimed on drain\n";
    std::cout << "  -> " << (ok ? "OK" : "FAILED") << "\n";
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = demo_content_addressed_store() && ok;
    ok = demo_thread_safe_keyed_store() && ok;
    ok = demo_completion_latch() && ok;
    ok = demo_spsc_staging_ring() && ok;
    ok = demo_aging_store() && ok;
    ok = demo_thread_safe_set() && ok;

    std::cout << (ok ? "\nAll concurrency-primitive demos passed.\n"
                     : "\nA concurrency-primitive demo FAILED.\n");
    return ok ? 0 : 1;
}
