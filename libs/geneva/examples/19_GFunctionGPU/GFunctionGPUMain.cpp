/**
 * @file GFunctionGPUMain.cpp
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

/**
 * @file
 * Example 19 -- a stock GFunctionIndividual (the standard benchmark-function individual, e.g. the
 * parabola) evaluated on the GPU through the unified courtier GPU consumer, selected purely by
 * "--consumer gpu".
 *
 * It demonstrates the store-based GPU path end-to-end for an ORDINARY library individual (not the
 * image-specific example 14): the problem contributes nothing but a marshaller
 * (GFunctionGPUMarshaller, which derives Gem::Geneva::GBaseGPUMarshallerT<double> and adds only the
 * function-id problem constant) via registerGPUMarshaller("cuda", ...) before constructing Go2, and Go2
 * builds + selects the generic GGPUConsumerT like any other consumer. The device kernel
 * (kernels/function_eval.cu, runtime-compiled by NVRTC) runs the SAME benchmark-function math as
 * GFunctionIndividual::evaluate() (Gem::Geneva::Benchmarks::eval), so a GPU run and a CPU run
 * (--consumer stc) agree. Because the individual is GPU-blind, "--consumer stc" evaluates it on the CPU
 * with no code change.
 *
 * PARITY-CHECK MODE (config key "parity_check_n" in config/GFunctionGPUGeneral.json, default 0 == off):
 * when set to a positive N, the program does NOT optimize. Instead it draws N random individuals from the
 * factory and, for each, compares the CPU reference fitness (the individual's own evaluate()) against the
 * GPU fitness (the CUDA kernel), prints per-item and summary relative errors and a PASS/FAIL verdict, then
 * exits. The scalar is double, so parity is near-exact; a small relative tolerance with a best-of-N
 * criterion (Inv 18) absorbs any reduction-order rounding.
 */

// Standard headers
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <vector>

// Geneva headers
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/gpu/GGPUConsumer.hpp"
#include "geneva/GMarshallerSetup.hpp"
#include "geneva/Go2.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"

// Example-local headers
#include "GFunctionGPUMarshaller.hpp"

using namespace Gem::Geneva;
namespace gpu = Gem::Courtier::GPU;
namespace gen = Gem::Geneva::Genome;
namespace gind = Gem::Geneva::Individuals;

namespace {

/******************************************************************************/
/**
 * @brief GPU/CPU parity check: cross-checks the CUDA kernel against GFunctionIndividual::evaluate().
 *
 * Draws @p n random individuals from the factory and, for each, evaluates it once on the CPU (its own
 * evaluate(), through the public process() path) and once on the GPU (the device kernel, via a directly
 * built GGPUConsumerT -- the same one the "gpu" mnemonic builds). It reports per-item and summary relative
 * errors and a best-of-N PASS/FAIL verdict. Because the benchmark functions are defined in double and the
 * kernel evaluates each item with a single sequential sum (like the CPU), agreement is near-exact.
 *
 * @param n The number of random individuals to cross-check
 * @param consumerConfig The GPU-consumer config file (backend + kernel selection)
 * @return EXIT_SUCCESS on parity pass, EXIT_FAILURE otherwise
 */
// Summary statistics over the per-item relative errors: logs min/median/mean/max and the within-tolerance
// fraction, and returns that fraction (which drives the pass/fail decision).
double reportParitySummary(const std::vector<double> &rel, int n, double rel_tol, double pass_fraction) {
    std::vector<double> sorted = rel;
    std::sort(sorted.begin(), sorted.end());
    const double relMin = sorted.front();
    const double relMax = sorted.back();
    const double relMedian = sorted[sorted.size() / 2];
    const double relMean = std::accumulate(rel.begin(), rel.end(), 0.0) / static_cast<double>(n);
    const std::size_t within = static_cast<std::size_t>(
        std::count_if(rel.begin(), rel.end(), [&](double r) { return r <= rel_tol; }));
    const double withinFraction = static_cast<double>(within) / static_cast<double>(n);

    glogger << std::format(
                   "Parity relative error over {} items:  min={:.3e}  median={:.3e}  mean={:.3e}  max={:.3e}",
                   n, relMin, relMedian, relMean, relMax)
            << '\n'
            << std::format(
                   "Within tolerance ({:.1e}):  {}/{} = {:.1f}%  (pass threshold {:.0f}%)",
                   rel_tol, within, n, 100.0 * withinFraction, 100.0 * pass_fraction)
            << '\n'
            << GLOGGING;

    return withinFraction;
}

int runParityCheck(int n, const std::string &consumerConfig) {
    using gen::GGenome;

    // Double-precision sequential sums on both sides: a tight relative tolerance suffices, with a best-of-N
    // criterion (Inv 18) so a single reduction-order outlier does not fail the whole check.
    constexpr double kRelTol = 1.0e-9;
    constexpr double kPassFraction = 0.90;
    constexpr double kEps = 1.0e-12; // floor for the relative-error denominator (near-zero fitness)

    glogger << "Example 19 (GFunctionGPU): PARITY CHECK -- CPU evaluate() vs GPU kernel over " << n
            << " random individuals" << '\n'
            << GLOGGING;

    gind::GFunctionIndividualFactory f("./config/GFunctionIndividual.json");

    // Draw n random individuals for the CPU reference and, for each, an independent clone for the GPU so the
    // two paths score IDENTICAL genomes (each process() stores its result on its own item).
    std::vector<std::shared_ptr<gind::GFunctionIndividual>> cpuInds;
    std::vector<std::unique_ptr<GGenome>> gpuBatch;
    cpuInds.reserve(static_cast<std::size_t>(n));
    gpuBatch.reserve(static_cast<std::size_t>(n));
    for(int i = 0; i < n; ++i) {
        auto ind = f.get_as<gind::GFunctionIndividual>();
        ind->randomInit(activityMode::ALLPARAMETERS); // spread the genome across its range (the factory
                                                      // hands back the init value, not a random point)
        gpuBatch.push_back(ind->clone());      // identical-genome copy for the device path
        cpuInds.push_back(ind);
    }

    // CPU reference: run each individual's own evaluation through the public process() path (the same
    // channel a CPU consumer uses) and read the stored raw fitness.
    std::vector<double> fitness_cpu(static_cast<std::size_t>(n));
    for(int i = 0; i < n; ++i) {
        cpuInds[i]->set_processing_status(Gem::Courtier::processingStatus::DO_PROCESS);
        cpuInds[i]->process();
        fitness_cpu[i] = cpuInds[i]->raw_fitness(0);
    }

    // GPU: build the device marshaller + consumer directly (the same GGPUConsumerT the "gpu" mnemonic
    // builds) and evaluate the whole batch in one bulk launch. full_success_or_fatal: every item must be evaluated.
    auto marshaller = std::make_shared<FunctionGPU::GFunctionGPUMarshaller>();
    auto consumer = std::make_shared<gpu::GGPUConsumerT<GGenome, double>>(consumerConfig, marshaller);
    consumer->setCloneFunction([](const std::unique_ptr<GGenome> &p) { return p->clone(); });
    consumer->processBatch(
        std::span<std::unique_ptr<GGenome>>(gpuBatch.data(), gpuBatch.size()),
        Gem::Courtier::GSubmissionPolicy::full_success_or_fatal());

    std::vector<double> fitness_gpu(static_cast<std::size_t>(n));
    for(int i = 0; i < n; ++i) {
        fitness_gpu[i] = gpuBatch[i]->raw_fitness(0);
    }

    // Per-item relative error, plus a per-item report for the first few.
    std::vector<double> rel(static_cast<std::size_t>(n));
    const int nShow = std::min(n, 8);
    for(int i = 0; i < n; ++i) {
        const double denom = std::max(std::abs(fitness_cpu[i]), kEps);
        rel[i] = std::abs(fitness_gpu[i] - fitness_cpu[i]) / denom;
        if(i < nShow) {
            glogger << std::format(
                           "  item {:3d}:  cpu={:.12g}  gpu={:.12g}  rel-err={:.3e}",
                           i, fitness_cpu[i], fitness_gpu[i], rel[i])
                    << '\n'
                    << GLOGGING;
        }
    }

    // Summary statistics over the relative errors (logs the report, returns the within-tolerance fraction).
    const double withinFraction = reportParitySummary(rel, n, kRelTol, kPassFraction);

    const bool pass = withinFraction >= kPassFraction;
    glogger << (pass ? "PARITY PASS" : "PARITY FAIL") << '\n' << GLOGGING;

    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // anonymous namespace

/******************************************************************************/
int main(int argc, char **argv) {
    // ---- example-specific settings, read from a Geneva config file ----------------------------
    std::string consumerConfig;
    int parityCheckN = 0;

    Gem::Common::GParserBuilder gpb;
    gpb.registerFileParameter<std::string>(
        "gpu_config", consumerConfig, std::string("./config/GGPUConsumer.json"),
        Gem::Common::VAR_IS_ESSENTIAL,
        "The courtier GPU consumer configuration file (backend + kernel selection)");
    gpb.registerFileParameter<int>(
        "parity_check_n", parityCheckN, 0, Gem::Common::VAR_IS_SECONDARY,
        "If > 0: run a GPU/CPU parity check over this many random individuals (CPU evaluate() vs the CUDA"
        " kernel) and exit, instead of optimizing; 0 == off (normal optimization run)");
    gpb.parseConfigFile("./config/GFunctionGPUGeneral.json");

    // ---- contribute the GPU marshaller; select it with "--consumer gpu" -----------------------
    // Registering BEFORE constructing Go2 makes gpu a normal construction-time mnemonic: Go2 builds the
    // generic GGPUConsumerT around this marshaller. The GPU consumer is device-only -- a CPU run uses the
    // individual's own evaluate() via a CPU consumer (the default "--consumer stc"), no code change.
    registerGPUMarshaller<FunctionGPU::GFunctionGPUMarshaller>("cuda", consumerConfig);

    // Go2 parses its own framework options from the command line / its own config file.
    Go2 go(argc, argv, "./config/Go2.json");

    // ---- --update-configs: materialize the configs this example owns, then exit ---------------
    // Go2 forces the local thread-pool consumer for a config refresh, so the GPU consumer is not built and
    // its config would be missed; materialize it directly here (the constructor only loads its config; the
    // backend/kernel are acquired lazily on the first dispatch, so no device is required).
    if(go.updateConfigsMode()) {
        auto marshaller = std::make_shared<FunctionGPU::GFunctionGPUMarshaller>();
        gpu::GGPUConsumerT<gen::GGenome, double>(consumerConfig, marshaller);
        gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json").get_as<gind::GFunctionIndividual>();
        go.optimize(); // refreshes Go2's owned configs, then exits; nothing device-dependent has run
    }

    // ---- client mode (networked consumers) ----------------------------------------------------
    if(go.clientMode()) {
        return go.clientRun();
    }

    // ---- parity-check mode: cross-check the GPU kernel against the CPU evaluate(), then exit ---
    if(parityCheckN > 0) {
        return runParityCheck(parityCheckN, consumerConfig);
    }

    // ---- as this is a server, allow interrupting the run "on the fly" -------------------------
    signal(G_SIGHUP, Gem::Common::sigHupHandler);

    // ---- create the individual factory and wire the optimization ------------------------------
    auto gfi_ptr = std::make_shared<gind::GFunctionIndividualFactory>("./config/GFunctionIndividual.json");
    go.registerContentCreator(gfi_ptr);

    // The genome carries only structure; the configured adaptor lives on an OA-owned config the factory
    // authors. Register it for the adapting algorithms (EA / SA) so Go2 hands it over before they run.
    {
        auto sample = gfi_ptr->get_as<gind::GFunctionIndividual>();
        auto cfg = gfi_ptr->getAdaptionConfig(*sample);
        go.registerAdaptionConfig("PERSONALITY_EA", cfg);
        go.registerAdaptionConfig("PERSONALITY_SA", cfg);
    }

    // Default algorithm is "ea"; override on the command line (e.g. --optimizationAlgorithms "sa").
    go.registerDefaultAlgorithm("ea");

    // Perform the optimization. With "--consumer gpu" every generation is scored in one bulk kernel launch.
    auto p = go.optimize()->getBestGlobalIndividual<gind::GFunctionIndividual>();

    std::cout << "Best result found:" << '\n' << p << '\n';
}
