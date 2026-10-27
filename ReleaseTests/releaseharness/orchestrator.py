"""Top-level orchestration: provision, run the job matrix, collect results.

Sequences the checks for each job in the order of ReleaseTestplan.md and
aggregates everything into a RunReport.
"""

from __future__ import annotations

import datetime as _dt
from pathlib import Path

from .backends.base import ContainerBackend
from .checks import (algorithms, benchmarks, build, consumers, ctest, examples,
                     install, metadata, outoftree)
from .logging_util import ensure_workdir, get_console_logger
from .model import CheckResult, Job, RunReport, Status, Tier
from .runner import JobContext


def _now() -> str:
    return _dt.datetime.now().isoformat(timespec="seconds")


def _run_job(ctx: JobContext, log) -> list[CheckResult]:
    """Run the full check sequence for one job, in test-plan order."""
    results: list[CheckResult] = []

    # Build system
    build_results = build.configure_and_build(ctx)
    results.extend(build_results)

    # If the build did not cleanly succeed, skip the rest (including docs) with a
    # clear note. Require PASS, not merely "not FAIL": an ERROR (e.g. exit 127,
    # missing tool) or a SKIP must NOT be treated as a usable build.
    build_ok = all(r.status is Status.PASS for r in build_results) or ctx.dry_run
    if not build_ok:
        log.warning("  %s: build failed; skipping downstream checks", ctx.job.slug)
        return results

    # Docs (only meaningful once the build succeeded).
    results.append(build.build_docs(ctx))

    # Unit / integration tests
    results.append(ctest.run_ctest(ctx))
    results.append(ctest.run_standard_tests(ctx))

    # Parallelization consumers
    results.extend(consumers.threaded(ctx))
    results.append(consumers.websocket(ctx))
    results.append(consumers.mpi(ctx))

    # Optimization algorithms
    results.extend(algorithms.run_all_algorithms(ctx))
    results.append(algorithms.go2_config_roundtrip(ctx))
    results.append(algorithms.checkpoint_identity(ctx))

    # Examples
    results.append(examples.simple_optimizer(ctx))
    results.append(examples.networked(ctx))
    results.append(examples.cuda(ctx))

    # Benchmarks (medium/full tiers: start each one once; aborting is fine)
    results.append(benchmarks.smoke_start(ctx))

    # Install / out-of-tree / linking
    results.append(install.make_install(ctx))
    results.append(install.ldd_check(ctx))
    results.append(outoftree.build_against_install(ctx))
    results.append(outoftree.build_tests_mismatch(ctx))

    return results


def provision(backend: ContainerBackend, jobs: list[Job], layout: dict[str, Path],
              *, boost_root: str | None = None, verbose: bool = False) -> int:
    """Build the base images/instances for the distinct (OS, compiler) pairs."""
    log = get_console_logger(verbose)
    seen: set[str] = set()
    rc = 0
    for job in jobs:
        tag = backend.image_tag(job.guest, job.spec)
        if tag in seen:
            continue
        seen.add(tag)
        log.info("provisioning %s (%s)", tag, backend.name)
        res = backend.build_image(job.guest, job.spec, images_dir=layout["images"],
                                  boost_root=boost_root)
        if not res.ok:
            log.error("  failed: %s", (res.stderr or res.stdout).strip()[:300])
            rc = 1
        else:
            log.info("  ok")
            if res.stdout.startswith("[dry-run]"):
                log.debug(res.stdout)
    return rc


def run_matrix(backend: ContainerBackend, jobs: list[Job], layout: dict[str, Path],
               source_dir: Path, *, quick: bool, dry_run: bool,
               max_parallel: int = 1, verbose: bool = False) -> RunReport:
    """Execute every job and return an aggregate report."""
    log = get_console_logger(verbose)
    report = RunReport(started_at=_now(), quick=quick, backend=backend.name)

    # Host-side static metadata checks run once, not per job.
    report.results.extend(metadata.run_host_checks(source_dir))

    if not dry_run and not backend.is_available():
        log.error("backend %s is not available on this host; aborting run. "
                  "Use --dry-run to preview, or install/repair the backend.",
                  backend.name)
        # Record an explicit failure so the run is RED, not a false green: an
        # aborted run built and tested nothing, and must not exit 0.
        report.add(CheckResult(
            job_slug="-", check="run/backend", tier=Tier.SHORT, status=Status.FAIL,
            message=f"backend {backend.name} unavailable; nothing was built or tested",
        ))
        report.finished_at = _now()
        return report

    def _run_one(job: Job) -> list[CheckResult]:
        # Never let one job's unexpected exception escape: under the parallel
        # executor it would propagate out of as_completed and abort the WHOLE
        # run (no report written, every other job's results lost). Turn it into
        # an ERROR result for this job instead, so the run stays RED and the
        # remaining jobs still complete.
        try:
            ctx = JobContext(
                job=job, backend=backend, source_dir=source_dir,
                builds_dir=layout["builds"], logs_dir=layout["logs"],
                quick=quick, dry_run=dry_run,
            )
            return _run_job(ctx, log)
        except Exception as exc:  # noqa: BLE001 - deliberate matrix-level guard
            log.error("  %s: unhandled exception: %r", job.slug, exc)
            return [CheckResult(
                job_slug=job.slug, check="run/exception", tier=Tier.SHORT,
                status=Status.ERROR, message=f"unhandled exception: {exc!r}"[:300],
            )]

    # report.add() / logging happen only on the calling thread; the parallel
    # work in _run_one() is fully isolated per job (each runs in its own
    # container with per-slug build/log dirs), so no extra locking is needed.
    def _emit(job: Job, results: list[CheckResult]) -> None:
        for res in results:
            report.add(res)
            level = log.info if res.status in (Status.PASS, Status.SKIP) else log.error
            level("    %-32s %-5s %s", res.check, res.status.value, res.message)

    workers = max(1, min(int(max_parallel), len(jobs))) if jobs else 1
    if workers <= 1:
        for i, job in enumerate(jobs, 1):
            log.info("[%d/%d] job %s", i, len(jobs), job.slug)
            _emit(job, _run_one(job))
    else:
        from concurrent.futures import ThreadPoolExecutor, as_completed
        log.info("running %d jobs, up to %d concurrently", len(jobs), workers)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            fut_to_job = {pool.submit(_run_one, job): job for job in jobs}
            for done, fut in enumerate(as_completed(fut_to_job), 1):
                job = fut_to_job[fut]
                log.info("[%d/%d] job %s done", done, len(jobs), job.slug)
                _emit(job, fut.result())

    report.finished_at = _now()
    return report
