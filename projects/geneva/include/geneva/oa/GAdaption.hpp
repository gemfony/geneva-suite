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
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Geneva headers go here
#include "geneva/genome/GAdaptionAuxKeys.hpp"
#include "geneva/genome/GAdaptionKernels.hpp"
#include "geneva/genome/GAuxiliaryStore.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "hap/GRandomBase.hpp"
#include "hap/GRandomLeasePool.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * The OA-side adaption logic, expressed as stateless free functions over (individual, scratch, config,
 * RNG). The adaption is driven by an OA-OWNED GAdaptionConfig rather than by the structure-only genome
 * layout. The config supplies each group's adaptor kind + parameters; the genome supplies the mutable
 * internal value spans; the per-group evolving state lives in an OA-owned GAuxiliaryStore (the
 * individual's scratch_) passed in explicitly — the individual itself is pure data. The RNG is a
 * proxy leased from the process-global Gem::Hap::randomLeasePool() for the duration of one adaption
 * call (the candidate holds no RNG of its own). Each call touches only this individual's values +
 * its slot's scratch + its leased RNG and reads the shared config read-only, so the functions
 * compose with the EA's parallel adaptChildren_.
 */

namespace detail {

using Gem::Geneva::Genome::AuxKey;
using Gem::Geneva::Genome::BiGaussState;
using Gem::Geneva::Genome::FlipState;
using Gem::Geneva::Genome::GaussState;
using Gem::Geneva::Genome::GAuxiliaryStore;
using Gem::Geneva::Genome::GGenome;
using Gem::Geneva::Genome::GroupSpec;

/**
 * @brief Runs the Gauss kernel over one FP channel using the GaussState block (in scratch) under @p key.
 *
 * Iterates the channel's groups, adapting each active Gauss group's value slice with its own state record.
 * A no-op (returns 0) if no GaussState block was installed for @p key.
 *
 * @tparam T The channel's floating-point type (double or float).
 * @param scratch The per-individual auxiliary store holding the GaussState block under @p key (updated in place).
 * @param groups The channel's group specifications (bounds / grouping / adaptor config) from the shared config.
 * @param values The channel's full value array (each group adapts its [start, start+len) sub-span) in internal representation.
 * @param key The auxiliary-store key identifying this channel's GaussState block.
 * @param gr The random engine the mutation draws from.
 * @return The number of values actually adapted across the channel.
 */
template <typename T>
std::size_t adaptGaussChannel(
    GAuxiliaryStore &scratch,
    const std::vector<GroupSpec<T>> &groups,
    std::span<T> values,
    AuxKey key,
    Gem::Hap::GRandomBase &gr
) {
    if(not scratch.hasAux(key)) {
        return 0;
    }
    std::span<GaussState<T>> const states = scratch.metaRecords<GaussState<T>>(key);
    std::size_t n = 0;
    for(std::size_t gi = 0; gi < groups.size(); ++gi) {
        const GroupSpec<T> &g = groups[gi];
        if(not g.has_gauss || not g.active) {
            continue;
        }
        // The FP value step is taken in the NORMALIZED internal coordinate (interval width 1), so the
        // gaussian step is dimensionless (sigma is a fraction of the parameter's range); the physical
        // scale is reapplied by the external transform when the objective reads the value (§2.4).
        n += Gem::Geneva::Genome::adaptGaussGroup<T>(
            g.gauss, states[gi], values.subspan(g.start, g.len), gr
        );
    }
    return n;
}

/**
 * @brief Runs the bi-gaussian kernel over one FP channel using the BiGaussState block (in scratch) under @p key.
 *
 * The bi-gaussian counterpart of adaptGaussChannel(): iterates the channel's groups, adapting each active
 * bi-gaussian group's value slice with its own state record. A no-op (returns 0) if no BiGaussState block
 * was installed for @p key.
 *
 * @tparam T The channel's floating-point type (double or float).
 * @param scratch The per-individual auxiliary store holding the BiGaussState block under @p key (updated in place).
 * @param groups The channel's group specifications (bounds / grouping / adaptor config) from the shared config.
 * @param values The channel's full value array (each group adapts its [start, start+len) sub-span) in internal representation.
 * @param key The auxiliary-store key identifying this channel's BiGaussState block.
 * @param gr The per-individual random engine the mutation draws from.
 * @return The number of values actually adapted across the channel.
 */
template <typename T>
std::size_t adaptBiGaussChannel(
    GAuxiliaryStore &scratch,
    const std::vector<GroupSpec<T>> &groups,
    std::span<T> values,
    AuxKey key,
    Gem::Hap::GRandomBase &gr
) {
    if(not scratch.hasAux(key)) {
        return 0;
    }
    std::span<BiGaussState<T>> const states = scratch.metaRecords<BiGaussState<T>>(key);
    std::size_t n = 0;
    for(std::size_t gi = 0; gi < groups.size(); ++gi) {
        const GroupSpec<T> &g = groups[gi];
        if(not g.has_bigauss || not g.active) {
            continue;
        }
        // Normalized internal coordinate: the bi-gaussian step is dimensionless (§2.4).
        n += Gem::Geneva::Genome::adaptBiGaussGroup<T>(
            g.bigauss, states[gi], values.subspan(g.start, g.len), gr
        );
    }
    return n;
}

} // namespace detail

/******************************************************************************/
/**
 * @brief Runs the data-oriented adaption kernels over an individual once, driven by the config (the
 * "customAdaptions" half of adapt(), config-injected). Mirrors GGenome::customAdaptions()'s channel
 * order exactly (Gauss double/float, bi-Gauss double/float, int Gauss, int flip, bool flip), then folds
 * constrained values back into range.
 *
 * @param ind The individual whose internal value channels are mutated in place.
 * @param scratch The individual's auxiliary store holding the per-channel adaption-state blocks (updated in place).
 * @param cfg The shared, read-only adaption config supplying each channel's group specifications.
 * @param gr The per-individual random engine all kernels draw from.
 * @return The total number of values actually adapted across all channels.
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent channel sweep that must mirror GGenome::customAdaptions()'s fixed channel order (Gauss double/float, bi-Gauss double/float, int Gauss, int flip, bool flip) exactly, then the write-fold; splitting would risk that order drifting out of lockstep
inline std::size_t runAdaptionKernels(
    detail::GGenome &ind,
    detail::GAuxiliaryStore &scratch,
    const GAdaptionConfigBase &cfg,
    Gem::Hap::GRandomBase &gr
) {
    using namespace Gem::Geneva::Genome;

    std::size_t n = 0;
    n += detail::adaptGaussChannel<double>(scratch, cfg.doubleGroups(), ind.internalDoubleValues(), AUXKEY_GAUSS_DOUBLE, gr);
    n += detail::adaptGaussChannel<float>(scratch, cfg.floatGroups(), ind.internalFloatValues(), AUXKEY_GAUSS_FLOAT, gr);
    n += detail::adaptBiGaussChannel<double>(scratch, cfg.doubleGroups(), ind.internalDoubleValues(), AUXKEY_BIGAUSS_DOUBLE, gr);
    n += detail::adaptBiGaussChannel<float>(scratch, cfg.floatGroups(), ind.internalFloatValues(), AUXKEY_BIGAUSS_FLOAT, gr);

    // Integer Gauss. Integers are NOT normalized (§2.7), so the step is still scaled by the parameter's
    // integer range, taken from the genome's int channel layout (upper-lower constrained, init span plain).
    if(scratch.hasAux(AUXKEY_GAUSS_INT)) {
        std::span<GaussState<double>> const states = scratch.metaRecords<GaussState<double>>(AUXKEY_GAUSS_INT);
        std::span<std::int32_t> const values = ind.internalInt32Values();
        const auto &groups = cfg.int32Groups();
        const ChannelLayout<std::int32_t> &layout_i = ind.getLayout()->i;
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            const GroupSpec<std::int32_t> &g = groups[gi];
            if(not g.has_gauss || not g.active) {
                continue;
            }
            const std::int32_t int_range = ngScale(layout_i, g.start);
            n += adaptGaussIntGroup(g.gauss, states[gi], values.subspan(g.start, g.len), int_range, gr);
        }
    }

    // Flip over the int32 channel.
    if(scratch.hasAux(AUXKEY_FLIP_INT)) {
        std::span<FlipState> const states = scratch.metaRecords<FlipState>(AUXKEY_FLIP_INT);
        std::span<std::int32_t> const values = ind.internalInt32Values();
        const auto &groups = cfg.int32Groups();
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            const GroupSpec<std::int32_t> &g = groups[gi];
            if(not g.has_flip || not g.active) {
                continue;
            }
            n += adaptFlipIntGroup(g.flip, states[gi], values.subspan(g.start, g.len), gr);
        }
    }

    // Flip over the bool channel.
    if(scratch.hasAux(AUXKEY_FLIP_BOOL)) {
        std::span<FlipState> const states = scratch.metaRecords<FlipState>(AUXKEY_FLIP_BOOL);
        std::span<std::uint8_t> const values = ind.internalBoolValues();
        const auto &groups = cfg.boolGroups();
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            const GroupSpec<bool> &g = groups[gi];
            if(not g.has_flip || not g.active) {
                continue;
            }
            n += adaptFlipBoolGroup(g.flip, states[gi], values.subspan(g.start, g.len), gr);
        }
    }

    // Adaption-time write-fold (normalized-genome architecture §2.2): the gauss / bi-gauss / int-gauss
    // kernels add their step to the raw internal value, so a bounded value can overshoot its canonical
    // interval. Fold each bounded value back into range now -- a bounded FP value into the internal
    // [-0.5, 0.5), a bounded int into its closed [lo, hi] -- rather than letting the internal magnitude
    // grow unbounded. The fold is external-value-preserving, so the next step simply starts from the
    // folded (reflected) internal position; unbounded parameters are left to roam.
    if(n > 0) {
        ind.foldConstrainedValuesInPlace();
    }

    return n;
}

/******************************************************************************/
/**
 * @brief The OA-side replacement for GGenome::adapt(): runs the data-oriented adaption
 * kernels over an individual inside the same "guarantee a change, then a valid solution" retry loop the
 * individual's adapt() used, but driven by the OA-owned config and an RNG proxy leased from the
 * process-global Gem::Hap::randomLeasePool() (the candidate holds no RNG of its own).
 *
 * Mirrors GGenome::adapt() exactly: the inner loop retries customAdaptions (here:
 * runAdaptionKernels) until at least one value changed or max_unsuccessful_adaptions is exceeded; the
 * outer loop retries until the individual fulfils its constraints or max_retries_until_valid is
 * exceeded. Marks the individual due for processing iff at least one adaption happened, records the
 * adaption count on the individual, and returns it. Touches only this individual's values + aux state +
 * its thread-scoped RNG lease, so it composes with the EA's parallel adaptChildren_.
 */
inline std::size_t adaptIndividual(
    detail::GGenome &ind,
    detail::GAuxiliaryStore &scratch,
    const GAdaptionConfigBase &cfg
) {
    //! [hap-lease-idiom]
    // The candidate holds no RNG of its own -- lease a proxy for the duration of this adaption. The
    // lease is thread-scoped, so this composes with the EA's parallel adaptChildren_.
    auto             lease = Gem::Hap::randomLeasePool().acquire();
    Gem::Hap::GRandomBase &gr = *lease;
    //! [hap-lease-idiom]

    const std::size_t max_unsuccessful = cfg.getMaxUnsuccessfulAdaptions();
    const std::size_t max_retries = cfg.getMaxRetriesUntilValid();

    std::size_t n_adaption_attempts = 0;
    std::size_t n_adaptions = 0;
    std::size_t n_invalid_adaptions = 0;
    double validity = 0;

    while(true) {
        // Make sure at least one modification is performed.
        while(true) {
            if((n_adaptions = runAdaptionKernels(ind, scratch, cfg, gr)) > 0) {
                break;
            }
            if(max_unsuccessful > 0 && ++n_adaption_attempts > max_unsuccessful) {
                break;
            }
        }

        if(ind.fulfillsConstraints(validity) || ++n_invalid_adaptions > max_retries) {
            break;
        }
    }

    if(n_adaptions > 0) {
        ind.mark_as_due_for_processing();
    }
    scratch.setNAdaptions(n_adaptions);
    return n_adaptions;
}

/******************************************************************************/
/**
 * @brief Resets an individual's per-group adaption state to the config's seed values (the stall-reset).
 * Mirrors GGenome::updateAdaptorsOnStall(), but driven by the OA-owned config.
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent channel sweep (the local resetGauss/resetBiGauss/resetFlip lambdas applied in the fixed channel order used throughout this file), mirroring GGenome::updateAdaptorsOnStall(); splitting would risk that order drifting out of lockstep
inline void resetAdaptionState(detail::GAuxiliaryStore &scratch, const GAdaptionConfigBase &cfg) {
    using namespace Gem::Geneva::Genome;

    auto resetGauss = [&]<typename T>(const std::vector<GroupSpec<T>> &groups, AuxKey key) {
        if(not scratch.hasAux(key)) {
            return;
        }
        std::span<GaussState<adaption_fp_t<T>>> const states = scratch.metaRecords<GaussState<adaption_fp_t<T>>>(key);
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(not groups[gi].has_gauss) {
                continue;
            }
            states[gi].sigma = groups[gi].start_sigma;
            states[gi].ad_prob = groups[gi].start_ad_prob;
            states[gi].counter = 0;
        }
    };
    auto resetBiGauss = [&]<typename T>(const std::vector<GroupSpec<T>> &groups, AuxKey key) {
        if(not scratch.hasAux(key)) {
            return;
        }
        std::span<BiGaussState<adaption_fp_t<T>>> const states = scratch.metaRecords<BiGaussState<adaption_fp_t<T>>>(key);
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(not groups[gi].has_bigauss) {
                continue;
            }
            states[gi].sigma1 = groups[gi].start_sigma1;
            states[gi].sigma2 = groups[gi].start_sigma2;
            states[gi].delta = groups[gi].start_delta;
            states[gi].ad_prob = groups[gi].start_ad_prob;
            states[gi].counter = 0;
        }
    };
    auto resetFlip = [&]<typename T>(const std::vector<GroupSpec<T>> &groups, AuxKey key) {
        if(not scratch.hasAux(key)) {
            return;
        }
        std::span<FlipState> const states = scratch.metaRecords<FlipState>(key);
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(not groups[gi].has_flip) {
                continue;
            }
            states[gi].ad_prob = groups[gi].start_ad_prob;
        }
    };

    resetGauss(cfg.doubleGroups(), AUXKEY_GAUSS_DOUBLE);
    resetGauss(cfg.floatGroups(), AUXKEY_GAUSS_FLOAT);
    resetGauss(cfg.int32Groups(), AUXKEY_GAUSS_INT); // GaussState<adaption_fp_t<int32>> = GaussState<double>
    resetBiGauss(cfg.doubleGroups(), AUXKEY_BIGAUSS_DOUBLE);
    resetBiGauss(cfg.floatGroups(), AUXKEY_BIGAUSS_FLOAT);
    resetFlip(cfg.int32Groups(), AUXKEY_FLIP_INT);
    resetFlip(cfg.boolGroups(), AUXKEY_FLIP_BOOL);
}

/******************************************************************************/
/**
 * @brief Reads the current per-group sigma of a named Gauss adaptor from an OA-owned adaption-state
 * store (the slot's scratch — one entry per Gauss group), for the pluggable monitors and the in-fitness
 * sigma logging. Recognised names: "GDoubleGaussAdaptor",
 * "GFloatGaussAdaptor", "GInt32GaussAdaptor". Sigmas are returned as double (the float sigma widened).
 * For an individual that is detached from its slot (an archived best, a transport copy, or a standalone
 * individual), pass a scratch freshly seeded from the OA config (cfg.installInto(scratch)) to obtain the
 * configured seed sigma.
 */
inline std::vector<double> readAdaptionSigmas(
    const detail::GAuxiliaryStore &scratch,
    const GAdaptionConfigBase &cfg,
    const std::string &adaptor_name
) {
    using namespace Gem::Geneva::Genome;
    std::vector<double> out;

    if(adaptor_name == "GDoubleGaussAdaptor" && scratch.hasAux(AUXKEY_GAUSS_DOUBLE)) {
        std::span<const GaussState<double>> const states = scratch.metaRecords<GaussState<double>>(AUXKEY_GAUSS_DOUBLE);
        const auto &groups = cfg.doubleGroups();
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(groups[gi].has_gauss) {
                out.push_back(states[gi].sigma);
            }
        }
    }
    else if(adaptor_name == "GFloatGaussAdaptor" && scratch.hasAux(AUXKEY_GAUSS_FLOAT)) {
        std::span<const GaussState<float>> const states = scratch.metaRecords<GaussState<float>>(AUXKEY_GAUSS_FLOAT);
        const auto &groups = cfg.floatGroups();
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(groups[gi].has_gauss) {
                out.push_back(static_cast<double>(states[gi].sigma));
            }
        }
    }
    else if(adaptor_name == "GInt32GaussAdaptor" && scratch.hasAux(AUXKEY_GAUSS_INT)) {
        std::span<const GaussState<double>> const states = scratch.metaRecords<GaussState<double>>(AUXKEY_GAUSS_INT);
        const auto &groups = cfg.int32Groups();
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(groups[gi].has_gauss) {
                out.push_back(states[gi].sigma);
            }
        }
    }

    return out;
}

/******************************************************************************/
/**
 * @brief The convenience factory for an OA-owned adaption config, built from a representative genome so it
 * describes exactly the groups that exist. ConfigT selects the per-OA type (GEAAdaptionConfig or
 * the plain base). The genome (structure-only) supplies the group SKELETON, and the
 * caller authors the adaptors onto the returned config via its fluent API (cfg->groupDouble(i).gauss(...) /
 * cfg->forLabel(...).gauss(...)). It is an oa-side factory because GAdaptionConfig lives in geneva/oa/ while
 * GGenomeBuilder lives in geneva/genome/ (oa depends on ind, not the reverse).
 */
template <typename ConfigT = GAdaptionConfigBase>
std::shared_ptr<ConfigT> makeAdaptionConfig(const detail::GGenome &genome) {
    return std::make_shared<ConfigT>(genome);
}

/******************************************************************************/
/**
 * @brief Overwrites the current per-group sigma of EVERY Gauss group (double + float channels) in one
 * scratch store with a single global value. Used by the global-σ step controllers (ONE_FIFTH / CSA),
 * which own a single search-distribution sigma and push it into every group's state each generation so
 * the next adaption uses it. A no-op for channels with no installed Gauss block. The bi-Gauss sigmas are
 * intentionally left alone (the global-σ controllers operate on the single-Gauss path).
 *
 * @param scratch The per-individual auxiliary store whose Gauss state blocks are overwritten.
 * @param cfg The shared adaption config supplying the per-channel group specs.
 * @param global_sigma The global sigma to write into every Gauss group's state.
 */
inline void writeGlobalSigma(
    detail::GAuxiliaryStore &scratch,
    const GAdaptionConfigBase &cfg,
    double global_sigma
) {
    using namespace Gem::Geneva::Genome;
    if(scratch.hasAux(AUXKEY_GAUSS_DOUBLE)) {
        std::span<GaussState<double>> const st = scratch.metaRecords<GaussState<double>>(AUXKEY_GAUSS_DOUBLE);
        const auto &groups = cfg.doubleGroups();
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(groups[gi].has_gauss) {
                st[gi].sigma = global_sigma;
            }
        }
    }
    if(scratch.hasAux(AUXKEY_GAUSS_FLOAT)) {
        std::span<GaussState<float>> const st = scratch.metaRecords<GaussState<float>>(AUXKEY_GAUSS_FLOAT);
        const auto &groups = cfg.floatGroups();
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(groups[gi].has_gauss) {
                st[gi].sigma = static_cast<float>(global_sigma);
            }
        }
    }
}

/******************************************************************************/
/**
 * @brief Reads back one representative per-group sigma from a scratch store (the first installed Gauss
 * group, double channel preferred, else float). Used by the global-σ controllers to recover the seed
 * sigma at setup and to read the per-individual sigma when intermediate-recombining σ. Returns the
 * fallback when no Gauss state is present.
 *
 * @param scratch The per-individual auxiliary store to read from.
 * @param cfg The shared adaption config supplying the per-channel group specs.
 * @param fallback The value returned when no Gauss state is installed.
 * @return The first installed Gauss group's current sigma, or @p fallback.
 */
inline double readRepresentativeSigma(
    const detail::GAuxiliaryStore &scratch,
    const GAdaptionConfigBase &cfg,
    double fallback
) {
    using namespace Gem::Geneva::Genome;
    if(scratch.hasAux(AUXKEY_GAUSS_DOUBLE)) {
        std::span<const GaussState<double>> const st = scratch.metaRecords<GaussState<double>>(AUXKEY_GAUSS_DOUBLE);
        const auto &groups = cfg.doubleGroups();
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(groups[gi].has_gauss) {
                return st[gi].sigma;
            }
        }
    }
    if(scratch.hasAux(AUXKEY_GAUSS_FLOAT)) {
        std::span<const GaussState<float>> const st = scratch.metaRecords<GaussState<float>>(AUXKEY_GAUSS_FLOAT);
        const auto &groups = cfg.floatGroups();
        for(std::size_t gi = 0; gi < groups.size(); ++gi) {
            if(groups[gi].has_gauss) {
                return static_cast<double>(st[gi].sigma);
            }
        }
    }
    return fallback;
}

/******************************************************************************/
/**
 * @brief Reads back one representative per-group MAXIMUM sigma from the adaption config (the first
 * installed Gauss group, double channel preferred, else float). Used by the global-σ step controllers
 * (ONE_FIFTH / CSA) to clamp the single global sigma to the authored per-group band rather than to a
 * hard-coded constant: in the normalized coordinate model sigma is a fraction of the parameter range,
 * so max_sigma is the meaningful ceiling. Returns the fallback when no Gauss group is installed.
 *
 * @param cfg The shared adaption config supplying the per-channel group specs.
 * @param fallback The value returned when no Gauss group is installed.
 * @return The first installed Gauss group's authored max_sigma, or @p fallback.
 */
inline double readRepresentativeMaxSigma(const GAdaptionConfigBase &cfg, double fallback) {
    using namespace Gem::Geneva::Genome;
    const auto &dg = cfg.doubleGroups();
    for(std::size_t gi = 0; gi < dg.size(); ++gi) {
        if(dg[gi].has_gauss) {
            return static_cast<double>(dg[gi].gauss.max_sigma);
        }
    }
    const auto &fg = cfg.floatGroups();
    for(std::size_t gi = 0; gi < fg.size(); ++gi) {
        if(fg[gi].has_gauss) {
            return static_cast<double>(fg[gi].gauss.max_sigma);
        }
    }
    return fallback;
}

/******************************************************************************/
/**
 * @brief A small RAII helper that gives a single, slot-less individual its own adaption scratch + config
 * so the data-oriented adaption can be driven outside an optimization algorithm (test individuals'
 * modify hooks, standalone perturbation loops, serialization benchmarks). Construct once with the
 * individual's authored OA-owned config and call adapt() repeatedly to preserve sigma self-adaptation
 * across iterations, exactly as the individual's former adapt() did via its own per-individual aux state.
 * The config is validated against the genome's structure and copied (its base data is all the
 * data-oriented adaption needs).
 */
//! [adaptors-standalone]
class StandaloneAdapter {
public:
    StandaloneAdapter(const detail::GGenome &ind, const std::shared_ptr<GAdaptionConfigBase> &cfg)
      : cfg_(*cfg) {
        cfg_.checkConsistency(ind);
        cfg_.installInto(scratch_);
    }

    std::size_t adapt(detail::GGenome &ind) { return adaptIndividual(ind, scratch_, cfg_); }

private:
    detail::GAuxiliaryStore scratch_;
    GAdaptionConfigBase cfg_;
};
//! [adaptors-standalone]

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
