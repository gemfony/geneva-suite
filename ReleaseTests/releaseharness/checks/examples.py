"""Example checks (ReleaseTestplan.md: "Examples").

An in-tree example build+run is implemented; a networked example and the CUDA
example are wired with GPU/feature gating, the networked split being a stub.
"""

from __future__ import annotations

import time

from ..model import CheckResult, Tier
from ..runner import GUEST_BUILD, JobContext

# Examples read/write ./config/Go2.json relative to the CWD, so run each from
# its own build directory where the config/ folder is installed.
#
# NOTE: the quickstart is deliberately NOT built in-tree -- it ships as the
# canonical out-of-tree find_package(Geneva) project (see
# libs/geneva/quickstart/README). The in-tree "run an example to completion"
# smoke therefore uses 01_GSimpleOptimizer, the canonical minimal in-tree
# optimizer (the quickstart's out-of-tree role belongs to the
# outoftree/findgeneva check).
_SIMPLE_DIR = "libs/geneva/examples/01_GSimpleOptimizer"
_SIMPLE = "./GSimpleOptimizer"
_GCUDA_DIR = "libs/geneva/examples/14_GCUDAWorker"
_GCUDA = "./GCUDAWorker"


def simple_optimizer(ctx: JobContext) -> CheckResult:
    """01_GSimpleOptimizer builds (via gemfony-build-all) and runs to completion."""
    tier = Tier.SHORT
    if not ctx.job.spec.build_examples:
        return ctx.skipped("example/simple-optimizer", tier, "examples not built")
    if not ctx.should_run(tier):
        return ctx.skipped("example/simple-optimizer", tier, "skipped")
    started = time.monotonic()
    res = ctx.exec_in_guest(
        ["bash", "-lc", f"cd {GUEST_BUILD}/{_SIMPLE_DIR} && {_SIMPLE}"],
        workdir=GUEST_BUILD, timeout=900)
    return ctx.record("example/simple-optimizer", tier, res, started=started)


def networked(ctx: JobContext) -> CheckResult:
    """One networked example, client+server on localhost — STUB."""
    tier = Tier.LONG
    if not ctx.job.spec.build_examples:
        return ctx.skipped("example/networked", tier, "examples not built")
    return ctx.skipped(
        "example/networked", tier,
        "STUB: start a server + client example on localhost and compare (TODO)",
    )


def cuda(ctx: JobContext) -> CheckResult:
    """CUDA example 14 — runs only when a GPU is available and CUDA was built."""
    tier = Tier.LONG
    if not ctx.job.spec.with_cuda_rng:
        return ctx.skipped("example/cuda", tier, "CUDA not enabled / no GPU on host")
    if not ctx.should_run(tier):
        return ctx.skipped("example/cuda", tier, "LONG tier skipped in --quick")
    started = time.monotonic()
    res = ctx.exec_in_guest(
        ["bash", "-lc", f"cd {GUEST_BUILD}/{_GCUDA_DIR} && {_GCUDA}"],
        workdir=GUEST_BUILD, use_gpu=True, timeout=1800)
    return ctx.record("example/cuda", tier, res, started=started)
