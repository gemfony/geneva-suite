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

// Geneva headers go here
#include "weft/GArchive.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * @file GArchiveNamed.hpp
 * @brief The named-member spellings for a hand-written serialize(): @ref archive_named
 * (a plain member by NVP) and @ref archive_named_base (a base slice by NVP), both routing
 * to the @c GArchive (Weft) codecs. A serialize() body written against these helpers works
 * unchanged across the binary and JSON codecs.
 *
 * This is deliberately a tiny, low-dependency header (it pulls only @c GArchive), so the many
 * hand-written serialize sites -- including low-level ones like @c GProcessable -- can include
 * it without dragging in the whole member-reflection machinery of @c GMemberReflectionT.hpp
 * (which re-exports these same two helpers from here).
 */
/******************************************************************************/

/**
 * @brief The saving/loading direction of @p Archive as a plain @c bool (a @c GArchive codec
 * exposes @c is_saving as a @c static @c constexpr @c bool). A hand-written serialize that must
 * branch on direction (e.g. a wire-conditional member) uses this.
 */
template <typename Archive>
inline constexpr bool archive_is_saving_v = Archive::is_saving;

/**
 * @brief Emits @p ref through @p ar under NVP @p name.
 * @param ar The archive. @param name The NVP tag. @param ref The member reference.
 */
template <typename Archive, typename T>
inline void archive_named(Archive &ar, const char *name, T &ref) {
    ar &Gem::Weft::make_nvp(name, ref);
}

/**
 * @brief Emits the @p Base slice of @p derived as a named, nested sub-object, nesting via
 * @c named_base so base/derived members cannot collide on a shared name in the self-describing
 * (JSON) codec.
 * @tparam Base The base slice to serialize.
 * @param ar The archive. @param name The NVP tag. @param derived The derived object.
 */
template <typename Base, typename Archive, typename Derived>
inline void archive_named_base(Archive &ar, const char *name, Derived &derived) {
    ar &Gem::Weft::named_base<Base>(name, derived);
}

/**
 * @brief Emits @p bytes opaque bytes at @p data as a named binary blob. The length is
 * caller-managed both ways (serialize/recover it separately; pre-size the buffer on load).
 * @param ar The archive. @param name The NVP tag. @param data The block address. @param bytes The block length.
 */
template <typename Archive>
inline void archive_named_binary(Archive &ar, const char *name, void *data, std::size_t bytes) {
    ar.member(name); // key the blob (a JSON member; a no-op for the flat binary codec)
    ar &Gem::Weft::make_binary(data, bytes);
}

/**
 * @brief Emits a named caller-managed raw range of @p count @c T elements. As with
 * @ref archive_named_binary the count is caller-managed and the buffer is pre-sized on load.
 * @tparam T The element type (possibly const on save).
 * @param ar The archive. @param name The NVP tag. @param data The first element. @param count The element count.
 */
template <typename Archive, typename T>
inline void archive_named_array(Archive &ar, const char *name, T *data, std::size_t count) {
    ar.member(name); // key the range (a JSON member; a no-op for the flat binary codec)
    ar &Gem::Weft::make_array(data, count);
}

/******************************************************************************/

} // namespace Gem::Common
