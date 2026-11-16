"""Tests of the release harness's GPU detection (releaseharness.gpu).

Each case puts a fake ``nvidia-smi`` first on PATH, writes what a real one
prints in one situation, and checks the state the harness derives from it.
Run with ``python3 -B -m unittest discover -s ReleaseTests/tests`` (``-B``:
no bytecode is written into the source tree); ctest does so where configure
found a Python 3 interpreter.
"""

from __future__ import annotations

import os
import sys
import shlex
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from releaseharness import diagnostics, gpu  # noqa: E402

# What driver 615.71.09 prints without arguments: no "Driver Version" any more.
TABLE_615 = """\
Sun Nov 15 10:00:39 2026
+-----------------------------------------------------------------------------------------+
| NVIDIA-SMI 615.71.09              KMD Version: 615.71.09     CUDA UMD Version: 13.4     |
+-----------------------------------------+------------------------+----------------------+
| GPU  Name                 Persistence-M | Bus-Id          Disp.A | Volatile Uncorr. ECC |
|   0  NVIDIA GeForce RTX 4090        Off |   00000000:01:00.0 Off |                  Off |
+-----------------------------------------+------------------------+----------------------+
"""

MISMATCH = "Failed to initialize NVML: Driver/library version mismatch\nNVML library version: 615.71\n"

NO_DRIVER = ("NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver. "
             "Make sure that the latest NVIDIA driver is installed and running.\n")


def fake_nvidia_smi(directory: Path, *, table: str, query: str, status: int) -> None:
    """Writes an nvidia-smi into @p directory: without arguments it prints @p table and
    exits 0; asked for the driver version it prints @p query and exits with @p status.
    It uses shell builtins only, so it runs with nothing else on PATH."""
    script = directory / "nvidia-smi"
    script.write_text(
        "#!/bin/sh\n"
        'if [ "$#" -eq 0 ]; then\n'
        f"  printf '%s' {shlex.quote(table)}\n"
        "  exit 0\n"
        "fi\n"
        f"printf '%s' {shlex.quote(query)}\n"
        f"exit {status}\n"
    )
    script.chmod(0o755)


class GpuProbeTest(unittest.TestCase):
    """The state of the GPU is read from nvidia-smi's query, not from its table."""

    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory(prefix="geneva-fake-nvidia-smi-")
        self.bin = Path(self._dir.name)
        # The fake only: no real nvidia-smi can be found, and none is tried if the fake fails.
        self._env = mock.patch.dict(os.environ, {"PATH": str(self.bin)})
        self._env.start()

    def tearDown(self) -> None:
        self._env.stop()
        self._dir.cleanup()

    def test_driver_615_is_working(self) -> None:
        fake_nvidia_smi(self.bin, table=TABLE_615, query="615.71.09\n", status=0)
        self.assertTrue(gpu.gpu_available())
        self.assertEqual(diagnostics._diagnose_gpu().state, "WORKING")
        found = gpu.probe()
        self.assertEqual(found.state, gpu.WORKING, found.detail)
        self.assertIn("615.71.09", found.detail)

    def test_version_mismatch_is_usable_with_warning(self) -> None:
        fake_nvidia_smi(self.bin, table=MISMATCH, query=MISMATCH, status=18)
        self.assertTrue(gpu.gpu_available())
        self.assertEqual(diagnostics._diagnose_gpu().state, "VERSION_MISMATCH")
        found = gpu.probe()
        self.assertEqual(found.state, gpu.VERSION_MISMATCH, found.detail)

    def test_no_driver_is_unreachable(self) -> None:
        fake_nvidia_smi(self.bin, table=NO_DRIVER, query=NO_DRIVER, status=9)
        found = gpu.probe()
        self.assertEqual(found.state, gpu.UNREACHABLE, found.detail)
        self.assertIn("couldn't communicate", found.detail)
        self.assertFalse(gpu.gpu_available())

    def test_success_without_a_driver_is_unreachable(self) -> None:
        fake_nvidia_smi(self.bin, table=TABLE_615, query="", status=0)
        self.assertEqual(gpu.probe().state, gpu.UNREACHABLE)
        self.assertFalse(gpu.gpu_available())

    def test_no_nvidia_smi_is_absent(self) -> None:
        self.assertEqual(gpu.probe().state, gpu.ABSENT)
        self.assertFalse(gpu.gpu_available())
        self.assertEqual(diagnostics._diagnose_gpu().state, gpu.ABSENT)


if __name__ == "__main__":
    unittest.main()
