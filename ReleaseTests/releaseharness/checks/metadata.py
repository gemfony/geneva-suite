"""Documentation / release-metadata checks (ReleaseTestplan.md: "Documentation
and release metadata").

These are static, host-side checks (no container needed): the version the
build uses is parsed from GGlobalDefines.hpp, where it is written, and the CHANGES
file is confirmed to carry a matching entry. ``make doc`` itself runs as a build check (build.py).
"""

from __future__ import annotations

import re
import time
from pathlib import Path

from ..model import CheckResult, Status, Tier

_VERSION_HEADER = Path("libs/common/include/common/GGlobalDefines.hpp")

_VER_RE = {
    "major": re.compile(r"^#define[ \t]+GEM_VERSION_MAJOR[ \t]+(\d+)", re.MULTILINE),
    "minor": re.compile(r"^#define[ \t]+GEM_VERSION_MINOR[ \t]+(\d+)", re.MULTILINE),
    "patch": re.compile(r"^#define[ \t]+GEM_VERSION_PATCH[ \t]+(\d+)", re.MULTILINE),
}


def _result(check: str, status: Status, message: str, started: float) -> CheckResult:
    return CheckResult(job_slug="host", check=check, tier=Tier.SHORT,
                       status=status, duration_s=time.monotonic() - started,
                       message=message)


def cmake_version(source_dir: Path) -> tuple[str | None, CheckResult]:
    """Parse the version from GGlobalDefines.hpp (the top-level CMakeLists.txt reads it there)."""
    started = time.monotonic()
    header = source_dir / _VERSION_HEADER
    if not header.exists():
        return None, _result("metadata/version", Status.ERROR,
                              f"{_VERSION_HEADER} not found", started)
    text = header.read_text(encoding="utf-8", errors="replace")
    parts = {}
    for key, rx in _VER_RE.items():
        m = rx.search(text)
        if not m:
            return None, _result("metadata/version", Status.FAIL,
                                 f"GEM_VERSION_{key.upper()} not found", started)
        parts[key] = m.group(1)
    version = f"{parts['major']}.{parts['minor']}.{parts['patch']}"
    return version, _result("metadata/version", Status.PASS,
                            f"Geneva version {version}", started)


def changes_entry(source_dir: Path, version: str | None) -> CheckResult:
    """Confirm CHANGES has an entry for the Geneva version."""
    started = time.monotonic()
    changes = source_dir / "CHANGES"
    if not changes.exists():
        return _result("metadata/changes", Status.FAIL, "CHANGES not found", started)
    if version is None:
        return _result("metadata/changes", Status.SKIP,
                       "no version to match (version parse failed)", started)
    head = "\n".join(changes.read_text(encoding="utf-8", errors="replace").splitlines()[:40])
    if f"Version {version}" in head:
        return _result("metadata/changes", Status.PASS,
                       f"CHANGES has 'Version {version}'", started)
    return _result("metadata/changes", Status.FAIL,
                   f"no 'Version {version}' entry near top of CHANGES", started)


def run_host_checks(source_dir: Path) -> list[CheckResult]:
    version, ver_res = cmake_version(source_dir)
    return [ver_res, changes_entry(source_dir, version)]
