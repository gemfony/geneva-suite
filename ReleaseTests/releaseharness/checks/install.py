"""Install / runtime-linking checks (ReleaseTestplan.md: "Installation and
runtime linking").

``make_install`` performs a real ``make install`` into the configured prefix
inside the (ephemeral, ``--rm``) guest and verifies that the headers, the four
shared libraries and the exported CMake config package land where the
out-of-tree ``find_package(Geneva)`` consumer build (see checks/outoftree.py)
expects them. It is the prerequisite for that out-of-tree check, so it runs in
the SHORT tier (i.e. also under ``--medium``).
"""

from __future__ import annotations

import time

from ..model import CheckResult, Tier
from ..runner import GUEST_BUILD, JobContext


def make_install(ctx: JobContext) -> CheckResult:
    """`make install` into the prefix; verify headers, libs and CMake config land."""
    tier = Tier.SHORT
    if not ctx.should_run(tier):
        return ctx.skipped("install/make-install", tier, "skipped")
    prefix = ctx.job.spec.install_dir
    started = time.monotonic()
    script = (
        "set -e\n"
        "make -j$(nproc) install\n"
        f"PREFIX='{prefix}'\n"
        'test -d "$PREFIX/include/geneva" '
        '|| { echo "MISSING $PREFIX/include/geneva"; exit 1; }\n'
        'ls "$PREFIX"/lib*/libgemfony-geneva* >/dev/null 2>&1 '
        '|| { echo "MISSING libgemfony-geneva under $PREFIX/lib*"; exit 1; }\n'
        'cfg=$(find "$PREFIX" -name GenevaConfig.cmake 2>/dev/null | head -1)\n'
        '[ -n "$cfg" ] || { echo "MISSING GenevaConfig.cmake under $PREFIX"; exit 1; }\n'
        'echo "installed OK: $PREFIX (config package: $cfg)"\n'
    )
    res = ctx.exec_in_guest(["bash", "-lc", script],
                            workdir=GUEST_BUILD, timeout=1200)
    return ctx.record("install/make-install", tier, res, started=started)


def ldd_check(ctx: JobContext) -> CheckResult:
    """Run ldd on every installed shared library, fail on unresolved symbols.

    After install/make-install populates the prefix this check runs ``ldd`` on
    each non-symlink libgemfony-* library (the libraries and the optional
    modules) with ``LD_LIBRARY_PATH`` unset: the transitive ``Geneva::*``
    dependencies, and Boost's, must resolve through the libraries' own run
    paths, as they do for a program that uses the installation. Any ``not found``
    line is a packaging regression (a missing install run path among them) and
    fails the check.
    """
    tier = Tier.SHORT
    if not ctx.should_run(tier):
        return ctx.skipped("install/ldd", tier, "skipped")
    prefix = ctx.job.spec.install_dir
    started = time.monotonic()
    script = (
        "set -e\n"
        f"PREFIX='{prefix}'\n"
        "unset LD_LIBRARY_PATH\n"
        "fail=0\n"
        "checked=0\n"
        'for so in "$PREFIX"/lib*/libgemfony-*.so*; do\n'
        '  [ -f "$so" ] || continue\n'
        '  [ -L "$so" ] && continue\n'  # skip versioned symlinks
        "  checked=$((checked+1))\n"
        '  if ldd "$so" 2>&1 | grep -q "not found"; then\n'
        '    echo "UNRESOLVED in $so:"; ldd "$so" 2>&1 | grep "not found"\n'
        "    fail=1\n"
        "  fi\n"
        "done\n"
        '[ "$checked" -gt 0 ] || { echo "no installed libgemfony-* found under $PREFIX"; exit 1; }\n'
        '[ "$fail" -eq 0 ] && echo "ldd: $checked installed libs OK (no unresolved symbols)"\n'
        'exit "$fail"\n'
    )
    res = ctx.exec_in_guest(["bash", "-lc", script],
                            workdir=GUEST_BUILD, timeout=300)
    return ctx.record("install/ldd", tier, res, started=started)
