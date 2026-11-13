"""Test checks (ReleaseTestplan.md: "Unit and integration tests").

Runs the CTest suite (everything but the XL size class -- see run_ctest) and
the GenevaStandardTests executable directly, and scans for unexpected skips. The
thread-sanitizer run is gated on the build type being Sanitize and is LONG-tier.
"""

from __future__ import annotations

import re
import time

from ..model import BuildType, CheckResult, Status, Tier
from ..runner import GUEST_BUILD, JobContext

_CTEST_SUMMARY = re.compile(r"(\d+)% tests passed,\s*(\d+)\s+tests failed out of\s*(\d+)")
# Catch2 prints "All tests passed (N assertions in M test cases)" on success;
# used to assert that tests actually ran (>0), not just exited 0.
_CATCH_COUNT = re.compile(r"(\d+)\s+assertion")


def run_ctest(ctx: JobContext) -> CheckResult:
    """Run the full CTest suite; PASS only on zero failures."""
    if not ctx.job.spec.build_tests:
        return ctx.skipped("test/ctest", Tier.SHORT, "tests not built (minimal)")
    # Sanitize runs are slow -> LONG; otherwise SHORT.
    tier = Tier.LONG if ctx.job.spec.build_type is BuildType.SANITIZE else Tier.SHORT
    if not ctx.should_run(tier):
        return ctx.skipped("test/ctest", tier, "LONG tier skipped in --quick")

    started = time.monotonic()
    # Everything except the XL size class (over a minute per test): those are
    # convergence- and throughput-scale runs of many minutes each, and this harness
    # already starts every one of them once through benchmarks.smoke_start().
    # Running them here would pay that cost a second time, at full length, in every
    # cell. Selecting by SIZE rather than by the `benchmark` label is deliberate:
    # the size is what says a test is expensive, and a test that becomes expensive
    # without living in benchmarks/ must drop out of this run too.
    res = ctx.exec_in_guest(["bash", "-lc", "ctest --output-on-failure -j$(nproc) -LE XL"],
                            workdir=GUEST_BUILD, timeout=7200)
    result = ctx.record("test/ctest", tier, res, started=started)
    if not ctx.dry_run:
        m = _CTEST_SUMMARY.search(res.stdout + res.stderr)
        if m:
            failed = int(m.group(2))
            total = int(m.group(3))
            result.message = f"{total - failed}/{total} passed"
            # PASS requires a clean exit AND zero failures AND that tests
            # actually ran -- a stdout summary alone must not override a
            # non-zero ctest exit code.
            result.status = (Status.PASS
                             if (res.ok and failed == 0 and total > 0)
                             else Status.FAIL)
        elif result.status is Status.PASS:
            # Exit 0 but no ctest summary line -> no tests actually ran.
            result.status = Status.FAIL
            result.message = "ctest exited 0 but produced no summary (no tests ran?)"
    return result


def run_standard_tests(ctx: JobContext) -> CheckResult:
    """Run GenevaStandardTests directly and flag unexpected skips."""
    if not ctx.job.spec.build_tests:
        return ctx.skipped("test/standard", Tier.SHORT, "tests not built (minimal)")
    tier = Tier.SHORT
    if not ctx.should_run(tier):
        return ctx.skipped("test/standard", tier, "skipped")
    started = time.monotonic()
    res = ctx.exec_in_guest(
        ["bash", "-lc", "./libs/geneva/tests/UnitTests/GenevaStandardTests"],
        workdir=GUEST_BUILD, timeout=3600,
    )
    result = ctx.record("test/standard", tier, res, started=started)
    if result.status is Status.PASS and not ctx.dry_run:
        blob = res.stdout + res.stderr
        m = _CATCH_COUNT.search(blob)
        ran = int(m.group(1)) if m else 0
        if ran == 0:
            # Exit 0 but nothing actually ran (no tests matched / no-op binary).
            result.status = Status.FAIL
            result.message = "exit 0 but no assertions ran (no tests executed?)"
        elif "skipped" in blob.lower():
            result.message = f"{ran} assertions ran, with skipped cases (review log)"
        else:
            result.message = f"{ran} assertions ran"
    return result
