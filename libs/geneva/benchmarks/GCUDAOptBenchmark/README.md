# GCUDAOptBenchmark

A GPU-accelerated multi-algorithm comparison benchmark for the Geneva optimization library.
Runs one or more Geneva optimization algorithms (EA, SA, Swarm, GD) against a set of
mathematical benchmark functions, with all fitness evaluations offloaded to a CUDA GPU.
Produces per-run and summary CSV files for offline analysis.

---

## What it does

For each configured algorithm entry and each repetition the benchmark:

1. Creates a fresh algorithm instance from the appropriate Geneva factory and its JSON
   config file.
2. Creates a `GFunctionIndividual` seeded with the chosen benchmark function.
3. Calls `optimize()`. Every generation the algorithm submits the population to the unified
   courtier GPU consumer (`Gem::Courtier::GPU::GGPUConsumerT`, the SAME one example 14 uses),
   which evaluates all individuals in one bulk, runtime-compiled kernel launch and returns the
   fitness values to the algorithm without invoking the CPU-side `evaluate()`.
4. A pluggable monitor (`GBenchmarkTerminationMonitor`) records the final fitness,
   iteration count, wall-clock time, and termination reason at the end of each run.
5. After all repetitions of one algorithm tag, mean and standard deviation are computed
   with `Gem::Common::GStandardDeviation`.
6. Results are written to CSV files (see [Output](#output)).

The consumer and the `GenevaInitializer` are set up once in `main()` and
shared across all algorithm runs, avoiding the repeated consumer-teardown problem that
would arise if each run used its own `Go2` instance.

---

## Prerequisites

| Requirement | Version |
|-------------|---------|
| Geneva library (built) | ≥ 1.99 |
| CMake | ≥ 3.27 |
| GCC | ≥ 14 (C++23) |
| Boost | ≥ 1.91 (program_options, json) |

The benchmark itself builds with an ordinary C++ toolchain — it does **not** require the CUDA
language at build time. The GPU backend lives in the optional GPU consumer folded into
`gemfony-courtier`: a CUDA toolkit enables the CUDA backend. The GPU consumer is **device-only**; to
run on the CPU, use a CPU consumer instead (e.g. `--consumer stc`), which evaluates via the
individual's own `evaluate()`. The kernel is chosen at run time in `config/GGPUConsumer.json`, so no
compute-capability list needs editing.

---

## Building

The benchmark is built as part of the `benchmarks-geneva` target (and the default build) whenever
benchmarks are enabled — it no longer depends on CUDA being found at configure time.

```bash
# From an existing out-of-source Geneva build directory:
make GCUDAOptBenchmark -j$(nproc)

# Or build the full benchmark suite:
make benchmarks-geneva -j$(nproc)
```

To enable the benchmark suite in a fresh Geneva build, pass
`-DGENEVA_BUILD_BENCHMARKS=TRUE` to CMake.

---

## Running

The binary must be launched from the directory that contains the `config/` subdirectory
(i.e., the source directory of this benchmark), because all config paths are relative.

```bash
cd /path/to/geneva/benchmarks/geneva/GCUDAOptBenchmark
/path/to/build/benchmarks/geneva/GCUDAOptBenchmark/GCUDAOptBenchmark \
    [--config config/GCUDAOptBenchmark.json]
```

`--config` / `-c` selects the top-level JSON config file.
Default: `config/GCUDAOptBenchmark.json`.

```
GCUDAOptBenchmark options:
  -h [ --help ]          Show help message
  -c [ --config ] arg    Path to GCUDAOptBenchmark.json
                         (default: config/GCUDAOptBenchmark.json)
```

---

## Configuration

### Top-level config: `config/GCUDAOptBenchmark.json`

The file is read through `Gem::Common::GParserBuilder`, the same configuration facility the rest of
Geneva uses, so it is created with defaults if absent and each key is wrapped in a `comment`/`default`/
`value` object (only the `value` fields are shown below for brevity). The set of algorithms to compare
is described by three equal-length, parallel string arrays — the `i`-th element of each defines one
algorithm entry.

```json
{
    "benchmark_function": "PARABOLA",
    "n_runs":             "30",
    "n_dimensions":       "10",
    "individual_config":  "config/GFunctionIndividual.json",
    "output_dir":         ".",
    "batch_size":         "0",
    "flush_timeout_ms":   "50",
    "algo_tags":          ["ea_default", "sa_default", "swarm_default"],
    "algo_mnemonics":     ["ea", "sa", "swarm"],
    "algo_config_files":  ["config/GEvolutionaryAlgorithm.json", "config/GSimulatedAnnealing.json", "config/GSwarmAlgorithm.json"]
}
```

| Key | Type | Description |
|-----|------|-------------|
| `benchmark_function` | string | Name of the objective function (see [Supported functions](#supported-functions)) |
| `n_runs` | integer | Number of independent optimization runs per algorithm tag |
| `n_dimensions` | integer | Parameter-space dimensionality passed to `GFunctionIndividual` |
| `individual_config` | string | Path to `GFunctionIndividual` JSON config (relative to CWD) |
| `output_dir` | string | Directory for output CSV files; created automatically if absent |
| `batch_size` | integer | GPU batch size. `0` = flush by timeout; `>0` = flush when exactly that many individuals have arrived. Setting this to the population size gives the most predictable GPU utilization. |
| `flush_timeout_ms` | integer | When `batch_size == 0`: flush the GPU batch this many milliseconds after the first individual arrives in a new generation |
| `algo_tags` | string array | Free labels used in output file names and the summary table, one per algorithm entry |
| `algo_mnemonics` | string array | Algorithm types, one per entry: `"ea"`, `"sa"`, `"swarm"`, `"gd"` or `"cgd"` |
| `algo_config_files` | string array | Paths to the algorithm-specific JSON configs (relative to CWD), one per entry |

The three `algo_*` arrays must all have the same length; the benchmark aborts with a diagnostic if
they do not.

### Algorithm configs

Each algorithm entry points to its own JSON config file. The provided defaults are:

| File | Algorithm | Key tuning parameters |
|------|-----------|----------------------|
| `config/GEvolutionaryAlgorithm.json` | Evolutionary Algorithm | `population/size`, `population/nParents`, `maxIteration`, `maxStallIteration` |
| `config/GSimulatedAnnealing.json` | Simulated Annealing | `nNeighbourhoods`, `maxIteration`, `maxStallIteration` |
| `config/GSwarmAlgorithm.json` | Swarm Algorithm | `nNeighbourhoods`, `maxIteration`, `maxStallIteration` |
| `config/GConjugateGradientDescent.json` | Conjugate Gradient Descent | `nStartingPoints`, `maxIteration` |
| `config/GFunctionIndividual.json` | Individual | Parameter bounds and adaptor settings |

Termination is controlled per algorithm via `maxIteration` (hard upper bound) and
`maxStallIteration` (convergence proxy: stops when the best fitness has not improved for
this many consecutive iterations).

### Supported functions

The `benchmarkFunction` key must match one of the `solverFunction` enum names from
`GFunctionIndividual`:

| Name | Description |
|------|-------------|
| `PARABOLA` | Simple sum of squares; global minimum at origin |
| `NOISYPARABOLA` | Parabola plus uniform noise |
| `ROSENBROCK` | Banana-shaped valley; challenging for gradient-free methods |
| `ACKLEY` | Many local minima; tests global search ability |
| `RASTRIGIN` | Highly multimodal; large number of local minima |
| `SCHWEFEL` | Deceptive function; global minimum far from local minima |
| `SALOMON` | Concentric rings of local minima |
| `NEGPARABOLA` | Negated parabola (maximization test) |
| `ACKLEY_CANONICAL` | Canonical Ackley parametrisation |
| `GRIEWANK` | Widely distributed local minima |
| `LEVY` | Irregular oscillatory surface |
| `STYBLINSKI_TANG` | Multiple asymmetric minima |
| `ELLIPSOID` | Ill-conditioned quadratic; tests anisotropic search |
| `MICHALEWICZ` | Steep ridges and valleys; dimension-dependent difficulty |
| `ZAKHAROV` | Unimodal but with a non-separable coupling term |

---

## Output

Two CSV file types are written to `outputDir` at the end of the run.

### Raw per-run CSV

One file per algorithm tag:
```
raw_<tag>_<function>_<YYYYMMDD_HHMMSS>.csv
```

Columns:

| Column | Description |
|--------|-------------|
| `run_index` | 0-based run number |
| `algorithm_tag` | Value of `tag` from the config |
| `function` | Benchmark function name |
| `nDims` | Parameter dimension |
| `final_fitness` | Best fitness found at termination |
| `iterations` | Number of iterations executed |
| `wall_time_s` | Wall-clock time from `optimize()` start to end |
| `termination_reason` | `max_iter`, `stall`, or `target_reached` |
| `target_reached` | `1` if a quality threshold was set and reached, else `0` |

### Summary CSV

One file covering all algorithm tags:
```
summary_<YYYYMMDD_HHMMSS>.csv
```

Columns: `algorithm_tag`, `function`, `nDims`, `nRuns`,
`mean_fitness`, `sigma_fitness`, `mean_iterations`, `sigma_iterations`,
`mean_wall_time_s`, `sigma_wall_time_s`, `success_rate`.

`sigma_*` is the sample standard deviation across the `nRuns` repetitions.
`success_rate` is the fraction of runs that terminated via `target_reached`
(non-zero only when `qualityTermination/thresholdActive` is set to `true`
in the algorithm config).

A human-readable summary table is also printed to stdout at the end of the run.

---

## Architecture

```
GCUDAOptBenchmarkMain.cpp   (C++23, compiled by GCC)
  main() — parses config, creates GenevaInitializer, builds a
           Gem::Courtier::GPU::GGPUConsumerT<GOptimizableEntity> + GBenchmarkGPUMarshaller
           and registers it as the process consumer (GConsumerRegistry)
           (the SAME unified GPU consumer example 14 uses), runs
           GAlgorithmBenchmarkRunner, writes output via GBenchmarkResultWriter.
           There is no build-time CUDA compilation unit any more.

GBenchmarkGPUMarshaller.hpp
  GBenchmarkGPUMarshaller — a Gem::Courtier::GPU::GGPUEvaluableI marshaller: flattens a batch of
           GFunctionIndividuals into a row-major device buffer, passes the benchmark function id
           (read from the batch) as the opaque problem constant, and injects the device-computed
           fitness back via process(). The kernel reuses the shared function math
           (geneva/individuals/GBenchmarkFunctions.hpp) that the individual's evaluate() also uses, so
           a CPU run (via a CPU consumer such as --consumer stc) cross-checks the GPU.

kernels/benchmark_eval.cu
  The evaluation kernel, loaded and compiled at RUN TIME (NVRTC for CUDA) by the consumer's
           backend. One thread per individual; it mirrors the 15 functions
           of GBenchmarkFunctions.hpp (funcId 0..14). Edit the kernel + rerun — no rebuild needed.

GAlgorithmBenchmarkRunner.hpp/.cpp   (C++23, CUDA-agnostic)
  GBenchmarkTerminationMonitor — GBasePluggableOM subclass; captures
           termination reason, final fitness, and iteration count at INFOEND.
  GAlgorithmBenchmarkRunner — drives the nested run loop; creates algorithms
           via Geneva factories (not Go2, to avoid repeated consumer teardown);
           parses the benchmark-function name (or id) and calls GStandardDeviation
           for aggregation.

GBenchmarkResultWriter.hpp/.cpp
  Static CSV and stdout writer.

GBenchmarkRunResult.hpp
  Plain data structs: GBenchmarkRunResult (one run), GAlgorithmBenchmarkResult
           (aggregated over N runs), TerminationReason enum,
           terminationReasonToString().
```

### Consumer lifecycle

`GenevaInitializer` is constructed once in `main()` and lives for the duration of
the process. The `GGPUConsumerT` is registered as the single process consumer
(`GConsumerRegistry`) once, before any optimization starts. Algorithm instances are
created per-run via Geneva factories (`GOptimizationAlgorithmFactoryT<…>`) and
route all evaluations through that already-registered process consumer.
This avoids the double-teardown problem that arises when multiple `Go2` instances
each try to release the process consumer on destruction.

---

## Adding a new algorithm

1. Verify that Geneva ships the algorithm and its personality traits, so that
   `GOptimizationAlgorithmFactoryT<GTheAlgorithm>` builds it.
2. Add a `case` to `GAlgorithmBenchmarkRunner::makeAlgorithm()` in
   `GAlgorithmBenchmarkRunner.cpp` with a new mnemonic string.
3. Add a config file in `config/` and append a matching triple to the
   `algo_tags` / `algo_mnemonics` / `algo_config_files` arrays in
   `config/GCUDAOptBenchmark.json` (keep the three arrays equal-length).

## Adding a new benchmark function

1. Add the function implementation to `GBenchmarkFunctions.hpp` under a new
   `FUNC_*` integer constant and a corresponding `case` in `gbm::eval()`.
2. Ensure the `solverFunction` enum in `GFunctionIndividual.hpp` has a matching
   entry at the same integer position, so that `getDemoFunction()` returns the
   correct `funcId` for the kernel.
3. Use the new `solverFunction` name as the `benchmarkFunction` value in the
   top-level config.
