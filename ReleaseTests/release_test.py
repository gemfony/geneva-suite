#!/usr/bin/env python3
"""Geneva release-test harness — command-line entry point.

Drives the checks in ReleaseTestplan.md across a matrix of guest operating
systems, compilers and build types, inside containers (or VMs), and produces a
pass/fail report.

Sub-commands:
  plan       expand and print the matrix (use --dry-run to also print commands)
  provision  build the base images/instances (on request only)
  run        execute the matrix and write a report
  report     re-render the last saved JSON report

Run with no external dependencies beyond the standard library; PyYAML is used
for the optional config file if installed.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# The harness lives in the source tree, which stays free of generated files: no __pycache__.
sys.dont_write_bytecode = True

# Allow running both as `python3 release_test.py` and `python3 -m`.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from releaseharness import config as cfg_mod  # noqa: E402
from releaseharness import matrix as matrix_mod  # noqa: E402
from releaseharness import orchestrator, report  # noqa: E402
from releaseharness.backends import get_backend  # noqa: E402
from releaseharness.gpu import gpu_available  # noqa: E402
from releaseharness.logging_util import ensure_workdir, get_console_logger  # noqa: E402


def _default_config_path() -> Path | None:
    p = Path(__file__).resolve().parent / "config.yaml"
    return p if p.exists() else None


def _shared_options() -> argparse.ArgumentParser:
    """Options accepted both before AND after the sub-command (parent parser)."""
    # default=SUPPRESS so unspecified flags are simply absent from the namespace
    # and do NOT clobber a value given in the other position (the shared parser
    # is attached both before and after the sub-command).
    S = argparse.SUPPRESS
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--config", default=S,
                        help="path to config.yaml (default: ReleaseTests/config.yaml if present)")
    common.add_argument("--workdir", default=S,
                        help="override the writable work area")
    common.add_argument("--backend", default=S,
                        choices=["podman", "auto"],
                        help="container backend (only Podman is supported; default from config)")
    common.add_argument("--filter", default=S,
                        help="narrow the matrix: key=value[,key=value]; "
                             "keys: os, compiler, buildtype, label")
    common.add_argument("--build-type", default=S, metavar="TYPE[,TYPE...]",
                        help="restrict the matrix to these build type(s) before "
                             "expansion (Debug, Release, RelWithDebInfo, "
                             "MinSizeRel, Sanitize). e.g. '--build-type Release' "
                             "or '--build-type Debug' for a faster single-mode run")
    common.add_argument("--verbose", "-v", action="store_true", default=S,
                        help="verbose console logging")
    tier = common.add_mutually_exclusive_group()
    tier.add_argument("--quick", action="store_true", default=S,
                      help="SHORT-tier checks only (default)")
    tier.add_argument("--medium", action="store_true", default=S,
                      help="SHORT-tier checks across the full OS x compiler x "
                           "Debug+Release matrix, plus benchmarks built and "
                           "smoke-started once (may be aborted). Between --quick "
                           "and --full.")
    tier.add_argument("--full", action="store_true", default=S,
                      help="SHORT + LONG-tier checks (hours-long)")
    return common


def _build_parser() -> argparse.ArgumentParser:
    common = _shared_options()
    parser = argparse.ArgumentParser(
        prog="release_test.py",
        description="Geneva release-test harness.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        parents=[common],
    )

    sub = parser.add_subparsers(dest="command", required=True)

    p_plan = sub.add_parser("plan", help="print the expanded matrix", parents=[common])
    p_plan.add_argument("--dry-run", action="store_true",
                        help="also print the exact commands that would run")

    sub.add_parser("provision", help="build base images/instances (on request)",
                   parents=[common])

    p_run = sub.add_parser("run", help="execute the matrix", parents=[common])
    p_run.add_argument("--dry-run", action="store_true",
                       help="print commands without executing them")

    sub.add_parser("report", help="re-render the last saved JSON report",
                   parents=[common])

    sub.add_parser("doctor", help="report installed backends and GPU status",
                   parents=[common])

    return parser


def _load(args: argparse.Namespace):
    cfg_path = getattr(args, "config", None) or _default_config_path()
    cfg = cfg_mod.load(cfg_path)
    workdir = getattr(args, "workdir", None)
    if workdir:
        cfg.workdir = Path(workdir).resolve()
    backend = getattr(args, "backend", None)
    if backend:
        cfg.backend = backend
    return cfg


def _resolve_tier(args: argparse.Namespace) -> bool:
    """Return True for --full, False for --quick (the default)."""
    return bool(getattr(args, "full", False))


def _resolve_medium(args: argparse.Namespace) -> bool:
    """Return True for --medium (quick matrix + benchmark smoke-start)."""
    return bool(getattr(args, "medium", False))


def cmd_doctor(args: argparse.Namespace) -> int:
    from releaseharness import diagnostics

    print("Geneva release-test harness — environment doctor")
    print("=" * 64)
    diags = diagnostics.collect()
    ok_states = {"WORKING"}
    any_backend_ok = False
    for d in diags:
        marker = {
            "WORKING": "[ ok ]",
            "VERSION_MISMATCH": "[warn]",
            "PERMISSION_DENIED": "[FAIL]",
            "UNREACHABLE": "[FAIL]",
            "ABSENT": "[ -- ]",
        }.get(d.state, "[ ?? ]")
        print(f"\n{marker} {d.component:<8} : {d.state}")
        print(f"        {d.detail}")
        print(f"        -> {d.recommendation}")
        if d.component == "podman" and d.state in ok_states:
            any_backend_ok = True

    print("\n" + "-" * 64)
    if any_backend_ok:
        print("At least one container backend is WORKING. You can run:")
        print("  python3 release_test.py provision   # build the per-OS images")
        print("  python3 release_test.py run --quick  # base-mode build + ctest")
    else:
        print("No working container backend. Follow the recommendations above "
              "(Podman is preferred: rootless, no daemon, no group needed).")
    print(f"\nGPU usable for CUDA matrix entries: "
          f"{'yes' if gpu_available() else 'no (CUDA skipped)'}")
    return 0 if any_backend_ok else 1


def _parse_build_types(value: str | None):
    """Turn a comma-separated --build-type value into a set of BuildType.

    Returns None when nothing was requested (the full set of configured build
    types is then used). Matching is case-insensitive on the CMake names.
    """
    if not value:
        return None
    from releaseharness.model import BuildType
    valid = {b.value.lower(): b for b in BuildType}
    out = set()
    for tok in value.split(","):
        tok = tok.strip()
        if not tok:
            continue
        bt = valid.get(tok.lower())
        if bt is None:
            raise SystemExit(
                f"unknown --build-type {tok!r}; choose from "
                + ", ".join(b.value for b in BuildType))
        out.add(bt)
    return out or None


def _expand(cfg, args) -> list:
    full = _resolve_tier(args)
    medium = _resolve_medium(args)
    only = _parse_build_types(getattr(args, "build_type", None))
    jobs = matrix_mod.expand(cfg, full=full, gpu_available=gpu_available(),
                             only_build_types=only, medium=medium)
    return matrix_mod.apply_filter(jobs, getattr(args, "filter", None))


def cmd_plan(cfg, args) -> int:
    jobs = _expand(cfg, args)
    full = _resolve_tier(args)
    tier_name = "full" if full else ("medium" if _resolve_medium(args) else "quick")
    print(f"Matrix ({tier_name}): {len(jobs)} jobs")
    for note in cfg.notes:
        print(f"  note: {note}")
    print(f"  source_dir: {cfg.source_dir}")
    print(f"  workdir:    {cfg.workdir}")
    print(f"  backend:    {cfg.backend}")
    print(f"  GPU:        {'available' if gpu_available() else 'absent (CUDA skipped)'}")
    print()
    for j in jobs:
        print(f"  {j.slug:<48} [{j.label}]")

    if getattr(args, "dry_run", False):
        print("\nPlanned commands (representative, per job):")
        backend = get_backend(cfg.backend, dry_run=True, gpu_available=gpu_available())
        print(f"  (backend: {backend.name})")
        for j in jobs[:3]:  # show a few cells to keep output readable
            print(f"\n  --- {j.slug} ---")
            print("  .gcfg written into the build dir:")
            for line in j.spec.gcfg_text().splitlines():
                print(f"      {line}")
            run = backend.run(j.guest, j.spec,
                              ["bash", "-lc", "make -j$(nproc) gemfony-build-all"],
                              mounts=[], workdir="/work/build")
            print("  build command: " + " ".join(run.argv))
        if len(jobs) > 3:
            print(f"\n  ... and {len(jobs) - 3} more jobs (same shape).")
    return 0


def cmd_provision(cfg, args) -> int:
    verbose = getattr(args, "verbose", False)
    log = get_console_logger(verbose)
    layout = ensure_workdir(cfg.workdir)
    jobs = _expand(cfg, args)
    gpu = gpu_available()
    backend = get_backend(cfg.backend, dry_run=False, gpu_available=gpu)
    if not backend.is_available():
        log.error("backend %s not available; cannot provision. Try `doctor`.",
                  backend.name)
        return 2
    return orchestrator.provision(backend, jobs, layout,
                                  boost_root=cfg.boost_root, verbose=verbose)


def cmd_run(cfg, args) -> int:
    verbose = getattr(args, "verbose", False)
    log = get_console_logger(verbose)
    layout = ensure_workdir(cfg.workdir)
    jobs = _expand(cfg, args)
    if not jobs:
        log.error("matrix expanded to 0 jobs (check --filter / --build-type); "
                  "nothing to run -> failing.")
        return 1
    full = _resolve_tier(args)
    dry = getattr(args, "dry_run", False)
    gpu = gpu_available()
    backend = get_backend(cfg.backend, dry_run=dry, gpu_available=gpu)

    rep = orchestrator.run_matrix(
        backend, jobs, layout, cfg.source_dir,
        quick=not full, dry_run=dry, max_parallel=cfg.jobs, verbose=verbose,
    )
    text = report.render_text(rep)
    print("\n" + text)
    if not dry:
        report.write_json(rep, layout["reports"] / "last.json")
        report.write_html(rep, layout["reports"] / "last.html")
        log.info("wrote %s and last.html", layout["reports"] / "last.json")
    return 1 if rep.failed else 0


def cmd_report(cfg, args) -> int:
    last = cfg.workdir / "reports" / "last.json"
    if not last.exists():
        print(f"no saved report at {last}", file=sys.stderr)
        return 2
    print(report.render_from_json(last))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    if args.command == "doctor":
        return cmd_doctor(args)

    cfg = _load(args)
    if args.command == "plan":
        return cmd_plan(cfg, args)
    if args.command == "provision":
        return cmd_provision(cfg, args)
    if args.command == "run":
        return cmd_run(cfg, args)
    if args.command == "report":
        return cmd_report(cfg, args)
    parser.error(f"unknown command {args.command}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
