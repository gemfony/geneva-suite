"""Expand the configured axes into concrete jobs and apply filters."""

from __future__ import annotations

from dataclasses import replace

from .config import Config, FeatureSet
from .model import BuildSpec, BuildType, Compiler, Job
from .runner import GUEST_BUILD

# Install prefix used inside the guest. Each check runs in its own ephemeral
# (`--rm`) container and only the build mount (/work/build) is shared between
# them, so `make install` must land under that mount for the downstream
# out-of-tree find_package(Geneva) check to see it. A container-internal prefix
# like /opt/geneva would be discarded with the install check's container.
_GUEST_INSTALL_PREFIX = f"{GUEST_BUILD}/install-prefix"


# The release-test images ship NO CUDA toolkit (see backends/containerfile.py),
# so a containerized build can never compile CUDA regardless of the host GPU --
# enabling GEM_USE_CUDA_RNG there makes hap's ENABLE_LANGUAGE(CUDA) hard-fail
# ("Failed to find nvcc"). CUDA must therefore be gated on the IMAGE providing a
# toolkit, not on host gpu_available. Until a CUDA image variant exists this is
# always False. Flip to True (and add CUDA to the Containerfile + GPU passthrough
# in the backend) to actually exercise the CUDA build.
_IMAGES_HAVE_CUDA = False


def _spec(cfg: Config, compiler: Compiler, build_type: BuildType,
          fs: FeatureSet, gpu_available: bool) -> BuildSpec:
    # CUDA needs: the feature set wants it, a usable host GPU, AND a CUDA toolkit
    # inside the build image. The image has none, so this is currently always
    # off -- which keeps the otherwise-CUDA-less "full" set (MPI+benchmarks)
    # buildable instead of failing cmake configuration.
    want_cuda = fs.cuda and gpu_available and _IMAGES_HAVE_CUDA
    return BuildSpec(
        build_type=build_type,
        compiler=compiler,
        build_tests=fs.tests,
        build_examples=fs.examples,
        build_benchmarks=fs.benchmarks,
        with_mpi=fs.mpi,
        with_cuda_rng=want_cuda,
        skip_all_cuda=not want_cuda,
        boost_root=cfg.boost_root,
        install_dir=_GUEST_INSTALL_PREFIX,
    )


def expand(cfg: Config, *, full: bool, gpu_available: bool,
           only_build_types: set[BuildType] | None = None,
           medium: bool = False) -> list[Job]:
    """Build the full job list for the requested tier.

    ``--quick`` (full=False) uses only ``short_build_types`` and the ``base``
    feature set. ``--medium`` is the same matrix as ``--quick`` (SHORT build
    types, base feature set) but additionally builds the benchmarks so they can
    be smoke-started once (see checks/benchmarks.py); it does NOT add the long
    build types or the minimal/full feature cells. ``--full`` adds the long
    build types, the ``minimal`` build and the ``full`` (MPI/CUDA/benchmarks)
    feature set.

    ``only_build_types`` (from ``--build-type``) restricts the matrix to the
    given build types *before* expansion, so e.g. running only ``Release`` (or
    only ``Debug``) is faster: the other build types — and the hard-wired
    ``minimal`` (Debug) / ``full`` (Release) cells whose build type is excluded
    — are never generated.
    """
    def allowed(bt: BuildType) -> bool:
        return only_build_types is None or bt in only_build_types

    jobs: list[Job] = []
    base_fs = cfg.feature_sets["base"]
    if medium:
        # Medium tier: the quick matrix, but with benchmarks built so the
        # benchmark smoke-start check has something to launch.
        base_fs = replace(base_fs, benchmarks=True)

    build_types = [b for b in cfg.short_build_types if allowed(b)]
    if full:
        build_types += [b for b in cfg.long_build_types
                        if allowed(b) and b not in build_types]

    for guest in cfg.guests:
        for compiler in cfg.compilers:
            for bt in build_types:
                jobs.append(Job(guest=guest,
                                 spec=_spec(cfg, compiler, bt, base_fs, gpu_available),
                                 label="base"))

            if full:
                # Minimal build (all options off) — Debug only is enough.
                mini = cfg.feature_sets.get("minimal")
                if mini is not None and allowed(BuildType.DEBUG):
                    jobs.append(Job(
                        guest=guest,
                        spec=_spec(cfg, compiler, BuildType.DEBUG, mini, gpu_available),
                        label="minimal"))
                # Full-feature build (MPI + benchmarks, CUDA if GPU) — Release.
                full_fs = cfg.feature_sets.get("full")
                if full_fs is not None and allowed(BuildType.RELEASE):
                    jobs.append(Job(
                        guest=guest,
                        spec=_spec(cfg, compiler, BuildType.RELEASE, full_fs, gpu_available),
                        label="full"))

    return jobs


def _matches(job: Job, selectors: dict[str, str]) -> bool:
    for key, val in selectors.items():
        if key == "os" and job.guest.slug != val:
            return False
        if key == "compiler" and job.spec.compiler.value != val:
            return False
        if key == "buildtype" and job.spec.build_type.value != val:
            return False
        if key == "label" and job.label != val:
            return False
    return True


def apply_filter(jobs: list[Job], filter_str: str | None) -> list[Job]:
    """Filter jobs by a ``key=value,key=value`` selector string.

    Supported keys: ``os``, ``compiler``, ``buildtype``, ``label``.
    """
    if not filter_str:
        return jobs
    selectors: dict[str, str] = {}
    for part in filter_str.split(","):
        part = part.strip()
        if not part:
            continue
        if "=" not in part:
            raise ValueError(f"bad filter token {part!r}; expected key=value")
        key, val = part.split("=", 1)
        selectors[key.strip()] = val.strip()
    return [j for j in jobs if _matches(j, selectors)]
