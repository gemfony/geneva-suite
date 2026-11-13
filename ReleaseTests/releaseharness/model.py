"""Core data model for the release-test harness.

Pure dataclasses and enums shared by the matrix expander, the backends, the
individual checks and the reporter. No external dependencies.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, field, replace


class Tier(enum.Enum):
    """How expensive a check is.

    SHORT checks run under ``--quick``; LONG checks run only under ``--full``.
    """

    SHORT = "short"
    LONG = "long"


class Status(enum.Enum):
    """Outcome of a single check."""

    PASS = "pass"
    FAIL = "fail"
    SKIP = "skip"
    ERROR = "error"


class Compiler(enum.Enum):
    GCC = "gcc"
    CLANG = "clang"


class BuildType(enum.Enum):
    DEBUG = "Debug"
    RELEASE = "Release"
    REL_WITH_DEB_INFO = "RelWithDebInfo"
    MIN_SIZE_REL = "MinSizeRel"
    SANITIZE = "Sanitize"


@dataclass(frozen=True)
class GuestOS:
    """A guest operating system the matrix runs against.

    ``image`` is the base container image (e.g. ``ubuntu:26.04``); ``slug`` is
    a short identifier used in filenames and report rows.
    """

    slug: str
    image: str
    # NOTE: Boost is built from source (1.91, C++23) inside the image, not from
    # distro packages — see backends/containerfile.py. The distro libboost
    # packages are older than the required 1.91 and are not used.


@dataclass(frozen=True)
class BuildSpec:
    """Everything needed to configure one out-of-source Geneva build.

    Mirrors the variables understood by ``scripts/genevaConfig.gcfg`` so the
    harness can either emit a ``.gcfg`` for ``prepareBuild.sh`` or translate
    directly into ``cmake -D`` flags.
    """

    build_type: BuildType
    compiler: Compiler
    build_tests: bool = True
    build_examples: bool = True
    build_benchmarks: bool = False
    with_mpi: bool = False
    with_cuda_rng: bool = False
    skip_all_cuda: bool = True
    boost_root: str = "/opt/boost"
    install_dir: str = "/opt/geneva"

    def gcfg_text(self) -> str:
        """Render a ``.gcfg`` file body consumable by ``prepareBuild.sh``."""
        return (
            'CMAKE="/usr/bin/cmake"\n'
            f'COMPILER="{self.compiler.value}"\n'
            f'BOOSTROOT="{self.boost_root}"\n'
            'MPIROOT=""\n'
            f'BUILDMODE="{self.build_type.value}"\n'
            f'BUILDTESTCODE="{int(self.build_tests)}"\n'
            f'BUILDEXAMPLES="{int(self.build_examples)}"\n'
            f'BUILDBENCHMARKS="{int(self.build_benchmarks)}"\n'
            'VERBOSEMAKEFILE="0"\n'
            f'INSTALLDIR="{self.install_dir}"\n'
            'LINKEREXTRAFLAGS=""\n'
            f'BUILDMPICONSUMER="{int(self.with_mpi)}"\n'
            'WITHCOVERAGE="0"\n'
            f'SKIPALLCUDA="{int(self.skip_all_cuda)}"\n'
            'CUDA_NVCC=""\n'
            'CUDA_ROOT=""\n'
            f'USECUDARNG="{int(self.with_cuda_rng)}"\n'
            'CMAKEEXTRAFLAGS=""\n'
            'CXXEXTRAFLAGS=""\n'
        )


@dataclass(frozen=True)
class Job:
    """A single matrix cell: one OS x compiler x build configuration.

    A job runs a sequence of checks; each produces a :class:`CheckResult`.
    """

    guest: GuestOS
    spec: BuildSpec
    label: str = ""

    @property
    def slug(self) -> str:
        feats = []
        if self.spec.with_mpi:
            feats.append("mpi")
        if self.spec.with_cuda_rng:
            feats.append("cuda")
        if not (self.spec.build_tests or self.spec.build_examples or self.spec.build_benchmarks):
            feats.append("minimal")
        suffix = ("-" + "+".join(feats)) if feats else ""
        return (
            f"{self.guest.slug}-{self.spec.compiler.value}-"
            f"{self.spec.build_type.value}{suffix}"
        )

    def with_label(self, label: str) -> "Job":
        return replace(self, label=label)


@dataclass
class CheckResult:
    """Outcome of one named check within a job."""

    job_slug: str
    check: str
    tier: Tier
    status: Status
    duration_s: float = 0.0
    message: str = ""
    log_path: str | None = None

    def as_dict(self) -> dict:
        return {
            "job": self.job_slug,
            "check": self.check,
            "tier": self.tier.value,
            "status": self.status.value,
            "duration_s": round(self.duration_s, 2),
            "message": self.message,
            "log_path": self.log_path,
        }


@dataclass
class RunReport:
    """Aggregate of all check results for one harness invocation."""

    results: list[CheckResult] = field(default_factory=list)
    started_at: str = ""
    finished_at: str = ""
    quick: bool = True
    backend: str = ""

    def add(self, result: CheckResult) -> None:
        self.results.append(result)

    @property
    def failed(self) -> int:
        return sum(1 for r in self.results if r.status in (Status.FAIL, Status.ERROR))
