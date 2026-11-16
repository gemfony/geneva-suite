"""Environment diagnostics for the ``doctor`` sub-command.

Probes the container backends and the GPU and classifies each into an
actionable state, printing recommendations the user can act on directly.

Backend states:
  * ABSENT             — binary not installed; recommend the apt package.
  * UNREACHABLE        — binary present, but the engine/daemon is not reachable
                         (e.g. Docker daemon down, rootless runtime broken).
  * PERMISSION_DENIED  — binary present, daemon reachable, but access is denied
                         (Docker: user not in the ``docker`` group).
  * WORKING            — fully usable.

GPU states (decided by ``gpu.probe()``):
  * WORKING            — nvidia-smi names the driver of at least one GPU.
  * VERSION_MISMATCH   — nvidia-smi reports a driver/library version mismatch.
                         CUDA may still work (e.g. example 14 runs); recommend a
                         reboot or kernel-module reload to clear NVML.
  * UNREACHABLE        — nvidia-smi present but cannot talk to the driver.
  * ABSENT             — no nvidia-smi at all.
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass

from . import gpu


@dataclass
class Diagnosis:
    component: str
    state: str
    detail: str
    recommendation: str


def _run(argv: list[str], timeout: int = 20) -> tuple[int, str, str]:
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except FileNotFoundError:
        return 127, "", "not found"
    except subprocess.TimeoutExpired:
        return 124, "", f"timeout after {timeout}s"


def _diagnose_podman() -> Diagnosis:
    if shutil.which("podman") is None:
        return Diagnosis(
            "podman", "ABSENT", "podman binary not found",
            "Install Podman:  sudo apt install -y podman",
        )
    rc, out, err = _run(["podman", "info"], timeout=30)
    blob = (out + err).lower()
    if rc == 0:
        ver_rc, ver_out, _ = _run(["podman", "--version"])
        ver = ver_out.strip() if ver_rc == 0 else "podman"
        return Diagnosis(
            "podman", "WORKING", ver,
            "Ready (rootless). This is the harness default backend.",
        )
    # Rootless setup problems usually surface as runtime-dir / subuid issues.
    if "subuid" in blob or "subgid" in blob or "newuidmap" in blob:
        return Diagnosis(
            "podman", "UNREACHABLE",
            "rootless id-mapping not configured",
            "Add subuid/subgid ranges, e.g.:  "
            "sudo usermod --add-subuids 100000-165535 "
            "--add-subgids 100000-165535 $USER  &&  podman system migrate",
        )
    if "read-only" in blob or "runroot" in blob or "libpod" in blob:
        return Diagnosis(
            "podman", "UNREACHABLE",
            f"runtime directory not writable: {(err or out).strip()[:160]}",
            "Ensure $XDG_RUNTIME_DIR (/run/user/$UID) is writable; "
            "if it is read-only, set XDG_RUNTIME_DIR to a writable path or "
            "fix the user session (loginctl enable-linger $USER).",
        )
    return Diagnosis(
        "podman", "UNREACHABLE", (err or out).strip()[:200] or f"exit {rc}",
        "Run `podman info` to see the full error; check the rootless setup "
        "(subuid/subgid, $XDG_RUNTIME_DIR).",
    )


def _diagnose_gpu() -> Diagnosis:
    found = gpu.probe()
    if found.state == gpu.ABSENT:
        return Diagnosis(
            "gpu", found.state, found.detail,
            "No NVIDIA GPU detected — CUDA matrix entries are skipped. "
            "Install the driver if a GPU is expected.",
        )
    if found.state == gpu.WORKING:
        return Diagnosis("gpu", found.state, found.detail,
                         "CUDA passthrough: Podman --device nvidia.com/gpu=all (CDI).")
    if found.state == gpu.VERSION_MISMATCH:
        return Diagnosis(
            "gpu", found.state, found.detail,
            "Clear the stale kernel module: reboot, OR reload modules "
            "(sudo rmmod nvidia_uvm nvidia_drm nvidia_modeset nvidia; "
            "sudo modprobe nvidia). Ensure your user is in the 'video' and "
            "'render' groups for device access. CUDA passthrough still works "
            "for compute (Podman --device nvidia.com/gpu=all).",
        )
    return Diagnosis(
        "gpu", found.state, found.detail,
        "nvidia-smi cannot talk to the driver. Verify the kernel module is "
        "loaded (lsmod | grep nvidia) and reboot if it was just updated.",
    )


def collect() -> list[Diagnosis]:
    """Diagnose all backends and the GPU."""
    return [
        _diagnose_podman(),
        _diagnose_gpu(),
    ]
