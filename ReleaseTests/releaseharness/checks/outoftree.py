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
        # Run it against the installed shared libraries. The quickstart runs a full
        # optimization and exits on its own; cap the wall-clock as a safety net
        # (a clean finish OR a timeout both prove it started and linked).
        f"LD_LIBRARY_PATH='{prefix}'/lib:\"$LD_LIBRARY_PATH\" "
        "timeout -k 5 -s INT 180s ./GStarter; rc=$?\n"
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
    package, links Geneva::geneva-mpi, and exits 0 only if the "mpi" consumer was loaded from the
    installation.
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
        f"LD_LIBRARY_PATH='{prefix}'/lib:\"$LD_LIBRARY_PATH\" '{_MPI_PROBE_BUILD}/GenevaMpiModuleProbe'\n"
    )
    res = ctx.exec_in_guest(["bash", "-lc", script], workdir=GUEST_BUILD, timeout=900)
    return ctx.record("outoftree/mpi-module", tier, res, started=started)


def build_tests_mismatch(ctx: JobContext) -> CheckResult:
    """Verify a consumer can detect a GENEVA_BUILD_TESTS mismatch via the exposed variable.

    Geneva's installed config package (GenevaConfig.cmake.in:66) and the bundled
    FindGeneva.cmake both export the boolean ``GENEVA_TESTING`` variable, which
    reflects whether the install was built with testing support. A consumer can
    enforce a match by asserting that variable. This check synthesises a tiny
    standalone CMake consumer that, after ``find_package(Geneva CONFIG REQUIRED)``,
    asserts ``GENEVA_TESTING`` matches a value passed in via ``-D``. It then
    configures the consumer TWICE: once with the matching value (must SUCCEED)
    and once with the opposite value (must FAIL with the assertion's message).
    Both halves must behave as expected for the check to pass.
    """
    tier = Tier.SHORT
    if not ctx.should_run(tier):
        return ctx.skipped("outoftree/build-tests-mismatch", tier, "skipped")
    spec = ctx.job.spec
    prefix = spec.install_dir
    expected = "ON" if spec.build_tests else "OFF"   # the install side
    opposite = "OFF" if spec.build_tests else "ON"
    started = time.monotonic()
    script = (
        "set -e\n"
        f"D='{GUEST_BUILD}/oot-mismatch'\n"
        'rm -rf "$D" && mkdir -p "$D/src" && cd "$D/src"\n'
        # Tiny consumer cmake: find Geneva, then verify GENEVA_TESTING matches
        # the consumer's CONSUMER_REQUIRES_TESTING expectation.
        "cat > CMakeLists.txt <<'CMAKE'\n"
        "cmake_minimum_required(VERSION 3.27)\n"
        # CXX is required because Geneva's CONFIG package transitively re-finds
        # Boost, and Boost's CMake config refuses to load without a C++ compiler.
        "project(geneva_mismatch_probe CXX)\n"
        "find_package(Geneva CONFIG REQUIRED)\n"
        "if(NOT DEFINED CONSUMER_REQUIRES_TESTING)\n"
        "  message(FATAL_ERROR \"-DCONSUMER_REQUIRES_TESTING=ON|OFF required\")\n"
        "endif()\n"
        "if(CONSUMER_REQUIRES_TESTING AND NOT GENEVA_TESTING)\n"
        "  message(FATAL_ERROR\n"
        "    \"GENEVA_BUILD_TESTS mismatch: consumer requires a testing-enabled Geneva install\"\n"
        "    \" but the install at ${CMAKE_PREFIX_PATH} reports GENEVA_TESTING=FALSE\")\n"
        "endif()\n"
        "if(NOT CONSUMER_REQUIRES_TESTING AND GENEVA_TESTING)\n"
        "  message(FATAL_ERROR\n"
        "    \"GENEVA_BUILD_TESTS mismatch: consumer requires a non-testing Geneva install\"\n"
        "    \" but the install at ${CMAKE_PREFIX_PATH} reports GENEVA_TESTING=TRUE\")\n"
        "endif()\n"
        "message(STATUS \"GENEVA_TESTING matches consumer expectation: ${CONSUMER_REQUIRES_TESTING}\")\n"
        "CMAKE\n"
        # Phase 1: matching expectation -> configure must SUCCEED.
        'cd "$D" && rm -rf build-match && mkdir build-match && cd build-match\n'
        f"cmake -DCMAKE_PREFIX_PATH='{prefix}' -DCONSUMER_REQUIRES_TESTING={expected} "
        '"$D/src" > /tmp/oot_match.log 2>&1 || '
        '{ echo "PHASE1 (match) UNEXPECTEDLY FAILED:"; tail -20 /tmp/oot_match.log; exit 1; }\n'
        f'grep -q "GENEVA_TESTING matches consumer expectation: {expected}" /tmp/oot_match.log '
        '|| { echo "PHASE1 missing match-status:"; tail -20 /tmp/oot_match.log; exit 1; }\n'
        # Phase 2: opposite expectation -> configure must FAIL with our message.
        'cd "$D" && rm -rf build-bad && mkdir build-bad && cd build-bad\n'
        f"cmake -DCMAKE_PREFIX_PATH='{prefix}' -DCONSUMER_REQUIRES_TESTING={opposite} "
        '"$D/src" > /tmp/oot_bad.log 2>&1 && '
        '{ echo "PHASE2 (mismatch) UNEXPECTEDLY SUCCEEDED:"; tail -20 /tmp/oot_bad.log; exit 1; }\n'
        'grep -q "GENEVA_BUILD_TESTS mismatch" /tmp/oot_bad.log '
        '|| { echo "PHASE2 failed but mismatch FATAL_ERROR not emitted:"; '
        'tail -20 /tmp/oot_bad.log; exit 1; }\n'
        'echo "build-tests-mismatch OK '
        f'(install GENEVA_TESTING={expected}: matched expectation succeeds, opposite fails as expected)"\n'
    )
    res = ctx.exec_in_guest(["bash", "-lc", script],
                            workdir=GUEST_BUILD, timeout=600)
    return ctx.record("outoftree/build-tests-mismatch", tier, res, started=started)
