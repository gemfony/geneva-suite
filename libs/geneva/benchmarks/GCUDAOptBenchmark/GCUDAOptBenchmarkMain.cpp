/**
 * @file GCUDAOptBenchmarkMain.cpp
 *
 * Main driver for the GPU-accelerated algorithm comparison benchmark.
 *
 * The whole population of each generation is scored in one bulk, runtime-compiled kernel launch
 * through the unified courtier GPU consumer (Gem::Courtier::GPU::GGPUConsumerT) -- the SAME consumer
 * example 14 uses. There is no build-time CUDA compilation unit any more: the kernel
 * (kernels/benchmark_eval.cu via NVRTC) is loaded at run time from config/GGPUConsumer.json. The GPU
 * consumer is device-only (backend cuda); a CPU run uses the individual's own evaluate() via a CPU
 * consumer such as --consumer stc.
 */

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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

#include "common/GConfigEmission.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "courtier/gpu/GGPUConsumer.hpp"
#include "geneva/GenevaInitializer.hpp"
#include "GAlgorithmBenchmarkRunner.hpp"
#include "GBenchmarkGPUMarshaller.hpp"
#include "GBenchmarkResultWriter.hpp"

namespace po = boost::program_options;
using namespace Gem::Geneva;
using namespace Gem::Geneva::Benchmarks;

/******************************************************************************/
/**
 * @brief Reads BenchmarkConfig from a JSON config file via Gem::Common::GParserBuilder.
 *
 * The benchmark uses the same configuration facility as the rest of Geneva (Inv 1) rather than a
 * hand-rolled reader: a missing file is created with defaults, and the layout stays consistent with
 * every other Geneva config. The array of algorithm entries is expressed as three equal-length,
 * parallel string vectors (@c algo_tags / @c algo_mnemonics / @c algo_config_files), the @c i-th
 * element of each describing one AlgorithmEntry, since GParserBuilder models homogeneous vectors
 * rather than arrays of heterogeneous records.
 *
 * @param configFile The path to the configuration file to read (created with defaults if absent)
 * @return A populated BenchmarkConfig
 */
static BenchmarkConfig loadConfig(const std::string &configFile) {
    BenchmarkConfig cfg;

    // The three parallel vectors describing the algorithm entries. Their defaults reproduce the
    // canonical ea/sa/swarm comparison, so a freshly created config runs a meaningful benchmark.
    std::vector<std::string> algoTags{"ea_default", "sa_default", "swarm_default"};
    std::vector<std::string> algoMnemonics{"ea", "sa", "swarm"};
    std::vector<std::string> algoConfigFiles{
        "config/GEvolutionaryAlgorithm.json",
        "config/GSimulatedAnnealing.json",
        "config/GSwarmAlgorithm.json"
    };

    Gem::Common::GParserBuilder gpb;

    gpb.registerFileParameter("benchmark_function", cfg.functionName, cfg.functionName)
        << "The GFunctionIndividual demo function to optimize (e.g. PARABOLA)";
    gpb.registerFileParameter("n_runs", cfg.nRuns, cfg.nRuns)
        << "The number of independent runs per algorithm entry";
    gpb.registerFileParameter("n_dimensions", cfg.nDimensions, cfg.nDimensions)
        << "The parameter dimension of the optimization problem";
    gpb.registerFileParameter("individual_config", cfg.individualConfigFile, cfg.individualConfigFile)
        << "The path to the GFunctionIndividual configuration file";
    gpb.registerFileParameter("output_dir", cfg.outputDir, cfg.outputDir)
        << "The directory into which the output CSV files are written";
    gpb.registerFileParameter("batch_size", cfg.batchSize, cfg.batchSize)
        << "The GPU consumer batch size (0 = flush by timeout)";
    gpb.registerFileParameter("flush_timeout_ms", cfg.flushTimeoutMs, cfg.flushTimeoutMs)
        << "The GPU consumer flush timeout in milliseconds (used when batch_size == 0)";

    gpb.registerFileParameter("algo_tags", algoTags, algoTags)
        << "Free labels for the algorithm entries, one per entry (e.g. ea_pop100)";
    gpb.registerFileParameter("algo_mnemonics", algoMnemonics, algoMnemonics)
        << "Algorithm mnemonics, one per entry: ea, sa, swarm, gd or cgd";
    gpb.registerFileParameter("algo_config_files", algoConfigFiles, algoConfigFiles)
        << "Algorithm-specific config file paths, one per entry";

    if (not gpb.parseConfigFile(configFile)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GCUDAOptBenchmark loadConfig(): Error!" << '\n'
            << "Could not parse configuration file " << configFile << '\n'
        );
    }

    if (algoTags.size() != algoMnemonics.size() || algoTags.size() != algoConfigFiles.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GCUDAOptBenchmark loadConfig(): Error!" << '\n'
            << "The algo_tags (" << algoTags.size() << "), algo_mnemonics ("
            << algoMnemonics.size() << ") and algo_config_files (" << algoConfigFiles.size()
            << ") vectors must all have the same length." << '\n'
        );
    }

    cfg.algorithms.clear();
    for (std::size_t i = 0; i < algoTags.size(); ++i) {
        cfg.algorithms.push_back(AlgorithmEntry{algoTags[i], algoMnemonics[i], algoConfigFiles[i]});
    }

    return cfg;
}

/******************************************************************************/

int main(int argc, char **argv) {
    std::string configFile = "config/GCUDAOptBenchmark.json";

    // --update-configs: materialize every config this benchmark owns -- its own config, each configured
    // algorithm's config, the individual config and the GPU consumer config -- from code defaults, then
    // exit without benchmarking. The GPU consumer constructor only loads its config (the backend/kernel
    // are acquired lazily on the first dispatch), so no device is required. Handled before the
    // program-options parser, which would otherwise reject the unregistered switch.
    if (Gem::Common::configEmissionRequested(argc, argv)) {
        Gem::Common::beginConfigEmission();
        Gem::Geneva::GenevaInitializer const gi;
        BenchmarkConfig const cfg = loadConfig(configFile);
        GAlgorithmBenchmarkRunner(cfg).emitConfigs();
        auto marshaller = std::make_shared<GBenchmarkGPUMarshaller>();
        Gem::Courtier::GPU::GGPUConsumerT<gen::GGenome>("./config/GGPUConsumer.json", marshaller);
        Gem::Common::finishConfigEmission();
    }

    po::options_description desc("GCUDAOptBenchmark options");
    desc.add_options()
        ("help,h",   "Show help message")
        ("config,c", po::value<std::string>(&configFile),
                     "Path to GCUDAOptBenchmark.json (default: config/GCUDAOptBenchmark.json)");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help")) {
        std::cout << desc << std::endl;
        return 0;
    }

    // A missing config file is created with defaults by GParserBuilder (the Geneva idiom), so there is
    // no need to guard against its absence here.
    BenchmarkConfig cfg;
    try {
        cfg = loadConfig(configFile);
    } catch (const std::exception &e) {
        std::cerr << "Failed to parse config '" << configFile << "': " << e.what() << std::endl;
        return 1;
    }

    if (cfg.algorithms.empty()) {
        std::cerr << "No algorithm entries in config. Nothing to do." << std::endl;
        return 1;
    }

    std::filesystem::create_directories(cfg.outputDir);

    std::cout << "GCUDAOptBenchmark\n"
              << "  function   : " << cfg.functionName  << "\n"
              << "  dimensions : " << cfg.nDimensions   << "\n"
              << "  runs/algo  : " << cfg.nRuns          << "\n"
              << "  algorithms : " << cfg.algorithms.size() << "\n"
              << "  output dir : " << cfg.outputDir      << "\n"
              << std::endl;

    // Initialize Geneva — must outlive the runner and all optimization.
    Gem::Geneva::GenevaInitializer const gi;

    // Build the unified GPU consumer (the SAME GGPUConsumerT example 14 uses) and register it as the
    // process consumer. The whole population is scored in one bulk, runtime-compiled kernel launch; the
    // kernel is selected in config/GGPUConsumer.json (the GPU consumer is device-only). The clone function
    // is the polymorphic GGenome clone needed by the clone-on-partial-return policy.
    auto marshaller = std::make_shared<GBenchmarkGPUMarshaller>();
    auto consumer = std::make_shared<Gem::Courtier::GPU::GGPUConsumerT<gen::GGenome>>(
        "./config/GGPUConsumer.json", marshaller);
    consumer->setCloneFunction([](const std::unique_ptr<gen::GGenome> &p) {
        return p->clone();
    });
    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().setConsumer(consumer);

    GAlgorithmBenchmarkRunner runner(cfg);
    const auto results = runner.run();

    for (const auto &r : results) {
        GBenchmarkResultWriter::writeRawCSV(r, cfg.outputDir);
    }
    GBenchmarkResultWriter::writeSummaryCSV(results, cfg.outputDir);
    GBenchmarkResultWriter::printSummary(results);

    return 0;
}
