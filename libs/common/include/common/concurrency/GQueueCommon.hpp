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
#include <concepts>
#include <cstddef>
#include <optional>
#include <utility>

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * Selects the storage/synchronisation backend of GMPMCQueueT. The two backends are
 * interface-identical (both satisfy the MPMCQueue concept below); this enum just picks one at
 * compile time.
 *
 *  - Deque:        std::deque-backed (GBlockingMPMCQueueT). Supports an unbounded capacity
 *                  (Cap == 0). This is the default, so existing behaviour is preserved.
 *  - Preallocated: a fixed-size, preallocated ring buffer (GPreallocatedMPMCQueueT). No per-item
 *                  allocation and contiguous, cache-friendly storage; bounded only (Cap > 0).
 */
enum class QueueBackend {
    Deque,
    Preallocated
};

/******************************************************************************/
/**
 * The blocking-MPMC-queue API contract, enshrined ONCE as a concept so that every backend is
 * provably interface-compatible. Both GBlockingMPMCQueueT and GPreallocatedMPMCQueueT are
 * static_assert-checked against this in GMPMCQueueT, and a type-parametrised test suite exercises
 * the same behaviour against each.
 *
 * The contract is the established GBlockingMPMCQueueT surface: a perfect-forwarding push family
 * (covering copy and move, so the queue serves both std::shared_ptr and move-only std::unique_ptr),
 * optional-returning pops (no out-parameter, works for move-only T), and a terminal close() that
 * lets blocked producers return false and lets consumers drain then see std::nullopt. Shutdown is
 * signalled through return values, never through exceptions.
 *
 * @tparam Q The queue type being checked for conformance
 * @tparam T The element type the queue is expected to store
 */
template <typename Q, typename T>
concept MPMCQueue = requires(Q q, const Q cq, T item, std::chrono::milliseconds d) {
    typename Q::value_type;

    // Producer side: bool-returning, perfect-forwarding (instantiated here with an rvalue T).
    { q.try_push(std::move(item)) } -> std::same_as<bool>;
    { q.push(std::move(item)) } -> std::same_as<bool>;
    { q.push_wait(std::move(item), d) } -> std::same_as<bool>;

    // Consumer side: optional-returning (end-of-stream / timeout / empty == std::nullopt).
    { q.try_pop() } -> std::same_as<std::optional<T>>;
    { q.pop() } -> std::same_as<std::optional<T>>;
    { q.pop_wait(d) } -> std::same_as<std::optional<T>>;

    // Lifecycle and observers.
    { q.close() } -> std::same_as<void>;
    { cq.is_closed() } -> std::same_as<bool>;
    { cq.size() } -> std::same_as<std::size_t>;
    { cq.empty() } -> std::same_as<bool>;
    { cq.remaining_space() } -> std::same_as<std::size_t>;

    // Compile-time capacity description.
    { Q::capacity() } -> std::convertible_to<std::size_t>;
    { Q::bounded() } -> std::convertible_to<bool>;
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
