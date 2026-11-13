/**
 * @file GCUDAWorker.cpp
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
 * Example 14 -- the Mona-Lisa problem: evolve a set of alpha-blended circle-triangles so that their
 * superimposition resembles a target image. The candidate fitness is the per-pixel/per-channel
 * deviation from the target through a rational saturation function.
 *
 * This is a THIN launcher: the optimization individual (GImageIndividual) AND its GPU marshaller
 * (GMonaLisaGPUMarshaller) are contributed by ONE runtime-loaded shared object (libGImageIndividual.so),
 * exactly as in the GPU North Star (example 21). Nothing problem-specific about the genome or the device
 * marshaller is compiled into this launcher; it names only the flat-genome base (Genome::GGenome) and the
 * loaded factory. Load and run with:
 *
 *   ./GCUDAWorker --module ./libGImageIndividual.so --consumer gpu     (evaluate on the device)
 *   ./GCUDAWorker --module ./libGImageIndividual.so --consumer stc     (CPU cross-check via evaluate())
 *
 * The evaluation runs on the GPU via the unified courtier GPU consumer (Gem::Courtier::GPU::GGPUConsumerT):
 * a whole generation is flattened and scored in ONE bulk kernel launch (runtime-compiled with NVRTC;
 * pixel-parallel so even a small population fills the GPU). The SAME render+score math is also available on
 * the CPU (the individual's evaluate()), so a CPU run cross-checks the GPU. Switch the kernel in
 * config/GGPUConsumer.json -- no recompilation needed.
 *
 * The launcher keeps the two pieces a generic optimizer cannot provide: it loads the target image (which
 * defines the canvas and the fitness reference), and it registers GImagePOM, which rasterises the current
 * best candidate at the end of each iteration and writes it to ./results/ as a PNG so the picture can be
 * watched converging. GImagePOM reads the best candidate through the flat-genome base, so it needs no
 * knowledge of the concrete individual type.
 *
 * Because there is no network involved, this example is also a convenient stress-test harness for the
 * courtier framework, the device consumer, and Hap.
 *
 * PARITY-CHECK MODE (config key "parity_check_n", default 0 == off): when set to a positive N, the
 * program does NOT optimise. Instead it draws N random individuals from the loaded factory and, for each,
 * compares the CPU reference fitness (the individual's own evaluate(), the SAME render+score the kernel
 * replicates) against the GPU fitness (the CUDA kernel via the device consumer). It prints per-item and
 * summary RELATIVE errors and a PARITY PASS/FAIL verdict, then exits with 0 (pass) or 1 (fail). Parity is
 * tolerance-based, not exact: the kernel accumulates in parallel (atomic/reordered reduction) while
 * evaluate() sums sequentially, so identical genomes differ by floating-point rounding. Combined with the
 * non-deterministic RNG (a fresh random population each run), the verdict is a best-of-N fraction within a
 * generous relative tolerance -- never an exact-match assertion.
 */

// Standard headers
#include <algorithm>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <numeric>
#include <span>
#include <string>
#include <vector>

// Geneva headers
#include "common/GParserBuilder.hpp"
#include "courtier/gpu/GGPUConsumer.hpp"
#include "geneva/Go2.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeFactory.hpp"

// Example-local headers (host-side rendering + the device marshaller; both header-only, no dependency on
// the concrete individual, which lives in the runtime module).
#include "GImagePOM.hpp"
#include "GImageScalar.hpp"
#include "GMonaLisaGPUMarshaller.hpp"
#include "GMonaLisaProblem.hpp"

using namespace Gem::Geneva;
namespace gpu = Gem::Courtier::GPU;
namespace gen = Gem::Geneva::Genome;

namespace {

/******************************************************************************/
/**
 * GPU/CPU parity check for example 14 (entered when the config key "parity_check_n" is > 0).
 *
 * It cross-checks the CUDA kernel against the individual's own evaluate() on IDENTICAL genomes:
 *   1. Draw @p n random individuals from the loaded content-creator factory (base interface -- the concrete
 *      type stays in the module).
 *   2. Clone each genome for the GPU BEFORE evaluating on the CPU (both process() calls store their
 *      result on the item, so the two paths must run on separate copies of the same genome).
 *   3. CPU reference: run each individual's own evaluation via the public process() path (which invokes
 *      evaluate() and stores the result) and read raw_fitness(0) -- exactly what a CPU consumer does.
 *   4. GPU: build the marshaller + device consumer directly (the same GGPUConsumerT the "gpu" mnemonic
 *      builds) and evaluate the whole batch in one bulk launch via processBatch(); read raw_fitness(0) back.
 *   5. Report per-item and summary RELATIVE errors and a best-of-N PASS/FAIL verdict.
 *
 * Parity is tolerance-based (float rounding + parallel/atomic reduction order on the device vs sequential
 * summation on the host) and the RNG is non-deterministic, so the criterion is a fraction of items within a
 * generous relative tolerance -- not an exact match.
 *
 * @param factory The loaded content-creator factory producing random individuals
 * @param n The number of random individuals to check (> 0)
 * @param consumerConfig The GPU-consumer config file (backend + kernel selection)
 * @return The process exit code: 0 if parity passes, 1 if it fails
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
    const std::size_t within =
        static_cast<std::size_t>(std::count_if(rel.begin(), rel.end(), [&](double r) { return r <= rel_tol; }));
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

int runParityCheck(const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> &factory, int n,
                   const std::string &consumerConfig) {
    using gen::GGenome;

    // Generous RELATIVE tolerance: the device kernel accumulates in the selected scalar (float by default)
    // with a parallel/atomic reduction, while evaluate() sums sequentially, so even identical genomes differ
    // by floating-point rounding whose magnitude grows with the pixel/triangle count. 1e-2 (1%) comfortably
    // absorbs that reordering noise without hiding a genuine kernel/evaluate mismatch (which would show up as
    // errors orders of magnitude larger).
    constexpr double kRelTol = 1.0e-2;
    // At least this fraction of items must land within kRelTol for an overall PASS (best-of-N, Inv 18).
    constexpr double kPassFraction = 0.90;
    // Floor for the relative-error denominator, so a near-zero CPU fitness cannot blow the ratio up.
    constexpr double kEps = 1.0e-12;

    glogger << "Example 14 (Mona-Lisa): PARITY CHECK -- CPU evaluate() vs GPU kernel over " << n
            << " random individuals" << '\n'
            << GLOGGING;

    // Draw n random individuals for the CPU reference and, for each, an independent clone for the GPU so the
    // two paths score IDENTICAL genomes (each process() stores its result on its own item).
    std::vector<std::shared_ptr<GGenome>> cpuInds;
    std::vector<std::unique_ptr<GGenome>> gpuBatch;
    cpuInds.reserve(static_cast<std::size_t>(n));
    gpuBatch.reserve(static_cast<std::size_t>(n));
    for(int i = 0; i < n; ++i) {
        auto ind = factory->get(); // a fresh random genome
        gpuBatch.push_back(ind->clone()); // identical-genome copy for the device path
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
    // builds) and evaluate the whole batch in a single bulk launch. full_success_or_fatal: every item must be evaluated.
    auto marshaller = std::make_shared<MonaLisa::GMonaLisaGPUMarshaller>();
    auto consumer =
        std::make_shared<gpu::GGPUConsumerT<GGenome, gimage_fp_t>>(consumerConfig, marshaller);
    consumer->setCloneFunction([](const std::unique_ptr<GGenome> &p) {
        return p->clone();
    });
    consumer->processBatch(
        std::span<std::unique_ptr<GGenome>>(gpuBatch.data(), gpuBatch.size()),
        Gem::Courtier::GSubmissionPolicy::full_success_or_fatal());

    std::vector<double> fitness_gpu(static_cast<std::size_t>(n));
    for(int i = 0; i < n; ++i) {
        fitness_gpu[i] = gpuBatch[i]->raw_fitness(0);
    }

    // Per-item relative error, plus per-item report for the first few.
    std::vector<double> rel(static_cast<std::size_t>(n));
    const int nShow = std::min(n, 8);
    for(int i = 0; i < n; ++i) {
        const double denom = std::max(std::abs(fitness_cpu[i]), kEps);
        rel[i] = std::abs(fitness_gpu[i] - fitness_cpu[i]) / denom;
        if(i < nShow) {
            glogger << std::format(
                           "  item {:3d}:  cpu={:.9g}  gpu={:.9g}  rel-err={:.3e}",
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

// NOLINTNEXTLINE(readability-function-size) -- example 14's main(): the launcher's linear setup script (config parsing, --update-configs, client mode, target-image load, parity-check dispatch, monitor + adaption-config registration, run); splitting would scatter tightly sequential one-shot setup steps
int main(int argc, char **argv) {
    // ---- example-specific settings, read from a Geneva config file ----------------------------
    // Ordinary config-file parameters parsed through the standard GParserBuilder (which also writes the file
    // with documented defaults on first run). Edit config/GImageGeneral.json and re-run -- no rebuild.
    std::string targetFile;
    std::string consumerConfig;
    bool logImages = false;
    bool emitBestOnly = false;
    int parityCheckN = 0;

    Gem::Common::GParserBuilder gpb;
    gpb.registerFileParameter<std::string>(
        "target_image", targetFile, std::string("./pictures/ml-small.png"),
        Gem::Common::VAR_IS_ESSENTIAL,
        "The target image (PNG) the triangle superimposition should resemble;"
        " also defines the canvas resolution");
    gpb.registerFileParameter<std::string>(
        "gpu_config", consumerConfig, std::string(GIMAGE_DEFAULT_GPUCONFIG),
        Gem::Common::VAR_IS_ESSENTIAL,
        "The courtier GPU consumer configuration file (backend + kernel selection)");
    gpb.registerFileParameter<bool>(
        "log_images", logImages, true, Gem::Common::VAR_IS_SECONDARY,
        "Whether to write the best candidate image to ./results/ after each iteration");
    gpb.registerFileParameter<bool>(
        "emit_best_only", emitBestOnly, true, Gem::Common::VAR_IS_SECONDARY,
        "When logging images, only emit one for iterations that improved the best result");
    gpb.registerFileParameter<int>(
        "parity_check_n", parityCheckN, 0, Gem::Common::VAR_IS_SECONDARY,
        "If > 0: run a GPU/CPU parity check over this many random individuals (CPU evaluate() vs the"
        " CUDA kernel) and exit, instead of optimizing; 0 == off (normal optimization run)");
    gpb.parseConfigFile("./config/GImageGeneral.json");

    // Go2 parses its own framework options from the command line / its own config file and, in its
    // constructor, loads the --module: the module contributes BOTH the individual (claimed as the
    // optimization problem) AND its GPU marshaller (registered under the "cuda" device target), so
    // "--consumer gpu" can build the GPU consumer around it.
    Go2 go(argc, argv, "./config/Go2.json");

    // ---- --update-configs: materialize the configs this example owns, then exit ---------------
    // Go2 forces the local thread-pool consumer for a config refresh, so the GPU consumer is not built and
    // its config would be missed; materialize it directly here (the constructor only loads the config file;
    // the backend/kernel are acquired lazily on the first dispatch, so no device is required). This runs
    // BEFORE loadTarget() below, which needs the target image -- irrelevant to a config refresh and absent
    // when configs are materialized. GImageGeneral.json was already refreshed by the parse above; go.optimize()
    // then refreshes Go2's own configs and the loaded individual's config, and exits.
    if(go.updateConfigsMode()) {
        auto marshaller = std::make_shared<MonaLisa::GMonaLisaGPUMarshaller>();
        gpu::GGPUConsumerT<gen::GGenome, gimage_fp_t>(consumerConfig, marshaller);
        go.optimize();
    }

    // Client mode (networked consumers): the module is already loaded, so a client can deserialize work.
    if(go.clientMode()) {
        return go.clientRun();
    }

    // ---- load the target image (defines the canvas resolution and the fitness reference) -----
    // The target is process-wide state shared (via the single RTLD_GLOBAL symbol namespace) by the loaded
    // individual's evaluate() (CPU), the loaded marshaller (GPU), and the image POM below.
    Gem::Geneva::MonaLisa::loadTarget(targetFile);
    const auto &tgt = Gem::Geneva::MonaLisa::target();
    glogger << "Example 14 (Mona-Lisa): target " << targetFile << " (" << tgt.width << "x"
            << tgt.height << ")" << '\n'
            << GLOGGING;

    // Pull the loaded problem's factory through the base interface (this launcher never names the concrete
    // individual type). A hard error if no problem was loaded.
    auto factory = go.getContentCreator();
    if(not factory) {
        std::cerr << "No optimization problem is available. Load the Mona-Lisa problem with"
                  << " --module ./libGImageIndividual.so" << '\n';
        return 1;
    }

    // ---- parity-check mode: cross-check the GPU kernel against the CPU evaluate(), then exit --
    // Config key "parity_check_n" (0 == off). When positive, do NOT optimize: draw that many random
    // individuals and compare each individual's CPU evaluate() against the device kernel on identical
    // genomes, print the relative errors + a PASS/FAIL verdict, and return the verdict as the exit code.
    if(parityCheckN > 0) {
        return runParityCheck(factory, parityCheckN, consumerConfig);
    }

    // ---- as this is a server, allow interrupting the run "on the fly" -------------------------
    signal(G_SIGHUP, Gem::Common::sigHupHandler);

    // ---- register the image-emitting pluggable optimization monitor --------------------------
    if(logImages) {
        go.registerPluggableOM(std::make_shared<GImagePOM>("./results/", emitBestOnly));
    }

    // The genome carries only structure; its main + location Gauss adaptors live on an OA-owned config the
    // loaded factory authors. Reach it generically through GGenomeFactory::getAdaptionConfig(),
    // so this launcher need not know the concrete type. Register it for the only adapting algorithm we may
    // select: the dimension-aware evolutionary algorithm (ea). The from-scratch sep-CMA strategy (sepcma)
    // manages its own search distribution and needs no adaption config.
    auto oef = std::dynamic_pointer_cast<gen::GGenomeFactory>(factory);
    auto sample = factory->get(); // a sample individual (also parses the factory's config)
    const auto *flat = dynamic_cast<const gen::GGenome *>(sample.get());
    if(oef && (flat != nullptr)) {
        if(auto cfg = oef->getAdaptionConfig(*flat)) {
            go.registerAdaptionConfig("PERSONALITY_EA", cfg);
        }
    }

    // ---- run the optimization with the algorithm chosen by mnemonic --------------------------
    // Default is "ea"; override on the command line to compare against the from-scratch sep-CMA strategy,
    // e.g.  --optimizationAlgorithms "sepcma".
    go.registerDefaultAlgorithm("ea");

    // Perform the optimization. The per-iteration picture (the useful output) is written by GImagePOM. The
    // best individual is read back through the flat-genome base, so this launcher stays problem-agnostic.
    auto best = go.optimize()->getBestGlobalIndividual<gen::GGenome>();

    glogger << "Example 14 finished. Best fitness: " << best->raw_fitness(0) << '\n' << GLOGGING;

    return 0;
}
