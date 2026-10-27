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

// Boost header files go here

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Geneva headers go here
#include "common/GMemberReflectionT.hpp" // Gem::Common::archive_named (named-member emitter)
#include "weft/GBinaryArchive.hpp"        // Gem::Weft::GBinary[IO]Archive -- the wire-blob codec
#include "geneva/genome/GAdaptionKernels.hpp"
#include "geneva/genome/GGenomeLayout.hpp"

/******************************************************************************/
/**
 * Non-intrusive serialization support for the (otherwise POD-clean) adaption-layout structs. The bodies
 * are exposed as free gem_archive_serialize() overloads in the structs' own namespace, so the GArchive
 * codec's class dispatch finds them by ADL (the analogue of a non-intrusive serialize()); each is written
 * once against Gem::Common::archive_named and works unchanged across the binary and JSON codecs.
 *
 * The layout is serialised by value as part of GGenome's transport encoding (it carries no pointers and no
 * evolving state, so a flat serialisation is sufficient and self-contained).
 */
namespace Gem::Geneva::Genome {

/**
 * @brief Serializes a GaussConfig (the static Gauss adaptor configuration) field by field.
 * @tparam Archive The GArchive codec type.
 * @tparam T The Gauss config's floating-point type.
 * @param ar The archive to read from / write to.
 * @param g The GaussConfig to serialize.
 */
template <class Archive, typename T>
void gem_archive_serialize(Archive &ar, GaussConfig<T> &g) {
    using Gem::Common::archive_named;
    archive_named(ar, "sigma_sigma", g.sigma_sigma);
    archive_named(ar, "min_sigma", g.min_sigma);
    archive_named(ar, "max_sigma", g.max_sigma);
    archive_named(ar, "min_ad_prob", g.min_ad_prob);
    archive_named(ar, "max_ad_prob", g.max_ad_prob);
    archive_named(ar, "adapt_ad_prob", g.adapt_ad_prob);
    archive_named(ar, "adapt_sigma_prob", g.adapt_sigma_prob);
    archive_named(ar, "adaption_threshold", g.adaption_threshold);
    archive_named(ar, "mode", g.mode);
}

/**
 * @brief Serializes a BiGaussConfig (the static bi-gaussian adaptor configuration) field by field.
 * @tparam Archive The GArchive codec type.
 * @tparam T The bi-gaussian config's floating-point type.
 * @param ar The archive to read from / write to.
 * @param g The BiGaussConfig to serialize.
 */
template <class Archive, typename T>
void gem_archive_serialize(Archive &ar, BiGaussConfig<T> &g) {
    using Gem::Common::archive_named;
    archive_named(ar, "sigma_sigma1", g.sigma_sigma1);
    archive_named(ar, "sigma_sigma2", g.sigma_sigma2);
    archive_named(ar, "sigma_delta", g.sigma_delta);
    archive_named(ar, "min_sigma1", g.min_sigma1);
    archive_named(ar, "max_sigma1", g.max_sigma1);
    archive_named(ar, "min_sigma2", g.min_sigma2);
    archive_named(ar, "max_sigma2", g.max_sigma2);
    archive_named(ar, "min_delta", g.min_delta);
    archive_named(ar, "max_delta", g.max_delta);
    archive_named(ar, "min_ad_prob", g.min_ad_prob);
    archive_named(ar, "max_ad_prob", g.max_ad_prob);
    archive_named(ar, "adapt_ad_prob", g.adapt_ad_prob);
    archive_named(ar, "adapt_sigma_prob", g.adapt_sigma_prob);
    archive_named(ar, "adaption_threshold", g.adaption_threshold);
    archive_named(ar, "use_symmetric_sigmas", g.use_symmetric_sigmas);
    archive_named(ar, "mode", g.mode);
}

/**
 * @brief Serializes a FlipConfig (the static flip adaptor configuration) field by field.
 * @tparam Archive The GArchive codec type.
 * @param ar The archive to read from / write to.
 * @param g The FlipConfig to serialize.
 */
template <class Archive>
void gem_archive_serialize(Archive &ar, FlipConfig &g) {
    using Gem::Common::archive_named;
    archive_named(ar, "min_ad_prob", g.min_ad_prob);
    archive_named(ar, "max_ad_prob", g.max_ad_prob);
    archive_named(ar, "adapt_ad_prob", g.adapt_ad_prob);
    archive_named(ar, "mode", g.mode);
}

// The genome layout holds structure-only groups. The adaptor configuration lives on the (transient,
// non-serialized) OA-owned GAdaptionConfig, so only the structure is serialized here.
/**
 * @brief Serializes a GroupStructure (structure only: start / len / label_id / active).
 * @tparam Archive The GArchive codec type.
 * @tparam T The group's value type.
 * @param ar The archive to read from / write to.
 * @param g The GroupStructure to serialize.
 */
template <class Archive, typename T>
void gem_archive_serialize(Archive &ar, GroupStructure<T> &g) {
    using Gem::Common::archive_named;
    archive_named(ar, "start", g.start);
    archive_named(ar, "len", g.len);
    archive_named(ar, "label_id", g.label_id);
    archive_named(ar, "active", g.active);
}

/******************************************************************************/
// ChannelLayout transport encoding -- "layout interning" (the per-item layout payload is the same for
// every individual in a population and dominates the wire size for large genomes). A channel's per-value
// arrays (lower / upper / init_lower / init_upper / fold) are UNIFORM within each group by construction
// (a group is built with one bound + one perimeter + one fold bit), and `active` mirrors the group flag.
// So when every group is uniform
// we serialise ONE representative value-set PER GROUP -- O(groups) instead of O(values) -- and
// reconstruct the per-value arrays on load. ESCAPE ROUTE: a channel whose groups are NOT uniform (a
// layout with per-value variation) falls back to the full per-value arrays, flagged by
// `compact == false`. This is fully per-item: each individual carries its own (compact-or-full) layout,
// so a population with VARYING layouts (e.g. heterogeneous / meta-optimization individuals) is handled
// correctly -- no cross-item sharing is assumed.

/**
 * @brief True iff every group's per-value layout data is constant across the group (the normal case).
 * @tparam T The channel's value type.
 * @param c The channel to inspect.
 * @return true if every group's lower/upper/init_lower/init_upper/fold are uniform across the group;
 * false as soon as any per-value variation is found (triggering the full, non-compact encoding).
 */
template <typename T>
bool channelGroupsUniform(const ChannelLayout<T> &c) {
    for(const auto &g : c.groups) {
        for(std::uint32_t k = g.start + 1; k < g.start + g.len; ++k) {
            if(c.lower[k] != c.lower[g.start] || c.upper[k] != c.upper[g.start] ||
               c.init_lower[k] != c.init_lower[g.start] || c.init_upper[k] != c.init_upper[g.start] ||
               c.fold[k] != c.fold[g.start]) {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Saves a ChannelLayout, using the compact one-value-set-per-group encoding when groups are
 * uniform and falling back to full per-value arrays otherwise (flagged by the `compact` field).
 * @tparam Archive The saving GArchive codec type.
 * @tparam T The channel's value type.
 * @param ar The archive to write to.
 * @param c The channel to save.
 */
template <class Archive, typename T>
void saveChannelLayout(Archive &ar, const ChannelLayout<T> &c) {
    using Gem::Common::archive_named;
    bool compact = channelGroupsUniform<T>(c);
    archive_named(ar, "compact", compact);
    archive_named(ar, "groups", c.groups); // const referent -> T deduces const; both backends save it
    if(compact) {
        // One representative value-set per group; per-value arrays + `active` are rebuilt on load.
        std::vector<T> g_lower, g_upper, g_init_lower, g_init_upper;
        std::vector<std::uint8_t> g_fold;
        const std::size_t ng = c.groups.size();
        g_lower.reserve(ng);
        g_upper.reserve(ng);
        g_init_lower.reserve(ng);
        g_init_upper.reserve(ng);
        g_fold.reserve(ng);
        for(const auto &g : c.groups) {
            g_lower.push_back(c.lower[g.start]);
            g_upper.push_back(c.upper[g.start]);
            g_init_lower.push_back(c.init_lower[g.start]);
            g_init_upper.push_back(c.init_upper[g.start]);
            g_fold.push_back(c.fold[g.start]);
        }
        archive_named(ar, "g_lower", g_lower);
        archive_named(ar, "g_upper", g_upper);
        archive_named(ar, "g_init_lower", g_init_lower);
        archive_named(ar, "g_init_upper", g_init_upper);
        archive_named(ar, "g_fold", g_fold);
    }
    else {
        archive_named(ar, "lower", c.lower);
        archive_named(ar, "upper", c.upper);
        archive_named(ar, "init_lower", c.init_lower);
        archive_named(ar, "init_upper", c.init_upper);
        archive_named(ar, "fold", c.fold);
        archive_named(ar, "active", c.active);
    }
}

/**
 * @brief Loads a ChannelLayout, rebuilding the per-value arrays from the compact per-group encoding
 * when `compact` is set, or reading the full per-value arrays directly otherwise.
 * @tparam Archive The loading GArchive codec type.
 * @tparam T The channel's value type.
 * @param ar The archive to read from.
 * @param c The channel to populate.
 */
template <class Archive, typename T>
void loadChannelLayout(Archive &ar, ChannelLayout<T> &c) {
    using Gem::Common::archive_named;
    bool compact = false;
    archive_named(ar, "compact", compact);
    archive_named(ar, "groups", c.groups);
    if(compact) {
        std::vector<T> g_lower, g_upper, g_init_lower, g_init_upper;
        std::vector<std::uint8_t> g_fold;
        archive_named(ar, "g_lower", g_lower);
        archive_named(ar, "g_upper", g_upper);
        archive_named(ar, "g_init_lower", g_init_lower);
        archive_named(ar, "g_init_upper", g_init_upper);
        archive_named(ar, "g_fold", g_fold);
        // The groups tile [0, size()) contiguously, so size == past-the-end of the last group.
        const std::size_t size = c.groups.empty()
                                     ? 0
                                     : static_cast<std::size_t>(c.groups.back().start + c.groups.back().len);
        c.lower.assign(size, T{});
        c.upper.assign(size, T{});
        c.init_lower.assign(size, T{});
        c.init_upper.assign(size, T{});
        c.fold.assign(size, std::uint8_t{0});
        c.active.assign(size, std::uint8_t{0});
        for(std::size_t gi = 0; gi < c.groups.size(); ++gi) {
            const auto &g = c.groups[gi];
            for(std::uint32_t k = g.start; k < g.start + g.len; ++k) {
                c.lower[k] = g_lower[gi];
                c.upper[k] = g_upper[gi];
                c.init_lower[k] = g_init_lower[gi];
                c.init_upper[k] = g_init_upper[gi];
                c.fold[k] = g_fold[gi];
                c.active[k] = g.active ? std::uint8_t{1} : std::uint8_t{0};
            }
        }
    }
    else {
        archive_named(ar, "lower", c.lower);
        archive_named(ar, "upper", c.upper);
        archive_named(ar, "init_lower", c.init_lower);
        archive_named(ar, "init_upper", c.init_upper);
        archive_named(ar, "fold", c.fold);
        archive_named(ar, "active", c.active);
    }
}

/**
 * @brief Non-intrusive ChannelLayout (de)serializer for a GArchive codec: dispatches the split
 * save/load bodies on the codec's compile-time direction.
 * @tparam Archive The GArchive codec type.
 * @tparam T The channel's value type.
 * @param ar The archive to read from / write to.
 * @param c The channel to serialize.
 */
template <class Archive, typename T>
void gem_archive_serialize(Archive &ar, ChannelLayout<T> &c) {
    if constexpr (Archive::is_saving) {
        saveChannelLayout(ar, c);
    } else {
        loadChannelLayout(ar, c);
    }
}

/**
 * @brief Serializes a whole GGenomeLayout: its four value channels (d / f / i / b) plus the interned labels.
 *
 * Loading replaces the structure wholesale, so the target's cached content id is dropped afterwards: a
 * layout that had already answered layoutId() would otherwise keep reporting the id of the structure it
 * held before the load, and that id is exactly what the transport's send-once machinery keys on.
 *
 * @tparam Archive The GArchive codec type.
 * @param ar The archive to read from / write to.
 * @param l The layout to serialize.
 */
//! [weft-layout-nonintrusive]
template <class Archive>
void gem_archive_serialize(Archive &ar, GGenomeLayout &l) {
    using Gem::Common::archive_named;
    archive_named(ar, "d", l.d);
    archive_named(ar, "f", l.f);
    archive_named(ar, "i", l.i);
    archive_named(ar, "b", l.b);
    archive_named(ar, "labels", l.labels);

    if constexpr (not Archive::is_saving) {
        l.invalidateId();
    }
}
//! [weft-layout-nonintrusive]

} /* namespace Gem::Geneva::Genome */

namespace Gem::Geneva::Genome {

/******************************************************************************/
/**
 * @brief Serializes a (structure-only) layout into a standalone binary blob (the transport "send-once"
 * wire form). The blob is always BINARY and self-contained, independent of the surrounding archive's
 * format: it is carried as the whole body of a SEND_BLOB message and cached in the wire registry, so
 * it is decoded on its own by layoutFromWireBlob(), never embedded in another archive.
 *
 * @param layout The layout to serialize.
 * @return The serialized layout as a binary blob.
 */
inline std::string layoutToWireBlob(const GGenomeLayout &layout) {
    // The layout tree is archive-generic (gem_archive_serialize overloads above), so it encodes
    // directly through the Weft flat-binary codec; the const_cast is safe because saving never
    // mutates (the same borrow-to-save idiom as GCommonInterfaceT's GEM_BINARY arm).
    Gem::Weft::GBinaryOArchive oa;
    oa &Gem::Weft::make_nvp("layout", const_cast<GGenomeLayout &>(layout));
    return oa.str();
}

/******************************************************************************/
/**
 * @brief Reconstructs a layout from a binary blob produced by layoutToWireBlob().
 *
 * @param blob The binary blob to decode.
 * @return A freshly-owned, immutable layout reconstructed from the blob.
 */
inline std::shared_ptr<const GGenomeLayout> layoutFromWireBlob(const std::string &blob) {
    auto layout = std::make_shared<GGenomeLayout>();
    // GBinaryIArchive holds a string_view over `blob`, which outlives it here.
    Gem::Weft::GBinaryIArchive ia(blob);
    ia &Gem::Weft::make_nvp("layout", *layout);
    return layout;
}

} /* namespace Gem::Geneva::Genome */
