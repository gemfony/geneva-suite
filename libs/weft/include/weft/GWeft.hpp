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

// Standard header files go here
#include <string>

/**
 * @file GWeft.hpp
 * @brief Umbrella header and library identity for Weft, the standalone
 * serialization engine.
 *
 * @mainpage Weft — a self-contained C++23 serialization engine
 *
 * Weft (German @e Schussfaden — the weft thread woven through the warp) is the
 * foundation layer of the Geneva library collection: a small, dependency-free
 * serialization engine that other libraries are woven onto. It provides
 *
 *   - a Boost.Serialization-style intrusive/non-intrusive @c serialize contract
 *     (member @c serialize reachable through @c Gem::Weft::access, or a free
 *     @c gem_archive_serialize found by ADL);
 *   - two value codecs — a compact little-endian binary codec
 *     (@ref Gem::Weft::GBinaryOArchive / @ref Gem::Weft::GBinaryIArchive) and a
 *     human-readable JSON codec (@ref Gem::Weft::GJsonOArchive /
 *     @ref Gem::Weft::GJsonIArchive), a capability Boost.Serialization lacks;
 *   - polymorphic-pointer support through a content-addressed type registry
 *     (@ref Gem::Weft::GPolymorphicRegistry) and a registry-thunk dispatch
 *     (@ref GEM_REGISTER_ARCHIVABLE);
 *   - its own tiny error facility (@ref Gem::Weft::weft_exception).
 *
 * Weft depends on nothing beyond the C++23 standard library and — for the JSON
 * codec only — Boost.JSON, so it can be reused outside Geneva. This umbrella
 * header pulls in the whole public surface.
 */

// Weft header files go here
#include "weft/GArchive.hpp"
#include "weft/GArchivePolymorphic.hpp"
#include "weft/GBinaryArchive.hpp"
#include "weft/GJsonArchive.hpp"
#include "weft/GPolymorphicRegistry.hpp"
#include "weft/GWeftError.hpp"

namespace Gem::Weft {

/******************************************************************************/
/**
 * @brief Returns the Weft library version as a human-readable string.
 *
 * This is a genuine out-of-line symbol that anchors the @c gemfony-weft shared
 * object (the codecs and registry are otherwise header-only templates), so the
 * library builds and installs uniformly with the other Geneva libraries.
 *
 * @return The Weft version string (e.g. "1.99.0-beta1").
 */
[[nodiscard]] std::string version();

/******************************************************************************/

} // namespace Gem::Weft
