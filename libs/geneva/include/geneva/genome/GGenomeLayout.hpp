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

// Standard header files go here
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

// Geneva headers go here
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/concurrency/GLoadOnceCellT.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GAdaptionKernels.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
/**
 * The adaption floating-point type for a parameter type: double parameters adapt in double, float
 * parameters in float, everything else (int / bool) carries a double-typed Gauss config that simply
 * stays unused (those channels are mutated by flip adaptors). The trait is defined locally so the
 * layout (in geneva/genome/) stays self-contained.
 *
 * @tparam T The parameter type whose adaption floating-point type is being selected.
 */
template <typename T> struct adaption_fp { using type = double; };
template <> struct adaption_fp<double> { using type = double; };
template <> struct adaption_fp<float> { using type = float; };
template <typename T> using adaption_fp_t = typename adaption_fp<T>::type;


/******************************************************************************/
/**
 * The reflecting fold, a free function over (val, lo, hi). It maps an unbounded internal value into the
 * half-open external range [lo, hi). Computed in long double to keep the double behaviour bit-for-bit.
 * NaN / infinity are rejected unconditionally (they would silently corrupt the range comparison and
 * every value derived from them).
 *
 * @tparam T The floating-point value type (double or float).
 * @param val The unbounded internal value to fold into range.
 * @param lo The lower (inclusive) boundary of the external range.
 * @param hi The upper (exclusive) boundary of the external range.
 * @return The folded value in the half-open range [lo, hi); lo if the range is collapsed (hi <= lo).
 * @throw geneva_exception if val is NaN or infinite.
 */
template <typename T>
T foldConstrainedFP(const T &val, const T &lo, const T &hi) {
    if(std::isnan(val) || std::isinf(val)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In foldConstrainedFP(): Error" << '\n'
            << "val is " << (std::isnan(val) ? "NaN" : "infinite") << '\n'
        );
    }

    // A FROZEN parameter has lo == hi (a user may fix a parameter by setting equal bounds). The fold
    // interval [lo, hi) is then empty and the region computation below would divide by (upper - lower) == 0.
    // The only meaningful value of a collapsed range is that single point, so return it directly.
    if(hi <= lo) {
        return lo;
    }

    const auto local_val = Gem::Common::narrow<long double>(val);
    const auto lower = Gem::Common::narrow<long double>(lo);
    const auto upper = Gem::Common::narrow<long double>(hi);

    if(local_val >= lower && local_val < upper) {
        return val;
    }

    // Determine which fold region the value lies in (compare transferFunction.pdf shipped with the
    // tree implementation). floor() is required for correctness with negative values.
    const auto region =
        Gem::Common::narrow<std::int64_t>(std::floor((local_val - lower) / (upper - lower)));

    long double mapping = 0.L;
    if(region % 2 == 0) { // even region (0, 2, ... or a negative even range): ascending
        mapping = local_val - (static_cast<long double>(region) * (upper - lower));
    }
    else { // odd region: descending
        mapping = -local_val + (static_cast<long double>(region - 1) * (upper - lower) + 2 * upper);
    }

    T result = Gem::Common::narrow<T>(mapping);

    // The long-double fold lands strictly inside [lo, hi); a narrowing cast can, at low precision,
    // round onto a boundary. Enforce the half-open contract (a no-op for double in practice).
    if(result < lo) {
        result = lo;
    }
    else if(result >= hi) {
        result = std::nextafter(hi, lo);
    }
    return result;
}

/******************************************************************************/
/**
 * The reflecting fold of GConstrainedIntT::transfer(), re-expressed as a free function over (val, lo,
 * hi). Integer ranges are CLOSED [lo, hi] (both boundaries included), unlike the half-open FP range.
 *
 * @tparam T The integer value type.
 * @param val The unbounded internal value to fold into range.
 * @param lo The lower (inclusive) boundary of the closed external range.
 * @param hi The upper (inclusive) boundary of the closed external range.
 * @return The folded value in the closed range [lo, hi].
 */
template <typename T>
T foldConstrainedInt(const T &val, const T &lo, const T &hi) {
    if(val >= lo && val <= hi) {
        return val;
    }

    const T value_range = hi - lo + T(1); // both boundaries are included
    T mapping = T(0);

    if(val < lo) {
        const T n_below = (lo - (val + T(1))) / value_range;
        mapping = val + (value_range * (n_below + T(1)));
        if(n_below % 2 == 0) {
            mapping = lo + hi - mapping; // revert (descending range)
        }
    }
    else { // val > hi
        const T n_above = (val - hi - T(1)) / value_range;
        mapping = val - (value_range * (n_above + T(1)));
        if(n_above % 2 == 0) {
            mapping = lo + hi - mapping; // revert (descending range)
        }
    }
    return mapping;
}

/******************************************************************************/
// --- Normalized internal coordinate (normalized-genome architecture, §2.1) ---------------------------
// The OA-facing INTERNAL value lives in the centered, width-1 interval [-0.5, 0.5); the user-facing
// EXTERNAL value is an affine image of it (plus, for a bounded parameter, the reflecting fold above).
// With a width-1 interval, scale == (upper - lower) and anchor == (upper + lower) / 2, so internal
// -0.5 maps to `lower` and +0.5 to `upper`. `scale` is the single scale concept of the model, computed
// here from the bounds (ngScale) -- there is no separately stored copy. These helpers are the single
// source of truth for the coordinate transform and back the live read/write path (GGenome). All maps
// compose in long double for a faithful, well-conditioned round-trip even for offset / narrow boxes.

/** @brief external = anchor + u * scale, composed in long double and narrowed back to T. */
template <typename T>
T ngInternalToExternal(const T &u, const T &scale, const T &anchor) {
    const long double x = Gem::Common::narrow<long double>(anchor)
                          + Gem::Common::narrow<long double>(u) * Gem::Common::narrow<long double>(scale);
    return Gem::Common::narrow<T>(x);
}

/** @brief u = (external - anchor) / scale, composed in long double and narrowed back to T. A collapsed
 *  scale (frozen parameter, scale <= 0) maps everything to the interval centre 0. */
template <typename T>
T ngExternalToInternal(const T &x, const T &scale, const T &anchor) {
    if(not(scale > T(0))) {
        return T(0);
    }
    const long double u = (Gem::Common::narrow<long double>(x) - Gem::Common::narrow<long double>(anchor))
                          / Gem::Common::narrow<long double>(scale);
    return Gem::Common::narrow<T>(u);
}

/** @brief Reflecting fold of an internal value into the canonical interval [-0.5, 0.5). Reuses the
 *  single reflection implementation (foldConstrainedFP) -- the centered fold is just a parameterisation. */
template <typename T>
T ngFoldInternal(const T &u) {
    return foldConstrainedFP<T>(u, T(-0.5), T(0.5));
}

/** @brief Enforce the half-open [lo, hi) contract on an external value. Necessary because the affine
 *  map + narrowing can round a correctly half-open internal value (uf < 0.5) onto the EXCLUSIVE upper
 *  bound (e.g. 0.5 + (0.5-eps)*scale -> hi). This is the external-coordinate analogue of the boundary
 *  enforcement inside foldConstrainedFP, applied AFTER the affine map so the conditioning benefit of
 *  folding in the centered interval is preserved (no re-fold in user coordinates). */
template <typename T>
T ngClampHalfOpen(T x, const T &lo, const T &hi) {
    if(hi <= lo) {
        return lo; // frozen / collapsed range
    }
    if(x < lo) {
        return lo;
    }
    if(x >= hi) {
        return std::nextafter(hi, lo);
    }
    return x;
}

/******************************************************************************/
/**
 * The STRUCTURE of one adaption group, as held by the shared genome layout: a contiguous run of values
 * within a single channel that form one group (a group of length 1 mirrors a standalone parameter object;
 * a longer group a collection). This is structure ONLY -- the adaptor configuration (Gauss / bi-Gauss /
 * flip rates and seeds) is NOT here; it lives on the OA-owned GAdaptionConfig (a GroupSpec, below). The
 * layout therefore carries no adaption intent at all, staying a pure, shared, immutable structural
 * descriptor. `active` is structural (whether the group is mutable at all -- adaptionMode::NEVER), so it
 * stays here and is copied into the config when one is built from the genome. A parameter's natural scale
 * is NOT stored: it is the single `scale` concept of the normalized model, computed on demand from the
 * channel's bounds (ngScale) wherever it is needed -- there is no redundant `range` field.
 */
template <typename T>
struct GroupStructure {
    std::uint32_t start = 0;    ///< index of the first value of this group within the channel
    std::uint32_t len = 1;      ///< number of values in this group
    std::int32_t label_id = -1; ///< interned label index into GGenomeLayout::labels (-1 = unlabeled)
    bool active = true;         ///< false ⇔ adaptionMode::NEVER (the group is never adapted / mutated)

    /** @brief Field-wise structural equality (the basis of the layout's collision-safe id compare). */
    bool operator==(const GroupStructure &) const = default;
};

/******************************************************************************/
/**
 * One adaption group's full configuration, owned by the OA's GAdaptionConfig (NOT by the genome layout).
 * It carries the group's STRUCTURE (mirrored from the layout's GroupStructure: start / len / label_id /
 * active) plus the *static* adaption config (rates / bounds / mode) and the *seed* values for the
 * per-individual adaption state (which the OA installs into a slot's auxiliary store). The evolving state
 * itself lives in the store, not here. A GAdaptionConfig is built from a genome (snapshotting the
 * structure) and then authored via its fluent API; the adaptor settings never touch the shared layout.
 */
template <typename T>
struct GroupSpec {
    std::uint32_t start = 0;       ///< index of the first value of this group within the channel
    std::uint32_t len = 1;         ///< number of values in this group
    std::int32_t label_id = -1;    ///< interned label index into GGenomeLayout::labels (-1 = unlabeled)
    bool active = true;            ///< false ⇔ adaptionMode::NEVER (the group is never adapted)
    bool has_gauss = false;        ///< whether a Gauss adaptor is configured (FP groups only)
    GaussConfig<adaption_fp_t<T>> gauss{};        ///< the static Gauss configuration (valid iff has_gauss)
    adaption_fp_t<T> start_sigma = adaption_fp_t<T>(1);   ///< GaussState sigma seed + updateOnStall reset target
    adaption_fp_t<T> start_ad_prob = adaption_fp_t<T>(1); ///< Gauss/BiGauss/Flip ad_prob seed + reset target

    // Bi-gaussian alternative (FP groups only): mutually exclusive with has_gauss for the same group.
    bool has_bigauss = false;      ///< whether a bi-gaussian adaptor is configured (FP groups only)
    BiGaussConfig<adaption_fp_t<T>> bigauss{};            ///< the static bi-gaussian configuration (valid iff has_bigauss)
    adaption_fp_t<T> start_sigma1 = adaption_fp_t<T>(1);  ///< BiGaussState sigma1 seed + reset target
    adaption_fp_t<T> start_sigma2 = adaption_fp_t<T>(1);  ///< BiGaussState sigma2 seed + reset target
    adaption_fp_t<T> start_delta = adaption_fp_t<T>(0.5); ///< BiGaussState delta seed + reset target

    // Flip adaptor (int32 / bool groups only).
    bool has_flip = false;         ///< whether a flip adaptor is configured (int / bool groups only)
    FlipConfig flip{};             ///< the static flip configuration (valid iff has_flip)
};

/******************************************************************************/
/**
 * The structural description of one value channel (all parameters of a single type). Per-value: the
 * hard bound [lower, upper], the init perimeter [init_lower, init_upper], a `fold` bit and an active
 * flag; plus the list of adaption groups tiling the channel. Held by the shared GGenomeLayout; the
 * per-individual GGenome only stores the value array and a handle to this.
 *
 * The `fold` bit is the single bounded/unbounded distinction (§2.5):
 *   - fold == true  (bounded): the value folds into the half-open [lower, upper) (FP) / closed [lower,
 *                              upper] (int); an external write out of range throws. The mutation scale
 *                              is the bound width (upper - lower).
 *   - fold == false (unbounded): there is NO hard bound (lower/upper are unused as a constraint); the
 *                              value may roam ℝ. The mutation scale is the init-perimeter width.
 * The init perimeter [init_lower, init_upper] is the random-init region; for a bounded parameter it may
 * be narrower than the bound (a tight start within a wide box) and is the scale only for unbounded
 * parameters (where there is no bound to scale by).
 */
template <typename T>
struct ChannelLayout {
    std::vector<T> lower;            ///< per value: lower bound (fold ceiling if folding; unused as a constraint otherwise)
    std::vector<T> upper;            ///< per value: upper bound (fold ceiling if folding; unused as a constraint otherwise)
    std::vector<T> init_lower;       ///< per value: lower random-init perimeter (and the mutation scale for unbounded)
    std::vector<T> init_upper;       ///< per value: upper random-init perimeter (and the mutation scale for unbounded)
    std::vector<std::uint8_t> fold;  ///< per value: 1 ⇔ the value folds into [lower,upper) (bounded); 0 ⇔ unbounded
    std::vector<std::uint8_t> active;///< per value: mirrors the owning group's active flag (1/0)
    std::vector<GroupStructure<T>> groups;///< contiguous groups tiling [0, size()) -- STRUCTURE only

    /** @brief The number of values in this channel. @return The per-value array length. */
    [[nodiscard]] std::size_t size() const { return lower.size(); }
};

/******************************************************************************/
// Per-value scale / anchor for the normalized internal coordinate (§2.1). A bounded parameter is scaled
// by its hard bound [lower, upper); an unbounded one (no hard bound) by its init perimeter
// [init_lower, init_upper). No separate state is stored -- these are computed from the arrays present.

/** @brief The mutation scale of value k: the bound width (folding) or the init-perimeter width (unbounded). */
template <typename T>
T ngScale(ChannelLayout<T> const &ch, std::size_t k) {
    return ch.fold[k] ? (ch.upper[k] - ch.lower[k]) : (ch.init_upper[k] - ch.init_lower[k]);
}

/** @brief The anchor (interval centre) of value k, matching ngScale's interval choice. */
template <typename T>
T ngAnchor(ChannelLayout<T> const &ch, std::size_t k) {
    return ch.fold[k] ? ((ch.lower[k] + ch.upper[k]) / T(2))
                      : ((ch.init_lower[k] + ch.init_upper[k]) / T(2));
}

/******************************************************************************/
/** @brief Identifies one of the four value channels (used to address a group across channels). */
enum class ChannelTag : std::uint8_t { Double, Float, Int32, Bool };

/******************************************************************************/
/** @brief A reference to one adaption group: which channel it lives in + its index within that channel. */
struct GroupRef {
    ChannelTag channel;     ///< the value channel the group belongs to
    std::size_t index;      ///< the group's index within that channel's `groups`
};

/******************************************************************************/
/**
 * A 128-bit content id for a genome layout: a strong structural hash of the (structure-only) layout.
 * It is the key the transport layer uses to send a layout once and reference it by id thereafter
 * (the layout send-once wire transport). Being a hash of the CONTENT -- not a process-local pointer or counter --
 * it is stable across processes and safe for evolving structure: a layout whose structure changes
 * (e.g. a NEAT-style architecture amendment) hashes to a different id, so it is treated as a new layout
 * automatically, with no "layout changed" signalling. Hash collisions are guarded against by an exact
 * structural compare (GGenomeLayout::sameStructure) wherever a collision could cause incorrect dedup.
 */
struct LayoutId {
    std::uint64_t hi = 0; ///< the high 64 bits of the content hash
    std::uint64_t lo = 0; ///< the low 64 bits of the content hash

    /** @brief Bitwise equality of the two id halves. @return true iff both halves match. */
    bool operator==(const LayoutId &) const = default;
    /** @brief A total order over the 128-bit id (for use as an ordered-map key).
     *  @param o The id to compare against. @return true iff this id sorts before o. */
    bool operator<(const LayoutId &o) const noexcept {
        return (hi != o.hi) ? (hi < o.hi) : (lo < o.lo);
    }
};

/******************************************************************************/
/**
 * The shared, immutable per-type structural descriptor of a flat genome (DM §2 metadata). It holds
 * one ChannelLayout per supported value type. A single layout is built once (by GGenomeBuilder, and
 * in turn by a factory) and shared by every individual of a problem via std::shared_ptr<const ...>,
 * so per-individual state is just the value arrays. The layout carries no evolving state; the
 * per-individual, per-group adaption state (sigma, ...) lives in the individual's scratch (GAuxiliaryStore).
 *
 * Optionally, groups carry an interned LABEL: `labels` holds each distinct label string once, and a
 * GroupStructure stores a small integer index into it (GroupStructure::label_id, -1 = unlabeled). Labels are
 * per-GROUP (never per-value) and one-to-many -- a single label can tag many groups (e.g. "position"
 * tags every cx/cy group in an image problem), so a per-label adaption setting applies to all of them
 * while each group keeps its own evolving state. Labels let a (later, OA-owned) adaption config address
 * groups by name; structureless problems carry no labels and address by index.
 */
class GGenomeLayout {
public:
    ChannelLayout<double> d;        ///< the double channel
    ChannelLayout<float> f;         ///< the float channel
    ChannelLayout<std::int32_t> i;  ///< the int32 channel
    ChannelLayout<bool> b;          ///< the bool channel

    std::vector<std::string> labels;///< the interned, distinct group-label strings (label_id indexes this)

    /***************************************************************************/
    // Special members. The layout caches its content id lazily in a GLoadOnceCellT (which owns a mutex
    // and is thus neither copyable nor movable), so the value-member copy/move are spelled out. A newly
    // CONSTRUCTED copy starts with a fresh, cold cell and recomputes the (identical) id on first use; an
    // ASSIGNED-TO layout may already have answered layoutId() for its previous structure, so both
    // assignment operators explicitly invalidate the cache (see invalidateId()).

    /** @brief The default constructor (empty layout). */
    GGenomeLayout() = default;
    /** @brief Copy constructor: copies the structural members; the id cache is left cold. */
    GGenomeLayout(const GGenomeLayout &cp)
      : d(cp.d), f(cp.f), i(cp.i), b(cp.b), labels(cp.labels) {}
    /** @brief Move constructor: moves the structural members; the id cache is left cold. */
    GGenomeLayout(GGenomeLayout &&cp) noexcept
      : d(std::move(cp.d)), f(std::move(cp.f)), i(std::move(cp.i)), b(std::move(cp.b)),
        labels(std::move(cp.labels)) {}
    /** @brief Copy assignment: copies the structural members and drops the cached id, which described
     *  the structure this layout used to have.
     *  @param cp The layout to copy from. @return A reference to this layout. */
    GGenomeLayout &operator=(const GGenomeLayout &cp) {
        if(this != &cp) {
            d = cp.d; f = cp.f; i = cp.i; b = cp.b; labels = cp.labels;
            id_cell_.reset();
        }
        return *this;
    }
    /** @brief Move assignment: moves the structural members and drops the cached id, which described
     *  the structure this layout used to have.
     *  @param cp The layout to move from. @return A reference to this layout. */
    GGenomeLayout &operator=(GGenomeLayout &&cp) noexcept {
        if(this != &cp) {
            d = std::move(cp.d); f = std::move(cp.f); i = std::move(cp.i); b = std::move(cp.b);
            labels = std::move(cp.labels);
            id_cell_.reset();
        }
        return *this;
    }
    /** @brief The destructor. */
    ~GGenomeLayout() = default;

    /***************************************************************************/
    /**
     * @brief The layout's 128-bit content id: a strong structural hash, computed once and cached.
     *
     * Two layouts with identical structure (channels, per-value bounds/kind/active, groups and labels)
     * produce the same id; any structural difference produces (with overwhelming probability) a
     * different id. The id is used by the transport layer to send a layout once and reference it by id.
     * Computed lazily and thread-safely on this (immutable) layout, so concurrent first calls are safe.
     *
     * @return A const reference to the cached content id.
     */
    const LayoutId &layoutId() const {
        return id_cell_.getOrCompute([this] { return computeLayoutId(); });
    }

    /**
     * @brief Drops the cached content id, so the next layoutId() recomputes it from the current structure.
     *
     * The structural members (`d`, `f`, `i`, `b`, `labels`) are public and therefore mutable from outside;
     * a caller that changes them after layoutId() has already been read must say so with this call, or the
     * layout keeps reporting the id of the structure it used to have. The two assignment operators and the
     * non-intrusive deserializer do it for their own wholesale replacements. This matters beyond tidiness:
     * the transport's send-once machinery keys a cached layout blob on this id, so a stale id references
     * the wrong layout on the wire.
     *
     * @note Like the underlying cell's reset(), this is a single-threaded lifecycle operation: it must not
     *       race a concurrent layoutId() reader.
     */
    void invalidateId() {
        id_cell_.reset();
    }

    /**
     * @brief Exact structural equality -- the collision-safe compare backing the content id.
     *
     * Compares every per-value array, group vector and the interned label table. Two layouts compare
     * equal iff they are byte-for-byte structurally identical; this is what makes a hash collision on
     * layoutId() safe to detect (and reject) at the point it could cause an incorrect dedup.
     *
     * @param o The other layout to compare against.
     * @return true iff the two layouts have identical structure.
     */
    bool sameStructure(const GGenomeLayout &o) const {
        return sameChannel(d, o.d) && sameChannel(f, o.f) && sameChannel(i, o.i) &&
               sameChannel(b, o.b) && labels == o.labels;
    }

    /**
     * @brief Interns a label string, returning its id; an already-present string returns its existing id.
     * @param name The label string to intern.
     * @return The id (index into `labels`) of the interned or pre-existing string.
     */
    std::int32_t internLabel(const std::string &name) {
        const std::int32_t existing = labelId(name);
        if(existing >= 0) {
            return existing;
        }
        labels.push_back(name);
        return static_cast<std::int32_t>(labels.size() - 1);
    }

    /**
     * @brief Resolves a label string to its id, or -1 if it is not interned.
     * @param name The label string to look up.
     * @return The id (index into `labels`) of the string, or -1 if it is not present.
     */
    std::int32_t labelId(const std::string &name) const {
        for(std::size_t k = 0; k < labels.size(); ++k) {
            if(labels[k] == name) {
                return static_cast<std::int32_t>(k);
            }
        }
        return -1;
    }

    /**
     * @brief Resolves a label id to its string; returns "" for -1 / out-of-range.
     * @param id The label id to resolve (index into `labels`).
     * @return A reference to the label string, or a reference to an empty string for -1 / out-of-range.
     */
    const std::string &labelName(std::int32_t id) const {
        static const std::string empty;
        if(id < 0 || static_cast<std::size_t>(id) >= labels.size()) {
            return empty;
        }
        return labels[static_cast<std::size_t>(id)];
    }

    /**
     * @brief Resolves a label string to every group it tags, across all channels (one-to-many).
     * @param name The label string to look up.
     * @return A vector of GroupRef for every group tagged with this label; empty if the label is not interned.
     */
    std::vector<GroupRef> groupsForLabel(const std::string &name) const {
        std::vector<GroupRef> out;
        const std::int32_t id = labelId(name);
        if(id < 0) {
            return out;
        }
        collectGroups(d, ChannelTag::Double, id, out);
        collectGroups(f, ChannelTag::Float, id, out);
        collectGroups(i, ChannelTag::Int32, id, out);
        collectGroups(b, ChannelTag::Bool, id, out);
        return out;
    }

private:
    /**
     * @brief Appends a GroupRef for every group in one channel that carries the given label id.
     * @tparam T The channel's value type.
     * @param ch The channel to scan.
     * @param tag The channel tag recorded in each emitted GroupRef.
     * @param id The label id to match against each group's label_id.
     * @param out The output vector that matching GroupRef entries are appended to.
     */
    template <typename T>
    static void
    collectGroups(const ChannelLayout<T> &ch, ChannelTag tag, std::int32_t id, std::vector<GroupRef> &out) {
        for(std::size_t gi = 0; gi < ch.groups.size(); ++gi) {
            if(ch.groups[gi].label_id == id) {
                out.push_back(GroupRef{.channel=tag, .index=gi});
            }
        }
    }

    /***************************************************************************/
    // Content-id hashing. A two-lane 64-bit FNV-1a (the two lanes use different basis values and an
    // extra rotation on the second lane, so they decorrelate into an effective 128-bit hash). It is a
    // fast, non-cryptographic hash; correctness against the rare collision is provided by sameStructure().

    /** @brief The incremental two-lane hasher over the structural bytes of a layout. */
    struct Hasher128 {
        std::uint64_t h1 = 14695981039346656037ULL;                   ///< FNV-1a offset basis (lane 1)
        std::uint64_t h2 = 14695981039346656037ULL ^ 0x9E3779B97F4A7C15ULL; ///< distinct basis (lane 2)
        static constexpr std::uint64_t prime = 1099511628211ULL;     ///< the 64-bit FNV prime

        /** @brief Folds one byte into both lanes. @param byte The byte to absorb. */
        void absorb(std::uint8_t byte) {
            h1 = (h1 ^ byte) * prime;
            h2 = (h2 ^ byte) * prime;
            h2 = (h2 << 13) | (h2 >> 51); // rotate lane 2 so the two lanes diverge
        }
        /** @brief Folds a trivially-copyable value's object representation into the hash.
         *  @tparam T The value type. @param v The value whose bytes are absorbed. */
        template <typename T>
        void value(T v) {
            static_assert(std::is_trivially_copyable_v<T>, "value() hashes the object representation");
            const auto *p = reinterpret_cast<const unsigned char *>(&v);
            for(std::size_t k = 0; k < sizeof(T); ++k) {
                absorb(p[k]);
            }
        }
        /** @brief Folds a string (length-prefixed, so "ab"+"c" ≠ "a"+"bc").
         *  @param s The string to absorb. */
        void string(const std::string &s) {
            value<std::uint64_t>(s.size());
            for(char const c : s) {
                absorb(static_cast<std::uint8_t>(c));
            }
        }
        /** @brief The accumulated 128-bit id. @return The two-lane hash as a LayoutId. */
        [[nodiscard]] LayoutId id() const { return LayoutId{.hi=h1, .lo=h2}; }
    };

    /** @brief Folds one channel's full structural content into the hasher (length-prefixed throughout,
     *  so array boundaries are unambiguous). @tparam T The channel's value type. @param h The hasher.
     *  @param c The channel to absorb. */
    template <typename T>
    static void hashChannel(Hasher128 &h, const ChannelLayout<T> &c) {
        auto fp = [&](const std::vector<T> &v) {
            h.value<std::uint64_t>(v.size());
            for(const T &x : v) {
                h.value<T>(x);
            }
        };
        fp(c.lower);
        fp(c.upper);
        fp(c.init_lower);
        fp(c.init_upper);
        h.value<std::uint64_t>(c.fold.size());
        for(std::uint8_t const fl : c.fold) {
            h.value<std::uint8_t>(fl);
        }
        h.value<std::uint64_t>(c.active.size());
        for(std::uint8_t const a : c.active) {
            h.value<std::uint8_t>(a);
        }
        h.value<std::uint64_t>(c.groups.size());
        for(const GroupStructure<T> &g : c.groups) {
            h.value<std::uint32_t>(g.start);
            h.value<std::uint32_t>(g.len);
            h.value<std::int32_t>(g.label_id);
            h.value<std::uint8_t>(g.active ? std::uint8_t{1} : std::uint8_t{0});
        }
    }

    /** @brief Hashes the bool channel. vector<bool> is a bit-packed proxy container, so its values must
     *  be absorbed by value (a plain bool), not by object representation. @param h The hasher.
     *  @param c The bool channel to absorb. */
    static void hashChannel(Hasher128 &h, const ChannelLayout<bool> &c) {
        auto bv = [&](const std::vector<bool> &v) {
            h.value<std::uint64_t>(v.size());
            for(bool const x : v) {
                h.value<std::uint8_t>(x ? std::uint8_t{1} : std::uint8_t{0});
            }
        };
        bv(c.lower);
        bv(c.upper);
        bv(c.init_lower);
        bv(c.init_upper);
        h.value<std::uint64_t>(c.fold.size());
        for(std::uint8_t const fl : c.fold) {
            h.value<std::uint8_t>(fl);
        }
        h.value<std::uint64_t>(c.active.size());
        for(std::uint8_t const a : c.active) {
            h.value<std::uint8_t>(a);
        }
        h.value<std::uint64_t>(c.groups.size());
        for(const GroupStructure<bool> &g : c.groups) {
            h.value<std::uint32_t>(g.start);
            h.value<std::uint32_t>(g.len);
            h.value<std::int32_t>(g.label_id);
            h.value<std::uint8_t>(g.active ? std::uint8_t{1} : std::uint8_t{0});
        }
    }

    /** @brief Computes the 128-bit content id over all four channels and the label table.
     *  @return The freshly computed content id. */
    LayoutId computeLayoutId() const {
        Hasher128 h;
        hashChannel(h, d);
        hashChannel(h, f);
        hashChannel(h, i);
        hashChannel(h, b);
        h.value<std::uint64_t>(labels.size());
        for(const std::string &s : labels) {
            h.string(s);
        }
        return h.id();
    }

    /** @brief Exact structural equality of one channel (every per-value array + the group vector).
     *  @tparam T The channel's value type. @param a The first channel. @param bb The second channel.
     *  @return true iff the two channels are structurally identical. */
    template <typename T>
    static bool sameChannel(const ChannelLayout<T> &a, const ChannelLayout<T> &bb) {
        return a.lower == bb.lower && a.upper == bb.upper && a.init_lower == bb.init_lower &&
               a.init_upper == bb.init_upper && a.fold == bb.fold && a.active == bb.active &&
               a.groups == bb.groups;
    }

    /***************************************************************************/
    // The lazily-computed, cached content id. mutable because layoutId() is logically const on the
    // immutable layout; the load-once cell makes the first concurrent computation thread-safe and every
    // later read lock-free. A copy/move-CONSTRUCTED layout gets a fresh, cold cell and recomputes its id
    // from the copied structure on first use; an assigned-to or deserialized-into layout has its cell
    // invalidated explicitly (invalidateId()), because it may already have answered for its old structure.
    mutable Gem::Common::Concurrency::GLoadOnceCellT<LayoutId> id_cell_;
};

/******************************************************************************/

} /* namespace Gem::Geneva::Genome */

/******************************************************************************/
/** @brief std::hash specialization so a LayoutId can key an unordered_map (the transport registry). */
template <>
struct std::hash<Gem::Geneva::Genome::LayoutId> {
    /** @brief Hashes a LayoutId by mixing its two halves.
     *  @param id The id to hash. @return A size_t hash of the id. */
    std::size_t operator()(const Gem::Geneva::Genome::LayoutId &id) const noexcept {
        // The id is already a strong hash; xor-with-shift mixes the two halves into a size_t.
        return static_cast<std::size_t>(id.hi ^ (id.lo + 0x9E3779B97F4A7C15ULL + (id.hi << 6) + (id.hi >> 2)));
    }
};

/******************************************************************************/
