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

// Geneva headers go here
#include "common/concurrency/GLoadOnceCellT.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A load-once, thereafter-immutable store for a problem's hardware-independent constant data.
 *
 * This is the metadata-boundary facility ((c) in the evaluator-unification plan): a runtime-loadable
 * individual module holds its hardware-independent problem constants -- a neural-network training set, a
 * Mona-Lisa target image, a list of parabola minima -- in one static instance of this store. The store is
 * filled EXACTLY ONCE from the factory's @c init_() hook (which @c Gem::Common::GFactoryT::globalInit()
 * already runs under its @c init_mutex_, so the fill is serialised process-wide), and is thereafter
 * immutable. Immutability is what lets both readers reach it with no locking: the free evaluator reads it in
 * its body, and -- when a device runs -- the consumer's problem-specific plug packs it for the device, from
 * the same single source (retiring today's duplicate load between the CPU path and the GPU marshaller).
 *
 * It is the domain name/vocabulary for the shared @c Gem::Common::Concurrency::GLoadOnceCellT cell: the
 * externally-filled flavour (@c ensureLoaded() to fill, @c get() to read lock-free on the eval hot path).
 * The single-writer / many-lock-free-readers mechanism lives once in @c common/concurrency/ (Inv 2); this is
 * only an alias so existing call sites and the problem-store terminology are preserved.
 *
 * @tparam DataType The (default-constructible) payload type holding the problem constants
 */
template <typename DataType>
using GProblemStoreT = Gem::Common::Concurrency::GLoadOnceCellT<DataType>;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
