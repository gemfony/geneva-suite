"""Out-of-tree application build (ReleaseTestplan.md: "Out-of-tree application
build").

``build_against_install`` is the real out-of-tree consumer test: it configures
and builds the shipped quickstart as a standalone CMake project that
locates the *installed* Geneva via ``find_package(Geneva)`` (the installed
CONFIG package, with the bundled FindGeneva.cmake as fallback) and links the
``Geneva::geneva`` imported target, then runs the resulting binary. It depends
on install/make-install having populated the prefix, so it runs in the SHORT
tier (i.e. also under ``--medium``).
"""

from __future__ import annotations

import time

from ..model import BuildType, Compiler, CheckResult, Tier
from ..runner import GUEST_BUILD, GUEST_SRC, JobContext

# The quickstart is the canonical downstream template: it is NOT built in-tree
# and exists precisely to be consumed via find_package(Geneva) from an install.
_OOT_SRC = f"{GUEST_SRC}/libs/geneva/quickstart"
_OOT_BUILD = f"{GUEST_BUILD}/oot-gstarter"


def build_against_install(ctx: JobContext) -> CheckResult:
    """Configure+build+run the quickstart standalone via find_package(Geneva)."""
    tier = Tier.SHORT
    if not ctx.should_run(tier):
        return ctx.skipped("outoftree/findgeneva", tier, "skipped")
    if ctx.job.spec.build_type is BuildType.SANITIZE:
        # A standalone downstream app built WITHOUT -fsanitize links against the
        # TSan/ASan-instrumented installed Geneva libraries and aborts at startup
        # (the sanitizer runtime must be in the main executable). Out-of-tree
        # install/link correctness is covered by the non-Sanitize cells, so skip
        # this combination rather than report a spurious failure.
        return ctx.skipped("outoftree/findgeneva", tier,
                           "n/a for Sanitize builds (sanitized libs vs. "
                           "non-sanitized downstream app)")
    spec = ctx.job.spec
    prefix = spec.install_dir
    cxx = "clang++" if spec.compiler is Compiler.CLANG else "g++"
    cc = "clang" if spec.compiler is Compiler.CLANG else "gcc"
    started = time.monotonic()
    script = (
        "set -e\n"
        f"rm -rf '{_OOT_BUILD}'\n"
        f"mkdir -p '{_OOT_BUILD}'\n"
        f"cd '{_OOT_BUILD}'\n"
        # Out-of-tree configure: Geneva is found ONLY via the install prefix
        # (CMAKE_PREFIX_PATH) -> exercises find_package(Geneva) / Geneva::geneva.
        "cmake "
        f"-DCMAKE_PREFIX_PATH='{prefix}' "
        f"-DBOOST_ROOT='{spec.boost_root}' "
        f"-DCMAKE_BUILD_TYPE='{spec.build_type.value}' "
        f"-DCMAKE_C_COMPILER='{cc}' -DCMAKE_CXX_COMPILER='{cxx}' "
        f"'{_OOT_SRC}'\n"
        # Build only the GStarter executable (skip the optional Catch2 test).
        "make -j$(nproc) GStarter\n"
        "test -x ./GStarter || { echo 'MISSING GStarter binary'; exit 1; }\n"
        # Run it against the installed shared libraries, found through the run
        # paths alone: no LD_LIBRARY_PATH, which would hide a missing run path.
        # The quickstart runs a full optimization and exits on its own; cap the
        # wall-clock as a safety net (a clean finish OR a timeout both prove it
        # started and linked).
        "timeout -k 5 -s INT 180s env -u LD_LIBRARY_PATH ./GStarter; rc=$?\n"
        'if [ "$rc" = 0 ] || [ "$rc" = 124 ]; then\n'
        '  echo "out-of-tree GStarter ran OK (rc=$rc)"\n'
        "else\n"
        '  echo "out-of-tree GStarter FAILED at runtime rc=$rc"; exit "$rc"\n'
        "fi\n"
    )
    res = ctx.exec_in_guest(["bash", "-lc", script],
                            workdir=GUEST_BUILD, timeout=1800)
    return ctx.record("outoftree/findgeneva", tier, res, started=started)


_MPI_PROBE_SRC = f"{GUEST_SRC}/libs/geneva/tests/OutOfTree/MpiModuleProbe"
_MPI_PROBE_BUILD = f"{GUEST_BUILD}/oot-mpi-probe"


def build_mpi_module_probe(ctx: JobContext) -> CheckResult:
    """Build and run the MPI-module probe against the install (MPI cells only).

    The MPI consumer lives in libgemfony-geneva-mpi, a module installed next to libgemfony-geneva and
    loaded at run time. The probe (libs/geneva/tests/OutOfTree/MpiModuleProbe) finds the installed
    package, links Geneva::courtier-mpi, and exits 0 only if the "mpi"
    consumer was loaded from the installation.
    """
    tier = Tier.SHORT
    if not ctx.job.spec.with_mpi:
        return ctx.skipped("outoftree/mpi-module", tier, "built without the MPI consumer")
    if not ctx.should_run(tier):
        return ctx.skipped("outoftree/mpi-module", tier, "skipped")
    if ctx.job.spec.build_type is BuildType.SANITIZE:
        return ctx.skipped("outoftree/mpi-module", tier,
                           "n/a for Sanitize builds (sanitized libs vs. non-sanitized probe)")
    spec = ctx.job.spec
    prefix = spec.install_dir
    cxx = "clang++" if spec.compiler is Compiler.CLANG else "g++"
    cc = "clang" if spec.compiler is Compiler.CLANG else "gcc"
    started = time.monotonic()
    script = (
        "set -e\n"
        f"rm -rf '{_MPI_PROBE_BUILD}'\n"
        f"cmake -S '{_MPI_PROBE_SRC}' -B '{_MPI_PROBE_BUILD}' "
        f"-DCMAKE_PREFIX_PATH='{prefix}' -DBOOST_ROOT='{spec.boost_root}' "
        f"-DCMAKE_C_COMPILER='{cc}' -DCMAKE_CXX_COMPILER='{cxx}'\n"
        f"cmake --build '{_MPI_PROBE_BUILD}'\n"
        # Found through the run paths alone (the probe's, the libraries' and the module's): no
        # LD_LIBRARY_PATH, which would hide a missing one.
        f"env -u LD_LIBRARY_PATH '{_MPI_PROBE_BUILD}/GenevaMpiModuleProbe'\n"
    )
    res = ctx.exec_in_guest(["bash", "-lc", script], workdir=GUEST_BUILD, timeout=900)
    return ctx.record("outoftree/mpi-module", tier, res, started=started)
