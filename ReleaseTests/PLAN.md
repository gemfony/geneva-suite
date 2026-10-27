# Geneva Release-Test Harness — Phased Implementation Plan

Status date: 2026-05-24

This document is the design plan for a Python-based release-test harness that
executes the checks in `../ReleaseTestplan.md` across a matrix of guest
operating systems, compilers and build types, inside containers (or VMs),
and produces a pass/fail report.

The harness lives entirely under `ReleaseTests/`. It does not modify any
existing source or build file in the repository.

---

## 0. Goals and constraints

* Drive every item of `ReleaseTestplan.md` reproducibly.
* Matrix: guest OS `{ubuntu-26.10}` x compiler `{gcc, clang}`
  x build-type `{Debug, Release, RelWithDebInfo, Sanitize}` x feature flags
  (MPI on/off, CUDA on/off, tests/examples/benchmarks on/off, minimal build).
* SHORT vs LONG split: a fast release smoke-check (`--quick`) that only runs
  short-running builds and the CTest suite, versus a `--full` run that also
  runs benchmarks, the sanitizer suite, long optimization runs and networked
  examples.
* Provisioning is **on request only** (`--setup` / `provision` sub-command).
  Nothing is created or destroyed unless the user asks.
* Container backend abstraction: primary backend **Podman** (rootless, no
  daemon, no group membership needed), but pluggable so Docker (drop-in,
  OCI-compatible) and Multipass (full VMs, for true multi-distro VM testing)
  can be swapped in.
* Boost policy: each image **builds Boost 1.91.0 from source in C++23 mode**
  (`b2 cxxflags="-std=c++23"`). Distro `libboost` packages are NOT used —
  Geneva requires Boost >= 1.91. A single per-OS image (Boost built once with
  GCC) serves BOTH the GCC and Clang Geneva builds, since GCC and Clang share
  libstdc++ on Linux.
* Work area: `/home/rberlich/Testplan` (logs, generated Dockerfiles, reports,
  per-job build dirs). Configurable via `--workdir`.
* Reporting: a pass/fail matrix per `OS x compiler x build-type x features`,
  plus full per-job logs.
* Pure stdlib + optional `PyYAML` (already present: 6.0.3). No other deps.

---

## Phase 1 — Framework skeleton (IMPLEMENTED)

Deliverables:

* `release_test.py` — CLI entry point (argparse): sub-commands
  `plan`, `run`, `provision`, `report`, plus global flags
  `--config`, `--workdir`, `--quick`/`--full`, `--dry-run`, `--backend`,
  `--filter`, `--jobs`.
* `releaseharness/` package:
  * `config.py` — load/validate the YAML config; defaults baked in so the
    tool runs with zero config.
  * `matrix.py` — expand the configured axes into concrete `Job` objects,
    apply the short/long tag filter and `--filter` selectors.
  * `model.py` — dataclasses: `Job`, `BuildSpec`, `CheckResult`, `Severity`,
    enums for OS/compiler/build-type, the SHORT/LONG `Tier` enum.
  * `logging_util.py` — per-job log files + console summary.
* `--dry-run` prints the fully expanded matrix and the exact commands that
  *would* run, without touching Docker or the build.

Independently useful: `release_test.py plan --dry-run` shows the whole matrix
and is the primary validation entry point.

## Phase 2 — Container abstraction + Docker backend (IMPLEMENTED)

Deliverables:

* `releaseharness/backends/base.py` — `ContainerBackend` ABC:
  `is_available()`, `build_image(spec)`, `run(image, argv, mounts, gpus)`,
  `cleanup()`.
* `releaseharness/backends/containerfile.py` — Containerfile generation. One
  image per OS installs both toolchains (recent GCC and Clang), CMake, Doxygen,
  optionally OpenMPI, and builds **Boost 1.91.0 from source in C++23 mode**.
* `releaseharness/backends/docker.py` — shared OCI backend. Generates the
  per-OS Containerfile, builds the image, and runs build/test commands inside
  it with the repo bind-mounted read-only and a writable per-job build dir
  mounted from the work area. The image is keyed on (OS, MPI), not compiler.
* `releaseharness/backends/registry.py` — backend selection + auto-detect
  (`podman` -> `docker` -> `multipass`).
* `releaseharness/backends/podman.py` — Podman backend (the DEFAULT): subclass
  of the OCI backend overriding only the executable name and the GPU flag
  (`--device nvidia.com/gpu=all` via CDI).
* `releaseharness/diagnostics.py` — `doctor` diagnostics: classifies each
  backend (ABSENT / UNREACHABLE / PERMISSION_DENIED / WORKING) and the GPU
  (ABSENT / UNREACHABLE / VERSION_MISMATCH / WORKING) with actionable advice.
* `releaseharness/backends/multipass.py` — Multipass (VM) backend: STUB with
  a documented launch/exec/mount flow; needed where containers can't help
  (kernel-level features, real GPU passthrough on some hosts, full distro VMs).

Independently useful: `provision --backend docker` builds the base images.

## Phase 3 — Build + CTest sequence per job (IMPLEMENTED)

Deliverables:

* `releaseharness/checks/build.py` — drives an out-of-source build inside the
  container. Two strategies, selectable:
  1. reuse the repo's `scripts/prepareBuild.sh` by generating a `.gcfg`
     from the `BuildSpec` (primary; matches how releases are actually built);
  2. a direct `cmake` invocation (fallback / minimal-build case).
  Runs `make core`, `make gemfony-build-all`, `make doc` per the test plan.
* `releaseharness/checks/ctest.py` — runs `ctest -j$(nproc) -LE XL` in the build dir
  (every size class but XL; the XL programs are the benchmarks, which
  `benchmarks.py` already starts once each)
  and parses the pass/fail counts; also runs `GenevaStandardTests` directly and
  scans for unexpected skips.
* These two are SHORT-tier for Debug/Release and LONG-tier for Sanitize and
  for the benchmark builds.

Independently useful: `run --quick --filter os=ubuntu-26.04,compiler=clang`
does one fast build+ctest in one container.

## Phase 4 — Reporting (IMPLEMENTED)

Deliverables:

* `releaseharness/report.py` — collects `CheckResult`s, renders a text
  pass/fail matrix to stdout, writes a JSON results file and an HTML matrix
  to the work area. `report` sub-command re-renders the last JSON.

Independently useful: stand-alone `report` sub-command.

## Phase 5 — Functional checks (PARTIALLY IMPLEMENTED / STUBBED)

These map to the middle sections of the test plan. Framework + command
wiring implemented; the long-running invocations are tagged LONG and several
are structured stubs with explicit TODOs:

* `checks/consumers.py` — run an optimization to completion under each
  consumer via example 01 (`-c <name>`): StdThread (`stc`; serial =
  `stc --nWorkerThreads 1`), websocket (`beast`), MPI (`mpi`, only when MPI
  present). Serial + threaded implemented (run from the example build dir so it
  finds its `config/`); websocket/MPI are STUBS with the intended command lines.
* `checks/algorithms.py` — EA, SA, Swarm, GD, ParameterScan each run to
  completion on a simple individual (`-a <key> -c stc`); `Go2` JSON config
  round-trip; checkpoint
  save/restore identity. Wiring + example mapping present; checkpoint-identity
  comparison is a STUB (TODO: byte/objective compare of two runs).
* `checks/examples.py` — `01_GSimpleOptimizer` build+run; one networked example
  (client+server on localhost); CUDA example `14_GCUDAWorker` (GPU-gated). The
  quickstart's own build+run is `checks/outoftree.py` (it is not built in-tree).

## Phase 6 — Install / out-of-tree / linking (STUBBED)

* `checks/install.py` — `make install` into a prefix in the work area; verify
  headers/libs/cmake config land under the prefix; `ldd` the main executables
  for unresolved symbols; `ldconfig`/`LD_LIBRARY_PATH` resolution.
* `checks/outoftree.py` — build a tiny app against the installed tree via
  `FindGeneva`; verify the `GENEVA_BUILD_TESTS` mismatch produces a useful
  error.
  Both are structured stubs (command flow documented, marked TODO) because
  they require a completed install which is itself a LONG job.

## Phase 7 — Docs / release metadata (IMPLEMENTED, lightweight)

* `checks/metadata.py` — static checks that run on the HOST (no container):
  CHANGES has an entry for the version in `CMakeLists.txt`; version macros
  consistent; `make doc` warning scan (the run itself is in Phase 3).

## Phase 8 — CUDA / GPU matrix (DOCUMENTED + GPU-gated)

See "GPU passthrough findings" below. The harness:
* detects a usable GPU on the host (`nvidia-smi` returns success);
* if absent, **skips all CUDA matrix entries gracefully** and records them as
  `SKIPPED (no GPU)` in the report rather than failing;
* if present and the backend is Docker, passes `--gpus all`
  (requires `nvidia-container-toolkit`); Podman uses CDI
  `--device nvidia.com/gpu=all`; Multipass cannot pass through a consumer GPU
  on this host (documented limitation) so CUDA on Multipass is skipped.

---

## SHORT vs LONG tagging summary

| Check | Tier |
|---|---|
| Debug/Release build (core + gemfony-build-all) | SHORT |
| `ctest` suite | SHORT |
| `GenevaStandardTests` direct, skip scan | SHORT |
| `make doc` | SHORT |
| Serial + threaded consumer run | SHORT |
| Minimal build (all options off) | SHORT |
| RelWithDebInfo / Sanitize builds | LONG |
| Thread-sanitizer test run | LONG |
| Benchmarks build + run | LONG |
| Websocket / MPI consumer runs | LONG |
| All five algorithms to completion | LONG |
| Checkpoint save/restore identity | LONG |
| Networked example (client/server) | LONG |
| CUDA example (GPU only) | LONG |
| Install + out-of-tree + ldd | LONG |

`--quick` runs only SHORT-tier checks; `--full` runs both tiers.

---

## Container / GPU passthrough findings on THIS host (2026-05-24)

Host: Ubuntu 26.04 LTS ("resolute"), Python 3.14.4, PyYAML 6.0.3.

* `podman`: **5.7.0, WORKING rootless** — the default backend. subuid/subgid
  ranges are configured (`rberlich:100000:65536`). Builds the per-OS images and
  runs containers without a daemon or group membership.
* `docker`: **29.1.3 installed but PERMISSION_DENIED** — the user is not in the
  `docker` group, so `docker info` is refused. `doctor` recommends
  `sudo usermod -aG docker $USER && newgrp docker` (or simply use Podman).
* `multipass`: present but not used; the container backends cover the matrix.
* GPU: `nvidia-smi` reports a **driver/library version mismatch** (NVML stale),
  but CUDA compute still works (example 14 runs). `doctor` reports this as
  VERSION_MISMATCH (usable-with-warning) and recommends a reboot / kernel-module
  reload, plus `video`/`render` group membership. CUDA matrix entries are NOT
  skipped solely on the NVML mismatch.

Image guest OSes (verified package availability):
* Ubuntu 24.04: gcc 13.2, clang 18.0, cmake 3.28.3 — clang and cmake meet
  Geneva's GCC>=14 / Clang>=18 / CMake>=3.27 requirements, the distro gcc does
  not (it predates the C++23 floor and is rejected at configure time).
* Ubuntu 26.04: gcc 15.2, clang 21.1, cmake 4.2.3 — all meet the requirements.

Passthrough notes (for when a GPU is available):
* **Podman + GPU**: generate a CDI spec
  (`sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml`), then run with
  `--device nvidia.com/gpu=all`.
* **Docker + GPU**: install `nvidia-container-toolkit`, then run with
  `--gpus all`. Verify inside the container with `nvidia-smi`.
* **Multipass + GPU**: QEMU/LXD VMs do **not** support consumer-GPU passthrough
  out of the box on Linux; treat CUDA-on-Multipass as unsupported and skip.

The harness has been run live with Podman: per-OS images (Boost 1.91 from
source, C++20) build, then Geneva builds with BOTH gcc and clang and `ctest`
runs inside Ubuntu 24.04 and 26.04. See README.md for the recorded results.

---

## How to run

```bash
cd ReleaseTests

# Show the planned matrix and commands without doing anything:
python3 release_test.py plan --dry-run
python3 release_test.py plan --full --dry-run

# Provision base images (on request only):
python3 release_test.py provision --backend docker

# Fast release smoke check (SHORT tier only):
python3 release_test.py run --quick

# Full release run (SHORT + LONG):
python3 release_test.py run --full

# Narrow the matrix:
python3 release_test.py run --quick --filter os=ubuntu-26.04,compiler=clang,buildtype=Debug

# Re-render the last report:
python3 release_test.py report
```

Config lives in `config.yaml`; built-in defaults make it optional.
