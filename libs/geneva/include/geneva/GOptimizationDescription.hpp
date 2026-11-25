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
#include "geneva/GNamespaceAliases.hpp"

// Standard header files go here
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Geneva header files go here
#include "common/GModuleLoader.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GLauncherT.hpp"
#include "geneva/GModuleLoader.hpp"
#include "geneva/genome/GGenome.hpp"

namespace Gem::Geneva {

class GOptimizer;

/******************************************************************************/
/**
 * @brief Where Geneva's consumer modules lie: next to libgemfony-geneva, the module of the
 * consumer @c m as libgemfony-geneva-m.so (stc, beast, mqtt, mpi and gpu, as far as they are
 * built). A program that builds its consumer or client without a launcher hands this to
 * courtier's functions (Gem::Courtier::buildConsumer() and its neighbours).
 * @return Their location
 */
[[nodiscard]] Gem::Courtier::GConsumerModules consumerModules();

/******************************************************************************/
/**
 * @brief What Geneva, the producer of optimization work, says about itself to courtier's launcher
 * (Gem::Courtier::GLauncherT<GGenome>), in every process of a program:
 *  - the keys of the configuration file (n_producer_threads, copy_best_individuals_only,
 *    individual_plugin_path) and the option --individual, read before modules are loaded;
 *  - what Geneva takes from a module (an individual, optimization algorithms, GPU marshallers)
 *    and the module of the individual; the consumer modules of GGenome (consumerModules());
 *  - the options -a, --cp_file and those of the optimization algorithms;
 *  - once the role of the process is known: the random number factory, no GPU random numbers in a
 *    process that is not the submitter, and the check that every polymorphic type is archivable.
 *
 * GOptimizer is built from it on the submitter (GOptimizer(const GOptimizationDescription &)). Its
 * constructor installs the fatal report (Gem::Common::installFatalReport()): an error nobody
 * handles ends the program with its message and the exit code 1.
 */
class GOptimizationDescription final : public Gem::Courtier::GProducerDescriptionI {
public:
    /** @brief Installs the fatal report; nothing is read before the launcher runs */
    GOptimizationDescription();

    /** @brief The keys of the configuration file @param gpb The launcher's parser */
    void addConfiguration(Gem::Common::GParserBuilder &gpb) override;
    /** @brief --individual, which names a module to load @param gpb The launcher's parser */
    void addEarlyOptions(Gem::Common::GParserBuilder &gpb) override;
    /** @brief Individuals, optimization algorithms and GPU marshallers @return The handlers */
    std::vector<Gem::Common::GContributionHandler> moduleHandlers() override;
    /** @brief The module of the individual, if one is named @return Its path, or nothing */
    std::vector<std::filesystem::path> modulesToLoad() override;
    /** @brief Geneva's consumer modules @return consumerModules() */
    [[nodiscard]] std::optional<Gem::Courtier::GConsumerModules> consumerModules() const override;
    /**
     * @brief -a, --cp_file and the options of the optimization algorithms (listed by --showAll)
     * @param gpb The launcher's parser
     * @throws gem_exception if the module named by --individual carries no individual
     */
    void addOptions(Gem::Common::GParserBuilder &gpb) override;
    /** @brief Random numbers and the archive check, by role @param role The role of the process */
    void configure(Gem::Courtier::processRole role) override;

    /** @brief The algorithms named with -a, in their order @return Them; empty if -a is not given */
    [[nodiscard]] std::vector<std::string> algorithms() const;
    /** @brief The checkpoint named with --cp_file @return Its path; "empty" if none is named */
    [[nodiscard]] const std::string &checkpointFile() const;
    /** @brief Whether only the best individuals pass from one algorithm to the next
     *  @return The key copy_best_individuals_only */
    [[nodiscard]] bool copyBestIndividualsOnly() const;
    /** @brief The threads of the random number factory @return The key n_producer_threads */
    [[nodiscard]] std::uint16_t nProducerThreads() const;
    /** @brief The individual a module contributed @return Its factory; null if none did */
    [[nodiscard]] GIndividualFactoryPtr loadedIndividual() const;

private:
    std::uint16_t n_producer_threads_ = 0;   ///< The key n_producer_threads
    bool copy_best_individuals_only_ = true; ///< The key copy_best_individuals_only
    std::string file_individual_;            ///< The key individual_plugin_path
    std::string cl_individual_;              ///< --individual
    std::string cl_algorithms_;              ///< -a, as given
    std::string cp_file_ = "empty";          ///< --cp_file
    LoadedModule taken_;                     ///< What the modules contributed
};

/******************************************************************************/
/**
 * @brief What an optimization program does, for runOptimization(). Only produce() is needed.
 */
struct GOptimizationProgram {
    /** The configuration file of the launcher and of Geneva */
    std::filesystem::path configFile = "./config/GOptimizer.json";
    /** Registers the program's own options into the launcher's parser */
    std::move_only_function<void(Gem::Common::GParserBuilder &)> userOptions;
    /** Every process, once its role is known: may end the program by returning an exit code */
    std::move_only_function<std::optional<int>(Gem::Courtier::processRole)> prepare;
    /** The submitter only: the optimization, on a GOptimizer built from the description; its value
     *  is the exit code */
    std::move_only_function<int(GOptimizer &, const GOptimizationDescription &)> produce;
};

/**
 * @brief The short form of an optimization program's main(): Geneva's description handed to
 * courtier's launcher, which owns the command line, the modules, the consumer, the role of the
 * process and a client's run; produce() runs on the submitter with a GOptimizer built from the
 * description. A program that sets something on the launcher builds description and launcher
 * itself (the long form: Gem::Courtier::GLauncherT<GGenome>::run(description, program)).
 * @param argc The number of arguments @param argv The arguments
 * @param program What the program does
 * @return The exit code (see Gem::Courtier::GLauncherT::run())
 */
int runOptimization(int argc, char **argv, GOptimizationProgram program);

/******************************************************************************/

} /* namespace Gem::Geneva */

// The launcher of Geneva's item type is instantiated once, in libgemfony-geneva
extern template class Gem::Courtier::GLauncherT<Gem::Geneva::Genome::GGenome>;
