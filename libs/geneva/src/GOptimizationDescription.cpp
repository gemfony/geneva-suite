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

#include "geneva/GOptimizationDescription.hpp"

// Standard header files go here
#include <format>
#include <mutex>

// Boost header files go here
// Boost.DLL locates this library on disk, next to which the consumer modules are installed.
#define BOOST_DLL_USE_STD_FS 1
#include <boost/dll/runtime_symbol_info.hpp> // this_line_location()

// Geneva header files go here
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GFatalReport.hpp"
#include "common/GLogger.hpp"
#include "geneva/GOptimizer.hpp"
#include "geneva/oa/GFactoryStore.hpp"
#include "hap/GRandomFactory.hpp"
#include "weft/GArchivePolymorphic.hpp" // verifyArchiveRegistrations()

// The launcher of Geneva's item type, instantiated once for every Geneva program
template class Gem::Courtier::GLauncherT<Gem::Geneva::Genome::GGenome>;

namespace Gem::Geneva {

/******************************************************************************/
/**
 * The consumer modules lie in the directory of this library; the build names their prefix
 * (GENEVA_CONSUMER_MODULE_PREFIX, "libgemfony-geneva-").
 */
Gem::Courtier::GConsumerModules consumerModules() {
    static const Gem::Courtier::GConsumerModules location{
        .directory = boost::dll::this_line_location().parent_path(),
        .prefix = GENEVA_CONSUMER_MODULE_PREFIX
    };
    return location;
}

/******************************************************************************/
/**
 * An error nobody handles ends the program with a message and the exit code 1, not an abort.
 */
GOptimizationDescription::GOptimizationDescription() {
    Gem::Common::installFatalReport();
}

/******************************************************************************/
void GOptimizationDescription::addConfiguration(Gem::Common::GParserBuilder &gpb) {
    gpb.registerFileParameter<std::uint16_t>(
        "n_producer_threads",
        std::uint16_t{0},
        [this](std::uint16_t n) { n_producer_threads_ = n; }
    ) << "The number of threads simultaneously producing random numbers";
    gpb.registerFileParameter<bool>(
        "copy_best_individuals_only",
        true,
        [this](bool copy) { copy_best_individuals_only_ = copy; }
    ) << "Indicates whether only the best individuals should be copied when"
      << '\n'
      << "switching from one optimization algorithm to the next";
    gpb.registerFileParameter<std::string>(
        "individual_plugin_path",
        std::string(),
        [this](std::string const &p) { file_individual_ = p; }
    ) << "Filesystem path to a runtime individual (optimization-problem) plugin (.so) to load at "
         "startup."
      << '\n'
      << "Empty (the default) means the individual is compiled into this binary. The --individual"
      << '\n'
      << "command-line option overrides this setting.";
}

/******************************************************************************/
void GOptimizationDescription::addEarlyOptions(Gem::Common::GParserBuilder &gpb) {
    gpb.registerCLParameter<std::string>(
        "individual,i",
        cl_individual_,
        std::string(),
        "Filesystem path to a runtime individual (optimization-problem) plugin (.so) to load at "
        "startup. Overrides the individual_plugin_path config-file setting. Omit it to use an "
        "individual compiled into this binary."
    );
}

/******************************************************************************/
/**
 * The handlers count into one record for all modules of the program: one individual in all.
 */
std::vector<Gem::Common::GContributionHandler> GOptimizationDescription::moduleHandlers() {
    return genevaContributionHandlers(taken_);
}

/******************************************************************************/
std::vector<std::filesystem::path> GOptimizationDescription::modulesToLoad() {
    const std::string &path = cl_individual_.empty() ? file_individual_ : cl_individual_;
    if(path.empty()) {
        return {};
    }
    return {std::filesystem::path(path)};
}

/******************************************************************************/
std::optional<Gem::Courtier::GConsumerModules> GOptimizationDescription::consumerModules() const {
    return Gem::Geneva::consumerModules();
}

/******************************************************************************/
/**
 * Called once the modules are loaded: the module named as the individual must have contributed
 * it (--module makes no such promise, so a module loaded with it is not checked).
 */
void GOptimizationDescription::addOptions(Gem::Common::GParserBuilder &gpb) {
    if(const auto named = modulesToLoad(); not named.empty()) {
        const auto same = [](const std::filesystem::path &a, const std::filesystem::path &b) {
            return std::filesystem::weakly_canonical(a) == std::filesystem::weakly_canonical(b);
        };
        if(not taken_.individual or not same(named.front(), taken_.individual_module)) {
            throw Gem::Common::gem_exception(
                Gem::Common::g_error_streamer()
                << "'" << named.front().string()
                << "' was given as the optimization problem (--individual), but the module"
                << " carries no individual." << '\n'
                << "A module that contributes algorithms rather than a problem is loaded with "
                   "--module."
                << '\n'
            );
        }
    }

    std::string listing;
    std::vector<std::string> keys;
    oaFactoryStore()->getKeyVector(keys);
    for(const auto &key : keys) {
        listing += std::format("{}:  {}\n", key, oaFactoryStore()->get(key)->getName());
    }
    gpb.beginCLSection("Optimization options", "Options of the optimization algorithms");
    gpb.registerCLParameter<std::string>(
        "optimizationAlgorithms,a",
        cl_algorithms_,
        std::string(),
        std::format(
            "A comma-separated list of optimization algorithms, e.g. \"arg1,arg2\". {} algorithms "
            "have been registered: \n{}",
            keys.size(),
            listing
        )
    );
    gpb.registerCLParameter<std::string>(
        "cp_file,f",
        cp_file_,
        std::string("empty"),
        "A file (including its path) holding a checkpoint for the first optimization algorithm. "
        "The run continues from the state it holds, with the settings of this job: halt criteria, "
        "checkpoint settings, threads and report interval come from this job's configuration and "
        "command line, and only the optimization monitors registered by this job observe the run"
    );
    for(const auto &factory : oaFactoryStore()->getContentSnapshot()) {
        factory->addCLOptions(gpb);
    }
}

/******************************************************************************/
/**
 * A process that is not the submitter only evaluates and draws few random numbers: it is kept off
 * the GPU, so that many clients on one machine do not each need a CUDA context. The archive check
 * runs once per process, after every module is loaded: a type that is registered for its identity
 * but not for the archive fails here, not at the first (possibly remote) deserialization.
 */
void GOptimizationDescription::configure(Gem::Courtier::processRole role) {
    if(role != Gem::Courtier::processRole::SUBMITTER) {
        Gem::Hap::setGpuRandomNumbersAllowed(false);
    }
    static std::once_flag archive_check;
    std::call_once(archive_check, [] {
        Gem::Weft::verifyArchiveRegistrations();
        GEM_LOGGER << "GArchive registration self-check passed: "
                   << Gem::Weft::archiveRegisteredHierarchyCount()
                   << " polymorphic hierarchies are fully archive-dispatchable." << '\n'
                   << GEM_LOGGING;
    });
    static std::once_flag random_numbers;
    std::call_once(random_numbers, [this] {
        Gem::Hap::randomFactory()->setNProducerThreads(n_producer_threads_);
    });
}

/******************************************************************************/
std::vector<std::string> GOptimizationDescription::algorithms() const {
    return cl_algorithms_.empty() ? std::vector<std::string>{}
                                  : Gem::Common::splitString(cl_algorithms_, ",");
}

const std::string &GOptimizationDescription::checkpointFile() const {
    return cp_file_;
}

bool GOptimizationDescription::copyBestIndividualsOnly() const {
    return copy_best_individuals_only_;
}

std::uint16_t GOptimizationDescription::nProducerThreads() const {
    return n_producer_threads_;
}

GIndividualFactoryPtr GOptimizationDescription::loadedIndividual() const {
    return taken_.individual;
}

/******************************************************************************/
int runOptimization(int argc, char **argv, GOptimizationProgram program) {
    GOptimizationDescription description;
    Gem::Courtier::GLauncherT<Genome::GGenome> launcher(argc, argv, program.configFile);
    return launcher.run(
        description,
        {.userOptions = std::move(program.userOptions),
         .prepare = std::move(program.prepare),
         .produce = [&description, &produce = program.produce]() -> int {
             if(not produce) {
                 return 0;
             }
             GOptimizer go(description);
             return produce(go, description);
         }}
    );
}

/******************************************************************************/

} /* namespace Gem::Geneva */
