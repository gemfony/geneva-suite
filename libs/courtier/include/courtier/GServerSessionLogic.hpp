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
#include <cstdint>
#include <functional>
#include <memory>

namespace Gem::Courtier {

/******************************************************************************/
/**
 * What a server's session and the networked consumer share (GPeerSessionT, GNetworkedConsumerT):
 * how a session that found no work is woken, and what a request for an item yields.
 */

/******************************************************************************/
/**
 * @brief How a parked caller is told that work may be available (see
 * GNetworkedConsumerT::checkoutOrPark()). It is called once, from whichever thread made the work
 * available, so it must not block, must not throw and must not call into the consumer: it hands
 * the news to the caller's own executor. It refers to its caller weakly, so that the consumer's
 * list of parked callers never keeps one alive.
 *
 * It returns whether it reached its caller. The consumer wakes as many callers as items became
 * available, and one that is gone must not use up a wake-up that another caller needs.
 */
using GWakeFn = std::move_only_function<bool()>;

/**
 * @brief The outcome of GNetworkedConsumerT::checkoutOrPark(): an item, or the promise of one
 * wake-up, or neither (the server is stopping).
 *
 * @tparam processable_type The work-item type
 */
template <typename processable_type>
struct GCheckoutResult {
    std::unique_ptr<processable_type> item; ///< Set: an item was checked out
    bool parked = false; ///< True: nothing was pending; the caller will be woken once
    /// With an item: which checkout of its slot this is. A put-back of the item names it, so that it
    /// acts only while this is still the slot's current checkout (GNetworkedConsumerT::requeue()).
    std::uint64_t checkout = 0;
};

/******************************************************************************/

} /* namespace Gem::Courtier */
