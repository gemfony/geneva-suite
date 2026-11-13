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
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

// Boost header files go here
#include <boost/json.hpp>

// Geneva headers go here
#include "common/GReflectiveInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "courtier/GWireProtocolT.hpp"           // the wire-protocol seam (befriended below)
#include "courtier/GWireSerializationContext.hpp" // layout send-once: the wire (de)serialization context
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GAdaptionKernels.hpp"
#include "geneva/genome/GGenomeLayout.hpp"
#include "geneva/genome/GGenomeLayoutSerialization.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include <random>
#include <ranges>
#include <utility>
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GSerializableFunctionObjectT.hpp"
#include "common/GSerializationHelperFunctionsT.hpp" // serialization of std::chrono time_point (GProcessable timing)
#include "courtier/GProcessable.hpp" // the non-generic processing-lifecycle base
#include "geneva/genome/GMultiConstraintT.hpp" // GPreEvaluationValidityCheckT (registered on the shared policy)
#include "geneva/Interface/GRateableI.hpp"
#include "geneva/genome/GAuxiliaryStore.hpp" // the OA-owned scratch (personality + per-group adaption PODs)
#include "geneva/genome/GIndividualProcessingResult.hpp"
#include "geneva/genome/GProblemPolicy.hpp"
#include "hap/GRandomT.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The flat-genome representation layer of the individual hierarchy and the non-template concrete cast /
 * signature target the optimization algorithms operate on.
 *
 * GGenome derives the genome-agnostic GGenome and adds the parameter STORAGE: four
 * contiguous, type-homogeneous value arrays (double / float / int32 / bool) plus a handle to a shared,
 * immutable GGenomeLayout that describes their bounds, grouping and adaption configuration. The
 * per-individual, per-group adaption state (Gauss sigma, ...) lives in the OA-owned slot scratch, not in
 * the genome. Because all genome state is generic, a concrete individual adds no extra members and only
 * supplies a constructor (building its genome with GGenomeBuilder + setGenome()) and evaluate();
 * the clone/load/compare/serialize machinery is provided here and reused unchanged via GGenomeT.
 *
 * Value access is genome-agnostic: GGenome implements the per-type value-channel virtuals declared on
 * GGenome (streamline_ / assignValueVector_ / countParametersX_ / boundaries_ and the FP /
 * internal views), so the algorithms read and write parameter values through the base's templated surface
 * without a downcast. A floating-point parameter is stored in the NORMALIZED INTERNAL coordinate (magnitude
 * ~ 1, confined to [-0.5, 0.5) for a bounded parameter); the user-facing EXTERNAL value is its affine
 * image (normalized-genome architecture, §2). This realizes the two-reader split: the OAs read/write the
 * raw internal value (streamlineInternal_/assignValueVectorInternal_), while the objective function, GPU
 * marshaller and user inspection read the scaled external value. The fold lives at WRITE time, not read
 * time. Integer parameters are not normalized (§2.7): they keep the closed-range integer fold on read.
 *
 * Feasibility is a concrete operation on the base (GGenome::fulfillsConstraints), reading this
 * genome's values through the shared GProblemPolicy's constraint -- the genome adds no feasibility hook.
 */
class GGenome // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Courtier::GProcessable
  , public Gem::Common::GReflectiveInterfaceBaseT<GGenome, Gem::Common::GCommonInterfaceT<GGenome>>
  , public Interface::GRateableI {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;
    friend struct Gem::Common::GReflectiveInterfaceAccess;
    /// The wire-protocol seam (GGenomeWireProtocol.hpp) writes a returned evaluation onto a live
    /// population element, which is exactly what the protected result/validity setters exist for.
    friend struct Gem::Courtier::GWireProtocolT<GGenome>;

    /** @brief Single declaration of ALL this class's data, feeding the GReflectiveInterfaceBaseT-generated
     *  name_()/load_()/compare_() as well as save()/load() below.
     *
     *  - the GProcessable lifecycle base (a stateful non-container base) rides make_base_object_member<>;
     *  - the four plain veto/feasibility members are ordinary make_member (serialized + loaded + compared);
     *  - the cloneable pre-/post-processors, the shared 1:N policy and the result store are state but not
     *    per-individual identity, so they use the cmp_skip factories (serialized + loaded, not compared);
     *  - the OA scratch is copy-loaded and not compared, and its wire form (serialized on a checkpoint,
     *    omitted on the wire) rides make_wire_omitted_ptr_member;
     *  - the four value channels are plain members;
     *  - the shared structural layout_ is a load-only member (make_load_only_member): plain-assigned on
     *    load (it is SHARED, not value-copied) and excluded from compare_() (it is problem metadata, not
     *    per-individual identity) and from the folded serialize() -- save()/load() below emit it through
     *    the bespoke send-once wire protocol.
     *  @tparam Self The (const or non-const) deduced type of *this
     *  @param self A reference to *this whose members are tied into the tuple
     *  @return A tuple of named member references */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_base_object_member<Gem::Courtier::GProcessable>("GProcessable", self),
            Gem::Common::make_member("pre_processing_disabled_", self.pre_processing_disabled_),
            Gem::Common::make_member("post_processing_disabled_", self.post_processing_disabled_),
            Gem::Common::make_member("assigned_iteration_", self.assigned_iteration_),
            Gem::Common::make_member("validity_level_", self.validity_level_),
            Gem::Common::make_uncompared_cloneable_member("pre_processor_ptr_", self.pre_processor_ptr_),
            Gem::Common::make_uncompared_cloneable_member("post_processor_ptr_", self.post_processor_ptr_),
            Gem::Common::make_uncompared_member("policy_", self.policy_),
            Gem::Common::make_uncompared_member("stored_results_cnt_", self.stored_results_cnt_),
            Gem::Courtier::make_wire_omitted_ptr_member("scratch_", self.scratch_),
            Gem::Common::make_member("dv_", self.dv_),
            Gem::Common::make_member("fv_", self.fv_),
            Gem::Common::make_member("iv_", self.iv_),
            Gem::Common::make_member("bv_", self.bv_),
            Gem::Common::make_load_only_member("layout_", self.layout_)
        );
    }

    /** @brief Post-load hook (invoked by the GReflectiveInterfaceBaseT-generated load_() after the members are
     *  loaded): re-key the parameter-count cache to the just-shared layout, exactly as the former
     *  hand-written load_()'s setLayout() call did. The cache also re-keys lazily on the next access, so
     *  this is the eager, explicit form of that invalidation.
     *  @see setLayout(), cachedCount() */
    void postLoad_() { counts_layout_sp_.reset(); }

    /**
     * @brief Serialises the genome: every member plus the shared structural layout, the latter either by
     * value (self-contained form) or by content id (transport send-once form), as selected by the active
     * wire-serialisation scope. See the body for the two forms.
     * @tparam Archive The GArchive codec type
     * @param ar The archive to write the genome into
     * @param version The (unused) serialization version number
     */
    template <typename Archive>
    void save(Archive &ar, [[maybe_unused]] const unsigned int version) const {
        using Gem::Common::archive_named;
        Gem::Common::serialize_members(ar, localMembers_());

        const auto *ctx = Gem::Courtier::GWireSerializationScope::current();

        // The layout is shared & immutable in memory; sharing does not survive serialisation. Two wire
        // forms (see GGenome's historical note): SELF-CONTAINED (full layout by value, the only form
        // with no active scope -- checkpoint / file) and SEND-ONCE (referenced by content id, shipped to a
        // peer only the first time the id is seen). A leading `layout_interned` tag is self-describing.
        bool interned = (ctx != nullptr) && ctx->enabled && ctx->may_intern_blobs &&
                        (ctx->registry != nullptr) && (layout_ != nullptr);
        archive_named(ar, "layout_interned", interned);
        if(not interned) {
            GGenomeLayout layout_copy = layout_ ? *layout_ : GGenomeLayout{};
            archive_named(ar, "layout_", layout_copy);
            return;
        }

        const LayoutId lid = layout_->layoutId();
        Gem::Courtier::GWireBlobId wid{lid.hi, lid.lo};
        archive_named(ar, "blob_id_hi", wid[0]);
        archive_named(ar, "blob_id_lo", wid[1]);
        if(not ctx->registry->has(wid)) {
            ctx->registry->put(wid, layoutToWireBlob(*layout_));
        }
        bool layout_present = not ctx->registry->peerHasBlob(ctx->peer, wid);
        archive_named(ar, "layout_present", layout_present);
        if(layout_present) {
            GGenomeLayout layout_copy = *layout_;
            archive_named(ar, "layout_", layout_copy);
            ctx->registry->markPeerHasBlob(ctx->peer, wid);
        }
    }

    /**
     * @brief Restores the genome from an archive: the value channels plus a freshly-owned layout, read
     * either by value or resolved from a content id against the wire registry (with a cache-miss fetch),
     * following the self-describing tag written by save().
     * @tparam Archive The GArchive codec type
     * @param ar The archive to read the genome from
     * @param version The (unused) serialization version number
     */
    template <typename Archive>
    // NOLINTNEXTLINE(readability-function-size) -- one coherent serialization sweep: the self-describing wire-format tag dispatch (by-value / interned-by-id, with cache-miss fetch) must stay in lockstep with save()'s tag order; splitting would scatter tightly coupled archive-decode branches
    void load(Archive &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        Gem::Common::serialize_members(ar, localMembers_());

        bool interned = false;
        archive_named(ar, "layout_interned", interned);
        if(not interned) {
            auto fresh = std::make_shared<GGenomeLayout>();
            archive_named(ar, "layout_", *fresh);
            this->setLayout(fresh);
            return;
        }

        Gem::Courtier::GWireBlobId wid{};
        archive_named(ar, "blob_id_hi", wid[0]);
        archive_named(ar, "blob_id_lo", wid[1]);
        bool layout_present = false;
        archive_named(ar, "layout_present", layout_present);

        const auto *ctx = Gem::Courtier::GWireSerializationScope::current();
        if(layout_present) {
            auto fresh = std::make_shared<GGenomeLayout>();
            archive_named(ar, "layout_", *fresh);
            this->setLayout(fresh);
            if(ctx != nullptr && ctx->registry != nullptr && not ctx->registry->has(wid)) {
                ctx->registry->put(wid, layoutToWireBlob(*fresh));
            }
            return;
        }

        std::string blob;
        bool have = (ctx != nullptr) && (ctx->registry != nullptr) && ctx->registry->tryGet(wid, blob);
        std::string fetch_error;
        if(not have && ctx != nullptr && ctx->fetch_blob) {
            // The fetch resolves a cache miss over the network; on success it yields a non-empty blob, on
            // failure a std::unexpected carrying the reason (surfaced in the throw below).
            if(auto fetched = ctx->fetch_blob(wid); fetched.has_value()) {
                blob = std::move(fetched.value());
                if(ctx->registry != nullptr) {
                    ctx->registry->put(wid, blob);
                }
                have = true;
            }
            else {
                fetch_error = std::move(fetched.error());
            }
        }
        if(not have || blob.empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGenome::load(): Error!" << '\n'
                << "An id-only layout reference could not be resolved (cache miss with no usable fetch)."
                << '\n'
                << (fetch_error.empty() ? std::string{} : ("Fetch failure: " + fetch_error + '\n'))
            );
        }
        this->setLayout(layoutFromWireBlob(blob));
    }

    /**
     * @brief The single (de)serialization entry point, split by direction on the GArchive codec's
     * compile-time direction, routing to save()/load().
     * @tparam Archive The GArchive codec type
     * @param ar The archive to read from / write to
     * @param version The serialization format version, forwarded to the split
     */
    template <typename Archive>
    void serialize(Archive &ar, const unsigned int version) {
        if constexpr (Archive::is_saving) {
            save(ar, version);
        } else {
            load(ar, version);
        }
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GGenome";

    /** @brief The default constructor */
    GGenome();
    /** @brief Initialization with the number of fitness criteria.
     *  @param n_fitness_criteria The number of fitness criteria this genome will report */
    explicit GGenome(std::size_t n_fitness_criteria);
    /** @brief The copy constructor.
     *  @param cp The genome to copy from (value channels and shared layout handle) */
    GGenome(GGenome const &cp);
    /** @brief The destructor */
    ~GGenome() override = default;

    /** @brief Un-hide the inherited public load(shared_ptr/unique_ptr/ref) overloads, which the boost
     *  split-member load(Archive&, unsigned) below would otherwise hide by name. */
    using Gem::Common::GCommonInterfaceT<GGenome>::load;

    /** @brief Installs the value arrays + shared layout produced by a GGenomeBuilder.
     *  @param g The genome data (the four value channels plus the shared immutable layout) */
    void setGenome(GenomeData const &g);

    /** @brief Direct, shared access to the structural layout (problem metadata).
     *  @return A shared handle to the immutable layout describing bounds, grouping and adaption config */
    std::shared_ptr<const GGenomeLayout> getLayout() const noexcept { return layout_; }

    /***************************************************************************/
    // Access to the raw INTERNAL (normalized) value arrays (the OA-owned adaption kernels write here).
    // One accessor per channel: the explicit object parameter lets constness flow from the caller --
    // a mutable genome yields std::span<T>, a const genome std::span<const T> (span CTAD over the vector).

    /** @brief @return A span over the raw internal double channel (const iff *this is const) */
    template <typename Self>
    auto internalDoubleValues(this Self &&self) noexcept { return std::span{self.dv_}; }
    /** @brief @return A span over the raw internal float channel (const iff *this is const) */
    template <typename Self>
    auto internalFloatValues(this Self &&self) noexcept { return std::span{self.fv_}; }
    /** @brief @return A span over the raw internal int32 channel (const iff *this is const) */
    template <typename Self>
    auto internalInt32Values(this Self &&self) noexcept { return std::span{self.iv_}; }
    /** @brief @return A span over the raw internal bool channel, bytes 1/0 (const iff *this is const) */
    template <typename Self>
    auto internalBoolValues(this Self &&self) noexcept { return std::span{self.bv_}; }

    /***************************************************************************/
    // Bulk-flatten fast path (the GPU marshallers) -- see GGenome's historical notes.

    /** @brief Bulk-flatten the double channel directly into a caller buffer (external/scaled values).
     *  @param dst The destination buffer (must be large enough)
     *  @param am The activity mode selecting which parameters are written
     *  @return The number of values written into dst */
    std::size_t streamlineInto(double *dst, activityMode const &am = activityMode::DEFAULTACTIVITYMODE) const {
        return streamlineIntoImpl<double>(dst, layout_->d, dv_, am);
    }
    /** @brief Bulk-flatten the float channel (see the double overload).
     *  @param dst The destination buffer. @param am The activity mode. @return The number written */
    std::size_t streamlineInto(float *dst, activityMode const &am = activityMode::DEFAULTACTIVITYMODE) const {
        return streamlineIntoImpl<float>(dst, layout_->f, fv_, am);
    }
    /** @brief Bulk-flatten the int32 channel (see the double overload).
     *  @param dst The destination buffer. @param am The activity mode. @return The number written */
    std::size_t streamlineInto(std::int32_t *dst, activityMode const &am = activityMode::DEFAULTACTIVITYMODE) const {
        return streamlineIntoImpl<std::int32_t>(dst, layout_->i, iv_, am);
    }

    /** @brief Adaption-time write-fold (§2.2): folds every bounded value back into range (FP + int32). */
    void foldConstrainedValuesInPlace() {
        foldChannelInPlace<double>(layout_->d, dv_);
        foldChannelInPlace<float>(layout_->f, fv_);
        foldChannelInPlace<std::int32_t>(layout_->i, iv_);
    }

    /***************************************************************************/
    // Algorithm-facing genome operations (the flat-genome overrides of the GGenome virtuals).

    /** @brief Transformation of the individual's parameters into a JSON object.
     *  @return A boost::json::object holding this individual's parameters, metadata and results */
    virtual boost::json::object toJSON() const;

    /** @brief Transformation of the individual's parameters into a list of comma-separated values.
     *  @param with_name_and_type Whether to prefix each value with its name and type
     *  @param with_commas Whether to separate the values with commas
     *  @param use_raw_fitness Whether to emit the raw rather than the transformed fitness
     *  @param show_validity Whether to append the individual's validity flag
     *  @return The parameters (and optionally fitness/validity) as a single CSV string */
    virtual std::string toCSV(
        bool with_name_and_type = false,
        bool with_commas = true,
        bool use_raw_fitness = true,
        bool show_validity = true
    ) const;

    /** @brief Perform a cross-over operation between this genome and another (a GGenome).
     *  @param cp_base The other entity to cross over with (must be a GGenome)
     *  @return A new genome holding the recombined parameter values */
    virtual std::shared_ptr<GGenome> crossOverWith(GGenome const &cp_base) const;

    /** @brief Retrieves parameters relevant for the evaluation from another GGenome.
     *  @param cp_base The entity whose evaluation-relevant parameters are absorbed into this one */
    virtual void cannibalize(GGenome &cp_base);

    /** @brief Absorbs the parameter values and the shared structural layout of @p src into this genome,
     *  in place (no relocation), leaving this genome's results, lifecycle and OA scratch alone.
     *
     *  The genome-half counterpart of absorbResultsFrom(): a worker that MODIFIED its copy returns the
     *  whole individual, and the server writes those parameters into the population element it already
     *  holds rather than swapping the element out (its address is load-bearing).
     *  @param src The returned genome supplying the parameter values + layout */
    void absorbGenomeFrom(const GGenome &src) {
        dv_ = src.dv_;
        fv_ = src.fv_;
        iv_ = src.iv_;
        bv_ = src.bv_;
        // The LAYOUT is deliberately NOT taken: it is immutable problem metadata owned by the problem
        // definition and SHARED by the whole population, so a worker cannot have changed it -- and
        // installing the returned copy would silently de-share it for this one element (costing both
        // memory and the send-once id sharing that the shared handle buys).
    }

    /***************************************************************************/
    // Deleted functions

    explicit GGenome(float const &) = delete;  ///< Intentionally undefined
    explicit GGenome(double const &) = delete; ///< Intentionally undefined

protected:
    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceBaseT base from
    // localMembers_() (the value channels compare/load normally; layout_ is a load-only member: shared
    // on load, excluded from compare_()). The post-load count-cache re-key is done by postLoad_() above.
    // serialize() is NOT generated -- save()/load() above keep the bespoke send-once layout wire
    // protocol.

    /** @brief Random initialization of the genome's parameter values.
     *  @param am The activity mode selecting which parameters are randomly initialised
     *  @return true if at least one parameter value was changed */
    virtual bool randomInit_(activityMode const &am);

    /** @brief Applies modifications to this object. This is needed for testing purposes */
    bool modify_GUnitTests_();
    /** @brief Performs self tests that are expected to succeed. This is needed for testing purposes */
    void specificTestsNoFailureExpected_GUnitTests_();
private:

    /** @brief Installs a new shared layout and invalidates the parameter-count cache.
     *  @param layout The new shared, immutable layout (the count cache is re-keyed to it) */
    void setLayout(std::shared_ptr<const GGenomeLayout> layout) {
        layout_ = std::move(layout);
        // Re-key the count cache. Holding a copy of the shared_ptr keeps the layout alive while cached, so
        // a later pointer comparison can never alias a freed-then-reallocated layout (ABA-safe).
        counts_layout_sp_.reset();
    }

    /** @brief Maps an activity mode to a cache column (0 == ALL, 1 == ACTIVEONLY, 2 == INACTIVEONLY).
     *  @param am The activity mode. @return The cache column index */
    static std::size_t amIndex(activityMode const &am) {
        switch(am) {
        case activityMode::ACTIVEONLY: return 1;
        case activityMode::INACTIVEONLY: return 2;
        case activityMode::ALLPARAMETERS: // == DEFAULTACTIVITYMODE
        default: return 0;
        }
    }

    /** @brief Returns a cached parameter count, recomputing the whole cache if the layout has changed.
     *  The cache is keyed on the SHARED layout (a held shared_ptr), so a structural change (a new layout
     *  object) re-keys it and the layout cannot be freed-and-reused underneath us while cached.
     *  @param type_idx The channel index (0 double, 1 float, 2 int32, 3 bool)
     *  @param am The activity mode. @return The number of matching parameters */
    std::size_t cachedCount(std::size_t type_idx, activityMode const &am) const {
        if(counts_layout_sp_.get() != layout_.get()) {
            // Recompute all 4 channels x 3 activity modes for the current layout.
            for(std::size_t a = 0; a < 3; ++a) {
                const activityMode m = (a == 1) ? activityMode::ACTIVEONLY
                                     : (a == 2) ? activityMode::INACTIVEONLY
                                                : activityMode::ALLPARAMETERS;
                counts_cache_[0][a] = countImpl<double>(layout_->d, m);
                counts_cache_[1][a] = countImpl<float>(layout_->f, m);
                counts_cache_[2][a] = countImpl<std::int32_t>(layout_->i, m);
                counts_cache_[3][a] = countImpl<bool>(layout_->b, m);
            }
            counts_layout_sp_ = layout_; // re-key (also pins the layout while cached)
        }
        return counts_cache_[type_idx][amIndex(am)];
    }

    /** @brief Retrieve the active double parameter at the given positional index.
     *  @param idx The position among the active double parameters. @return Its (folded) value */
    virtual double getVarVal_d_(std::size_t idx);
    /** @brief Retrieve the active float parameter at the given positional index.
     *  @param idx The position among the active float parameters. @return Its (folded) value */
    virtual float getVarVal_f_(std::size_t idx);
    /** @brief Retrieve the active int32 parameter at the given positional index.
     *  @param idx The position among the active int32 parameters. @return Its (folded) value */
    virtual std::int32_t getVarVal_i_(std::size_t idx);
    /** @brief Retrieve the active bool parameter at the given positional index.
     *  @param idx The position among the active bool parameters. @return Its value */
    virtual bool getVarVal_b_(std::size_t idx);

    /** @brief Retrieval of a suitable position for cross over inside of a vector.
     *  @param lower The minimum allowed cross-over position (must be > 0)
     *  @param upper The exclusive upper bound of the cross-over range
     *  @return A valid cross-over position within the vector */
    std::size_t getCrossOverPos(std::size_t lower, std::size_t upper);

    /***************************************************************************/
    // Activity / fold helpers (genome-agnostic value mapping).

    /** @brief Whether a value with the given active flag matches the requested activity mode.
     *  @param active The per-parameter active flag (non-zero == active)
     *  @param am The requested activity mode
     *  @return true if the parameter should be included under the given activity mode */
    static bool amMatch(std::uint8_t active, activityMode const &am) {
        switch(am) {
        case activityMode::ACTIVEONLY: return active != 0;
        case activityMode::INACTIVEONLY: return active == 0;
        case activityMode::ALLPARAMETERS: // == DEFAULTACTIVITYMODE
        default: return true;
        }
    }

    /** @brief THE single source of truth for the per-element internal->external map (§2.1).
     *  @tparam T The channel's value type (double / float / int32)
     *  @param ch The channel layout. @param stored The internally stored value. @param k The index
     *  @return The external (user-coordinate) representation of the stored value */
    template <typename T>
    static T externalValue(ChannelLayout<T> const &ch, T stored, std::size_t k) {
        if constexpr(std::is_floating_point_v<T>) {
            const T scale = ngScale<T>(ch, k);
            const T anchor = ngAnchor<T>(ch, k);
            if(not ch.fold[k]) {
                return ngInternalToExternal<T>(stored, scale, anchor);
            }
#ifdef DEBUG
            if(scale > T(0) && not(stored >= T(-0.5) && stored < T(0.5))) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GGenome::externalValue(): Error!" << '\n'
                    << "Internal coordinate " << stored << " of bounded parameter " << k
                    << " is outside the canonical interval [-0.5, 0.5) -- a write path failed to fold." << '\n'
                );
            }
#endif /* DEBUG */
            return ngClampHalfOpen<T>(
                ngInternalToExternal<T>(stored, scale, anchor), ch.lower[k], ch.upper[k]
            );
        }
        else {
            if(not ch.fold[k]) {
                return stored;
            }
            return foldConstrainedInt<T>(stored, ch.lower[k], ch.upper[k]);
        }
    }

    /** @brief The per-element EXTERNAL->INTERNAL map for a floating-point external write (§2.2).
     *  @tparam T The floating-point channel's value type (double / float)
     *  @param ch The channel layout. @param x The external value. @param k The index
     *  @param allow_upper_bound Whether a value exactly on the (inclusive) upper bound is accepted
     *  @return The normalized internal coordinate to store */
    template <typename T>
    static T externalToInternalChecked(
        ChannelLayout<T> const &ch, T x, std::size_t k, bool allow_upper_bound = false
    ) {
        static_assert(std::is_floating_point_v<T>, "FP-only external write");
        const T scale = ngScale<T>(ch, k);
        const T anchor = ngAnchor<T>(ch, k);
        if(ch.fold[k]) {
            if(scale <= T(0)) {
                return T(0);
            }
            const bool below = x < ch.lower[k];
            const bool above = allow_upper_bound ? (x > ch.upper[k]) : not(x < ch.upper[k]);
            if(below || above) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GGenome::externalToInternalChecked(): Error!" << '\n'
                    << "External value " << x << " is outside the "
                    << (allow_upper_bound ? "range [" : "half-open range [")
                    << ch.lower[k] << ", " << ch.upper[k] << (allow_upper_bound ? "]" : ")")
                    << " of bounded parameter " << k << '\n'
                );
            }
            return ngClampHalfOpen<T>(ngExternalToInternal<T>(x, scale, anchor), T(-0.5), T(0.5));
        }
        return ngExternalToInternal<T>(x, scale, anchor);
    }

    /***************************************************************************/
    // Per-type channel helpers -- the templated bodies the public surface forwards to.

    /** @brief Collects a channel's active, range-folded values into an output vector.
     *  @tparam T The channel's value type. @param out The output vector (cleared then filled)
     *  @param ch The channel layout. @param store The internal value storage. @param am The activity mode */
    template <typename T>
    void streamlineImpl(
        std::vector<T> &out, ChannelLayout<T> const &ch, std::vector<T> const &store, activityMode const &am
    ) const {
        out.clear();
        for(std::size_t k = 0; k < store.size(); ++k) {
            if(amMatch(ch.active[k], am)) {
                out.push_back(externalValue<T>(ch, store[k], k));
            }
        }
    }

    /** @brief Writes the active channel's external values directly into a caller buffer (count returned).
     *  @tparam T The channel's value type. @param dst The destination buffer
     *  @param ch The channel layout. @param store The internal value storage. @param am The activity mode
     *  @return The number of values written into dst */
    template <typename T>
    std::size_t streamlineIntoImpl(
        T *dst, ChannelLayout<T> const &ch, std::vector<T> const &store, activityMode const &am
    ) const {
        std::size_t n = 0;
        for(std::size_t k = 0; k < store.size(); ++k) {
            if(amMatch(ch.active[k], am)) {
                dst[n++] = externalValue<T>(ch, store[k], k);
            }
        }
        return n;
    }

    /** @brief Adaption-time write-fold (§2.2): contains every bounded value in the store.
     *  @tparam T The channel's value type. @param ch The channel layout. @param store The storage (folded) */
    template <typename T>
    static void foldChannelInPlace(ChannelLayout<T> const &ch, std::vector<T> &store) {
        for(std::size_t k = 0; k < store.size(); ++k) {
            if(not ch.fold[k]) {
                continue;
            }
            if constexpr(std::is_floating_point_v<T>) {
                store[k] = ngFoldInternal<T>(store[k]);
            }
            else {
                store[k] = foldConstrainedInt<T>(store[k], ch.lower[k], ch.upper[k]);
            }
        }
    }

    /** @brief Writes a vector of EXTERNAL values back into the active slots of a channel (§2.2).
     *  @tparam T The channel's value type. @param in The incoming external values
     *  @param ch The channel layout. @param store The storage to update. @param am The activity mode */
    template <typename T>
    void assignImpl(
        std::vector<T> const &in, ChannelLayout<T> const &ch, std::vector<T> &store, activityMode const &am
    ) {
        std::size_t pos = 0;
        for(std::size_t k = 0; k < store.size(); ++k) {
            if(amMatch(ch.active[k], am)) {
                const T x = in.at(pos++);
                if constexpr(std::is_floating_point_v<T>) {
                    store[k] = externalToInternalChecked<T>(ch, x, k);
                }
                else {
                    store[k] = x;
                }
            }
        }
    }

    /** @brief Collects a channel's active values in their raw INTERNAL representation (§2.3).
     *  @tparam T The channel's value type. @param out The output vector (cleared then filled)
     *  @param ch The channel layout. @param store The internal value storage. @param am The activity mode */
    template <typename T>
    void streamlineInternalImpl(
        std::vector<T> &out, ChannelLayout<T> const &ch, std::vector<T> const &store, activityMode const &am
    ) const {
        out.clear();
        for(std::size_t k = 0; k < store.size(); ++k) {
            if(amMatch(ch.active[k], am)) {
                out.push_back(store[k]);
            }
        }
    }

    /** @brief Writes a vector of raw INTERNAL values back into the active slots of an FP channel (§2.2).
     *  @tparam T The FP channel's value type. @param in The incoming internal values
     *  @param ch The channel layout. @param store The storage to update. @param am The activity mode */
    template <typename T>
    void assignInternalImpl(
        std::vector<T> const &in, ChannelLayout<T> const &ch, std::vector<T> &store, activityMode const &am
    ) {
        static_assert(std::is_floating_point_v<T>, "the internal write is FP-only");
        std::size_t pos = 0;
        for(std::size_t k = 0; k < store.size(); ++k) {
            if(amMatch(ch.active[k], am)) {
                const T u = in.at(pos++);
                store[k] = (ch.fold[k]) ? ngFoldInternal<T>(u) : u;
            }
        }
    }

    /** @brief Counts how many of a channel's parameters match the requested activity mode.
     *  @tparam T The channel's value type. @param ch The channel layout. @param am The activity mode
     *  @return The number of matching parameters */
    template <typename T>
    std::size_t countImpl(ChannelLayout<T> const &ch, activityMode const &am) const {
        std::size_t n = 0;
        for(std::uint8_t const a : ch.active) {
            if(amMatch(a, am)) {
                ++n;
            }
        }
        return n;
    }

    /** @brief Collects the lower/upper bounds of a channel's active parameters.
     *  @tparam T The channel's value type. @param l Lower bounds out. @param u Upper bounds out
     *  @param ch The channel layout. @param am The activity mode */
    template <typename T>
    void boundariesImpl(
        std::vector<T> &l, std::vector<T> &u, ChannelLayout<T> const &ch, activityMode const &am
    ) const {
        l.clear();
        u.clear();
        for(std::size_t k = 0; k < ch.size(); ++k) {
            if(not amMatch(ch.active[k], am)) {
                continue;
            }
            if(ch.fold[k]) {
                l.push_back(ch.lower[k]);
                u.push_back(ch.upper[k]);
            }
            else {
                l.push_back(std::numeric_limits<T>::lowest());
                u.push_back((std::numeric_limits<T>::max)());
            }
        }
    }

    /***************************************************************************/
    // The bool channel is stored as bytes; these adapt the byte storage to the bool-typed surface.

    /** @brief Collects the active bool parameters from the byte storage into a bool vector.
     *  @param out The output vector (cleared then filled). @param am The activity mode */
    void streamlineBool(std::vector<bool> &out, activityMode const &am) const {
        out.clear();
        for(std::size_t k = 0; k < bv_.size(); ++k) {
            if(amMatch(layout_->b.active[k], am)) {
                out.push_back(bv_[k] != 0);
            }
        }
    }
    /** @brief Writes a bool vector back into the active slots of the byte storage.
     *  @param in The incoming bool values. @param am The activity mode */
    void assignBool(std::vector<bool> const &in, activityMode const &am) {
        std::size_t pos = 0;
        for(std::size_t k = 0; k < bv_.size(); ++k) {
            if(amMatch(layout_->b.active[k], am)) {
                bv_[k] = in.at(pos++) ? static_cast<std::uint8_t>(1) : static_cast<std::uint8_t>(0);
            }
        }
    }
    /** @brief Collects the bounds of the active bool parameters (always [false, true]).
     *  @param l Lower bounds out. @param u Upper bounds out. @param am The activity mode */
    void boundariesBool(std::vector<bool> &l, std::vector<bool> &u, activityMode const &am) const {
        l.clear();
        u.clear();
        for(std::size_t k = 0; k < layout_->b.size(); ++k) {
            if(amMatch(layout_->b.active[k], am)) {
                l.push_back(false);
                u.push_back(true);
            }
        }
    }

    /***************************************************************************/
    // Random-init channel helpers (the per-channel ADAPTION kernels are OA-owned).

    /** @brief Randomly initialises a floating-point channel's active parameters.
     *  @tparam T The channel's value type. @param store The internal storage (overwritten at active positions)
     *  @param ch The channel layout. @param am The activity mode. @param gr The leased random proxy to draw from.
     *  @return true if a value changed */
    template <typename T>
    bool randomInitFP(std::vector<T> &store, ChannelLayout<T> const &ch, activityMode const &am, Gem::Hap::GRandomBase &gr);
    /** @brief Randomly initialises the int32 channel's active parameters.
     *  @param am The activity mode. @param gr The leased random proxy to draw from. @return true if a value changed */
    bool randomInitInt(activityMode const &am, Gem::Hap::GRandomBase &gr);
    /** @brief Randomly initialises the bool channel's active parameters.
     *  @param am The activity mode. @param gr The leased random proxy to draw from. @return true if a value changed */
    bool randomInitBool(activityMode const &am, Gem::Hap::GRandomBase &gr);

    /***************************************************************************/
    // The §2 per-type virtual overrides of the GGenome value-channel dispatch targets (forward
    // to the templated helpers above). The algorithms reach these through the base's streamline<T>() etc.

    /** @brief Streamlines the double channel into v. @param v Output value vector. @param am Activity mode. */
    virtual void streamline_(std::vector<double> &v, activityMode const &am) const { streamlineImpl<double>(v, layout_->d, dv_, am); }
    /** @brief Streamlines the float channel into v. @param v Output value vector. @param am Activity mode. */
    virtual void streamline_(std::vector<float> &v, activityMode const &am) const { streamlineImpl<float>(v, layout_->f, fv_, am); }
    /** @brief Streamlines the int32 channel into v. @param v Output value vector. @param am Activity mode. */
    virtual void streamline_(std::vector<std::int32_t> &v, activityMode const &am) const { streamlineImpl<std::int32_t>(v, layout_->i, iv_, am); }
    /** @brief Streamlines the bool channel into v. @param v Output value vector. @param am Activity mode. */
    virtual void streamline_(std::vector<bool> &v, activityMode const &am) const { streamlineBool(v, am); }

    /** @brief Assigns v into the double channel's active slots. @param v Incoming values. @param am Activity mode. */
    virtual void assignValueVector_(std::vector<double> const &v, activityMode const &am) { assignImpl<double>(v, layout_->d, dv_, am); }
    /** @brief Assigns v into the float channel's active slots. @param v Incoming values. @param am Activity mode. */
    virtual void assignValueVector_(std::vector<float> const &v, activityMode const &am) { assignImpl<float>(v, layout_->f, fv_, am); }
    /** @brief Assigns v into the int32 channel's active slots. @param v Incoming values. @param am Activity mode. */
    virtual void assignValueVector_(std::vector<std::int32_t> const &v, activityMode const &am) { assignImpl<std::int32_t>(v, layout_->i, iv_, am); }
    /** @brief Assigns v into the bool channel's active slots. @param v Incoming values. @param am Activity mode. */
    virtual void assignValueVector_(std::vector<bool> const &v, activityMode const &am) { assignBool(v, am); }

    /** @brief @param am Activity mode. @return The number of matching double parameters (cached). */
    virtual std::size_t countParametersDouble_(activityMode const &am) const { return cachedCount(0, am); }
    /** @brief @param am Activity mode. @return The number of matching float parameters (cached). */
    virtual std::size_t countParametersFloat_(activityMode const &am) const { return cachedCount(1, am); }
    /** @brief @param am Activity mode. @return The number of matching int32 parameters (cached). */
    virtual std::size_t countParametersInt32_(activityMode const &am) const { return cachedCount(2, am); }
    /** @brief @param am Activity mode. @return The number of matching bool parameters (cached). */
    virtual std::size_t countParametersBool_(activityMode const &am) const { return cachedCount(3, am); }

    /** @brief Collects double-channel bounds. @param l Lower bounds out. @param u Upper bounds out. @param am Activity mode. */
    virtual void boundaries_(std::vector<double> &l, std::vector<double> &u, activityMode const &am) const { boundariesImpl<double>(l, u, layout_->d, am); }
    /** @brief Collects float-channel bounds. @param l Lower bounds out. @param u Upper bounds out. @param am Activity mode. */
    virtual void boundaries_(std::vector<float> &l, std::vector<float> &u, activityMode const &am) const { boundariesImpl<float>(l, u, layout_->f, am); }
    /** @brief Collects int32-channel bounds. @param l Lower bounds out. @param u Upper bounds out. @param am Activity mode. */
    virtual void boundaries_(std::vector<std::int32_t> &l, std::vector<std::int32_t> &u, activityMode const &am) const { boundariesImpl<std::int32_t>(l, u, layout_->i, am); }
    /** @brief Collects bool-channel bounds. @param l Lower bounds out. @param u Upper bounds out. @param am Activity mode. */
    virtual void boundaries_(std::vector<bool> &l, std::vector<bool> &u, activityMode const &am) const { boundariesBool(l, u, am); }

    /** @brief Reads the raw internal double channel into v. @param v Output vector. @param am Activity mode. */
    virtual void streamlineInternal_(std::vector<double> &v, activityMode const &am) const { streamlineInternalImpl<double>(v, layout_->d, dv_, am); }
    /** @brief Reads the raw internal float channel into v. @param v Output vector. @param am Activity mode. */
    virtual void streamlineInternal_(std::vector<float> &v, activityMode const &am) const { streamlineInternalImpl<float>(v, layout_->f, fv_, am); }

    /** @brief Internal-writes (folds) v into the double channel's active slots. @param v Internal values. @param am Activity mode. */
    virtual void assignValueVectorInternal_(std::vector<double> const &v, activityMode const &am) { assignInternalImpl<double>(v, layout_->d, dv_, am); }
    /** @brief Internal-writes (folds) v into the float channel's active slots. @param v Internal values. @param am Activity mode. */
    virtual void assignValueVectorInternal_(std::vector<float> const &v, activityMode const &am) { assignInternalImpl<float>(v, layout_->f, fv_, am); }

    /***************************************************************************/
    // Data: the four contiguous value channels + the shared structural layout.

    std::vector<double> dv_;        ///< the double channel values (internal representation)
    std::vector<float> fv_;         ///< the float channel values (internal representation)
    std::vector<std::int32_t> iv_;  ///< the int32 channel values (internal representation)
    std::vector<std::uint8_t> bv_;  ///< the bool channel values (1/0)

    /** @brief The shared, immutable structural descriptor (bounds / grouping / adaption config) */
    std::shared_ptr<const GGenomeLayout> layout_ = std::make_shared<const GGenomeLayout>();


    /***************************************************************************/
    // The layout-keyed parameter-count cache (watertight: re-keyed whenever the shared layout changes;
    // holding the shared_ptr pins the layout while cached, so pointer comparison can never alias). All
    // mutable, because the count accessors are logically const.

    mutable std::shared_ptr<const GGenomeLayout> counts_layout_sp_; ///< the layout the cache is keyed to
    mutable std::array<std::array<std::size_t, 3>, 4>
        counts_cache_{}; ///< [channel: d/f/i/b][activity mode: ALL/ACTIVEONLY/INACTIVEONLY]

    /***************************************************************************/
    // ---- Folded in from the former GOptimizableEntity category root ----

public:

    using payload_type = GGenome;
    using result_type = individual_processing_result;


    /***************************************************************************/
    // Processing: the result store + the evaluation orchestration.

    /**
     * @brief Performs the evaluation of this candidate: drives the optional pre-processor, the feasibility
     * check and fitness calculation (or adopts pre-computed raw results from res_vec), the evaluation-policy
     * transform and the optional post-processor, measuring the time of each step. On an exception the
     * fitnesses are set to the worst case and a processing exception is rethrown. Only an item with the
     * DO_PROCESS status is accepted.
     * @param res_vec Optional pre-computed raw results (e.g. from a remote/GPU evaluator); if empty,
     *        evaluate() is invoked. Its size must match the criteria count.
     * @return The first stored result after processing
     */
    individual_processing_result process(
        const std::vector<individual_processing_result> &res_vec =
            std::vector<individual_processing_result>()
    );

    /**
     * @brief Sets the vector of stored results to a given collection and marks the candidate PROCESSED.
     * @param result_cnt The new result vector (size must match the configured number of stored results)
     * @return The first stored result after the assignment
     */
    individual_processing_result
    markAsProcessedWith(std::vector<individual_processing_result> const &result_cnt);

    /**
     * @brief Read-only retrieval of a stored result. Throws if the PROCESSED flag is not set.
     * @param id The position of the stored result to return
     * @return The stored result at position id
     */
    [[nodiscard]] individual_processing_result getStoredResult(std::size_t id = 0) const;

    /** @brief @return The number of result slots held by this candidate */
    [[nodiscard]] std::size_t getNStoredResults() const { return stored_results_cnt_.size(); }

    /** @brief Read-only retrieval of the whole result store (no PROCESSED-flag precondition, unlike
     *  getStoredResult()), used to carry results across a return reconciliation. @return The result store */
    [[nodiscard]] const std::vector<individual_processing_result> &getStoredResults() const {
        return stored_results_cnt_;
    }

    /***************************************************************************/
    // Pre-/post-processing (used by nested / post-optimizing algorithms).

    /** @brief @return true if pre-processing is currently allowed (not vetoed) */
    [[nodiscard]] bool mayBePreProcessed() const noexcept { return not pre_processing_disabled_; }
    /** @brief Allow or veto pre-processing. @param veto true to disable, false to allow */
    void vetoPreProcessing(bool veto) noexcept { pre_processing_disabled_ = veto; }
    /** @brief Registers a pre-processor (ignored if empty). @param pre_processor_ptr The processor */
    void registerPreProcessor(
        const std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<GGenome>>& pre_processor_ptr
    ) {
        if(pre_processor_ptr) {
            pre_processor_ptr_ = pre_processor_ptr;
        }
    }

    /** @brief @return true if post-processing is currently allowed (not vetoed) */
    [[nodiscard]] bool mayBePostProcessed() const { return not post_processing_disabled_; }
    /** @brief Allow or veto post-processing. @param veto true to disable, false to allow */
    void vetoPostProcessing(bool veto) { post_processing_disabled_ = veto; }
    /** @brief Registers a post-processor (ignored if empty). @param post_processor_ptr The processor */
    void registerPostProcessor(
        const std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<GGenome>>& post_processor_ptr
    ) {
        if(post_processor_ptr) {
            post_processor_ptr_ = post_processor_ptr;
        }
    }
    /** @brief @return The registered post-processor, or an empty pointer if none is registered */
    [[nodiscard]] std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<GGenome>>
    postProcessor() const {
        return post_processor_ptr_;
    }
    /** @brief Removes any registered post-processor */
    void clearPostProcessor() { post_processor_ptr_.reset(); }

    /** @brief Loads otherwise-constant data into this (freshly de-serialized) item from a template held at
     *  a remote site, so that data need not travel with every work item (a networked-client convenience;
     *  the default is a no-op, a derived type overrides the hook below if it carries such data).
     *  @param cd_ptr A template item whose constant data is loaded into this one */
    void loadConstantData(std::shared_ptr<GGenome> cd_ptr) {
        this->loadConstantData_(std::move(cd_ptr));
    }

    /***************************************************************************/
    // Fitness accessors / multi-criterion support.

    /**
     * @brief Registers a (raw) result value of the fitness calculation at a given criterion position.
     * @param id The position of the fitness criterion (must be < the criteria count)
     * @param value The raw fitness value to register
     */
    void setResult(std::size_t id, double value);
    /** @brief @return true if more than one fitness criterion is present */
    [[nodiscard]] bool hasMultipleFitnessCriteria() const { return this->getNStoredResults() > 1; }
    /**
     * @brief Retrieve the (raw, transformed) fitness tuple at a given evaluation position.
     * @param id The evaluation position (fitness criterion index); defaults to 0
     * @return A (raw, transformed) fitness tuple at the requested position
     */
    [[nodiscard]] std::tuple<double, double> getFitnessTuple(std::uint32_t id = 0) const;
    /**
     * @brief Checks whether this candidate is at least as good as a set of raw boundaries.
     * @param boundaries One boundary value per fitness criterion
     * @return true if every raw fitness is at least as good as its boundary, false otherwise
     */
    bool isGoodEnough(std::vector<double> const &boundaries);

    /***************************************************************************/
    // Policy-derived accessors (forwarded to the shared GProblemPolicy).

    /** @brief Installs the shared problem policy (the 1:N feasibility/ranking rules).
     *  @param policy The shared policy to reference (must not be empty) */
    void setPolicy(std::shared_ptr<GProblemPolicy> policy);
    /** @brief @return The shared problem policy referenced by this candidate */
    [[nodiscard]] std::shared_ptr<GProblemPolicy> getPolicy() const { return policy_; }

    /** @brief Sets the optimization direction on the shared policy. @param mode MAXIMIZE or MINIMIZE */
    void setMaxMode(maxMode const &mode) { policy_->setMaxMode(mode); }
    /** @brief @return The optimization direction from the shared policy */
    [[nodiscard]] maxMode getMaxMode() const { return policy_->getMaxMode(); }
    /** @brief @return The worst-case evaluation value for the current direction */
    [[nodiscard]] virtual double getWorstCase() const { return policy_->getWorstCase(); }
    /** @brief @return The best-case evaluation value for the current direction */
    [[nodiscard]] virtual double getBestCase() const { return policy_->getBestCase(); }

    /** @brief Sets the policy for invalid solutions. @param eval_policy The evaluation policy */
    void setEvaluationPolicy(evaluationPolicy eval_policy) { policy_->setEvaluationPolicy(eval_policy); }
    /** @brief @return The evaluation policy for invalid solutions */
    [[nodiscard]] evaluationPolicy getEvaluationPolicy() const { return policy_->getEvaluationPolicy(); }

    /** @brief @return The sigmoid steepness from the shared policy */
    [[nodiscard]] double getSteepness() const { return policy_->getSteepness(); }
    /** @brief Sets the sigmoid steepness on the shared policy. @param steepness The steepness (> 0) */
    void setSteepness(double steepness) { policy_->setSteepness(steepness); }
    /** @brief @return The sigmoid barrier from the shared policy */
    [[nodiscard]] double getBarrier() const { return policy_->getBarrier(); }
    /** @brief Sets the sigmoid barrier on the shared policy. @param barrier The barrier (> 0) */
    void setBarrier(double barrier) { policy_->setBarrier(barrier); }

    /** @brief @return The computed validity level of this candidate (<= 1 means feasible) */
    [[nodiscard]] double getValidityLevel() const { return validity_level_; }
    /** @brief @return true if this candidate fulfils its constraints (validity level <= 1) */
    [[nodiscard]] bool constraintsFulfilled() const { return validity_level_ <= 1.; }
    /** @brief @return true if this candidate is a valid solution (meant for processed candidates) */
    [[nodiscard]] bool isValid() const;
    /** @brief @return true if this candidate is an invalid solution */
    [[nodiscard]] bool isInValid() const { return not this->isValid(); }

    /***************************************************************************/
    // Iteration bookkeeping (per individual). The stall count and best-known fitness are OA state and
    // live on GOptimizationAlgorithmBase; the adaption-retry limits are OA adaption policy and live on
    // the OA-owned GAdaptionConfig -- neither is carried on the individual any more.

    /** @brief Sets the parent algorithm's iteration. @param parent_alg_iteration The iteration */
    void setAssignedIteration(std::uint32_t const &parent_alg_iteration) {
        assigned_iteration_ = parent_alg_iteration;
    }
    /** @brief @return The parent algorithm's current iteration */
    [[nodiscard]] std::uint32_t getAssignedIteration() const { return assigned_iteration_; }

    /** @brief @return The number of adaptions performed during the last adaption (read from the OA
     *  scratch, where the adaption machinery records it; 0 if no scratch is attached) */
    [[nodiscard]] std::size_t getNAdaptions() const { return scratch_ ? scratch_->getNAdaptions() : 0; }

    /**
     * @brief Public constraint check used by the OA-owned adaption retry loop. Feasibility is a concrete
     * base operation: it reads this entity's parameter values (streamlineFP, below) through the shared
     * policy's constraint. Returns true if the entity satisfies its constraints and writes the validity
     * level to the out-parameter.
     * @param validity_level Out-parameter receiving the computed validity level
     * @return true if the candidate satisfies its constraints, false otherwise
     */
    bool fulfillsConstraints(double &validity_level) const {
        return policy_->fulfillsConstraints(*this, validity_level);
    }

    /**
     * @brief Registers a constraint with this entity's shared problem policy (the 1:N feasibility rule).
     * Forwarded to GProblemPolicy::registerConstraint, which clones the constraint.
     * @param c_ptr The validity-check constraint to register (must not be empty)
     */
    void registerConstraint(std::shared_ptr<GPreEvaluationValidityCheckT<GGenome>> c_ptr) {
        policy_->registerConstraint(std::move(c_ptr));
    }

    /***************************************************************************/
    // OA-owned scratch (the personality OBJECT + per-group adaption POD blocks). Held ON the work item so
    // it permutes coherently through every sort / select / swap; it is server-side state, dropped at the
    // algorithm boundary (resetPersonality / clearScratch), and never travels on the wire (see serialize()).

    /** @brief The optimization-algorithm-owned scratch (personality + POD adaption state).
     *  @return A reference to the scratch store */
    GAuxiliaryStore &scratch() noexcept { return *scratch_; }
    /** @brief The OA-owned scratch (const). @return A const reference to the scratch store */
    [[nodiscard]] const GAuxiliaryStore &scratch() const noexcept { return *scratch_; }

    /**
     * @brief Converts the personality base pointer to the desired type. Only accessible when
     * personality_type derives from GPersonalityTraits.
     * @tparam personality_type The concrete personality-traits type (must derive from GPersonalityTraits)
     * @return A shared pointer to the personality traits cast to personality_type
     */
    template <typename personality_type>
        requires std::derived_from<personality_type, GPersonalityTraits>
    std::shared_ptr<personality_type> getPersonalityTraits() {
#ifdef DEBUG
        if(not scratch_->personalityRef()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGenome::getPersonalityTraits<personality_type>() : Empty personality "
                   "pointer found"
                << '\n'
            );
        }
#endif /* DEBUG */
        return Gem::Common::convertSmartPointer<GPersonalityTraits, personality_type>(
            scratch_->personalityRef()
        );
    }

    /** @brief The personality-traits base pointer.
     *  @return A shared pointer to this entity's GPersonalityTraits
     *  @throw geneva_exception (DEBUG builds) if the personality pointer is empty */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits() {
#ifdef DEBUG
        if(not scratch_->personalityRef()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGenome::getPersonalityTraits() : Empty personality pointer found" << '\n'
            );
        }
#endif /* DEBUG */
        return scratch_->personalityRef();
    }

    /** @brief Sets the personality of this entity.
     *  @param gpt The personality-traits object to install (must be non-null)
     *  @throw geneva_exception if gpt is empty */
    void setPersonality(std::shared_ptr<GPersonalityTraits> gpt) {
        if(not gpt) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGenome::setPersonality() : Empty personality pointer passed" << '\n'
            );
        }
        scratch_->personalityRef() = std::move(gpt);
    }

    /** @brief Resets the OA-owned scratch (personality + any POD blocks held on this entity) */
    void resetPersonality() { scratch_->clearScratch(); }

    /** @brief A string identifier for the current personality.
     *  @return The personality's name(), or "PERSONALITY_NONE" if no personality is set */
    [[nodiscard]] std::string getPersonality() const {
        if(scratch_->personalityRef()) {
            return scratch_->personalityRef()->name();
        }
        return std::string("PERSONALITY_NONE");
    }

    /***************************************************************************/
    // Genome value channels (genome-agnostic). The public per-type templates are the ergonomic surface the
    // optimization algorithms use; each dispatches to a non-template virtual that the flat genome
    // (GGenome) implements. The algorithms therefore read and write parameter values without knowing
    // the storage layout -- no downcast. "Read my parameters as a vector" is a universal optimization
    // operation; only the storage is genome-specific, so the base declares it and the flat impl overrides.

    /**
     * @brief Streamlines all parameters of type par_type into a vector (cleared first).
     * @tparam par_type The parameter value type (double, float, std::int32_t or bool)
     * @param par_vec The vector the parameter values are written into (cleared first)
     * @param am The activity mode controlling which parameters are included
     */
    template <typename par_type>
    void streamline(
        std::vector<par_type> &par_vec,
        activityMode const &am = activityMode::DEFAULTACTIVITYMODE
    ) const {
        this->streamline_(par_vec, am);
    }

    /**
     * @brief Assigns values from a vector to the parameters of type par_type (marks the item for
     * reprocessing).
     * @tparam par_type The parameter value type (double, float, std::int32_t or bool)
     * @param par_vec The vector of values to scatter onto the matching parameters
     * @param am The activity mode controlling which parameters are written
     */
    template <typename par_type>
    void assignValueVector(
        std::vector<par_type> const &par_vec,
        activityMode const &am = activityMode::DEFAULTACTIVITYMODE
    ) {
#ifdef DEBUG
        if(countParameters<par_type>() != par_vec.size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGenome::assignValueVector():" << '\n'
                << "Sizes don't match: " << countParameters<par_type>() << " / " << par_vec.size()
                << '\n'
            );
        }
#endif /* DEBUG */
        this->assignValueVector_(par_vec, am);
        // As we have modified our internal data sets, make sure the item is reprocessed
        this->mark_as_due_for_processing();
    }

    /**
     * @brief The number of parameters of type par_type.
     * @tparam par_type The parameter value type (double, float, std::int32_t or bool)
     * @param am The activity mode controlling which parameters are counted
     * @return The number of parameters of the requested type
     */
    template <typename par_type>
    [[nodiscard]] [[nodiscard]] [[nodiscard]] std::size_t countParameters(activityMode const &am = activityMode::DEFAULTACTIVITYMODE) const {
        if constexpr(std::is_same_v<par_type, double>) {
            return countParametersDouble_(am);
        }
        else if constexpr(std::is_same_v<par_type, float>) {
            return countParametersFloat_(am);
        }
        else if constexpr(std::is_same_v<par_type, std::int32_t>) {
            return countParametersInt32_(am);
        }
        else if constexpr(std::is_same_v<par_type, bool>) {
            return countParametersBool_(am);
        }
        else {
            static_assert(sizeof(par_type) == 0, "countParameters: unsupported parameter type");
            return 0;
        }
    }

    /**
     * @brief Lower/upper boundaries of all parameters of type par_type (cleared first).
     * @tparam par_type The parameter value type (double, float, std::int32_t or bool)
     * @param l_bnd_vec The vector the lower boundaries are written into (cleared first)
     * @param u_bnd_vec The vector the upper boundaries are written into (cleared first)
     * @param am The activity mode controlling which parameters are included
     */
    template <typename par_type>
    void boundaries(
        std::vector<par_type> &l_bnd_vec,
        std::vector<par_type> &u_bnd_vec,
        activityMode const &am = activityMode::DEFAULTACTIVITYMODE
    ) const {
        this->boundaries_(l_bnd_vec, u_bnd_vec, am);
    }

    /***************************************************************************/
    // The precision-agnostic floating point view (double + float widened to double). Implemented once
    // here on top of the per-type channels above, so every genome layout gets it for free.

    /** @brief @param am The activity mode. @return The combined count of double and float parameters */
    [[nodiscard]] std::size_t
    countFPParameters(activityMode const &am = activityMode::DEFAULTACTIVITYMODE) const {
        return countParameters<double>(am) + countParameters<float>(am);
    }

    /** @brief Streamlines all FP parameters into a single double vector (double-typed first, then widened
     *  float-typed). @param par_vec The output vector (cleared first). @param am The activity mode */
    void streamlineFP(
        std::vector<double> &par_vec,
        activityMode const &am = activityMode::DEFAULTACTIVITYMODE
    ) const {
        par_vec.clear();
        this->streamline<double>(par_vec, am);

        std::vector<float> float_vec;
        this->streamline<float>(float_vec, am);
        par_vec.append_range(float_vec); // each float implicitly widened to double
    }

    /** @brief Scatters a double vector produced by streamlineFP() back onto the FP parameters.
     *  @param par_vec The combined double vector. @param am The activity mode */
    void assignFPValueVector(
        std::vector<double> const &par_vec,
        activityMode const &am = activityMode::DEFAULTACTIVITYMODE
    ) {
        const std::size_t n_double = countParameters<double>(am);
        const std::size_t n_float = countParameters<float>(am);

#ifdef DEBUG
        if(n_double + n_float != par_vec.size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGenome::assignFPValueVector():" << '\n'
                << "Sizes don't match: " << (n_double + n_float) << " / " << par_vec.size() << '\n'
            );
        }
#endif /* DEBUG */

        if(n_double > 0) {
            std::vector<double> const double_vec(
                par_vec.begin(),
                par_vec.begin() + static_cast<std::ptrdiff_t>(n_double)
            );
            this->assignValueVector<double>(double_vec, am);
        }
        if(n_float > 0) {
            std::vector<float> float_vec;
            float_vec.append_range(par_vec | std::views::drop(n_double)); // each double implicitly narrowed to float
            this->assignValueVector<float>(float_vec, am);
        }
    }

    /** @brief Lower/upper boundaries of all FP parameters (matching streamlineFP() ordering).
     *  @param l_bnd_vec Lower bounds out (cleared). @param u_bnd_vec Upper bounds out (cleared). @param am The activity mode */
    void boundariesFP(
        std::vector<double> &l_bnd_vec,
        std::vector<double> &u_bnd_vec,
        activityMode const &am = activityMode::DEFAULTACTIVITYMODE
    ) const {
        std::vector<double> l_double;
        std::vector<double> u_double;
        this->boundaries<double>(l_double, u_double, am);

        std::vector<float> l_float;
        std::vector<float> u_float;
        this->boundaries<float>(l_float, u_float, am);

        l_bnd_vec.clear();
        u_bnd_vec.clear();
        l_bnd_vec.append_range(l_double);
        l_bnd_vec.append_range(l_float); // each float implicitly widened to double
        u_bnd_vec.append_range(u_double);
        u_bnd_vec.append_range(u_float); // each float implicitly widened to double
    }

    /***************************************************************************/
    // The INTERNAL (normalized) floating-point view (normalized-genome architecture §2.3): the OAs read
    // and write the raw normalized internal coordinate (the two-reader split). Ordering matches
    // streamlineFP: double-typed first, then float-typed widened to double.

    /** @brief Streamlines all FP parameters in their raw INTERNAL representation into a double vector.
     *  @param par_vec The output vector (cleared first). @param am The activity mode */
    void streamlineFPInternal(
        std::vector<double> &par_vec,
        activityMode const &am = activityMode::DEFAULTACTIVITYMODE
    ) const {
        par_vec.clear();
        this->streamlineInternal_(par_vec, am);

        std::vector<float> float_vec;
        this->streamlineInternal_(float_vec, am);
        par_vec.append_range(float_vec); // each float implicitly widened to double
    }

    /** @brief Scatters a vector of raw INTERNAL values back onto the FP parameters (folds bounded values,
     *  never range-validates). @param par_vec The combined internal-value vector. @param am The activity mode */
    void assignFPValueVectorInternal(
        std::vector<double> const &par_vec,
        activityMode const &am = activityMode::DEFAULTACTIVITYMODE
    ) {
        const std::size_t n_double = countParameters<double>(am);
        const std::size_t n_float = countParameters<float>(am);

#ifdef DEBUG
        if(n_double + n_float != par_vec.size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGenome::assignFPValueVectorInternal():" << '\n'
                << "Sizes don't match: " << (n_double + n_float) << " / " << par_vec.size() << '\n'
            );
        }
#endif /* DEBUG */

        if(n_double > 0) {
            std::vector<double> const double_vec(
                par_vec.begin(),
                par_vec.begin() + static_cast<std::ptrdiff_t>(n_double)
            );
            this->assignValueVectorInternal_(double_vec, am);
        }
        if(n_float > 0) {
            std::vector<float> float_vec;
            float_vec.append_range(par_vec | std::views::drop(n_double)); // each double implicitly narrowed to float
            this->assignValueVectorInternal_(float_vec, am);
        }
        // As with the external assign, modifying the parameters marks the item for reprocessing.
        this->mark_as_due_for_processing();
    }

    /***************************************************************************/
    /**
     * @brief Retrieves a parameter of a given type at the specified active index.
     * @tparam val_type The value type to retrieve (double, float, std::int32_t or bool)
     * @param target A (type-index, name, position) tuple; the third element is the active index
     * @return The parameter value of the requested type at the requested index
     */
    template <typename val_type>
    val_type getVarVal(std::tuple<std::size_t, std::string, std::size_t> const &target) {
        static_assert(
            std::is_same_v<val_type, double> || std::is_same_v<val_type, float> ||
                std::is_same_v<val_type, std::int32_t> || std::is_same_v<val_type, bool>,
            "GGenome::getVarVal<>(): unsupported value type (use double, float, std::int32_t or bool)"
        );

        const std::size_t idx = std::get<2>(target);

        if constexpr (std::is_same_v<val_type, double>) {
            return this->getVarVal_d_(idx);
        } else if constexpr (std::is_same_v<val_type, float>) {
            return this->getVarVal_f_(idx);
        } else if constexpr (std::is_same_v<val_type, std::int32_t>) {
            return this->getVarVal_i_(idx);
        } else { // bool, by the static_assert above
            return this->getVarVal_b_(idx);
        }
    }

    /***************************************************************************/
    // Algorithm-facing genome operations (declared here so the algorithms reach them through the base;
    // the flat genome implements them over its value channels).





    /***************************************************************************/
    // Randomization.

    /**
     * @brief Randomly initializes the parameters (marking the item for reprocessing on change).
     * @param am The activity mode selecting which parameters are (re-)initialized
     * @return true if at least one parameter was changed
     */
    bool randomInit(activityMode const &am);

protected:
    /***************************************************************************/
    // Result-store mutators (for the genome / process orchestration).

    /**
     * @brief Sets the fitness from a vector of externally-computed raw values, applying the feasibility
     * check and evaluation-policy transform, then marking the candidate PROCESSED. An internal helper (an
     * alternative to process(res_vec) used on the genome copy/move path); not a public API -- external
     * results are injected through process(res_vec).
     * @param f_cnt A vector of raw fitness values (size must match the criteria count)
     */
    void setFitness_(std::vector<double> const &f_cnt);

    /** @brief Modifiable retrieval of a stored result. @param id The position. @return A modifiable reference */
    individual_processing_result &modifyStoredResult(std::size_t id = 0) {
        return stored_results_cnt_.at(id);
    }
    /** @brief Sets the number of stored results, copying new_val into new positions.
     *  @param n_stored_results The new number of result slots. @param new_val The fill value */
    void setNStoredResults(std::size_t n_stored_results, individual_processing_result new_val) {
        stored_results_cnt_.resize(n_stored_results, new_val);
    }
    /** @brief Sets the number of stored results, default-initializing new positions.
     *  @param n_stored_results The new number of result slots */
    void setNStoredResults(std::size_t n_stored_results) {
        stored_results_cnt_.resize(n_stored_results, individual_processing_result());
    }
    /** @brief Registers a result at a given position. @param id The position. @param r The result */
    void registerResult(std::size_t id, const individual_processing_result &r) {
        stored_results_cnt_.at(id) = r;
    }
    /** @brief Replaces the whole result store WITHOUT touching the processing status (unlike
     *  markAsProcessedWith(), which forces PROCESSED). Used to carry results across a return
     *  reconciliation, where the status is set separately from the returned lifecycle.
     *  @param results The result store to install */
    void setStoredResults(const std::vector<individual_processing_result> &results) {
        stored_results_cnt_ = results;
    }
    /** @brief Records the feasibility (validity) level -- normally filled by fulfillsConstraints() during
     *  evaluation; exposed here so a return reconciliation can carry the worker-computed value.
     *  @param validity_level The validity level (<= 1 == feasible) */
    void setValidityLevel(double validity_level) { validity_level_ = validity_level; }

    /** @brief @return A reference to the shared problem policy (for the genome's feasibility check) */
    GProblemPolicy &policy() { return *policy_; }
    /** @brief @return A const reference to the shared problem policy */
    [[nodiscard]] const GProblemPolicy &policy() const { return *policy_; }

    /***************************************************************************/
    // Secondary-result combiners (the user's evaluate() may return one of these).

    /** @brief @return The sum of all stored transformed fitness values */
    [[nodiscard]] double sumCombiner() const;
    /** @brief @return The sum of the absolute values of all stored transformed fitness values */
    [[nodiscard]] double fabsSumCombiner() const;
    /** @brief @return The square root of the sum of squares of all stored transformed fitness values */
    [[nodiscard]] double squaredSumCombiner() const;
    /** @brief @return The square root of the weighed sum of squares of all stored transformed fitness values
     *  @param weights The per-criterion weights (size must match the criteria count) */
    [[nodiscard]] double weighedSquaredSumCombiner(std::vector<double> const &weights) const;

    /***************************************************************************/
    // GCommonInterfaceT / configuration contract.

    /** @brief Adds local configuration options (eval policy, sigmoid, max mode, adaption limits).
     *  @param gpb The parser builder the configuration options are registered with */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_(), compare_() and name_() are generated by the Gem::Common::GReflectiveInterfaceBaseT base from
    // class_name and the single localMembers_() declaration (which carries the GProcessable base slice, the
    // plain members and the serialized-but-uncompared processors / policy / result store / scratch).
    // clone_() stays pure here -- this is the abstract category root; each concrete candidate supplies it.

    /***************************************************************************/
    // A candidate carries NO random-number state of its own -- it is pure data. Every external
    // operation that needs randomness on it (adaption, random-init, cross-over position) leases a
    // proxy from the process-global Gem::Hap::randomLeasePool() for the duration of the operation, so
    // the number of live proxies is bounded by peak concurrency (O(worker threads)), not by the
    // population size.

    /***************************************************************************/
    // Pure-virtual hooks implemented by the genome layer.


    /** @brief The evaluation hook a concrete individual implements: computes and RETURNS this individual's
     *  raw result vector (size 1 for a single-criterion problem, one entry per criterion otherwise -- main
     *  at index 0). It reads the individual through @c this (genome via streamline(), any per-run context via
     *  the individual's own accessors) and RETURNS the results; the caller (@c runEvaluation_) writes them and
     *  applies feasibility + policy + PROCESSED, so the hook itself sets no fitness. The single evaluation
     *  call site dispatches here for a locally-evaluated individual. Implemented by every concrete
     *  individual (the value-bearing genome layer's problem definition).
     *  @return The raw result vector */
    virtual std::vector<double> evaluate() = 0;

private:
    /***************************************************************************/
    // GRateableI surface (read the stored results).

    /** @brief @param id The criterion index. @return The stored raw fitness for that criterion */
    [[nodiscard]] double raw_fitness_(std::size_t id) const final { return this->getStoredResult(id).rawFitness(); }
    /** @brief @param id The criterion index. @return The stored transformed fitness for that criterion */
    [[nodiscard]] double transformed_fitness_(std::size_t id) const final {
        return this->getStoredResult(id).transformedFitness();
    }
    /** @brief @return A vector of all stored raw fitness results */
    [[nodiscard]] std::vector<double> raw_fitness_vec_() const final;
    /** @brief @return A vector of all stored transformed fitness results */
    [[nodiscard]] std::vector<double> transformed_fitness_vec_() const final;

    /***************************************************************************/
    // Per-type genome value channels -- the non-template dispatch targets of the public
    // streamline<T>/assignValueVector<T>/countParameters<T>/boundaries<T> templates (and the getVarVal<T>
    // template). Implemented by the concrete flat genome. These are the entire seam that makes value
    // access genome-agnostic; each overload takes the per-type value/boundary vector(s) and the
    // activityMode controlling which parameters participate.






    // The INTERNAL (normalized) FP channels (§2.3): only double/float have an internal/external
    // distinction (int / bool are not normalized).

    /** @brief Re-attaches the OA-owned scratch from @p original onto this individual (the wire omits the
     *  scratch, so a networked return arrives without it; the consumer grafts it back from the retained
     *  original -- see Gem::Courtier::GProcessable::graftOaScratchFrom()).
     *  @param original The originally-submitted item supplying the scratch (ignored if not a GGenome) */
    void graftOaScratchFrom_(const Gem::Courtier::GProcessable &original) override {
        const auto *src = dynamic_cast<const GGenome *>(&original);
        if(src != nullptr && src->scratch_) {
            // This deep-copy runs under the consumer's mutex (the original is reconciled in place). It is
            // deliberately a COPY, not a move: every call site discards the original immediately afterwards,
            // so a move WOULD be safe and O(1) -- but coupling correctness to that "source dies next"
            // invariant is a footgun a future reorder could trip silently. SIGNPOST: if profiling at high
            // client/return rates ever shows this mutex as hot, switch to an explicit consume -- take the
            // source by rvalue-ref (graftOaScratchFrom_(GProcessable&&)) so call sites must std::move it and
            // the steal is visible -- rather than turning this into a silent move. Until then, keep the copy.
            scratch_ = std::make_unique<GAuxiliaryStore>(*src->scratch_);
        }
    }

    /** @brief In-place return reconciliation (see Gem::Courtier::GProcessable::absorbResultsFrom): absorbs
     *  the returned item's computed results + evaluation-derived local state + processing lifecycle, while
     *  KEEPING this live population element's own genome value channels (an `evaluation` return leaves them
     *  untouched; a full return grafts the genome separately, see GNetworkedConsumerT::checkin) and its
     *  OA-owned scratch. Keeping the object in place (rather than swapping in the deserialized return) is
     *  what preserves its heap address across a networked round-trip.
     *  @param src The returned, evaluated item whose results + lifecycle are absorbed */
    void absorbResultsFrom_(const Gem::Courtier::GProcessable &src) override;

    /** @brief In-place full deep copy (see Gem::Courtier::GProcessable::loadContentFrom): replaces this
     *  item's whole content (genome + results + scratch + lifecycle) with a copy of @p src without
     *  relocating the object, so a concurrent snapshot of population addresses stays valid. Used by the
     *  clone-on-partial-return refill to substitute a viable sibling into a failed slot.
     *  @param src The source item to deep-copy in place
     *  @return true (the optimization individual supports in-place substitution) */
    bool loadContentFrom_(const Gem::Courtier::GProcessable &src) override;

    /** @brief Default no-op constant-data load; a derived type overrides if it deposits constant data at a
     *  remote site. @param cd_ptr A template item whose constant data would be loaded into this one */
    virtual void loadConstantData_([[maybe_unused]] std::shared_ptr<GGenome> cd_ptr) { /* nothing */ }

    /***************************************************************************/
    // Evaluation internals.

    /** @brief The evaluation body run inside process(): feasibility check + evaluate()/res_vec
     *  adoption + the evaluation-policy transform. @param res_vec Optional pre-computed raw results */
    void runEvaluation_(const std::vector<individual_processing_result> &res_vec);

    /** @brief runEvaluation_() feasible branch: adopt the raw results and apply the evaluation-policy
     *  transform (or worst-case the surface if the user flagged an error). @param res_vec Optional
     *  pre-computed raw results */
    void finalizeFeasibleEvaluation_(const std::vector<individual_processing_result> &res_vec);

    /** @brief Adopts the raw results into the result store -- either from a non-empty res_vec (external
     *  GPU/network path) or from a local evaluate() -- returning the main (index-0) raw result and storing
     *  the secondary criteria. Worst-cases the whole surface and rethrows on any failure. Extracted from
     *  runEvaluation_(). @param res_vec Optional pre-computed raw results @return The main raw result */
    double adoptRawResults_(const std::vector<individual_processing_result> &res_vec);

    /** @brief Applies the configured invalidity policy (worst-case, or the sigmoid barrier value)
     *  to the whole quality surface of a constraint-violating candidate. Shared by
     *  runEvaluation_() and setFitness_() (formerly two identical copies). */
    void applyInvalidityPolicy_();

    /** @brief Runs the registered pre-processor (if allowed) on this candidate */
    void preProcess_();
    /** @brief Runs the registered post-processor (if allowed) on this candidate */
    void postProcess_();

    /** @brief Sets every raw and transformed fitness to the same value. @param val The value */
    void setAllFitnessTo(double val) { this->setAllFitnessTo(val, val); }
    /** @brief Sets every raw fitness to raw_value and every transformed fitness to transformed_value.
     *  @param raw_value The raw value. @param transformed_value The transformed value */
    void setAllFitnessTo(double raw_value, double transformed_value);

    /** @brief Resets every stored result to a default-constructed value */
    void clear_stored_results_vec();
    /** @brief Bridges the GProcessable status machine to the result store (clears it on a status reset) */
    void clearStoredResults_() override { this->clear_stored_results_vec(); }

    /** @brief Creates a deep clone of this object (supplied by the concrete leaf). @return A heap copy */
    [[nodiscard]] GGenome *clone_() const override = 0;

    /***************************************************************************/
    // Data.

    bool pre_processing_disabled_ = false;  ///< Whether pre-processing was disabled entirely
    bool post_processing_disabled_ = false; ///< Whether post-processing was disabled entirely

    std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<GGenome>>
        pre_processor_ptr_; ///< Actions to be performed before processing
    std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<GGenome>>
        post_processor_ptr_; ///< Actions to be performed after processing

    std::vector<individual_processing_result> stored_results_cnt_ =
        std::vector<individual_processing_result>(1, individual_processing_result()); ///< The result store

    /** @brief The shared, problem-uniform feasibility / ranking policy (referenced 1:N) */
    std::shared_ptr<GProblemPolicy> policy_ = std::make_shared<GProblemPolicy>();

    std::uint32_t assigned_iteration_ = 0; ///< The parent algorithm's optimization-cycle iteration
    double validity_level_ = 0.;        ///< How valid the current solution is (<= 1 == feasible)

    /** @brief The OA-owned scratch (personality object + per-group adaption POD blocks). Always allocated
     *  (so the accessors never null-check); deep-copied on clone/load; serialized only on a checkpoint
     *  (omitted on the wire, see serialize()); excluded from the compared identity (not in localMembers_). */
    std::unique_ptr<GAuxiliaryStore> scratch_ = std::make_unique<GAuxiliaryStore>();
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Genome */

/******************************************************************************/
/**
 * @brief Needed for GArchive serialization
 */
/******************************************************************************/

/******************************************************************************/
// The wire-protocol seam: what a courtier message MEANS to Geneva (the geneva_command vocabulary and
// the GWireProtocolT<GGenome> specialization). Included HERE, at the end of the type's own header, so
// that no translation unit can instantiate a courtier transport for a Geneva individual without the
// specialization being visible -- the primary template must never be picked up by accident.
#include "geneva/genome/GGenomeWireProtocol.hpp"

/******************************************************************************/
