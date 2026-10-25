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

// Standard headers
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <vector>

// Boost headers

// Geneva headers
#include "common/GArchiveNamed.hpp"
#include "common/GParserBuilder.hpp"
#include "weft/GArchivePolymorphic.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"        // makeAdaptionConfig()
#include "geneva/oa/GAdaptionConfig.hpp"

/******************************************************************************/
/**
 * @brief A minimal optimization problem, written from scratch, to be built as a RUNTIME-LOADABLE plugin.
 *
 * This is an ordinary Geneva flat individual -- nothing about it is plugin-specific. It becomes loadable
 * purely because the companion translation unit (GLoadableParaboloidPlugin.cpp) gives it a small extern "C"
 * geneva_module_manifest() entry point via Gem::Geneva::individualManifest(). The very same class could
 * equally be COMPILED INTO a Geneva binary.
 *
 * The problem is an @f$ n @f$-dimensional paraboloid @f$ f(x)=\sum_i x_i^2 @f$, minimised at the origin.
 *
 * It follows the config-driven flat-individual pattern that GIndividualFactory<Derived> drives:
 *  - a @c Config struct of tunables, bound to the config file by @c describeConfig();
 *  - @c buildGenome() turning the config into the (structure-only) genome;
 *  - @c buildAdaptionConfig() supplying the OA-owned mutation config (the "config-strip" model: the
 *    adaptors live on the algorithm, not the genome), which the factory exposes through its base so a
 *    generic launcher can wire it up WITHOUT knowing this concrete type;
 *  - @c evaluate() computing the objective;
 *  - @c serialize() + a GEM_REGISTER_ARCHIVABLE in the .cpp, so the individual can cross the wire / a checkpoint.
 */
class GLoadableParaboloid : public Gem::Geneva::Genome::GGenomeT<GLoadableParaboloid> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    /** @brief The factory installs the genome via setGenome(), so the default constructor leaves it empty. */
    GLoadableParaboloid() = default;
    GLoadableParaboloid(const GLoadableParaboloid &) = default;

    /** @brief The values read from this problem's configuration file (with in-source defaults, so the
     *  problem also works with no config file at all). */
    //! [modules-individual-hooks]
    struct Config {
        std::size_t par_dim = 2; ///< the number of parameters (dimensions)
        double min = -10.;       ///< the lower boundary of each parameter
        double max = 10.;        ///< the upper boundary of each parameter
        double sigma = 0.5;      ///< the initial Gauss mutation width (a fraction of the range)
    };

    /** @brief Registers this problem's config-file options, binding them to @p c. */
    static void describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
        gpb.registerFileParameter<std::size_t>(
            "par_dim", c.par_dim, c.par_dim, Gem::Common::VAR_IS_ESSENTIAL, "Number of parameters"
        );
        gpb.registerFileParameter<double>("min", c.min, c.min, Gem::Common::VAR_IS_ESSENTIAL, "Lower bound");
        gpb.registerFileParameter<double>("max", c.max, c.max, Gem::Common::VAR_IS_ESSENTIAL, "Upper bound");
        gpb.registerFileParameter<double>(
            "sigma", c.sigma, c.sigma, Gem::Common::VAR_IS_ESSENTIAL, "Initial Gauss sigma"
        );
    }

    /** @brief Builds the (structure-only) genome + its shared, immutable layout from the parsed config. */
    static Gem::Geneva::Genome::GenomeData buildGenome(const Config &c) {
        Gem::Geneva::Genome::GGenomeBuilder b;
        b.addDoubleGroup(c.par_dim, c.min, c.max); // structure only; the adaptor lives on the OA config
        return b.build();
    }
    //! [modules-individual-hooks]

    /** @brief The OA-owned Gauss adaption config for a genome this problem produces. Reached generically
     *  through GGenomeFactory::getAdaptionConfig(), so a launcher need not know this type. */
    static std::shared_ptr<Gem::Geneva::OptimizationAlgorithms::GAdaptionConfigBase>
    buildAdaptionConfig(const Gem::Geneva::Genome::GGenome &sample, const Config &c) {
        namespace oa = Gem::Geneva::OptimizationAlgorithms;
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(sample);
        for(std::size_t i = 0; i < cfg->doubleGroups().size(); ++i) {
            cfg->groupDouble(i).gauss(c.sigma, 0.8, 1e-3, 2., 1.);
        }
        return cfg;
    }

protected:
    /** @brief The objective: an n-dimensional paraboloid f(x) = sum_i x_i^2, minimised at the origin
     *  (single criterion, returned as a one-element vector). */
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline<double>(v);
        return {std::ranges::fold_left(
            v | std::views::transform([](double x) { return x * x; }), 0., std::plus{})};
    }

private:
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<Gem::Geneva::Genome::GGenomeT<GLoadableParaboloid>>(ar, "GGenomeT", *this);
    }
};

/******************************************************************************/
