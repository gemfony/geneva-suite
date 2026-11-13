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
#include <concepts>
#include <filesystem>
#include <memory>
#include <vector>

// Boost header files go here

// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named_base (named-base emitter)
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "common/concurrency/GLoadOnceCellT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeFactory.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {
// The OA-owned adaption configuration produced (optionally) by a flat individual's
// buildAdaptionConfig() hook. Forward-declared so this header stays free of an OA dependency; the
// full type is only needed in the individual's own translation unit that implements the hook.
class GAdaptionConfigBase;
} // namespace Gem::Geneva::OptimizationAlgorithms

namespace Gem::Geneva::Genome {

/******************************************************************************/
// Optional static hooks a concrete flat individual may provide. The factory detects each via the
// concept below and adapts: an individual that omits a hook simply gets the default behaviour.

/** @brief Satisfied if Derived supplies a static finalize(const Config&) teardown hook. */
template <typename Derived>
concept HasFinalizeHook = requires(const typename Derived::Config &c) { Derived::finalize(c); };

/** @brief Satisfied if Derived supplies a static buildAdaptionConfig(const GGenome&, const Config&) hook. */
template <typename Derived>
concept HasBuildAdaptionConfigHook =
    requires(const GGenome &g, const typename Derived::Config &c) {
        Derived::buildAdaptionConfig(g, c);
    };

/** @brief Satisfied if Derived supplies a static applyConfig(Derived&, const Config&) per-object hook. */
template <typename Derived>
concept HasApplyConfigHook =
    requires(Derived &d, const typename Derived::Config &c) { Derived::applyConfig(d, c); };

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A generic, config-driven factory for flat individuals. Because GGenome holds all genome state
 * generically and the shared GGenomeLayout is built once and reused by every produced individual,
 * the factory machinery -- read the config, build the structure once, spawn, attach pre/post
 * processors -- is identical for every concrete flat individual. So instead of hand-writing a factory
 * class per individual, a Tier-2 (config-driven) flat individual only supplies two static hooks, and
 * uses this template directly.
 *
 * The Derived individual must provide:
 *  - a nested @c Config type (a plain, copyable struct holding the configurable values, e.g. par_dim,
 *    bounds, sigma);
 *  - @c static void describeConfig(Gem::Common::GParserBuilder&, Config&) -- registers the config-file
 *    options, binding them to the passed Config (called once per produced object on a fresh parser);
 *  - @c static GenomeData buildGenome(const Config&) -- builds the value arrays + the shared, immutable
 *    GGenomeLayout from the parsed config (typically via a GGenomeBuilder).
 *
 * The shared layout is built exactly once (a structural guarantee, not a hand-managed optimisation):
 * the first produced individual triggers Derived::buildGenome(config_), the result is cached, and every
 * subsequent individual is just a cheap value-array copy plus a bind to the same shared layout handle.
 * The user's JSON config files are unchanged; pre/post-processor registration is inherited from
 * GGenomeFactory and is genome-agnostic.
 *
 * Usage (Tier 2):
 * @code
 *   class MyIndividual : public GGenomeT<MyIndividual> {
 *   public:
 *       MyIndividual() = default;   // genome is installed by the factory
 *       std::vector<double> evaluate() override { ... }
 *
 *       struct Config { std::size_t par_dim = 5; double min = -10., max = 10., sigma = 0.5; };
 *       static void describeConfig(Gem::Common::GParserBuilder& gpb, Config& c) {
 *           gpb.registerFileParameter<std::size_t>("par_dim", c.par_dim, 5);
 *           // ...
 *       }
 *       static GenomeData buildGenome(const Config& c) {
 *           GGenomeBuilder b;
 *           b.addDoubleGroup(c.par_dim, c.min, c.max); // structure only; adaptors on the OA config
 *           return b.build();
 *       }
 *   };
 *
 *   GIndividualFactory<MyIndividual> f("config/MyIndividual.json");
 *   auto ind = f.get_as<MyIndividual>();
 * @endcode
 * The individual additionally carries its archive tag at file scope in its own .cpp (fully qualified,
 * in exactly one translation unit -- the stringized argument is the wire/checkpoint tag):
 * @code
 *   #include "weft/GArchivePolymorphic.hpp"
 *   GEM_REGISTER_ARCHIVABLE(Gem::Geneva::MyIndividual)
 * @endcode
 *
 * @tparam Derived The concrete flat individual type, supplying the Config / describeConfig / buildGenome hooks
 */
template <class Derived>
class GIndividualFactory // NOLINT(cppcoreguidelines-special-member-functions)
  : public GGenomeFactory {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /**
     * @brief Serialises only the factory base; Config and the genome cache are transient.
     *
     * @tparam Archive The GArchive codec type
     * @param ar The archive to read from / write to
     * @param version The (unused) serialization version number
     */
    template <class Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        // Only the base is serialised. The Config is transient -- re-read from the (still known)
        // config file on the first get_() after deserialisation -- and the built-genome cache is a
        // transient optimisation that is rebuilt lazily.
        Gem::Common::archive_named_base<GGenomeFactory>(ar, "GGenomeFactory", *this);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /***************************************************************************/
    /**
     * The standard constructor
     *
     * @param configFile The path of the configuration file holding the genome parameters
     */
    explicit GIndividualFactory(std::filesystem::path const &configFile)
      : GGenomeFactory(configFile) { /* nothing */
    }

    /***************************************************************************/
    /**
     * The copy constructor. The built-genome cache is deliberately *not* copied: a copied factory rebuilds
     * its own shared genome lazily (the load-once cell is also non-copyable).
     *
     * @param cp The factory to copy from (base state and parsed Config are copied)
     */
    GIndividualFactory(const GIndividualFactory<Derived> &cp)
      : GGenomeFactory(cp)
      , config_(cp.config_) { /* nothing */
    }

    /***************************************************************************/
    /**
     * The destructor. Invokes an optional Derived::finalize(const Config&) teardown hook when a genome
     * was actually produced -- a lifetime-bound teardown (e.g.
     * telling an external evaluator program to finalise). An individual without the hook gets the default
     * (trivial) teardown. A destructor must never propagate an exception, so the hook is shielded.
     */
    ~GIndividualFactory() override {
        if constexpr (HasFinalizeHook<Derived>) {
            if(genome_cell_.loaded()) {
                try {
                    Derived::finalize(config_);
                }
                // NOLINTNEXTLINE(bugprone-empty-catch) -- deliberate: a destructor must not propagate
                catch(...) { /* nothing */
                }
            }
        }
    }

    /***************************************************************************/
    /**
     * Creates a deep clone of this object
     *
     * @return A shared pointer to a deep copy of this factory
     */
    std::shared_ptr<Gem::Common::GFactoryT<GGenome>> clone() const override {
        return std::make_shared<GIndividualFactory<Derived>>(*this);
    }

    /***************************************************************************/
    /**
     * Builds the OA-owned adaption configuration for a genome produced by this factory, for the
     * adapting algorithms (typically passed to Go2::registerAdaptionConfig). Optional: it delegates to
     * the individual's @c static buildAdaptionConfig(const GGenome&, const Config&) hook if it
     * provides one, and otherwise returns a null pointer (the individual has no default adaption config,
     * e.g. a derivative-free or test individual). The configuration must have been parsed first (call
     * after a get_()/get() so config_ is populated).
     *
     * @param sample A sample genome produced by this factory, passed to the buildAdaptionConfig hook
     * @return The OA-owned adaption configuration, or a null pointer if the individual provides no hook
     */
    std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
    getAdaptionConfig(const GGenome &sample) const override {
        if constexpr (HasBuildAdaptionConfigHook<Derived>) {
            return Derived::buildAdaptionConfig(sample, config_);
        }
        else {
            return {};
        }
    }

protected:
    /***************************************************************************/
    /**
     * Describes the local configuration options -- delegated to the Derived static hook, which binds
     * the options to this factory's Config instance.
     *
     * @param gpb The parser builder the config-file options are registered on
     */
    void describeLocalOptions_(Gem::Common::GParserBuilder &gpb) override {
        Derived::describeConfig(gpb, config_);
    }

    /***************************************************************************/
    /**
     * Acts on the parsed configuration: builds the shared genome (value arrays + shared immutable
     * layout) exactly once, then installs it on the freshly produced individual (a cheap value-array
     * copy plus a bind to the shared layout handle).
     *
     * @param p The freshly produced individual to install the shared genome on (must be a GGenome)
     */
    void postProcess_(std::shared_ptr<GGenome> &p) override {
        // Build the shared genome exactly once (the cell serialises the first build; later produces read it
        // lock-free) and reuse it for every produced individual.
        const GenomeData &genome =
            genome_cell_.getOrCompute([this]() { return Derived::buildGenome(config_); });

        auto *fg = dynamic_cast<GGenome *>(p.get());
        if(fg == nullptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GIndividualFactory::postProcess_(): Error!" << '\n'
                << "The produced object is not a GGenome derivative" << '\n'
            );
        }
        fg->setGenome(genome);

        // Optional per-object configuration hook: lets a Derived individual apply its own non-genome
        // settings parsed into config_ (e.g. a transfer function) beyond building the genome. It is the
        // symmetric companion to buildAdaptionConfig(): an individual without the hook is simply left unconfigured.
        if constexpr (HasApplyConfigHook<Derived>) {
            Derived::applyConfig(*static_cast<Derived *>(fg), config_);
        }

    }

private:
    /***************************************************************************/
    /**
     * Creates an (empty-genome) individual of the desired type and registers its own base
     * GGenome configuration options (eval policy, validity thresholds, maxmode, ...) on the
     * parser via target->addConfigurationOptions(gpb).
     * Bound to this freshly produced object, those options are applied to it when GFactoryT parses (or
     * re-applies the cached) configuration. Derived-specific options are registered separately by
     * describeConfig(); the genome itself is installed in postProcess_ once the config has been parsed.
     *
     * @param gpb The parser builder the individual's base configuration options are registered on
     * @return A shared pointer to the freshly produced (empty-genome) individual
     */
    std::shared_ptr<GGenome> getObject_(Gem::Common::GParserBuilder &gpb) override {
        auto p = std::make_shared<Derived>();
        p->addConfigurationOptions(gpb);
        return p;
    }

    /***************************************************************************/
    /**
     * The default constructor; only needed for (de-)serialization, hence private. It hands a placeholder
     * path to the base (whose own default constructor is private); the real configFile is restored from
     * the archive by serialize() immediately afterwards. This lets an exported alias (e.g.
     * GFunctionIndividualFactory, serialized as part of a network-transported GMetaOptimizerIndividualT)
     * be reconstructed.
     */
    GIndividualFactory()
      : GGenomeFactory("empty") { /* nothing */
    }

    /***************************************************************************/
    // Data

    /** @brief Holds the configurable values, populated from the config file on each get_() */
    typename Derived::Config config_{};

    /** @brief The genome (value arrays + shared layout) built once and reused by every individual. The
     *  load-once cell serialises the single build and lets every later produce read it lock-free; a copied
     *  factory leaves it cold and rebuilds its own genome lazily. */
    mutable Gem::Common::Concurrency::GLoadOnceCellT<GenomeData> genome_cell_;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
