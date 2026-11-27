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
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

// Boost headers go here

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/concurrency/GQueueCommon.hpp" // for QueueBackend

namespace Gem::Hap {
/******************************************************************************/
// For bi_normal_distribution

constexpr double DEF_BINORM_MEAN = 0.;
constexpr double DEF_BINORM_SIGMA1 = 0.1;
constexpr double DEF_BINORM_SIGMA2 = 0.1;
constexpr double DEF_BINORM_DISTANCE = 0.5;

/******************************************************************************/
// The constants of the random factory (GRandomFactory). They are fixed here, not build switches:
// the package size and the queue type are part of installed classes whose inline code a program
// compiles, so a program and the library must agree on them.

/**
 * The number of random words in one package (random_container, a std::array member). A QUEUE proxy
 * takes one package from the factory's queue per DEFAULTARRAYSIZE draws, so the size spreads the
 * cost of the queue over many draws.
 */
constexpr std::size_t DEFAULTARRAYSIZE = 10000;

/**
 * The capacity of the factory's buffers of packages. The producers FILL the fresh-package buffer
 * to capacity, so this is a deliberate hap-specific memory budget, NOT the generic queue default
 * (Gem::Common::DEFAULTBUFFERSIZE == 5000, which at DEFAULTARRAYSIZE doubles per package would pin
 * ~400 MB of RSS). 64 packages (~5 MB) are ample head-room: consumers drain at most a handful of
 * packages per proxy refill while the producer pool refills continuously; a persistent shortfall
 * shows up in getNGetTimeouts() rather than being hidden by a huge buffer.
 */
constexpr std::size_t DEFAULTFACTORYBUFFERSIZE = 64;

/**
 * The MPMC-queue backend of the factory's package buffers: the std::deque-backed queue. The
 * preallocated ring buffer is an interface-identical alternative (see
 * Gem::Common::Concurrency::GMPMCQueueT); the choice is part of the factory's member types.
 */
constexpr Gem::Common::Concurrency::QueueBackend FACTORYQUEUEBACKEND =
    Gem::Common::Concurrency::QueueBackend::Deque;

/**
 * How long, in milliseconds, getNewRandomContainer() waits for a package before it returns empty
 * and the proxy tries again: short enough that a draw finds out soon when no producer is left.
 */
constexpr std::uint16_t DEFAULTFACTORYGETWAIT = 200;

/// How long the call that starts producer threads waits for their start before it warns that it waits
constexpr std::chrono::seconds PRODUCERSTARTFIRSTWARNING{10};
/// How long that call waits between its later warnings
constexpr std::chrono::seconds PRODUCERSTARTREPEATWARNING{60};

/**
 * How many seeds getSeed() makes from one batch of fresh entropy before it draws the next batch.
 */
constexpr std::size_t DEFAULTSEEDVECTORSIZE = 2000;

/**
 * The fallback number of threads that simultaneously produce [0,1[ random numbers when the core
 * count cannot be determined, and the floor for the hardware-derived count (see
 * autoProducerThreadCount()).
 */
constexpr std::uint16_t DEFAULT01PRODUCERTHREADS = 2;

/** @brief Divisor applied to Gem::Common::getNHardwareThreads() when auto-sizing the RNG producer pool. */
constexpr unsigned int PRODUCERTHREADS_HW_DIVISOR = 4;
/** @brief Upper clamp on the auto-sized producer count (bounds fresh-queue lock contention). */
constexpr std::uint16_t MAXAUTOPRODUCERTHREADS = 8;

/******************************************************************************/
/**
 * The source strategy a Gem::Hap::GRandomT<source> proxy uses to obtain random numbers.
 *
 * Renamed from the former RANDFLAVOURS as the set of sources grows: QUEUE was RANDOMPROXY,
 * LOCAL was RANDOMLOCAL. The underlying integer values are preserved (QUEUE=0, LOCAL=1) so
 * any streamed/serialized values are unaffected. STAGED=2 and QUARANTINE=3 are new sources
 * (no legacy value).
 */
enum class randomSource : Gem::Common::ENUMBASETYPE {
    QUEUE = 0, ///< numbers are taken from the central factory (the package queue) -- the default
    LOCAL = 1, ///< numbers are produced locally, from a per-proxy engine seeded by the factory
    STAGED =
        2, ///< numbers are claimed in chunks from a shared, bulk-filled staging pool into a per-proxy double buffer (a number can be handed out twice)
    QUARANTINE =
        3 ///< numbers are read in place from N rotating, bulk-filled pools (benign-race; relies on aligned-64-bit atomicity; a number can be handed out twice)
};

/******************************************************************************/
// Re-export the shared numeric enum stream operators (see numeric_enum_values_v in
// GCommonEnums.hpp) into Gem::Hap, so that argument-dependent lookup finds them
// for the enums above.
using Gem::Common::operator<<;
using Gem::Common::operator>>;

/******************************************************************************/
} /* namespace Gem::Hap */

/******************************************************************************/
/** @brief randomSource streams as its underlying numeric value (see numeric_enum_values_v) */
namespace Gem::Common {
template <>
inline constexpr auto numeric_enum_values_v<Gem::Hap::randomSource> =
    numeric_enum_range<Gem::Hap::randomSource::QUEUE, Gem::Hap::randomSource::QUARANTINE>();
} /* namespace Gem::Common */
