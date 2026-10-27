"""Parallelization-consumer checks (ReleaseTestplan.md: "Parallelization consumers").

Runs a simple optimization under each consumer via example 01
(GSimpleOptimizer). The consumer is selected with ``-c``:
  stc   -> GStdThreadConsumerT (multithreaded; the local default)
  beast -> GWebsocketConsumerT
There is no serial consumer any more (one consumer per process; stc is the
local one), so there is no serial check either. The StdThread consumer's thread
count is taken from the JSON config, not a CLI flag, so the threaded check
simply runs the stc consumer to completion.

The MPI consumer is exercised via example 15 (GMPIConsumer), launched with
``mpirun`` (rank 0 = server/broker, ranks 1..n = clients) to completion. The
websocket split remains a structured stub (needs server+client orchestration on
a localhost port).
"""

from __future__ import annotations

import time

from ..model import CheckResult, Tier
from ..runner import GUEST_BUILD, JobContext

# Example 01 is the canonical simple optimizer; built under projects/geneva/examples
# (post-Lager-A per-library layout). It writes/reads ./config/Go2.json relative
# to the CWD, so run it from its own build directory where config/ is installed.
_EX01_DIR = "projects/geneva/examples/01_GSimpleOptimizer"
_EX01 = "./GSimpleOptimizer"

# Example 15 drives the GMPIConsumerT directly via mpirun (rank 0 = server,
# ranks 1..n = clients); it reads ./config/*.json relative to its build dir.
_EX16_DIR = "projects/geneva/examples/15_GMPIConsumer"
_EX16 = "./GMPIConsumer"


def threaded(ctx: JobContext) -> list[CheckResult]:
    """Run with the multithreaded StdThread consumer (stc) to completion."""
    tier = Tier.SHORT
    if not ctx.job.spec.build_examples:
        return [ctx.skipped("consumer/threaded", tier, "examples not built")]
    if not ctx.should_run(tier):
        return [ctx.skipped("consumer/threaded", tier, "skipped")]
    started = time.monotonic()
    res = ctx.exec_in_guest(
        ["bash", "-lc", f"cd {GUEST_BUILD}/{_EX01_DIR} && {_EX01} -c stc"],
        workdir=GUEST_BUILD, timeout=900,
    )
    return [ctx.record("consumer/threaded", tier, res, started=started)]


def websocket(ctx: JobContext) -> CheckResult:
    """Client/server split on localhost via GWebsocketConsumerT (beast) — STUB.

    Intended flow: start the server example backgrounded (``-c beast`` in server
    mode), start a client pointing at localhost, wait for completion, compare
    results. Requires process orchestration and a free port; left as a TODO.
    """
    tier = Tier.LONG
    if not ctx.job.spec.build_examples:
        return ctx.skipped("consumer/websocket", tier, "examples not built")
    return ctx.skipped(
        "consumer/websocket", tier,
        "STUB: orchestrate `-c beast` server+client on localhost (TODO)",
    )


def mpi(ctx: JobContext) -> CheckResult:
    """GMPIConsumerT via example 15, launched with mpirun (only when MPI built).

    Runs one MPI process group to completion: rank 0 is the server/broker, the
    other ranks are clients. ``-np 3`` => 1 server + 2 clients. Flags needed in
    the build container: ``--allow-run-as-root`` (the container runs as root) and
    ``--oversubscribe`` (a build container advertises fewer slots than ranks).
    """
    tier = Tier.LONG
    if not ctx.job.spec.with_mpi:
        return ctx.skipped("consumer/mpi", tier, "MPI not enabled in this build")
    if not ctx.should_run(tier):
        return ctx.skipped("consumer/mpi", tier, "LONG tier skipped in --quick")
    started = time.monotonic()
    cmd = (f"cd {GUEST_BUILD}/{_EX16_DIR} && "
           f"mpirun --allow-run-as-root --oversubscribe -np 3 {_EX16}")
    res = ctx.exec_in_guest(["bash", "-lc", cmd], workdir=GUEST_BUILD, timeout=900)
    return ctx.record("consumer/mpi", tier, res, started=started)
