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
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

// Geneva headers go here
#include "common/GParserBuilder.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOAFactoryT.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * A CRTP-style scaffold for the per-algorithm factories. It generates the boilerplate every standard
 * optimization-algorithm factory repeats: the three constructors (the default one derives the config
 * path "./config/<class_name>.json"), getMnemonic() (= the personality nickname), getAlgorithmName()
 * (= the algorithm's human-readable name) and getObject_() (construct the algorithm and register its
 * configuration options).
 *
 * For the great majority of algorithms that IS the whole factory, so the factory is nothing but this
 * instantiation and needs no name of its own -- every built-in algorithm is registered as
 *
 *   GOptimizationAlgorithmFactoryT<GFoo>
 *
 * in src/oa/GBuiltinAlgorithms.cpp. An algorithm that needs more -- extra command-line options, a
 * postProcess_() step -- derives from the instantiation instead (GParameterScanFactory is the one
 * in-tree example):
 *
 *   class GFooFactory : public GOptimizationAlgorithmFactoryT<GFoo> { ... };
 *
 * Either way the result is the provider the algorithm store holds -- GOAFactoryT implements
 * Gem::Common::GProviderT -- so registering it (GInitializerT for a built-in, oaManifest() for a module)
 * needs no wrapper of any kind.
 *
 * @tparam oa_type                 The concrete algorithm. Must expose the GOptimizationAlgorithmT
 *                                 identifiers class_name and oa_algorithm_name, and the member alias
 *                                 personality_traits_type naming its own personality traits.
 * @tparam personality_traits_type The personality traits whose static `nickname` is the command-line
 *                                 mnemonic. Defaults to the algorithm's own traits, which is what every
 *                                 factory wants; spell it only to give an algorithm a second mnemonic.
 */
template <typename oa_type, typename personality_traits_type = typename oa_type::personality_traits_type>
class GOptimizationAlgorithmFactoryT : public GOAFactoryT<GOptimizationAlgorithmBase> {
public:
    /** @brief The default constructor: derives the config-file path from the algorithm's class name. */
    GOptimizationAlgorithmFactoryT()
      : GOAFactoryT<GOptimizationAlgorithmBase>(
            std::string("./config/") + std::string(oa_type::class_name) + ".json"
        ) { /* nothing */
    }

    /**
     * @brief Initialization with the name of the config file.
     *
     * @param config_file Path to the JSON configuration file from which the algorithm's options are read.
     */
    explicit GOptimizationAlgorithmFactoryT(std::filesystem::path const &config_file)
      : GOAFactoryT<GOptimizationAlgorithmBase>(config_file) { /* nothing */
    }

    /**
     * @brief Initialization with the config file and a content creator.
     *
     * @param config_file Path to the JSON configuration file from which the algorithm's options are read.
     * @param content_creator_ptr Factory used to populate the algorithm's population with individuals.
     */
    GOptimizationAlgorithmFactoryT(
        std::filesystem::path const &config_file,
        std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> content_creator_ptr
    )
      : GOAFactoryT<GOptimizationAlgorithmBase>(config_file, std::move(content_creator_ptr)) { /* nothing */
    }

    /**
     * @brief The copy constructor.
     *
     * @param The factory to be copied (default member-wise copy).
     */
    GOptimizationAlgorithmFactoryT(const GOptimizationAlgorithmFactoryT &) = default;
    /** @brief The destructor. */
    ~GOptimizationAlgorithmFactoryT() override = default;

    /**
     * @brief The command-line mnemonic / nickname describing this algorithm.
     *
     * @return The personality traits' nickname, used as the command-line mnemonic for this algorithm.
     */
    std::string getMnemonic() const override { return personality_traits_type::nickname; }
    /**
     * @brief The clear-text name of this algorithm.
     *
     * @return The human-readable algorithm name taken from oa_type::oa_algorithm_name.
     */
    std::string getAlgorithmName() const override { return std::string(oa_type::oa_algorithm_name); }

private:
    /**
     * @brief Creates the algorithm and registers its configuration options.
     *
     * @param gpb The parser builder onto which the new algorithm registers its configuration options.
     * @return A newly constructed algorithm of type oa_type, with its configuration options registered.
     */
    std::shared_ptr<GOptimizationAlgorithmBase>
    getObject_(Gem::Common::GParserBuilder &gpb) override {
        std::shared_ptr<oa_type> target(new oa_type());
        target->addConfigurationOptions(gpb);
        return target;
    }
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
