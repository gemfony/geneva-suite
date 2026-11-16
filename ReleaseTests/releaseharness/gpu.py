"""GPU detection.

Whether the host has a usable NVIDIA GPU is decided here, once, from the
machine interface of ``nvidia-smi``: the query
``nvidia-smi --query-gpu=driver_version --format=csv,noheader`` succeeds and
names a driver for at least one GPU. The human-readable table is not read; its
wording changes between driver releases (615 prints "KMD Version" where older
drivers printed "Driver Version").

On some hosts ``nvidia-smi`` reports a *driver/library version mismatch*
while CUDA compute still works (e.g. the CUDA example 14 runs). That degraded
state is treated as *usable-with-warning*, not absent, so CUDA matrix entries
are not skipped purely because NVML is stale. When there is no GPU at all,
CUDA matrix entries are skipped gracefully rather than failed.

``probe()`` gives the state with a detail line; ``gpu_available()`` the
yes/no the matrix needs; the ``doctor`` sub-command (``diagnostics``) prints
the state with a recommendation.
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass

#: nvidia-smi answers the query and names a driver for at least one GPU
WORKING = "WORKING"
#: nvidia-smi reports a driver/library version mismatch (CUDA may still work)
VERSION_MISMATCH = "VERSION_MISMATCH"
#: nvidia-smi is there but does not name a driver (it cannot reach one)
UNREACHABLE = "UNREACHABLE"
#: there is no nvidia-smi
ABSENT = "ABSENT"

_QUERY = ["nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"]


@dataclass(frozen=True)
class GpuState:
    """What ``probe()`` found: one of the four states and a line about it."""

    state: str
    detail: str


def probe(timeout: int = 15) -> GpuState:
    """Asks nvidia-smi for the driver version of every GPU.

    WORKING if it exits 0 and names a driver; VERSION_MISMATCH if its answer
    says "version mismatch"; ABSENT without nvidia-smi; UNREACHABLE otherwise.
    """
    if shutil.which("nvidia-smi") is None:
        return GpuState(ABSENT, "nvidia-smi not found")
    try:
        proc = subprocess.run(_QUERY, capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return GpuState(UNREACHABLE, f"nvidia-smi did not answer within {timeout} s")
    except OSError as e:
        return GpuState(UNREACHABLE, f"nvidia-smi could not be run: {e}")

    drivers = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    if proc.returncode == 0 and drivers:
        gpus = "1 GPU" if len(drivers) == 1 else f"{len(drivers)} GPUs"
        return GpuState(WORKING, f"{gpus}, driver {drivers[0]}")

    answer = (proc.stdout + proc.stderr).strip()
    if "version mismatch" in answer.lower():
        return GpuState(VERSION_MISMATCH,
                        "NVML driver/library version mismatch (nvidia-smi fails, "
                        "but CUDA compute may still work)")
    first_line = answer.splitlines()[0] if answer else ""
    return GpuState(UNREACHABLE,
                    first_line or f"nvidia-smi exited {proc.returncode} and named no driver")


def gpu_available() -> bool:
    """True if a GPU is present and at least usable-with-warning."""
    return probe().state in (WORKING, VERSION_MISMATCH)
