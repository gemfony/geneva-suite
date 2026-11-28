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
#include <concepts>
#include <cstddef>
#include <utility>

namespace Gem::Common::Concurrency {

/******************************************************************************/
/**
 * The keyed-store API contract, stated once as a concept (as MPMCQueue in GQueueCommon.hpp does for
 * the queues), so that the keyed stores of common/ spell the same operations with the same words:
 * GThreadSafeKeyedStoreT, GGlobalOptionsT (and with it GProviderStoreT) and the blob store of
 * GContentAddressedStoreT each static_assert it after their definition.
 *
 * The contract is what all of them can do: test for a key, look a value up into an out-parameter
 * (false on a miss, the out-parameter untouched), insert or overwrite, and count. A store adds what
 * its shape needs beside it (setOnce() and remove() on the maps, the throwing at() of the options,
 * the per-peer tracking of the content-addressed store), but it never spells one of these five
 * operations differently.
 *
 * @tparam S     The store type being checked for conformance
 * @tparam Key   The key type the store is expected to take
 * @tparam Value The value type the store is expected to hold
 */
template <typename S, typename Key, typename Value>
concept KeyedStore = requires(S s, const S cs, const Key key, Value value) {
    { cs.contains(key) } -> std::same_as<bool>;
    { cs.get(key, value) } -> std::same_as<bool>;
    { s.set(key, std::move(value)) } -> std::same_as<void>;
    { cs.size() } -> std::same_as<std::size_t>;
    { cs.empty() } -> std::same_as<bool>;
};

/******************************************************************************/

} /* namespace Gem::Common::Concurrency */
