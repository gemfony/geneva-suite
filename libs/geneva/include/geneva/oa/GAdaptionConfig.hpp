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
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <istream>
#include <ostream>
#include <ranges>
#include <string>
#include <type_traits>
#include <vector>

// Geneva headers go here
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GAdaptionAuxKeys.hpp"
#include "geneva/genome/GAdaptionKernels.hpp"
#include "geneva/genome/GAuxiliaryStore.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeLayout.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
// Names imported from the genome / kernel layer (the Gem::Geneva::Genome namespace).
using Gem::Geneva::Genome::adaption_fp_t;
using Gem::Geneva::Genome::BiGaussConfig;
using Gem::Geneva::Genome::ChannelTag;
using Gem::Geneva::Genome::FlipConfig;
using Gem::Geneva::Genome::GaussConfig;
using Gem::Geneva::Genome::GAuxiliaryStore;
using Gem::Geneva::Genome::GGenome;
using Gem::Geneva::Genome::GGenomeLayout;
using Gem::Geneva::Genome::GroupRef;
using Gem::Geneva::Genome::GroupSpec;
using Gem::Geneva::Genome::GroupStructure;

/******************************************************************************/
/**
 * The step-size control strategy an adapting algorithm applies on top of the per-group Gauss adaptors.
 * It is a property of the OA-owned adaption config so the same genome can be driven by different
 * controllers without touching the structure-only layout. The default reproduces the classic Geneva
 * behaviour bit-for-bit; the other modes are used by GEvolutionaryAlgorithm ("ea").
 *
 * - SELF_ADAPT        : classic mutative self-adaptive isotropic Gaussian (σSA). Each group's sigma
 *                       self-adapts log-normally with whatever sigma_sigma the user authored. This is
 *                       what GEvolutionaryAlgorithm ("ea") uses and is the default for every config.
 * - SELF_ADAPT_SCALED : as SELF_ADAPT, but the σ self-adaption learning rate is rescaled by the
 *                       parameter count n: a shared-σ group gets sigma_sigma = c/sqrt(2n), a
 *                       per-coordinate (length-1) group gets sigma_sigma = c/sqrt(2*sqrt(n)). This is
 *                       the textbook τ that the fixed Geneva default (0.8) lacks; at large n the fixed
 *                       rate is ~100x too hot and the σ random-walks instead of settling.
 * - ONE_FIFTH         : a single GLOBAL sigma driven by the Rechenberg 1/5 success rule. Per-group
 *                       log-normal self-adaption is suppressed; the OA pushes the global sigma into
 *                       every group's state each generation. O(1) controller state.
 * - CSA               : a single GLOBAL sigma driven by cumulative step-size adaptation (an evolution
 *                       path p_sigma). Per-group log-normal self-adaption is suppressed; the OA pushes
 *                       the global sigma into every group's state each generation. O(n) controller state.
 */
enum class stepControl : std::uint8_t {
    SELF_ADAPT = 0,
    SELF_ADAPT_SCALED = 1,
    ONE_FIFTH = 2,
    CSA = 3
};

// Numeric streaming opt-in for stepControl; must precede its first streaming
// use (see numeric_enum_io_v in GCommonEnums.hpp).
} /* namespace Gem::Geneva::OptimizationAlgorithms */
namespace Gem::Common {
template <> inline constexpr bool numeric_enum_io_v<Gem::Geneva::OptimizationAlgorithms::stepControl> = true;
} /* namespace Gem::Common */
namespace Gem::Geneva::OptimizationAlgorithms {

// stepControl streams as its integer code through the shared numeric_enum_io_v
// machinery of GCommonEnums.hpp (marker specialization at the end of this
// header). Re-export the operators so ADL finds them in this namespace even
// when this header is included on its own.
using Gem::Common::operator<<;
using Gem::Common::operator>>;

/******************************************************************************/
/**
 * A fluent handle to one or more adaption groups of a single channel held by a GAdaptionConfig. It
 * applies an adaptor setting (Gauss / bi-Gauss / integer-Gauss / flip / mode) to every group it spans
 * -- one group for a per-index handle, many for a label handle. The setters mirror the GGenomeBuilder's
 * ParamHandle exactly, so an adaption config authored here is bit-for-bit the same configuration the
 * builder would have baked into the (soon structure-only) layout. The handle is a thin view onto the
 * config's group vector; it must not outlive the config.
 *
 * @tparam T The channel's parameter type (double, float, std::int32_t or bool)
 */
template <typename T>
class GroupConfigHandle {
public:
    using adfp = adaption_fp_t<T>;

    /**
     * @brief Constructs a handle spanning the given group spec pointers.
     *
     * @param groups Non-owning pointers to the GroupSpec objects this handle will configure (must outlive the handle)
     */
    explicit GroupConfigHandle(std::vector<GroupSpec<T> *> groups)
      : groups_(std::move(groups)) {
        /* nothing */
    }

    /**
     * @brief Attaches a Gauss adaptor to the spanned group(s) (FP channels only).
     *
     * @param sigma The initial step width (standard deviation) of the Gaussian
     * @param sigma_sigma The self-adaption strength applied to sigma each generation
     * @param min_sigma The lower clamp for sigma during self-adaption
     * @param max_sigma The upper clamp for sigma during self-adaption
     * @param ad_prob The probability that a given parameter is adapted
     * @param adapt_ad_prob The self-adaption strength applied to ad_prob (default 0, i.e. fixed)
     * @param adaption_threshold The number of calls after which sigma is self-adapted (default 1)
     * @param mode The adaption mode (default WITHPROBABILITY)
     * @param min_ad_prob The lower clamp for ad_prob during self-adaption (default 0)
     * @param max_ad_prob The upper clamp for ad_prob during self-adaption (default 1)
     * @return A reference to this handle, for fluent chaining
     */
    // NOLINTNEXTLINE(readability-function-size) -- flat field-assignment sweep whose parameter list deliberately mirrors GGenomeBuilder's ParamHandle::gauss() one-for-one; splitting would just relocate the same assignment list without shrinking the mirrored interface
    GroupConfigHandle &gauss(
        adfp sigma,
        adfp sigma_sigma,
        adfp min_sigma,
        adfp max_sigma,
        adfp ad_prob,
        adfp adapt_ad_prob = static_cast<adfp>(0),
        std::uint32_t adaption_threshold = 1,
        adaptionMode mode = adaptionMode::WITHPROBABILITY,
        adfp min_ad_prob = static_cast<adfp>(0),
        adfp max_ad_prob = static_cast<adfp>(1)
    ) {
        static_assert(std::is_floating_point_v<T>, "gauss() is only available for floating point groups");
        for(GroupSpec<T> *g : groups_) {
            g->has_gauss = true;
            g->start_sigma = sigma;
            g->start_ad_prob = ad_prob;
            g->gauss.sigma_sigma = sigma_sigma;
            g->gauss.min_sigma = min_sigma;
            g->gauss.max_sigma = max_sigma;
            g->gauss.min_ad_prob = min_ad_prob;
            g->gauss.max_ad_prob = max_ad_prob;
            g->gauss.adapt_ad_prob = adapt_ad_prob;
            g->gauss.adaption_threshold = adaption_threshold;
            g->gauss.mode = mode;
        }
        return *this;
    }

    /**
     * @brief Attaches a bi-gaussian adaptor to the spanned group(s) (FP channels only).
     *
     * @param sigma1 The initial step width of the first Gaussian
     * @param sigma_sigma1 The self-adaption strength applied to sigma1
     * @param min_sigma1 The lower clamp for sigma1
     * @param max_sigma1 The upper clamp for sigma1
     * @param sigma2 The initial step width of the second Gaussian
     * @param sigma_sigma2 The self-adaption strength applied to sigma2
     * @param min_sigma2 The lower clamp for sigma2
     * @param max_sigma2 The upper clamp for sigma2
     * @param delta The initial distance between the two Gaussian peaks
     * @param sigma_delta The self-adaption strength applied to delta
     * @param min_delta The lower clamp for delta
     * @param max_delta The upper clamp for delta
     * @param ad_prob The probability that a given parameter is adapted
     * @param use_symmetric_sigmas If true, both Gaussians share a single sigma (default false)
     * @param adapt_ad_prob The self-adaption strength applied to ad_prob (default 0, i.e. fixed)
     * @param adaption_threshold The number of calls after which sigmas are self-adapted (default 1)
     * @param mode The adaption mode (default WITHPROBABILITY)
     * @param min_ad_prob The lower clamp for ad_prob during self-adaption (default 0)
     * @param max_ad_prob The upper clamp for ad_prob during self-adaption (default 1)
     * @return A reference to this handle, for fluent chaining
     */
    // NOLINTNEXTLINE(readability-function-size) -- flat field-assignment sweep whose parameter list deliberately mirrors GGenomeBuilder's ParamHandle::biGauss() one-for-one; splitting would just relocate the same assignment list without shrinking the mirrored interface
    GroupConfigHandle &biGauss(
        adfp sigma1,
        adfp sigma_sigma1,
        adfp min_sigma1,
        adfp max_sigma1,
        adfp sigma2,
        adfp sigma_sigma2,
        adfp min_sigma2,
        adfp max_sigma2,
        adfp delta,
        adfp sigma_delta,
        adfp min_delta,
        adfp max_delta,
        adfp ad_prob,
        bool use_symmetric_sigmas = false,
        adfp adapt_ad_prob = adfp(0),
        std::uint32_t adaption_threshold = 1,
        adaptionMode mode = adaptionMode::WITHPROBABILITY,
        adfp min_ad_prob = static_cast<adfp>(0),
        adfp max_ad_prob = static_cast<adfp>(1)
    ) {
        static_assert(std::is_floating_point_v<T>, "biGauss() is only available for floating point groups");
        for(GroupSpec<T> *g : groups_) {
            g->has_bigauss = true;
            g->start_sigma1 = sigma1;
            g->start_sigma2 = sigma2;
            g->start_delta = delta;
            g->start_ad_prob = ad_prob;
            g->bigauss.sigma_sigma1 = sigma_sigma1;
            g->bigauss.sigma_sigma2 = sigma_sigma2;
            g->bigauss.sigma_delta = sigma_delta;
            g->bigauss.min_sigma1 = min_sigma1;
            g->bigauss.max_sigma1 = max_sigma1;
            g->bigauss.min_sigma2 = min_sigma2;
            g->bigauss.max_sigma2 = max_sigma2;
            g->bigauss.min_delta = min_delta;
            g->bigauss.max_delta = max_delta;
            g->bigauss.min_ad_prob = min_ad_prob;
            g->bigauss.max_ad_prob = max_ad_prob;
            g->bigauss.adapt_ad_prob = adapt_ad_prob;
            g->bigauss.adaption_threshold = adaption_threshold;
            g->bigauss.use_symmetric_sigmas = use_symmetric_sigmas;
            g->bigauss.mode = mode;
        }
        return *this;
    }

    /**
     * @brief Attaches an integer Gauss adaptor to the spanned group(s) (int32 channel only).
     *
     * The Gaussian is drawn in double precision and cast to int32; sigma is therefore a double.
     *
     * @param sigma The initial step width (standard deviation) of the Gaussian
     * @param sigma_sigma The self-adaption strength applied to sigma each generation
     * @param min_sigma The lower clamp for sigma during self-adaption
     * @param max_sigma The upper clamp for sigma during self-adaption
     * @param ad_prob The probability that a given parameter is adapted
     * @param adapt_ad_prob The self-adaption strength applied to ad_prob (default 0, i.e. fixed)
     * @param adaption_threshold The number of calls after which sigma is self-adapted (default 1)
     * @param mode The adaption mode (default WITHPROBABILITY)
     * @param min_ad_prob The lower clamp for ad_prob during self-adaption (default 0)
     * @param max_ad_prob The upper clamp for ad_prob during self-adaption (default 1)
     * @return A reference to this handle, for fluent chaining
     */
    // NOLINTNEXTLINE(readability-function-size) -- flat field-assignment sweep whose parameter list deliberately mirrors GGenomeBuilder's ParamHandle::intGauss() one-for-one; splitting would just relocate the same assignment list without shrinking the mirrored interface
    GroupConfigHandle &intGauss(
        double sigma,
        double sigma_sigma,
        double min_sigma,
        double max_sigma,
        double ad_prob,
        double adapt_ad_prob = 0.,
        std::uint32_t adaption_threshold = 1,
        adaptionMode mode = adaptionMode::WITHPROBABILITY,
        double min_ad_prob = 0.,
        double max_ad_prob = 1.
    ) {
        static_assert(std::is_same_v<T, std::int32_t>, "intGauss() is only available for int32 groups");
        for(GroupSpec<T> *g : groups_) {
            g->has_gauss = true;
            g->start_sigma = sigma;
            g->start_ad_prob = ad_prob;
            g->gauss.sigma_sigma = sigma_sigma;
            g->gauss.min_sigma = min_sigma;
            g->gauss.max_sigma = max_sigma;
            g->gauss.min_ad_prob = min_ad_prob;
            g->gauss.max_ad_prob = max_ad_prob;
            g->gauss.adapt_ad_prob = adapt_ad_prob;
            g->gauss.adaption_threshold = adaption_threshold;
            g->gauss.mode = mode;
        }
        return *this;
    }

    /**
     * @brief Attaches a flip adaptor to the spanned group(s) (int32 / bool channels only).
     *
     * @param ad_prob The probability that a given parameter is flipped
     * @param adapt_ad_prob The self-adaption strength applied to ad_prob (default 0, i.e. fixed)
     * @param min_ad_prob The lower clamp for ad_prob during self-adaption (default 0)
     * @param max_ad_prob The upper clamp for ad_prob during self-adaption (default 1)
     * @param mode The adaption mode (default WITHPROBABILITY)
     * @return A reference to this handle, for fluent chaining
     */
    GroupConfigHandle &flip(
        double ad_prob,
        double adapt_ad_prob = 0.,
        double min_ad_prob = 0.,
        double max_ad_prob = 1.,
        adaptionMode mode = adaptionMode::WITHPROBABILITY
    ) {
        static_assert(std::is_integral_v<T>, "flip() is only available for integer / boolean groups");
        for(GroupSpec<T> *g : groups_) {
            g->has_flip = true;
            g->start_ad_prob = ad_prob;
            g->flip.min_ad_prob = min_ad_prob;
            g->flip.max_ad_prob = max_ad_prob;
            g->flip.adapt_ad_prob = adapt_ad_prob;
            g->flip.mode = mode;
        }
        return *this;
    }

    /**
     * @brief Sets the adaption mode for the spanned group(s) (NEVER means the group is not adapted).
     *
     * @param mode The adaption mode to apply to every adaptor kind of the spanned groups
     * @return A reference to this handle, for fluent chaining
     */
    GroupConfigHandle &adaptionMode(Gem::Geneva::adaptionMode mode) {
        for(GroupSpec<T> *g : groups_) {
            g->gauss.mode = mode;
            g->bigauss.mode = mode;
            g->flip.mode = mode;
            g->active = (mode != adaptionMode::NEVER);
        }
        return *this;
    }

    /**
     * @brief The number of groups this handle spans.
     *
     * @return The count of group specs configured by this handle
     */
    [[nodiscard]] std::size_t size() const { return groups_.size(); }

private:
    std::vector<GroupSpec<T> *> groups_;
};

/******************************************************************************/
/**
 * A fluent handle aggregating, by channel, every group that carries one interned label. It forwards an
 * adaptor setting to the groups of the channels for which that setting is valid (gauss / biGauss → the
 * FP groups; intGauss → the int32 groups; flip → the int32 + bool groups; adaptionMode → all). A label
 * usually tags a single channel (e.g. "position" → the FP cx/cy groups), but the aggregation is general.
 */
class GLabelConfigHandle {
public:
    /**
     * @brief Constructs a label handle aggregating the per-channel group handles tagged with one label.
     *
     * @param d The handle spanning the labelled double groups
     * @param f The handle spanning the labelled float groups
     * @param i The handle spanning the labelled int32 groups
     * @param b The handle spanning the labelled bool groups
     */
    GLabelConfigHandle(
        GroupConfigHandle<double> d,
        GroupConfigHandle<float> f,
        GroupConfigHandle<std::int32_t> i,
        GroupConfigHandle<bool> b
    )
      : d_(std::move(d))
      , f_(std::move(f))
      , i_(std::move(i))
      , b_(std::move(b)) {
        /* nothing */
    }

    /**
     * @brief Applies a Gauss adaptor to the labelled FP groups (passed in double width; the float groups are narrowed).
     *
     * @param sigma The initial step width (standard deviation) of the Gaussian
     * @param sigma_sigma The self-adaption strength applied to sigma each generation
     * @param min_sigma The lower clamp for sigma during self-adaption
     * @param max_sigma The upper clamp for sigma during self-adaption
     * @param ad_prob The probability that a given parameter is adapted
     * @param adapt_ad_prob The self-adaption strength applied to ad_prob (default 0, i.e. fixed)
     * @param adaption_threshold The number of calls after which sigma is self-adapted (default 1)
     * @param mode The adaption mode (default WITHPROBABILITY)
     * @param min_ad_prob The lower clamp for ad_prob during self-adaption (default 0)
     * @param max_ad_prob The upper clamp for ad_prob during self-adaption (default 1)
     * @return A reference to this handle, for fluent chaining
     */
    // NOLINTNEXTLINE(readability-function-size) -- flat forwarding sweep over the labelled FP channels (double + float), whose parameter list mirrors GroupConfigHandle::gauss() one-for-one; splitting would just relocate the same forwarding list
    GLabelConfigHandle &gauss(
        double sigma,
        double sigma_sigma,
        double min_sigma,
        double max_sigma,
        double ad_prob,
        double adapt_ad_prob = 0.,
        std::uint32_t adaption_threshold = 1,
        adaptionMode mode = adaptionMode::WITHPROBABILITY,
        double min_ad_prob = 0.,
        double max_ad_prob = 1.
    ) {
        if(d_.size() > 0) {
            d_.gauss(sigma, sigma_sigma, min_sigma, max_sigma, ad_prob, adapt_ad_prob, adaption_threshold, mode, min_ad_prob, max_ad_prob);
        }
        if(f_.size() > 0) {
            f_.gauss(
                static_cast<float>(sigma), static_cast<float>(sigma_sigma), static_cast<float>(min_sigma),
                static_cast<float>(max_sigma), static_cast<float>(ad_prob), static_cast<float>(adapt_ad_prob),
                adaption_threshold, mode, static_cast<float>(min_ad_prob), static_cast<float>(max_ad_prob)
            );
        }
        return *this;
    }

    /**
     * @brief Applies a bi-Gauss adaptor to the labelled floating-point (double + float) groups,
     * mirroring the per-index handle's biGauss() so a labelled bi-Gauss group is authorable by name.
     *
     * @param sigma1 The initial step width of the first Gaussian
     * @param sigma_sigma1 The self-adaption strength applied to sigma1
     * @param min_sigma1 The lower clamp for sigma1
     * @param max_sigma1 The upper clamp for sigma1
     * @param sigma2 The initial step width of the second Gaussian
     * @param sigma_sigma2 The self-adaption strength applied to sigma2
     * @param min_sigma2 The lower clamp for sigma2
     * @param max_sigma2 The upper clamp for sigma2
     * @param delta The initial distance between the two Gaussian peaks
     * @param sigma_delta The self-adaption strength applied to delta
     * @param min_delta The lower clamp for delta
     * @param max_delta The upper clamp for delta
     * @param ad_prob The probability that a given parameter is adapted
     * @param use_symmetric_sigmas If true, both Gaussians share a single sigma (default false)
     * @param adapt_ad_prob The self-adaption strength applied to ad_prob (default 0, i.e. fixed)
     * @param adaption_threshold The number of calls after which sigmas are self-adapted (default 1)
     * @param mode The adaption mode (default WITHPROBABILITY)
     * @param min_ad_prob The lower clamp for ad_prob during self-adaption (default 0)
     * @param max_ad_prob The upper clamp for ad_prob during self-adaption (default 1)
     * @return A reference to this handle, for fluent chaining
     */
    // NOLINTNEXTLINE(readability-function-size) -- flat forwarding sweep over the labelled FP channels (double + float), whose parameter list mirrors GroupConfigHandle::biGauss() one-for-one; splitting would just relocate the same forwarding list
    GLabelConfigHandle &biGauss(
        double sigma1,
        double sigma_sigma1,
        double min_sigma1,
        double max_sigma1,
        double sigma2,
        double sigma_sigma2,
        double min_sigma2,
        double max_sigma2,
        double delta,
        double sigma_delta,
        double min_delta,
        double max_delta,
        double ad_prob,
        bool use_symmetric_sigmas = false,
        double adapt_ad_prob = 0.,
        std::uint32_t adaption_threshold = 1,
        adaptionMode mode = adaptionMode::WITHPROBABILITY,
        double min_ad_prob = 0.,
        double max_ad_prob = 1.
    ) {
        if(d_.size() > 0) {
            d_.biGauss(
                sigma1, sigma_sigma1, min_sigma1, max_sigma1, sigma2, sigma_sigma2, min_sigma2,
                max_sigma2, delta, sigma_delta, min_delta, max_delta, ad_prob,
                use_symmetric_sigmas, adapt_ad_prob, adaption_threshold, mode, min_ad_prob,
                max_ad_prob
            );
        }
        if(f_.size() > 0) {
            f_.biGauss(
                static_cast<float>(sigma1), static_cast<float>(sigma_sigma1),
                static_cast<float>(min_sigma1), static_cast<float>(max_sigma1),
                static_cast<float>(sigma2), static_cast<float>(sigma_sigma2),
                static_cast<float>(min_sigma2), static_cast<float>(max_sigma2),
                static_cast<float>(delta), static_cast<float>(sigma_delta),
                static_cast<float>(min_delta), static_cast<float>(max_delta),
                static_cast<float>(ad_prob), use_symmetric_sigmas,
                static_cast<float>(adapt_ad_prob), adaption_threshold, mode,
                static_cast<float>(min_ad_prob), static_cast<float>(max_ad_prob)
            );
        }
        return *this;
    }

    /**
     * @brief Applies an integer Gauss adaptor to the labelled int32 groups.
     *
     * @param sigma The initial step width (standard deviation) of the Gaussian
     * @param sigma_sigma The self-adaption strength applied to sigma each generation
     * @param min_sigma The lower clamp for sigma during self-adaption
     * @param max_sigma The upper clamp for sigma during self-adaption
     * @param ad_prob The probability that a given parameter is adapted
     * @param adapt_ad_prob The self-adaption strength applied to ad_prob (default 0, i.e. fixed)
     * @param adaption_threshold The number of calls after which sigma is self-adapted (default 1)
     * @param mode The adaption mode (default WITHPROBABILITY)
     * @param min_ad_prob The lower clamp for ad_prob during self-adaption (default 0)
     * @param max_ad_prob The upper clamp for ad_prob during self-adaption (default 1)
     * @return A reference to this handle, for fluent chaining
     */
    // NOLINTNEXTLINE(readability-function-size) -- flat forwarding sweep to the labelled int32 channel, whose parameter list mirrors GroupConfigHandle::intGauss() one-for-one; splitting would just relocate the same forwarding list
    GLabelConfigHandle &intGauss(
        double sigma,
        double sigma_sigma,
        double min_sigma,
        double max_sigma,
        double ad_prob,
        double adapt_ad_prob = 0.,
        std::uint32_t adaption_threshold = 1,
        adaptionMode mode = adaptionMode::WITHPROBABILITY,
        double min_ad_prob = 0.,
        double max_ad_prob = 1.
    ) {
        if(i_.size() > 0) {
            i_.intGauss(sigma, sigma_sigma, min_sigma, max_sigma, ad_prob, adapt_ad_prob, adaption_threshold, mode, min_ad_prob, max_ad_prob);
        }
        return *this;
    }

    /**
     * @brief Applies a flip adaptor to the labelled int32 + bool groups.
     *
     * @param ad_prob The probability that a given parameter is flipped
     * @param adapt_ad_prob The self-adaption strength applied to ad_prob (default 0, i.e. fixed)
     * @param min_ad_prob The lower clamp for ad_prob during self-adaption (default 0)
     * @param max_ad_prob The upper clamp for ad_prob during self-adaption (default 1)
     * @param mode The adaption mode (default WITHPROBABILITY)
     * @return A reference to this handle, for fluent chaining
     */
    GLabelConfigHandle &flip(
        double ad_prob,
        double adapt_ad_prob = 0.,
        double min_ad_prob = 0.,
        double max_ad_prob = 1.,
        adaptionMode mode = adaptionMode::WITHPROBABILITY
    ) {
        if(i_.size() > 0) {
            i_.flip(ad_prob, adapt_ad_prob, min_ad_prob, max_ad_prob, mode);
        }
        if(b_.size() > 0) {
            b_.flip(ad_prob, adapt_ad_prob, min_ad_prob, max_ad_prob, mode);
        }
        return *this;
    }

    /**
     * @brief Sets the adaption mode for every labelled group, across all channels.
     *
     * @param mode The adaption mode to apply to all labelled groups
     * @return A reference to this handle, for fluent chaining
     */
    GLabelConfigHandle &adaptionMode(Gem::Geneva::adaptionMode mode) {
        if(d_.size() > 0) { d_.adaptionMode(mode); }
        if(f_.size() > 0) { f_.adaptionMode(mode); }
        if(i_.size() > 0) { i_.adaptionMode(mode); }
        if(b_.size() > 0) { b_.adaptionMode(mode); }
        return *this;
    }

private:
    GroupConfigHandle<double> d_;
    GroupConfigHandle<float> f_;
    GroupConfigHandle<std::int32_t> i_;
    GroupConfigHandle<bool> b_;
};

/******************************************************************************/
/**
 * The base of the OA-owned adaption configuration, and the SOLE home of the per-group adaptor settings
 * (Gauss / bi-Gauss / integer-Gauss / flip + their seeds): the GGenomeLayout is structure-only, so this
 * config -- owned by the optimization algorithm, not the genome -- is where adaption intent lives. The
 * settings are addressed by channel + index or by interned label, and the config is built FROM a genome
 * so it can only describe groups that actually exist. Concretely it holds one GroupSpec vector per
 * channel plus the interned label table.
 *
 * The base owns the OA-agnostic machinery: existence-validated authoring, a structural signature +
 * checkConsistency(genome) cross-check (so a genome and a config can be verified as belonging
 * together), label resolution, and the install-state-into-aux hook. A per-OA subclass (e.g.
 * GEAAdaptionConfig) may add algorithm-specific configuration on top.
 */
class GAdaptionConfigBase {
public:
    /**
     * @brief Builds a config describing exactly the groups of the passed genome.
     *
     * @param genome The genome whose (structure-only) layout is snapshotted into this config
     */
    explicit GAdaptionConfigBase(const GGenome &genome) { initFrom(*genome.getLayout()); }
    /**
     * @brief Builds a config describing exactly the groups of the passed layout.
     *
     * @param layout The genome layout whose group structure is snapshotted into this config
     */
    explicit GAdaptionConfigBase(const GGenomeLayout &layout) { initFrom(layout); }

    /** @brief The copy constructor. */
    GAdaptionConfigBase(const GAdaptionConfigBase &) = default;
    /** @brief The move constructor. */
    GAdaptionConfigBase(GAdaptionConfigBase &&) = default;
    /** @brief The copy assignment operator. */
    GAdaptionConfigBase &operator=(const GAdaptionConfigBase &) = default;
    /** @brief The move assignment operator. */
    GAdaptionConfigBase &operator=(GAdaptionConfigBase &&) = default;
    /** @brief The virtual destructor. */
    virtual ~GAdaptionConfigBase() = default;

    /***************************************************************************/
    // Authoring -- per channel + index (throws on an out-of-range index).

    /**
     * @brief Returns a fluent handle for authoring the indexed double group.
     *
     * @param index The position of the target group within the double channel
     * @return A handle spanning that single double group (throws if index is out of range)
     */
    GroupConfigHandle<double> groupDouble(std::size_t index) { return oneHandle(d_, index, "double"); }
    /**
     * @brief Returns a fluent handle for authoring the indexed float group.
     *
     * @param index The position of the target group within the float channel
     * @return A handle spanning that single float group (throws if index is out of range)
     */
    GroupConfigHandle<float> groupFloat(std::size_t index) { return oneHandle(f_, index, "float"); }
    /**
     * @brief Returns a fluent handle for authoring the indexed int32 group.
     *
     * @param index The position of the target group within the int32 channel
     * @return A handle spanning that single int32 group (throws if index is out of range)
     */
    GroupConfigHandle<std::int32_t> groupInt32(std::size_t index) { return oneHandle(i_, index, "int32"); }
    /**
     * @brief Returns a fluent handle for authoring the indexed bool group.
     *
     * @param index The position of the target group within the bool channel
     * @return A handle spanning that single bool group (throws if index is out of range)
     */
    GroupConfigHandle<bool> groupBool(std::size_t index) { return oneHandle(b_, index, "bool"); }

    /**
     * @brief Authoring by interned label -- spans every group that carries it (throws if none does).
     *
     * @param name The interned label string identifying the groups to author
     * @return A label handle aggregating, across all channels, every group tagged with that label
     */
    GLabelConfigHandle forLabel(const std::string &name) {
        const std::int32_t id = labelId(name);
        if(id < 0) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GAdaptionConfigBase::forLabel(): Error!" << '\n'
                << "Label \"" << name << "\" is not present in the genome." << '\n'
            );
        }
        GLabelConfigHandle h(
            labelHandle(d_, id), labelHandle(f_, id), labelHandle(i_, id), labelHandle(b_, id)
        );
        if(d_labelEmpty(id) && f_labelEmpty(id) && i_labelEmpty(id) && b_labelEmpty(id)) {
            // Defensive: a registered label string with no group carrying its id (should not happen).
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GAdaptionConfigBase::forLabel(): Error!" << '\n'
                << "Label \"" << name << "\" resolves to no group." << '\n'
            );
        }
        return h;
    }

    /***************************************************************************/
    // Validation.

    /**
     * @brief Throws unless the passed genome has exactly the structure this config was authored against.
     *
     * @param genome The genome whose layout is cross-checked against this config's group structure
     */
    void checkConsistency(const GGenome &genome) const { checkConsistency(*genome.getLayout()); }

    /**
     * @brief Throws unless the passed layout has exactly the structure this config was authored against.
     *
     * @param layout The genome layout whose per-channel group structure is cross-checked against this config
     */
    void checkConsistency(const GGenomeLayout &layout) const {
        checkChannel(d_, layout.d.groups, "double");
        checkChannel(f_, layout.f.groups, "float");
        checkChannel(i_, layout.i.groups, "int32");
        checkChannel(b_, layout.b.groups, "bool");
    }

    /***************************************************************************/
    // Label resolution (read access).

    /**
     * @brief Resolves a label string to its id, or -1 if it is not present.
     *
     * @param name The interned label string to look up
     * @return The label's zero-based id, or -1 if the label is not present
     */
    [[nodiscard]] std::int32_t labelId(const std::string &name) const {
        for(std::size_t k = 0; k < labels_.size(); ++k) {
            if(labels_[k] == name) {
                return static_cast<std::int32_t>(k);
            }
        }
        return -1;
    }

    /**
     * @brief Resolves a label string to every group it tags, across all channels (one-to-many).
     *
     * @param name The interned label string to look up
     * @return A vector of GroupRef entries (channel + group index) for every group carrying the label; empty if the label is absent
     */
    [[nodiscard]] std::vector<GroupRef> groupsForLabel(const std::string &name) const {
        std::vector<GroupRef> out;
        const std::int32_t id = labelId(name);
        if(id < 0) {
            return out;
        }
        collect(d_, ChannelTag::Double, id, out);
        collect(f_, ChannelTag::Float, id, out);
        collect(i_, ChannelTag::Int32, id, out);
        collect(b_, ChannelTag::Bool, id, out);
        return out;
    }

    /***************************************************************************/
    // State install.

    /**
     * @brief Seeds the per-group adaption state blocks into an OA-owned auxiliary store (the slot's
     * scratch) from this config (one block per adaptor kind + channel that is actually used). The
     * per-group adaption state is OA scratch carried on the individual itself (its GAuxiliaryStore),
     * so the seeding targets a GAuxiliaryStore directly. The seeds also serve as the reset targets used
     * by the stall-reset free function.
     *
     * @param scratch The OA-owned auxiliary store (slot scratch) into which the seeded adaption state blocks are installed
     */
    void installInto(GAuxiliaryStore &scratch) const {
        using namespace Gem::Geneva::Genome;
        seedGauss(scratch, d_, AUXKEY_GAUSS_DOUBLE);
        seedGauss(scratch, f_, AUXKEY_GAUSS_FLOAT);
        seedGauss(scratch, i_, AUXKEY_GAUSS_INT); // GaussState<adaption_fp_t<int32>> = GaussState<double>
        seedBiGauss(scratch, d_, AUXKEY_BIGAUSS_DOUBLE);
        seedBiGauss(scratch, f_, AUXKEY_BIGAUSS_FLOAT);
        seedFlip(scratch, i_, AUXKEY_FLIP_INT);
        seedFlip(scratch, b_, AUXKEY_FLIP_BOOL);
    }

    /***************************************************************************/
    // Read access for the adaption free functions.

    /** @brief Read access to the double channel's group specs. @return The vector of double group specs. */
    [[nodiscard]] const std::vector<GroupSpec<double>> &doubleGroups() const { return d_; }
    /** @brief Read access to the float channel's group specs. @return The vector of float group specs. */
    [[nodiscard]] const std::vector<GroupSpec<float>> &floatGroups() const { return f_; }
    /** @brief Read access to the int32 channel's group specs. @return The vector of int32 group specs. */
    [[nodiscard]] const std::vector<GroupSpec<std::int32_t>> &int32Groups() const { return i_; }
    /** @brief Read access to the bool channel's group specs. @return The vector of bool group specs. */
    [[nodiscard]] const std::vector<GroupSpec<bool>> &boolGroups() const { return b_; }
    /** @brief Read access to the interned label table. @return The vector of label strings (indexed by label id). */
    [[nodiscard]] const std::vector<std::string> &labels() const { return labels_; }

    /***************************************************************************/
    // Adaption-retry policy (moved off the individual). Bounds adaptIndividual()'s retry loop.

    /** @brief Sets the max consecutive unsuccessful adaptions per adaption (0 disables the check).
     *  @param max_unsuccessful_adaptions The maximum number of consecutive unsuccessful adaptions */
    void setMaxUnsuccessfulAdaptions(std::size_t max_unsuccessful_adaptions) {
        max_unsuccessful_adaptions_ = max_unsuccessful_adaptions;
    }
    /** @brief @return The max consecutive unsuccessful adaptions per adaption */
    [[nodiscard]] std::size_t getMaxUnsuccessfulAdaptions() const { return max_unsuccessful_adaptions_; }

    /** @brief Sets the max adaption retries until a valid solution is found (0 disables the check).
     *  @param max_retries_until_valid The maximum number of retries */
    void setMaxRetriesUntilValid(std::size_t max_retries_until_valid) {
        max_retries_until_valid_ = max_retries_until_valid;
    }
    /** @brief @return The max adaption retries until a valid solution is found */
    [[nodiscard]] std::size_t getMaxRetriesUntilValid() const { return max_retries_until_valid_; }

    /***************************************************************************/
    // Step-size-control strategy (used by GEvolutionaryAlgorithm; default reproduces the
    // classic σSA behaviour, so the stock EA is unaffected).

    /** @brief Selects the step-size-control strategy applied on top of the per-group Gauss adaptors.
     *  @param sc The strategy (SELF_ADAPT, SELF_ADAPT_SCALED, ONE_FIFTH or CSA)
     *  @return A reference to this config, for chaining */
    GAdaptionConfigBase &setStepControl(stepControl sc) {
        step_control_ = sc;
        return *this;
    }
    /** @brief The configured step-size-control strategy. @return The strategy currently in effect. */
    [[nodiscard]] stepControl getStepControl() const { return step_control_; }

    /** @brief Sets the learning-rate constant c used by SELF_ADAPT_SCALED (τ = c/sqrt(2n)).
     *  @param c The constant (≈1 by convention). @return A reference to this config, for chaining. */
    GAdaptionConfigBase &setLearningRateConstant(double c) {
        learning_rate_c_ = c;
        return *this;
    }
    /** @brief The learning-rate constant c. @return The constant used by the scaled self-adaption rate. */
    [[nodiscard]] double getLearningRateConstant() const { return learning_rate_c_; }

    /**
     * @brief The total number of floating-point parameters that are actually adapted by a Gauss /
     * bi-Gauss adaptor across the double + float channels. This is the dimension n that the
     * dimension-scaled step controllers reason about. Computed live from the group structure.
     *
     * @return The number of adapted FP parameters (Σ group length over active Gauss / bi-Gauss groups).
     */
    [[nodiscard]] std::size_t adaptedDimension() const {
        std::size_t n = 0;
        for(const GroupSpec<double> &g : d_) {
            if((g.has_gauss || g.has_bigauss) && g.active) {
                n += g.len;
            }
        }
        for(const GroupSpec<float> &g : f_) {
            if((g.has_gauss || g.has_bigauss) && g.active) {
                n += g.len;
            }
        }
        return n;
    }

    /**
     * @brief Applies the SELF_ADAPT_SCALED transformation: rewrites every Gauss / bi-Gauss group's
     * σ self-adaption rate (sigma_sigma) to the dimension-scaled textbook value. A shared-σ group (len
     * > 1, one σ for the whole group) uses the global rate c/sqrt(2n); a per-coordinate group (len == 1)
     * uses c/sqrt(2*sqrt(n)). A no-op when there are no adapted FP parameters. Idempotent enough for
     * setup use (the OA calls it once at init()).
     */
    void applyScaledSelfAdaptionRate() {
        const std::size_t n = adaptedDimension();
        if(n == 0) {
            return;
        }
        const double c = learning_rate_c_;
        const double tau_global = c / std::sqrt(2. * static_cast<double>(n));
        const double tau_coord = c / std::sqrt(2. * std::sqrt(static_cast<double>(n)));
        auto rescale = [&]<typename T>(std::vector<GroupSpec<T>> &groups) {
            for(GroupSpec<T> &g : groups) {
                const auto rate = static_cast<adaption_fp_t<T>>(g.len == 1 ? tau_coord : tau_global);
                if(g.has_gauss) {
                    g.gauss.sigma_sigma = rate;
                }
                if(g.has_bigauss) {
                    g.bigauss.sigma_sigma1 = rate;
                    g.bigauss.sigma_sigma2 = rate;
                    g.bigauss.sigma_delta = rate;
                }
            }
        };
        rescale(d_);
        rescale(f_);
    }

    /**
     * @brief Suppresses the per-group log-normal σ self-adaption on every Gauss / bi-Gauss group, so
     * an external controller (ONE_FIFTH / CSA) owns σ alone. Implemented by setting sigma_sigma to 0
     * (a zero-rate log-normal step is the identity) — the group still applies its `N(0, σ)` value step
     * (in the normalized internal coordinate) with whatever σ the OA wrote into its state, but never
     * self-adapts σ on its own.
     */
    void suppressPerGroupSigmaSelfAdaption() {
        auto zero = [&]<typename T>(std::vector<GroupSpec<T>> &groups) {
            for(GroupSpec<T> &g : groups) {
                g.gauss.sigma_sigma = adaption_fp_t<T>(0);
                g.bigauss.sigma_sigma1 = adaption_fp_t<T>(0);
                g.bigauss.sigma_sigma2 = adaption_fp_t<T>(0);
                g.bigauss.sigma_delta = adaption_fp_t<T>(0);
            }
        };
        zero(d_);
        zero(f_);
    }

protected:
    /***************************************************************************/
    /**
     * @brief Snapshots the group STRUCTURE from a (structure-only) genome layout into this config's group specs.
     *
     * The adaptor fields stay default-off until the caller authors them via the fluent API.
     *
     * @param layout The genome layout providing the per-channel group structure and the label table
     */
    void initFrom(const GGenomeLayout &layout) {
        copyStructure(layout.d.groups, d_);
        copyStructure(layout.f.groups, f_);
        copyStructure(layout.i.groups, i_);
        copyStructure(layout.b.groups, b_);
        labels_ = layout.labels;
    }

private:
    /***************************************************************************/
    /**
     * @brief Copies the structural fields of every source group into a fresh, adaptor-off group spec vector.
     *
     * @tparam T The channel's parameter type
     * @param src The structure-only source groups taken from the genome layout
     * @param dst The destination group spec vector (cleared and refilled with structure-only specs)
     */
    template <typename T>
    static void
    copyStructure(const std::vector<GroupStructure<T>> &src, std::vector<GroupSpec<T>> &dst) {
        dst.clear();
        dst.reserve(src.size());
        for(const GroupStructure<T> &s : src) {
            GroupSpec<T> g;
            g.start = s.start;
            g.len = s.len;
            g.label_id = s.label_id;
            g.active = s.active;
            dst.push_back(g);
        }
    }

    /***************************************************************************/
    /**
     * @brief Builds a single-group handle for the indexed group, throwing on an out-of-range index.
     *
     * @tparam T The channel's parameter type
     * @param groups The channel's group spec vector
     * @param index The position of the target group within that vector
     * @param channel The channel name used only in the error message (e.g. "double")
     * @return A handle spanning the one selected group
     */
    template <typename T>
    static GroupConfigHandle<T>
    oneHandle(std::vector<GroupSpec<T>> &groups, std::size_t index, const char *channel) {
        if(index >= groups.size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GAdaptionConfigBase::group" << channel << "(): Error!" << '\n'
                << "Group index " << index << " is out of range (" << groups.size() << " groups)." << '\n'
            );
        }
        return GroupConfigHandle<T>(std::vector<GroupSpec<T> *>{&groups[index]});
    }

    /**
     * @brief Builds a handle spanning every group in one channel that carries the given label id.
     *
     * @tparam T The channel's parameter type
     * @param groups The channel's group spec vector
     * @param id The label id to match against each group's label_id
     * @return A handle spanning all matching groups (possibly empty)
     */
    template <typename T>
    static GroupConfigHandle<T> labelHandle(std::vector<GroupSpec<T>> &groups, std::int32_t id) {
        std::vector<GroupSpec<T> *> targets;
        for(GroupSpec<T> &g : groups) {
            if(g.label_id == id) {
                targets.push_back(&g);
            }
        }
        return GroupConfigHandle<T>(std::move(targets));
    }

    /**
     * @brief Appends a GroupRef for every group in one channel that carries the given label id.
     *
     * @tparam T The channel's parameter type
     * @param groups The channel's group spec vector
     * @param tag The channel tag stamped into each emitted GroupRef
     * @param id The label id to match against each group's label_id
     * @param out The output vector that matching GroupRef entries are appended to
     */
    template <typename T>
    static void
    collect(const std::vector<GroupSpec<T>> &groups, ChannelTag tag, std::int32_t id, std::vector<GroupRef> &out) {
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(groups[gi].label_id == id) {
                out.push_back(GroupRef{.channel=tag, .index=gi});
            }
        }
    }

    /**
     * @brief Cross-checks one channel's config groups against the layout's groups, throwing on any mismatch.
     *
     * Verifies that the group counts match and that each group's start, len and label_id agree.
     *
     * @tparam T The channel's parameter type
     * @param cfg This config's group spec vector for the channel
     * @param layout The genome layout's structure-only group vector for the same channel
     * @param channel The channel name used only in the error message (e.g. "double")
     */
    template <typename T>
    static void
    checkChannel(const std::vector<GroupSpec<T>> &cfg, const std::vector<GroupStructure<T>> &layout, const char *channel) {
        if(cfg.size() != layout.size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GAdaptionConfigBase::checkChannel(): Error!" << '\n'
                << "Group count mismatch on the " << channel << " channel: config has " << cfg.size()
                << ", genome has " << layout.size() << '\n'
            );
        }
        for(auto const &[gi, c, l] : std::views::zip(std::views::iota(0uz), cfg, layout)) {
            if(c.start != l.start || c.len != l.len || c.label_id != l.label_id) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GAdaptionConfigBase::checkChannel(): Error!" << '\n'
                    << "Structural mismatch on the " << channel << " channel, group " << gi << '\n'
                );
            }
        }
    }

    /** @brief True if no double group carries the given label id. @param id The label id to check. @return true if absent from the double channel. */
    [[nodiscard]] bool d_labelEmpty(std::int32_t id) const { return noneWithLabel(d_, id); }
    /** @brief True if no float group carries the given label id. @param id The label id to check. @return true if absent from the float channel. */
    [[nodiscard]] bool f_labelEmpty(std::int32_t id) const { return noneWithLabel(f_, id); }
    /** @brief True if no int32 group carries the given label id. @param id The label id to check. @return true if absent from the int32 channel. */
    [[nodiscard]] bool i_labelEmpty(std::int32_t id) const { return noneWithLabel(i_, id); }
    /** @brief True if no bool group carries the given label id. @param id The label id to check. @return true if absent from the bool channel. */
    [[nodiscard]] bool b_labelEmpty(std::int32_t id) const { return noneWithLabel(b_, id); }

    /**
     * @brief Returns whether no group in the channel carries the given label id.
     *
     * @tparam T The channel's parameter type
     * @param groups The channel's group spec vector
     * @param id The label id to match against each group's label_id
     * @return true if no group has that label id, false otherwise
     */
    template <typename T>
    static bool noneWithLabel(const std::vector<GroupSpec<T>> &groups, std::int32_t id) {
        for(const GroupSpec<T> &g : groups) {
            if(g.label_id == id) {
                return false;
            }
        }
        return true;
    }

    /***************************************************************************/
    // State seeding helpers (mirror GGenome::installAdaptionStates).

    /**
     * @brief Installs and seeds a per-group Gauss state block into the scratch store (no-op if no group uses Gauss).
     *
     * @tparam T The channel's parameter type
     * @param scratch The auxiliary store into which the Gauss state block is installed
     * @param groups The channel's group specs supplying the start_sigma / start_ad_prob seeds
     * @param key The auxiliary-store key under which the Gauss state block is registered
     */
    template <typename T>
    static void seedGauss(GAuxiliaryStore &scratch, const std::vector<GroupSpec<T>> &groups, Gem::Geneva::Genome::AuxKey key) {
        using Gem::Geneva::Genome::GaussState;
        using Gem::Geneva::Genome::AuxScope;
        bool const any = std::ranges::any_of(groups, [](const GroupSpec<T> &g) { return g.has_gauss; });
        if(not any) {
            return;
        }
        scratch.installAuxBlock<GaussState<adaption_fp_t<T>>>(key, groups.size(), AuxScope::PerIndividual);
        std::span<GaussState<adaption_fp_t<T>>> const states = scratch.metaRecords<GaussState<adaption_fp_t<T>>>(key);
        for(auto const &[state, group] : std::views::zip(states, groups)) {
            state.sigma = group.start_sigma;
            state.ad_prob = group.start_ad_prob;
            state.counter = 0;
        }
    }

    /**
     * @brief Installs and seeds a per-group bi-Gauss state block into the scratch store (no-op if no group uses bi-Gauss).
     *
     * @tparam T The channel's parameter type
     * @param scratch The auxiliary store into which the bi-Gauss state block is installed
     * @param groups The channel's group specs supplying the start_sigma1/2, start_delta and start_ad_prob seeds
     * @param key The auxiliary-store key under which the bi-Gauss state block is registered
     */
    template <typename T>
    static void seedBiGauss(GAuxiliaryStore &scratch, const std::vector<GroupSpec<T>> &groups, Gem::Geneva::Genome::AuxKey key) {
        using Gem::Geneva::Genome::BiGaussState;
        using Gem::Geneva::Genome::AuxScope;
        bool const any = std::ranges::any_of(groups, [](const GroupSpec<T> &g) { return g.has_bigauss; });
        if(not any) {
            return;
        }
        scratch.installAuxBlock<BiGaussState<adaption_fp_t<T>>>(key, groups.size(), AuxScope::PerIndividual);
        std::span<BiGaussState<adaption_fp_t<T>>> const states = scratch.metaRecords<BiGaussState<adaption_fp_t<T>>>(key);
        for(auto const &[state, group] : std::views::zip(states, groups)) {
            state.sigma1 = group.start_sigma1;
            state.sigma2 = group.start_sigma2;
            state.delta = group.start_delta;
            state.ad_prob = group.start_ad_prob;
            state.counter = 0;
        }
    }

    /**
     * @brief Installs and seeds a per-group flip state block into the scratch store (no-op if no group uses flip).
     *
     * @tparam T The channel's parameter type
     * @param scratch The auxiliary store into which the flip state block is installed
     * @param groups The channel's group specs supplying the start_ad_prob seeds
     * @param key The auxiliary-store key under which the flip state block is registered
     */
    template <typename T>
    static void seedFlip(GAuxiliaryStore &scratch, const std::vector<GroupSpec<T>> &groups, Gem::Geneva::Genome::AuxKey key) {
        using Gem::Geneva::Genome::FlipState;
        using Gem::Geneva::Genome::AuxScope;
        bool const any = std::ranges::any_of(groups, [](const GroupSpec<T> &g) { return g.has_flip; });
        if(not any) {
            return;
        }
        scratch.installAuxBlock<FlipState>(key, groups.size(), AuxScope::PerIndividual);
        std::span<FlipState> const states = scratch.metaRecords<FlipState>(key);
        for(auto const &[state, group] : std::views::zip(states, groups)) {
            state.ad_prob = group.start_ad_prob;
        }
    }

    /***************************************************************************/
    // The config half of the genome's groups, per channel, + the interned label table.
    std::vector<GroupSpec<double>> d_;
    std::vector<GroupSpec<float>> f_;
    std::vector<GroupSpec<std::int32_t>> i_;
    std::vector<GroupSpec<bool>> b_;
    std::vector<std::string> labels_;

    /***************************************************************************/
    // Adaption-retry policy (moved off the individual: the retry loop is an OA-owned adaption concern,
    // not per-individual data). Bounds the "guarantee a change, then a valid solution" loop in
    // adaptIndividual(). Defaults reproduce the previous per-individual values.
    std::size_t max_unsuccessful_adaptions_ =
        Gem::Geneva::DEFMAXUNSUCCESSFULADAPTIONS; ///< Max consecutive unsuccessful adaptions per adaption (0 disables)
    std::size_t max_retries_until_valid_ =
        Gem::Geneva::DEFMAXRETRIESUNTILVALID; ///< Max adaption retries until a valid solution is found (0 disables)

    /***************************************************************************/
    // Step-size-control strategy + its tunables. The default reproduces the classic σSA behaviour, so
    // a config used by the stock EA is byte-identical to before.
    stepControl step_control_ = stepControl::SELF_ADAPT; ///< the step-size-control strategy
    double learning_rate_c_ = 1.;                        ///< the τ = c/sqrt(2n) constant for SELF_ADAPT_SCALED
};

/******************************************************************************/
/** @brief The evolutionary-algorithm adaption configuration. */
class GEAAdaptionConfig : public GAdaptionConfigBase {
public:
    using GAdaptionConfigBase::GAdaptionConfigBase;
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
