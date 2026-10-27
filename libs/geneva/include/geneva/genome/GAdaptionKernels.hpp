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
#include <cstdint>
#include <limits>
#include <span>

// Geneva headers go here
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "hap/GRandomDistributionsT.hpp"
#include "hap/GRandomT.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
/**
 * Data-oriented adaption kernels: the parameter-mutation mathematics expressed as stateless free
 * functions over a (config, state, values) triple. The config is the static, shared part
 * (sigma-adaption rate, bounds, mode, …); the state is the per-individual, per-group evolving part
 * (the current sigma / adaption probability / counter); both are plain POD so they live happily in
 * the shared adaption layout and the per-individual auxiliary store respectively, and the kernel has
 * no dependency on any parameter-object hierarchy.
 *
 * The Gauss kernel applies a gated `N(0, sigma)` value step (in the normalized internal coordinate,
 * whose interval has width 1, so sigma is a dimensionless fraction of the parameter's range) plus
 * log-normal self-adaption of the adaption probability and sigma, with a one-ULP "an adaption that
 * fires always changes the value/sigma" guarantee. The integer kernel, whose values are NOT normalized,
 * keeps an explicit range multiplier.
 *
 * Templated on the adaption floating-point type T (double for double parameters, float for float).
 */

/** @brief Static, shared Gauss-adaption configuration for one group of parameters. */
template <typename T>
struct GaussConfig {
    T sigma_sigma = T(0.8);                   ///< the log-normal self-adaption rate of sigma
    T min_sigma = T(0.001);                   ///< the lower bound of sigma
    T max_sigma = T(2.);                      ///< the upper bound of sigma
    T min_ad_prob = T(0.);                    ///< the lower bound of the adaption probability
    T max_ad_prob = T(1.);                    ///< the upper bound of the adaption probability
    T adapt_ad_prob = T(0.);                  ///< the self-adaption rate of the adaption probability (0 disables)
    T adapt_sigma_prob = T(0.);               ///< probability of a sigma self-adaption when adaption_threshold == 0
    std::uint32_t adaption_threshold = 1;     ///< sigma self-adapts every Nth adaption (0 ⇒ use adapt_sigma_prob)
    adaptionMode mode = adaptionMode::WITHPROBABILITY; ///< always / with-probability / never
};

/** @brief Per-individual, per-group evolving Gauss state. POD: standard-layout, trivially copyable. */
template <typename T>
struct GaussState {
    T sigma = T(1.);                          ///< the current mutation width
    T ad_prob = T(1.);                        ///< the current adaption probability
    std::uint32_t counter = 0;                ///< adaptions since the last sigma self-adaption
};

/******************************************************************************/
/**
 * @brief Adapts one group of values sharing a single GaussState.
 *
 * For each value the per-group adaption probability gates whether it mutates; a mutating value receives a
 * `N(0, sigma)` step in the normalized internal coordinate (with a one-ULP guarantee that a firing
 * adaption always changes the value), and sigma self-adapts log-normally on the threshold / probability
 * trigger. The adaption probability itself self-adapts once for the whole group up front. @p st is
 * updated in place.
 *
 * @tparam T The adaption floating-point type (double for double parameters, float for float).
 * @param cfg The static, shared Gauss configuration for this group (sigma bounds, self-adaption rates, mode).
 * @param st The per-individual evolving Gauss state (current sigma / adaption probability / counter); updated in place.
 * @param values The group's parameter values to adapt, in their normalized internal representation.
 * @param gr The per-individual random engine the mutation draws from.
 * @return The number of values that were actually adapted.
 *
 * @note Shared implementation behind the public adaptGaussGroup() function. @p vsDelta supplies
 *       the per-value gaussian step, drawn inline from the shared @c normal object; self-adaption
 *       and the gates also draw inline from @p gr.
 * @tparam ValueDelta A callable (g_normal_distribution<T>&, GRandomBase&, T sigma) -> T.
 */
template <typename T, typename ValueDelta>
// NOLINTNEXTLINE(readability-function-size) -- one coherent numeric kernel: ad_prob self-adaption, sigma self-adaption + trigger, and the value step share the same GaussState via closures, mirroring GAdaptorT::adapt()'s stages; splitting would scatter tightly coupled mutation math
std::size_t adaptGaussGroupImpl(
    const GaussConfig<T> &cfg,
    GaussState<T> &st,
    std::span<T> values,
    Gem::Hap::GRandomBase &gr,
    ValueDelta vsDelta
) {
    // Distribution objects hoisted out of the per-value loop (creating them per value is measurably slower).
    Gem::Hap::g_normal_distribution<T> normal;
    Gem::Hap::g_bernoulli_distribution bernoulli;
    using n_param = typename Gem::Hap::g_normal_distribution<T>::param_type;
    using b_param = Gem::Hap::g_bernoulli_distribution::param_type;

    // 1) Self-adapt the adaption probability once for the whole group (if requested).
    if(cfg.adapt_ad_prob > T(0.)) {
        st.ad_prob *= std::exp(normal(gr, n_param(T(0.), cfg.adapt_ad_prob)));
        Gem::Common::enforceRangeConstraint<T>(
            st.ad_prob,
            cfg.min_ad_prob,
            cfg.max_ad_prob,
            "adaptGaussGroup() / ad_prob"
        );
    }

    // Self-adapts sigma (log-normal step + one-ULP guarantee + range clamp), mirroring
    // GNumGaussAdaptorT::customAdaptAdaption.
    auto self_adapt_sigma = [&]() {
        const T sigma_before = st.sigma;
        const T sigma_mult = std::exp(normal(gr, n_param(T(0.), std::abs(cfg.sigma_sigma))));
        st.sigma *= sigma_mult;
        if(st.sigma == sigma_before) {
            const T dir = (sigma_mult < T(1.)) ? std::numeric_limits<T>::lowest()
                                               : std::numeric_limits<T>::max();
            st.sigma = std::nextafter(sigma_before, dir);
        }
        Gem::Common::enforceRangeConstraint<T>(
            st.sigma,
            cfg.min_sigma,
            cfg.max_sigma,
            "adaptGaussGroup() / sigma",
            false
        );
    };

    // The sigma self-adaption trigger, mirroring GAdaptorT::adaptAdaption.
    auto adapt_adaption = [&]() {
        if(cfg.adaption_threshold > 0) {
            if(++st.counter >= cfg.adaption_threshold) {
                st.counter = 0;
                self_adapt_sigma();
            }
        }
        else if(cfg.adapt_sigma_prob != T(0.)) {
            if(bernoulli(gr, b_param(std::abs(cfg.adapt_sigma_prob)))) {
                self_adapt_sigma();
            }
        }
    };

    // The value step (N(0, sigma) in the normalized internal coordinate + one-ULP guarantee), mirroring
    // GFPGaussAdaptorT::customAdaptions.
    auto gauss_step = [&](T &v) {
        const T before = v;
        const T delta = vsDelta(normal, gr, st.sigma);
        v = before + delta;
        if(v == before) {
            const T dir = (delta < T(0.)) ? std::numeric_limits<T>::lowest()
                                          : std::numeric_limits<T>::max();
            v = std::nextafter(before, dir);
        }
    };

    std::size_t n_adapted = 0;
    if(adaptionMode::WITHPROBABILITY == cfg.mode) {
        for(T &v : values) {
            if(bernoulli(gr, b_param(std::abs(st.ad_prob)))) {
                adapt_adaption();
                gauss_step(v);
                ++n_adapted;
            }
        }
    }
    else if(adaptionMode::ALWAYS == cfg.mode) {
        for(T &v : values) {
            adapt_adaption();
            gauss_step(v);
            ++n_adapted;
        }
    }
    // adaptionMode::NEVER: nothing to do.

    return n_adapted;
}

/******************************************************************************/
/**
 * @brief Adapts one Gauss group; the per-value N(0,sigma) step is drawn inline from @p gr.
 *
 * @tparam T The adaption floating-point type (double or float).
 * @param cfg The static, shared Gauss configuration for this group.
 * @param st The per-individual evolving Gauss state; updated in place.
 * @param values The group's parameter values to adapt, in their normalized internal representation.
 * @param gr The random engine for self-adaption, the gates, and the value step.
 * @return The number of values that were actually adapted.
 */
template <typename T>
std::size_t adaptGaussGroup(
    const GaussConfig<T> &cfg,
    GaussState<T> &st,
    std::span<T> values,
    Gem::Hap::GRandomBase &gr
) {
    using n_param = typename Gem::Hap::g_normal_distribution<T>::param_type;
    return adaptGaussGroupImpl<T>(
        cfg, st, values, gr,
        [](Gem::Hap::g_normal_distribution<T> &normal, Gem::Hap::GRandomBase &g, T sigma) {
            return normal(g, n_param(T(0.), sigma));
        }
    );
}

/******************************************************************************/
/**
 * The integer Gauss kernel: the mutation mathematics of GIntGaussAdaptorT (used e.g. by
 * GInt32GaussAdaptor), re-expressed in the same stateless (config, state, values) style as the FP
 * Gauss kernel. The self-adaption wrapper is IDENTICAL to the FP Gauss kernel -- the adaption
 * probability self-adapts log-normally, the per-value bernoulli gate is the same, and sigma
 * self-adapts log-normally on the threshold / probability trigger (sigma is a double, so it reuses the
 * GaussConfig<double> / GaussState<double> POD). Only the value step differs: instead of an FP delta it
 * adds a truncated gaussian integer step, with a guaranteed minimal change of +/-1 when the truncated step
 * is zero (mirroring GIntGaussAdaptorT::customAdaptions). There is NO fold in the kernel -- a
 * constrained integer folds into its range on read (GGenome / foldConstrainedInt), exactly as the
 * tree applies it via GConstrainedIntT.
 *
 * @param cfg The static, shared Gauss configuration for this group (sigma is a double; bounds, rates, mode).
 * @param st The per-individual evolving Gauss state (current sigma / adaption probability / counter); updated in place.
 * @param values The group's int32 parameter values to adapt (unbounded internal representation; folded on read).
 * @param range The integer value range scaling the truncated gaussian step.
 * @param gr The per-individual random engine the mutation draws from.
 * @return The number of values that were actually adapted.
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent numeric kernel: the integer-Gauss mutation math (ad_prob self-adaption, sigma self-adaption + trigger, truncated-gaussian value step), mirroring GIntGaussAdaptorT::customAdaptions; splitting would scatter tightly coupled mutation math
inline std::size_t adaptGaussIntGroup(
    const GaussConfig<double> &cfg,
    GaussState<double> &st,
    std::span<std::int32_t> values,
    std::int32_t range,
    Gem::Hap::GRandomBase &gr
) {
    Gem::Hap::g_normal_distribution<double> normal;
    Gem::Hap::g_bernoulli_distribution bernoulli;
    using n_param = Gem::Hap::g_normal_distribution<double>::param_type;
    using b_param = Gem::Hap::g_bernoulli_distribution::param_type;

    // 1) Self-adapt the adaption probability once for the whole group (if requested).
    if(cfg.adapt_ad_prob > 0.) {
        st.ad_prob *= std::exp(normal(gr, n_param(0., cfg.adapt_ad_prob)));
        Gem::Common::enforceRangeConstraint<double>(
            st.ad_prob,
            cfg.min_ad_prob,
            cfg.max_ad_prob,
            "adaptGaussIntGroup() / ad_prob"
        );
    }

    // Self-adapts sigma (log-normal step + one-ULP guarantee + range clamp), identical to the FP
    // Gauss kernel's self_adapt_sigma (sigma is a double here too).
    auto self_adapt_sigma = [&]() {
        const double sigma_before = st.sigma;
        const double sigma_mult = std::exp(normal(gr, n_param(0., std::abs(cfg.sigma_sigma))));
        st.sigma *= sigma_mult;
        if(st.sigma == sigma_before) {
            const double dir = (sigma_mult < 1.) ? std::numeric_limits<double>::lowest()
                                                 : std::numeric_limits<double>::max();
            st.sigma = std::nextafter(sigma_before, dir);
        }
        Gem::Common::enforceRangeConstraint<double>(
            st.sigma,
            cfg.min_sigma,
            cfg.max_sigma,
            "adaptGaussIntGroup() / sigma",
            false
        );
    };

    // The sigma self-adaption trigger, mirroring GAdaptorT::adaptAdaption.
    auto adapt_adaption = [&]() {
        if(cfg.adaption_threshold > 0) {
            if(++st.counter >= cfg.adaption_threshold) {
                st.counter = 0;
                self_adapt_sigma();
            }
        }
        else if(cfg.adapt_sigma_prob != 0.) {
            if(bernoulli(gr, b_param(std::abs(cfg.adapt_sigma_prob)))) {
                self_adapt_sigma();
            }
        }
    };

    // The value step (truncate(range * N(0, sigma)) with a guaranteed +/-1 minimal change), mirroring
    // GIntGaussAdaptorT::customAdaptions.
    auto gauss_int_step = [&](std::int32_t &v) {
        auto addition = static_cast<std::int32_t>(
            static_cast<double>(range) * normal(gr, n_param(0., st.sigma))
        );
        if(addition == 0) { // Enforce a minimal change of 1.
            addition = bernoulli(gr, b_param(0.5)) ? 1 : -1;
        }
        v += addition;
    };

    std::size_t n_adapted = 0;
    if(adaptionMode::WITHPROBABILITY == cfg.mode) {
        for(std::int32_t &v : values) {
            if(bernoulli(gr, b_param(std::abs(st.ad_prob)))) {
                adapt_adaption();
                gauss_int_step(v);
                ++n_adapted;
            }
        }
    }
    else if(adaptionMode::ALWAYS == cfg.mode) {
        for(std::int32_t &v : values) {
            adapt_adaption();
            gauss_int_step(v);
            ++n_adapted;
        }
    }
    // adaptionMode::NEVER: nothing to do.

    return n_adapted;
}

/******************************************************************************/
/**
 * The flip kernels: the mutation mathematics of the integer / boolean flip adaptors
 * for integer and boolean parameters, expressed in the same stateless (config, state, values) style as
 * the Gauss kernel. A flip adaptor has NO sigma -- its only evolving state is the adaption probability
 * ad_prob (which self-adapts log-normally like the Gauss one). There is no sigma self-adaption, so no
 * adaption-counter draw is made for a flip adaptor and none is modelled here. The value step is a
 * deterministic ±1 (integers) or a toggle (booleans); the integer fold into a constrained range is
 * applied by the genome on read (GGenome / foldConstrainedInt).
 *
 * The adaption-fp type for the integer and boolean channels is double (their adaption_fp_type), so the
 * flip config / state are plain double POD.
 */

/** @brief Static, shared flip-adaption configuration for one group of int32 / bool parameters. */
struct FlipConfig {
    double min_ad_prob = 0.;                  ///< the lower bound of the adaption probability
    double max_ad_prob = 1.;                  ///< the upper bound of the adaption probability
    double adapt_ad_prob = 0.;                ///< the self-adaption rate of the adaption probability (0 disables)
    adaptionMode mode = adaptionMode::WITHPROBABILITY; ///< always / with-probability / never
};

/** @brief Per-individual, per-group evolving flip state. POD: standard-layout, trivially copyable. */
struct FlipState {
    double ad_prob = 1.;                       ///< the current adaption probability
};

/**
 * @brief Self-adapts the flip adaption probability once for a group (mirrors GAdaptorT::adapt()'s
 * ad-prob step), shared by the int and bool flip kernels.
 *
 * @param cfg The static, shared flip configuration (adaption-probability bounds and self-adaption rate).
 * @param st The per-individual evolving flip state (current adaption probability); updated in place.
 * @param gr The per-individual random engine the log-normal step draws from.
 */
inline void selfAdaptFlipAdProb(const FlipConfig &cfg, FlipState &st, Gem::Hap::GRandomBase &gr) {
    if(cfg.adapt_ad_prob > 0.) {
        Gem::Hap::g_normal_distribution<double> normal;
        st.ad_prob *= std::exp(
            normal(gr, Gem::Hap::g_normal_distribution<double>::param_type(0., cfg.adapt_ad_prob))
        );
        Gem::Common::enforceRangeConstraint<double>(
            st.ad_prob,
            cfg.min_ad_prob,
            cfg.max_ad_prob,
            "selfAdaptFlipAdProb() / ad_prob"
        );
    }
}

/**
 * @brief Adapts one group of int32 values sharing a FlipState by flipping each ±1 (50/50), mirroring
 * GNumFlipAdaptorT::customAdaptions wrapped by GAdaptorT::adapt(vector).
 *
 * @param cfg The static, shared flip configuration (adaption-probability bounds, self-adaption rate, mode).
 * @param st The per-individual evolving flip state (current adaption probability); updated in place.
 * @param values The group's int32 parameter values to adapt (unbounded internal representation; folded on read).
 * @param gr The per-individual random engine the gating and ±1 choice draw from.
 * @return The number of values that were actually flipped.
 */
inline std::size_t adaptFlipIntGroup(
    const FlipConfig &cfg,
    FlipState &st,
    std::span<std::int32_t> values,
    Gem::Hap::GRandomBase &gr
) {
    Gem::Hap::g_bernoulli_distribution bernoulli;
    using b_param = Gem::Hap::g_bernoulli_distribution::param_type;

    selfAdaptFlipAdProb(cfg, st, gr);

    // The value step: +1 / -1 chosen 50/50 (matches GNumFlipAdaptorT::customAdaptions).
    auto flip_step = [&](std::int32_t &v) {
        if(bernoulli(gr, b_param(0.5))) {
            v += 1;
        }
        else {
            v -= 1;
        }
    };

    std::size_t n_adapted = 0;
    if(adaptionMode::WITHPROBABILITY == cfg.mode) {
        for(std::int32_t &v : values) {
            if(bernoulli(gr, b_param(std::abs(st.ad_prob)))) {
                flip_step(v);
                ++n_adapted;
            }
        }
    }
    else if(adaptionMode::ALWAYS == cfg.mode) {
        for(std::int32_t &v : values) {
            flip_step(v);
            ++n_adapted;
        }
    }
    // adaptionMode::NEVER: nothing to do.

    return n_adapted;
}

/**
 * @brief Adapts one group of boolean values (stored as bytes) sharing a FlipState by toggling each,
 * mirroring GBooleanAdaptor::customAdaptions (value = !value) wrapped by GAdaptorT::adapt(vector).
 *
 * @param cfg The static, shared flip configuration (adaption-probability bounds, self-adaption rate, mode).
 * @param st The per-individual evolving flip state (current adaption probability); updated in place.
 * @param values The group's boolean parameter values to adapt, stored one per byte (0 / 1).
 * @param gr The per-individual random engine the per-value gating draws from (the toggle itself is deterministic).
 * @return The number of values that were actually toggled.
 */
inline std::size_t adaptFlipBoolGroup(
    const FlipConfig &cfg,
    FlipState &st,
    std::span<std::uint8_t> values,
    Gem::Hap::GRandomBase &gr
) {
    Gem::Hap::g_bernoulli_distribution bernoulli;
    using b_param = Gem::Hap::g_bernoulli_distribution::param_type;

    selfAdaptFlipAdProb(cfg, st, gr);

    // The value step: a plain toggle (matches GBooleanAdaptor::customAdaptions); no RNG draw.
    auto toggle = [](std::uint8_t &v) { v = v ? static_cast<std::uint8_t>(0) : static_cast<std::uint8_t>(1); };

    std::size_t n_adapted = 0;
    if(adaptionMode::WITHPROBABILITY == cfg.mode) {
        for(std::uint8_t &v : values) {
            if(bernoulli(gr, b_param(std::abs(st.ad_prob)))) {
                toggle(v);
                ++n_adapted;
            }
        }
    }
    else if(adaptionMode::ALWAYS == cfg.mode) {
        for(std::uint8_t &v : values) {
            toggle(v);
            ++n_adapted;
        }
    }
    // adaptionMode::NEVER: nothing to do.

    return n_adapted;
}

/******************************************************************************/
/**
 * The bi-gaussian kernel, in the same (config, state, values) style as the Gauss kernel. Instead of a
 * single gaussian it samples from a bi-modal distribution of two gaussians separated by a distance
 * "delta"; sigma1, sigma2 and delta each self-adapt log-normally (sigma2 == sigma1 in the symmetric
 * case for the value step, but all three still self-adapt). The value step adds
 * bi_normal(0, sigma1, sigma2, delta) in the normalized internal coordinate, with the same one-ULP "an
 * adaption that fires always changes the value" guarantee as the Gauss kernel.
 */

/** @brief Static, shared bi-gaussian configuration for one group of FP parameters. */
template <typename T>
struct BiGaussConfig {
    T sigma_sigma1 = T(0.8);                   ///< the log-normal self-adaption rate of sigma1
    T sigma_sigma2 = T(0.8);                   ///< the log-normal self-adaption rate of sigma2
    T sigma_delta = T(0.8);                    ///< the log-normal self-adaption rate of delta
    T min_sigma1 = T(0.001);                   ///< the lower bound of sigma1
    T max_sigma1 = T(2.);                      ///< the upper bound of sigma1
    T min_sigma2 = T(0.001);                   ///< the lower bound of sigma2
    T max_sigma2 = T(2.);                      ///< the upper bound of sigma2
    T min_delta = T(0.);                       ///< the lower bound of delta
    T max_delta = T(2.);                       ///< the upper bound of delta
    T min_ad_prob = T(0.);                     ///< the lower bound of the adaption probability
    T max_ad_prob = T(1.);                     ///< the upper bound of the adaption probability
    T adapt_ad_prob = T(0.);                   ///< the self-adaption rate of the adaption probability (0 disables)
    T adapt_sigma_prob = T(0.);                ///< probability of a sigma self-adaption when adaption_threshold == 0
    std::uint32_t adaption_threshold = 1;      ///< sigmas/delta self-adapt every Nth adaption (0 ⇒ use adapt_sigma_prob)
    bool use_symmetric_sigmas = true;          ///< whether the value step uses sigma1 for both gaussians
    adaptionMode mode = adaptionMode::WITHPROBABILITY; ///< always / with-probability / never
};

/** @brief Per-individual, per-group evolving bi-gaussian state. POD: trivially copyable. */
template <typename T>
struct BiGaussState {
    T sigma1 = T(1.);                          ///< the current width of the first gaussian
    T sigma2 = T(1.);                          ///< the current width of the second gaussian
    T delta = T(0.5);                          ///< the current distance between the two gaussians
    T ad_prob = T(1.);                         ///< the current adaption probability
    std::uint32_t counter = 0;                 ///< adaptions since the last sigma self-adaption
};

/**
 * @brief Adapts one group of values sharing a single BiGaussState.
 *
 * Like adaptGaussGroup(), but the value step samples from a bi-modal distribution of two gaussians
 * separated by @c delta; sigma1, sigma2 and delta each self-adapt log-normally on the trigger. @p st is
 * updated in place.
 *
 * @tparam T The adaption floating-point type (double for double parameters, float for float).
 * @param cfg The static, shared bi-gaussian configuration (the three sigma/delta families, bounds, mode).
 * @param st The per-individual evolving bi-gaussian state (sigma1 / sigma2 / delta / adaption probability / counter); updated in place.
 * @param values The group's parameter values to adapt, in their normalized internal representation.
 * @param gr The per-individual random engine the mutation draws from.
 * @return The number of values that were actually adapted.
 */
template <typename T>
// NOLINTNEXTLINE(readability-function-size) -- one coherent numeric kernel: the bi-gaussian mutation math (ad_prob self-adaption, sigma1/sigma2/delta self-adaption + trigger, bi-modal value step), mirroring GNumBiGaussAdaptorT/GFPBiGaussAdaptorT; splitting would scatter tightly coupled mutation math
std::size_t adaptBiGaussGroup(
    const BiGaussConfig<T> &cfg,
    BiGaussState<T> &st,
    std::span<T> values,
    Gem::Hap::GRandomBase &gr
) {
    Gem::Hap::g_normal_distribution<T> normal;
    Gem::Hap::g_bernoulli_distribution bernoulli;
    Gem::Hap::bi_normal_distribution<T> bi_normal;
    using n_param = typename Gem::Hap::g_normal_distribution<T>::param_type;
    using b_param = Gem::Hap::g_bernoulli_distribution::param_type;
    using bn_param = typename Gem::Hap::bi_normal_distribution<T>::param_type;

    // 1) Self-adapt the adaption probability once for the whole group (if requested).
    if(cfg.adapt_ad_prob > T(0.)) {
        st.ad_prob *= std::exp(normal(gr, n_param(T(0.), cfg.adapt_ad_prob)));
        Gem::Common::enforceRangeConstraint<T>(
            st.ad_prob,
            cfg.min_ad_prob,
            cfg.max_ad_prob,
            "adaptBiGaussGroup() / ad_prob"
        );
    }

    // Self-adapts sigma1 / sigma2 / delta (log-normal step + range clamp), mirroring
    // GNumBiGaussAdaptorT::customAdaptAdaption (no ULP nudge there, unlike the single gauss).
    auto self_adapt = [&]() {
        st.sigma1 *= std::exp(normal(gr, n_param(T(0.), std::abs(cfg.sigma_sigma1))));
        st.sigma2 *= std::exp(normal(gr, n_param(T(0.), std::abs(cfg.sigma_sigma2))));
        st.delta *= std::exp(normal(gr, n_param(T(0.), std::abs(cfg.sigma_delta))));
        Gem::Common::enforceRangeConstraint<T>(st.sigma1, cfg.min_sigma1, cfg.max_sigma1, "adaptBiGaussGroup() / sigma1");
        Gem::Common::enforceRangeConstraint<T>(st.sigma2, cfg.min_sigma2, cfg.max_sigma2, "adaptBiGaussGroup() / sigma2");
        Gem::Common::enforceRangeConstraint<T>(st.delta, cfg.min_delta, cfg.max_delta, "adaptBiGaussGroup() / delta");
    };

    // The sigma self-adaption trigger, mirroring GAdaptorT::adaptAdaption.
    auto adapt_adaption = [&]() {
        if(cfg.adaption_threshold > 0) {
            if(++st.counter >= cfg.adaption_threshold) {
                st.counter = 0;
                self_adapt();
            }
        }
        else if(cfg.adapt_sigma_prob != T(0.)) {
            if(bernoulli(gr, b_param(std::abs(cfg.adapt_sigma_prob)))) {
                self_adapt();
            }
        }
    };

    // The value step (bi_normal(0, sigma1, sigma2|sigma1, delta) in the normalized internal coordinate
    // + one-ULP guarantee), mirroring GFPBiGaussAdaptorT::customAdaptions.
    auto bigauss_step = [&](T &v) {
        const T before = v;
        const T s2 = cfg.use_symmetric_sigmas ? st.sigma1 : st.sigma2;
        v += bi_normal(gr, bn_param(T(0.), st.sigma1, s2, st.delta));
        if(v == before) {
            v = std::nextafter(before, std::numeric_limits<T>::max());
        }
    };

    std::size_t n_adapted = 0;
    if(adaptionMode::WITHPROBABILITY == cfg.mode) {
        for(T &v : values) {
            if(bernoulli(gr, b_param(std::abs(st.ad_prob)))) {
                adapt_adaption();
                bigauss_step(v);
                ++n_adapted;
            }
        }
    }
    else if(adaptionMode::ALWAYS == cfg.mode) {
        for(T &v : values) {
            adapt_adaption();
            bigauss_step(v);
            ++n_adapted;
        }
    }
    // adaptionMode::NEVER: nothing to do.

    return n_adapted;
}

/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
